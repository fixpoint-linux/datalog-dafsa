//! vector.zig — vector tier query path (S2): SCAN-AND-RANK over __vec_q__.
//!
//! dl_vector_search / dl_vector_search_version (and the corpus-parameterized
//! forms) scan the corpus's vector relation, score every LIVE entity's stored
//! int8 vector with integer arithmetic, and emit the top k in EXACT cosine
//! order.  Nothing else is read: the MIH band postings (__sig0__..__sig15__)
//! and the never-read __itq_basis__ are gone from the query path — see the
//! SUPERSEDED notes in docs/datalog-dafsa-vector-search.md.
//!
//! THE QUERY, in five steps (one ranking path, shared with dl_vector_rerank):
//!   1. unpack the pre-quantized int8 query vector (96 u32 -> 384 i32 lanes);
//!   2. scan the corpus liveness relation -> a sorted sym set;
//!   3. scan __vec_q__: for each row of a LIVE sym, accumulate <q,v> and |v|^2
//!      in a sym-keyed map (row order is NOT assumed — a relation enumerates
//!      its base DAFSA then its overlay, so rows are not grouped by col 0);
//!   4. RankCand{ sym, dot, norm2, isqrt(norm2) } per map entry;
//!   5. sort by the SIGNED int64 cross-multiply dot_a*|b| vs dot_b*|a| (exact
//!      cosine order, integer isqrt — no float, no sqrtf) and emit at most k.
//!
//! Recall is 100% by construction (no pre-filter, no pigeonhole misses); the
//! cost is O(N*D) int8 multiply-accumulates and O(N) query-time memory for N
//! live vectors.  MEASURED costs are recorded in the docs; this is the trade
//! the scan design bought deliberately.
//!
//! LIVE vs VERSION read discipline (F1/F2, the load-bearing invariant):
//!   - dl_prefix / dl_lookup are LIVE-ONLY (dl.zig) — they never route to the
//!     snapshot view.  VERSION reads MUST use dl_query_bound_version.
//!   - LIVE search reads BOTH the liveness relation and __vec_q__ via
//!     dl_prefix.
//!   - VERSION search reads BOTH via dl_query_bound_version.
//! The two search entry points share ONE implementation that branches ONLY on
//! the read primitive (mirrors dl_search / dl_search_version in index.zig),
//! so a publish between the liveness scan and the vector scan can never
//! produce an inconsistent candidate set within a single mode.
//!
//! dl_vector_search_gated / _gated_version add ONE more membership test to the
//! same body: a caller-supplied sym gate (the hybrid path's lexical entity
//! set).  The rank set is then the top-k OF THE INTERSECTION — exact, with no
//! k*multiplier truncation — and the unfiltered forms are that same body with
//! no gate (behaviour unchanged).
//!
//! Strangler-hybrid ABI: `struct dl_vec_corpus` is a CONCRETE extern struct
//! still constructed/dereferenced by C (the retained dl_cli oracle and the
//! vector test suites), so it is defined here byte-identical to vector.h and
//! comptime-gated against @cImport("vector.h")'s translate-c layout.  dl_db
//! is dl.zig's DlDb; dl_prefix / dl_query_bound_version come from dl.zig.
//!
//! All allocation through raw libc (plus libc-backed std containers).

const std = @import("std");
const c = std.c;

const dl = @import("dl.zig");
const relation = @import("relation.zig");

// vector.h: struct dl_vec_corpus + the compile-time layout constants shared
// verbatim with the external encoder (reference for the comptime layout gate
// only).
const cv = @cImport({
    @cInclude("vector.h");
});

// ─── Compile-time layout constants (mirror src/vector.h) ──────────────────

const VEC_D = 384; // embedding dim (bge-small)
const VEC_IVEC_WORDS = VEC_D / 4; // 96 u32, 4 int8 packed each
const VEC_ENTITY_REL = "entity"; // arity-2, name sym-id in col 0

// ─── Public C-ABI struct (src/vector.h, byte-for-byte) ────────────────────

/// struct dl_vec_corpus — the vector tier is shared by multiple corpora; each
/// corpus keeps SEPARATE liveness / vec relations.  ENTITY is the historical
/// path (byte-compatible); OBSERVATION_CONTENT indexes observation CONTENT
/// text (col 1 of `observation`).
pub const DlVecCorpus = extern struct {
    filter_rel: ?[*:0]const u8, // liveness relation
    filter_col: u8, // which col holds the sym
    vec_rel: ?[*:0]const u8, // e.g. "__vec_q__" / "__vec_obs__"
};

// Comptime gate: our extern layout must be byte-identical to the C header.
comptime {
    std.debug.assert(@sizeOf(DlVecCorpus) == @sizeOf(cv.struct_dl_vec_corpus));
    std.debug.assert(@offsetOf(DlVecCorpus, "filter_col") == @offsetOf(cv.struct_dl_vec_corpus, "filter_col"));
    std.debug.assert(@offsetOf(DlVecCorpus, "vec_rel") == @offsetOf(cv.struct_dl_vec_corpus, "vec_rel"));
}

/// DL_VEC_CORPUS_ENTITY — the macro as a const instance.
pub const DL_VEC_CORPUS_ENTITY = DlVecCorpus{
    .filter_rel = VEC_ENTITY_REL,
    .filter_col = 0,
    .vec_rel = "__vec_q__",
};

/// DL_VEC_CORPUS_OBSERVATION_CONTENT — the macro as a const instance.
pub const DL_VEC_CORPUS_OBSERVATION_CONTENT = DlVecCorpus{
    .filter_rel = "observation",
    .filter_col = 1,
    .vec_rel = "__vec_obs__",
};

/// Callback type: typedef int (*dl_vec_cb)(uint32_t entity_sym, int score,
/// void *user).
pub const DlVecCb = ?*const fn (entity_sym: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int;

/// dl_tuple_cb == rel_enum_cb (shared type identity with dl.zig's DlTupleCb).
const DlTupleCb = relation.RelEnumCb;

// ─── Liveness filter set ──────────────────────────────────────────────────

/// One scan of the corpus liveness relation collects every live sym into a
/// growable array (realloc-doubling), then sort+dedup so per-row membership is
/// a bsearch.  Empty corpus -> empty set -> the search yields 0 candidates
/// (correct).
const VecFilterSet = struct {
    d: ?[*]u32 = null,
    n: usize = 0,
    cap: usize = 0,
    error_occurred: bool = false,
    col: u8,
};

fn collectFilterCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const s: *VecFilterSet = @ptrCast(@alignCast(user.?));
    if (@as(usize, s.col) >= arity) return 1; // malformed row: stop scan
    if (s.n >= s.cap) {
        const nc: usize = if (s.cap != 0) s.cap * 2 else 64;
        const nd: [*]u32 = @ptrCast(@alignCast(c.realloc(@ptrCast(s.d), nc * @sizeOf(u32)) orelse {
            s.error_occurred = true;
            return 1;
        }));
        s.d = nd;
        s.cap = nc;
    }
    s.d.?[s.n] = cols.?[s.col];
    s.n += 1;
    return 0;
}

/// sort + linear in-place dedup.
fn fsSortDedup(s: *VecFilterSet) void {
    // (n == 0 arrives with d == NULL; the C oracle's qsort(NULL, 0, ...) was
    // a tolerated no-op — skip the sort rather than unwrap a null slice.)
    if (s.n > 0) std.mem.sort(u32, s.d.?[0..s.n], {}, std.sort.asc(u32));
    var w: usize = 0;
    var i: usize = 0;
    while (i < s.n) : (i += 1) {
        if (w == 0 or s.d.?[w - 1] != s.d.?[i]) {
            s.d.?[w] = s.d.?[i];
            w += 1;
        }
    }
    s.n = w;
}

fn fsContains(s: *const VecFilterSet, sym: u32) bool {
    var lo: usize = 0;
    var hi: usize = s.n;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        if (s.d.?[mid] == sym) return true;
        if (s.d.?[mid] < sym) lo = mid + 1 else hi = mid;
    }
    return false;
}

fn fsFree(s: *VecFilterSet) void {
    c.free(@ptrCast(s.d));
    s.* = .{ .col = s.col };
}

// ─── Optional caller-supplied sym gate ────────────────────────────────────

/// A SECOND membership test alongside liveness: the caller passes the sym set
/// it wants ranked (the hybrid path passes the lexical entity set), and the
/// scan ranks only the intersection.  Passing the gate this way is what lets
/// the intersection be exact at any k — the caller no longer has to pre-truncate
/// the scan to k*multiplier and intersect afterwards.
///
/// `active == false` (NULL syms) is the unfiltered search: identical semantics
/// to dl_vector_search.  `active == true, n == 0` is an EMPTY gate -> zero
/// results, which is deliberately distinct from "no gate".
///
/// The caller's array is copied and sort+deduped once here, so the input may be
/// unsorted and may repeat syms (the CLI's lexical hits can do both); the
/// per-entity cost is then the same bsearch the liveness test already pays.
const VecGate = struct {
    set: VecFilterSet = .{ .col = 0 },
    active: bool = false,

    fn deinit(g: *VecGate) void {
        if (g.active) fsFree(&g.set);
    }
};

/// Build the gate from a caller array.  `null` syms -> inactive gate.  Returns
/// null on OOM (the caller reports the same error a scan failure would).
fn gateBuild(syms: ?[*]const u32, n_syms: usize) ?VecGate {
    var g = VecGate{};
    if (syms == null) return g;
    g.active = true;
    if (n_syms == 0) return g;
    const raw = c.malloc(n_syms * @sizeOf(u32)) orelse return null;
    const d: [*]u32 = @ptrCast(@alignCast(raw));
    @memcpy(d[0..n_syms], syms.?[0..n_syms]);
    g.set.d = d;
    g.set.n = n_syms;
    g.set.cap = n_syms;
    fsSortDedup(&g.set);
    return g;
}

// ─── Mode dispatch read primitive ────────────────────────────────────────

const VecReader = struct {
    db: ?*dl.DlDb,
    version: u32,
    use_version: bool,
};

/// Read a prefix: LIVE -> dl_prefix; VERSION -> dl_query_bound_version.
/// A negative return sets *err (dl_prefix: missing relation; version mode:
/// version == 0 / nonexistent / relation absent from that version).
fn vecReadPrefix(rd: *const VecReader, rel: [*c]const u8, leading: ?[*]const u32, k: u8, cb: DlTupleCb, user: ?*anyopaque, err: *bool) c_long {
    const n = if (rd.use_version)
        dl.dl_query_bound_version(rd.db, rd.version, rel, leading, k, cb, user)
    else
        dl.dl_prefix(rd.db, rel, leading, k, cb, user);
    if (n < 0) {
        err.* = true;
        return -1;
    }
    return n;
}

// ─── Ranking (shared by search and rerank) ───────────────────────────────

/// Integer square root (Newton), exact for the small norms here.
fn isqrtU64(n: u64) u32 {
    if (n == 0) return 0;
    var x: u64 = n;
    var y: u64 = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return @truncate(x);
}

const RankCand = struct {
    sym: u32,
    dot: i32, // <q, v>, |dot| <= 384*127^2 = 6.2e6 < 2^24
    norm2: u32, // |v|^2, same bound
    norm: u32, // integer isqrt(norm2)
};

/// a ranks above b iff dot_a/|a| > dot_b/|b|  <=>  dot_a*|b| > dot_b*|a|.
/// All four factors fit int64 (|dot|*norm <= 6.2e6 * 2490 ~ 1.5e10), and the
/// signed comparison is correct for every sign combination.
fn rankCandLt(_: void, a: RankCand, b: RankCand) bool {
    const lhs = @as(i64, a.dot) * @as(i64, b.norm);
    const rhs = @as(i64, b.dot) * @as(i64, a.norm);
    if (lhs != rhs) return lhs > rhs;
    return a.sym < b.sym;
}

/// Unpack the pre-quantized int8 query vector (96 u32, 4 int8 little-endian
/// packed per word) into 384 sign-extended lanes.
fn unpackQuery(q_int8: [*]const u32, q: *[VEC_D]i32) void {
    var i: usize = 0;
    while (i < VEC_D) : (i += 1)
        q[i] = @as(i8, @bitCast(@as(u8, @truncate(q_int8[i / 4] >> @truncate(8 * (i % 4))))));
}

/// Per-entity integer accumulators (i32/u32 are exact: both sums are bounded
/// by VEC_D*127^2 = 6.2e6 over the whole vector, whatever the chunk order).
const VecAccum = struct { dot: i32 = 0, norm2: u32 = 0 };

/// Scan context: the unpacked query and the sym-keyed accumulator map the row
/// callback fills.
///
/// `cur_*` is a one-entry cache, NOT an ordering assumption: 96 rows per entity
/// means a map probe per row would cost more than the 4 MACs it wraps (~60 ns
/// vs ~10 ns, MEASURED), so a repeat of the previous row's sym reuses its
/// accumulator.  Any other row (reordered, interleaved, first) falls back to
/// the map.  The cached pointer stays valid because the map is only mutated on
/// a cache MISS, and a miss re-reads the pointer.
const ScanCtx = struct {
    q: *const [VEC_D]i32,
    m: *std.AutoHashMap(u32, VecAccum),
    cur_sym: u32 = 0,
    cur_hit: bool = false,
    cur: ?*VecAccum = null,
    error_occurred: bool = false,
};

/// One __vec_q__ row: (entity_sym, chunk_idx, packed_4x_int8).  Accumulates
/// the chunk's contribution.  Liveness is NOT tested here: a bsearch per row
/// would cost more than the 4 MACs it guards (96 rows per entity).  Dead-sym
/// rows (append-only leftovers) are dropped after the scan, when the map's
/// distinct syms are reconciled with the live set — one bsearch per entity.
fn scanChunkCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const s: *ScanCtx = @ptrCast(@alignCast(user.?));
    const sym = cols.?[0];
    const chunk = cols.?[1];
    if (chunk >= VEC_IVEC_WORDS) return 0; // malformed row: ignore
    const packed_bits = cols.?[2];

    var dot: i32 = 0;
    var norm2: u32 = 0;
    var lane: usize = 0;
    while (lane < 4) : (lane += 1) {
        const b: i32 = @as(i8, @bitCast(@as(u8, @truncate(packed_bits >> @truncate(8 * lane)))));
        dot += b * s.q[chunk * 4 + lane];
        norm2 += @intCast(b * b);
    }

    if (!s.cur_hit or s.cur_sym != sym) {
        const gop = s.m.getOrPut(sym) catch {
            s.error_occurred = true;
            return 1;
        };
        if (!gop.found_existing) gop.value_ptr.* = .{};
        s.cur = gop.value_ptr;
        s.cur_sym = sym;
        s.cur_hit = true;
    }
    s.cur.?.dot += dot;
    s.cur.?.norm2 += norm2;
    return 0;
}

const noopCb = struct {
    fn cb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
        _ = cols;
        _ = arity;
        _ = user;
        return 0;
    }
}.cb;

// ─── Rev-gated columnar arena (search item 5) ─────────────────────────────
//
// A repeated-query loop re-pays the DAFSA row materialisation on EVERY query
// (MEASURED at 10k vectors / 960k rows: ~97 ms LIVE, of which the DAFSA walk
// is 70-90% and the int8 arithmetic is free).  The stored vectors do not change
// between queries, so the relation-VISIBLE payload is materialised once into
// one contiguous columnar arena and reused while the relation's content
// revision (`Relation.rev`, relation.zig rel_rev) is unchanged.
//
// INVALIDATION IS THE CONTENT REVISION, not the query.  `rev` is bumped by
// every mutation that changes the view — add / delete / overlay consolidation
// / view reset / rebuild (relation.zig) — so a snapshot tagged with the rev it
// was built at is used ONLY while that rev still holds:
//   - an ADD after a query bumps rev -> the next query rebuilds, and sees it;
//   - a DELETE bumps rev -> the next query rebuilds from the SHRUNKEN relation
//     (it never reads past a stale end);
//   - a publish/consolidate that folds the hot-write overlay bumps rev ->
//     rebuild (no stale read across a consolidation).
// A build that observed the rev move WHILE it enumerated (rev0 != rev1) is
// DISCARDED, never published: a torn snapshot must not be cached.  The query
// that hit that case still answers from its own scan, so it is never wrong.
//
// LIVE MODE ONLY.  VERSION reads keep going through dl_query_bound_version and
// never touch the arena (the LIVE-vs-VERSION discipline predates this cache);
// the version branch does not even read `Relation.cache`.
//
// The arena FILTERS NOTHING.  Liveness and the caller's gate are still applied
// per entity here, exactly as in the scan path: the arena supplies the vectors,
// the gate/liveness test filters the candidates.

/// One materialised vector: sym + the two query-INDEPENDENT norms (|v|^2 and
/// its integer isqrt) + the 384 int8 lanes in chunk-major order.  Columnar so
/// the rank loop streams the lanes linearly.
const VecArena = struct {
    // The Relation.rev this snapshot was built at.  0 is NOT a sentinel: a
    // never-mutated relation legitimately sits at rev 0.  "Not built" is the
    // null cache slot on the Relation, and an installed snapshot is trusted
    // only while its rev still equals the relation's.
    rev: u64 = 0,
    n: usize = 0,
    cap: usize = 0,
    sym: ?[*]u32 = null,
    norm2: ?[*]u32 = null,
    norm: ?[*]u32 = null,
    vec: ?[*]i8 = null,

    fn deinit(self: *VecArena) void {
        c.free(@ptrCast(self.sym));
        c.free(@ptrCast(self.norm2));
        c.free(@ptrCast(self.norm));
        c.free(@ptrCast(self.vec));
        c.free(@ptrCast(self));
    }
};

/// Release a `Relation.cache` slot (relation.zig calls it with whatever is
/// there — null included — at teardown).
fn vecArenaFree(p: ?*anyopaque) void {
    if (p) |q| {
        const a: *VecArena = @ptrCast(@alignCast(q));
        a.deinit();
    }
}

/// Build counter — the invalidation-proof instrument: a test asserts that a
/// mutation REALLY rebuilt the snapshot, not merely that the results look right.
var arena_builds: u64 = 0;

/// Grow the columnar arrays to hold at least `need` entries.  Each
/// c.realloc keeps the old block valid on failure, so a failed growth leaves
/// the arena consistent (the caller aborts the build and frees it).
fn arenaGrow(a: *VecArena, need: usize) bool {
    if (need <= a.cap) return true;
    var nc: usize = if (a.cap != 0) a.cap else 64;
    while (nc < need) nc *= 2;
    const ns = c.realloc(@ptrCast(a.sym), nc * @sizeOf(u32)) orelse return false;
    a.sym = @ptrCast(@alignCast(ns));
    const n2 = c.realloc(@ptrCast(a.norm2), nc * @sizeOf(u32)) orelse return false;
    a.norm2 = @ptrCast(@alignCast(n2));
    const nr = c.realloc(@ptrCast(a.norm), nc * @sizeOf(u32)) orelse return false;
    a.norm = @ptrCast(@alignCast(nr));
    const nv = c.realloc(@ptrCast(a.vec), nc * VEC_D) orelse return false;
    a.vec = @ptrCast(@alignCast(nv));
    a.cap = nc;
    return true;
}

/// Arena build state.  `slot` maps sym -> arena index for the build only (it is
/// freed when the build ends), and `cur_*` is the same one-entry cache the scan
/// uses — a repeat of the previous row's sym skips the map probe.  It caches
/// the SLOT INDEX, never a pointer into the arena, because growing the arena
/// reallocs its buffers.
const ArenaFill = struct {
    a: *VecArena,
    slot: *std.AutoHashMap(u32, u32),
    cur_sym: u32 = 0,
    cur_slot: u32 = 0,
    cur_hit: bool = false,
    error_occurred: bool = false,
};

fn arenaFillCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const f: *ArenaFill = @ptrCast(@alignCast(user.?));
    const sym = cols.?[0];
    const chunk = cols.?[1];
    // Malformed row: ignored BEFORE the map touch, exactly as scanChunkCb
    // does — a sym whose only rows are malformed must not become a candidate.
    if (chunk >= VEC_IVEC_WORDS) return 0;
    const packed_bits = cols.?[2];
    const a = f.a;

    if (!f.cur_hit or f.cur_sym != sym) {
        const gop = f.slot.getOrPut(sym) catch {
            f.error_occurred = true;
            return 1;
        };
        if (!gop.found_existing) {
            if (!arenaGrow(a, a.n + 1)) {
                f.error_occurred = true;
                return 1;
            }
            const slot: u32 = @intCast(a.n);
            a.n += 1;
            a.sym.?[slot] = sym;
            a.norm2.?[slot] = 0;
            // Absent chunks are zeros in the scan path; make that explicit.
            @memset(a.vec.?[@as(usize, slot) * VEC_D ..][0..VEC_D], 0);
            gop.value_ptr.* = slot;
        }
        f.cur_slot = gop.value_ptr.*;
        f.cur_sym = sym;
        f.cur_hit = true;
    }

    const base = a.vec.? + @as(usize, f.cur_slot) * VEC_D + @as(usize, chunk) * 4;
    var norm2: u32 = 0;
    var lane: usize = 0;
    while (lane < 4) : (lane += 1) {
        const b: i8 = @bitCast(@as(u8, @truncate(packed_bits >> @truncate(8 * lane))));
        base[lane] = b;
        norm2 += @intCast(@as(i32, b) * @as(i32, b));
    }
    a.norm2.?[f.cur_slot] += norm2;
    return 0;
}

/// Trim the columnar arrays to exactly `n` entries.  The doubling growth leaves
/// up to 2x slack (at 10k vectors: cap 16384 = 6.3 MB of lanes for 3.84 MB of
/// payload) and the arena is immutable after the build, so the exact size is
/// final.  c.realloc (not malloc+copy) so a large shrink usually happens in
/// place and does not fault in fresh pages.  `cap` is lowered FIRST and a failed
/// realloc leaves the original (larger) block valid, so `cap` never overstates
/// what is actually allocated.
fn arenaFit(a: *VecArena) void {
    const n = a.n;
    if (n == 0 or a.cap == n) return;
    a.cap = n;
    if (c.realloc(@ptrCast(a.vec), n * VEC_D)) |p| a.vec = @ptrCast(@alignCast(p));
    if (c.realloc(@ptrCast(a.sym), n * @sizeOf(u32))) |p| a.sym = @ptrCast(@alignCast(p));
    if (c.realloc(@ptrCast(a.norm2), n * @sizeOf(u32))) |p| a.norm2 = @ptrCast(@alignCast(p));
    if (c.realloc(@ptrCast(a.norm), n * @sizeOf(u32))) |p| a.norm = @ptrCast(@alignCast(p));
}

/// Materialise `vrel`'s visible rows into a fresh arena, or null (OOM / torn
/// rev — both make the caller fall back to the plain scan, never wrong).
fn arenaBuild(vrel: *relation.Relation) ?*VecArena {
    const mem = c.malloc(@sizeOf(VecArena)) orelse return null;
    const a: *VecArena = @ptrCast(@alignCast(mem));
    a.* = .{};

    // A read of this relation consolidates the hot-write overlay (rel_prefix
    // does it at its top anyway).  Doing it BEFORE the rev read means the rev
    // describes exactly the content the enumeration below will see.
    _ = relation.rel_overlay_flush(vrel);
    const rev0 = relation.rel_rev(vrel);

    var slot = std.AutoHashMap(u32, u32).init(std.heap.c_allocator);
    defer slot.deinit();
    var f = ArenaFill{ .a = a, .slot = &slot };

    if (relation.rel_prefix(vrel, null, 0, arenaFillCb, &f) < 0 or f.error_occurred) {
        a.deinit();
        return null;
    }

    var i: usize = 0;
    while (i < a.n) : (i += 1) a.norm.?[i] = isqrtU64(a.norm2.?[i]);
    arenaFit(a);

    if (relation.rel_rev(vrel) != rev0) { // mutated mid-build: do not publish
        a.deinit();
        return null;
    }
    a.rev = rev0;
    arena_builds += 1;
    return a;
}

/// Install `a` as `vrel`'s snapshot, dropping any previous one.
fn arenaInstall(vrel: *relation.Relation, a: *VecArena) void {
    if (vrel.cache) |old| vecArenaFree(old);
    vrel.cache = a;
    vrel.cache_free = &vecArenaFree;
}

/// Rank the arena against `q`, applying the SAME liveness + gate tests the scan
/// path applies per entity.
fn arenaRank(a: *const VecArena, q: *const [VEC_D]i32, live: *const VecFilterSet, gate: *const VecGate) ?CandSet {
    if (a.n == 0) return CandSet{};
    const raw = c.calloc(a.n, @sizeOf(RankCand)) orelse return null;
    const rc: [*]RankCand = @ptrCast(@alignCast(raw));
    var cs = CandSet{ .rc = rc, .n = 0 };
    const syms = a.sym.?;
    const vec = a.vec.?;
    var e: usize = 0;
    while (e < a.n) : (e += 1) {
        const sym = syms[e];
        if (!fsContains(live, sym)) continue;
        if (gate.active and !fsContains(&gate.set, sym)) continue;
        const base = vec + e * VEC_D;
        var dot: i32 = 0;
        var d: usize = 0;
        while (d < VEC_D) : (d += 1) dot += @as(i32, base[d]) * q[d];
        rc[cs.n] = .{
            .sym = sym,
            .dot = dot,
            .norm2 = a.norm2.?[e],
            .norm = a.norm.?[e],
        };
        cs.n += 1;
    }
    return cs;
}

/// Candidates from the rev-gated arena, building it on a miss.  Null means the
/// arena path is NOT usable here (no such relation / wrong arity / OOM / a rev
/// that moved mid-build) — the caller then takes the one-pass scan, which
/// answers identically.
fn arenaCands(vrel: *relation.Relation, q: *const [VEC_D]i32, live: *const VecFilterSet, gate: *const VecGate) ?CandSet {
    const rev = relation.rel_rev(vrel);
    if (vrel.cache) |p| {
        const a: *VecArena = @ptrCast(@alignCast(p));
        if (a.rev == rev) return arenaRank(a, q, live, gate);
    }
    const a = arenaBuild(vrel) orelse return null;
    const cs = arenaRank(a, q, live, gate) orelse {
        a.deinit();
        return null;
    };
    arenaInstall(vrel, a);
    return cs;
}

// ─── Candidate assembly ───────────────────────────────────────────────────

/// An owned RankCand array.  `n == 0` is a valid empty result (no candidates);
/// `rc == null` only ever accompanies n == 0.
const CandSet = struct {
    rc: ?[*]RankCand = null,
    n: usize = 0,

    fn deinit(self: *const CandSet) void {
        c.free(@ptrCast(self.rc));
    }
};

/// The one-pass scan path: score every stored vector in ONE enumeration, then
/// reconcile the sym-keyed accumulators against liveness + the gate.  Used by
/// VERSION mode always, and by LIVE mode when the arena is unusable.
fn scanCands(rd: *const VecReader, corpus: *const DlVecCorpus, q: *const [VEC_D]i32, live: *const VecFilterSet, gate: *const VecGate, err: *bool) ?CandSet {
    var acc = std.AutoHashMap(u32, VecAccum).init(std.heap.c_allocator);
    defer acc.deinit();
    var sc = ScanCtx{ .q = q, .m = &acc };
    if (vecReadPrefix(rd, corpus.vec_rel, null, 0, scanChunkCb, &sc, err) < 0 or sc.error_occurred)
        return null;

    // rank: one RankCand per LIVE entity that had at least one chunk (the map
    // may also hold dead syms — append-only vector rows — so it is an upper
    // bound on the candidate count).
    const n_scanned = acc.count();
    if (n_scanned == 0) return CandSet{};
    const rc_raw = c.calloc(n_scanned, @sizeOf(RankCand)) orelse return null;
    const rc: [*]RankCand = @ptrCast(@alignCast(rc_raw));
    var cs = CandSet{ .rc = rc, .n = 0 };

    var it = acc.iterator();
    while (it.next()) |e| {
        if (!fsContains(live, e.key_ptr.*)) continue;
        // Second membership test (only when the caller gated the search): the
        // rank set is the INTERSECTION, so truncating the scan is unnecessary.
        if (gate.active and !fsContains(&gate.set, e.key_ptr.*)) continue;
        const norm2 = e.value_ptr.norm2;
        rc[cs.n] = .{
            .sym = e.key_ptr.*,
            .dot = e.value_ptr.dot,
            .norm2 = norm2,
            .norm = isqrtU64(norm2),
        };
        cs.n += 1;
    }
    return cs;
}

// ─── Shared search body (LIVE / VERSION differ only in the read primitive) ──

fn vectorSearchImpl(rd: *const VecReader, corpus: *const DlVecCorpus, q_in: ?[*]const u32, gate_syms: ?[*]const u32, n_gate: usize, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    var err = false;

    if (rd.db == null or q_in == null or k <= 0 or cb == null) return -1;

    var q: [VEC_D]i32 = undefined;
    unpackQuery(q_in.?, &q);

    // The gate is built BEFORE the scan so an OOM is reported like any other
    // search failure rather than as a silently empty result.
    var gate = gateBuild(gate_syms, n_gate) orelse return -1;
    defer gate.deinit();

    // Relation-existence probe: a version (or a live store) WITHOUT the vec
    // relation is an error, not an empty result.  leading=[0] touches no row
    // if the relation exists (even empty) — the same trick the liveness probe
    // used, now on the only relation the query path reads.
    {
        const probe = [1]u32{0};
        if (vecReadPrefix(rd, corpus.vec_rel, &probe, 1, noopCb, null, &err) < 0) return -1;
    }

    // liveness: one scan of the corpus filter relation, sort+dedup -> bsearch.
    // A missing liveness relation is an error; an empty one yields 0 results.
    var live = VecFilterSet{ .col = corpus.filter_col };
    defer fsFree(&live);
    if (vecReadPrefix(rd, corpus.filter_rel, null, 0, collectFilterCb, &live, &err) < 0 or live.error_occurred)
        return -1;
    fsSortDedup(&live);

    // candidates: the rev-gated arena in LIVE mode (a repeated-query loop
    // re-reads the same stored vectors), else the one-pass scan.  The arena is
    // consulted ONLY on the live branch — a VERSION read never sees it.
    var cands: CandSet = blk: {
        if (!rd.use_version) {
            if (dl.relFixedByName(rd.db, corpus.vec_rel)) |vrel| {
                // Only the arity-3 __vec_q__-shaped row layout is materialised;
                // anything else falls through to the scan unchanged.
                if (relation.rel_arity(vrel) == 3) {
                    if (arenaCands(vrel, &q, &live, &gate)) |cs| break :blk cs;
                }
            }
        }
        break :blk scanCands(rd, corpus, &q, &live, &gate, &err) orelse return -1;
    };
    defer cands.deinit();
    if (cands.n == 0) return 0;
    const rc = cands.rc.?;

    std.mem.sort(RankCand, rc[0..cands.n], {}, rankCandLt);

    // emit at most k, highest cosine first (counted before the cb so an early
    // stop still counts the row it saw).
    var emitted: c_long = 0;
    var i: usize = 0;
    while (i < cands.n and emitted < k) : (i += 1) {
        emitted += 1;
        if (cb.?(rc[i].sym, @intCast(rc[i].dot), user) != 0) break;
    }
    return emitted;
}

pub export fn dl_vector_search_corpus(db: ?*dl.DlDb, corpus: ?*const DlVecCorpus, q_int8: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    const rd = VecReader{ .db = db, .version = 0, .use_version = false };
    return vectorSearchImpl(&rd, corpus orelse return -1, q_int8, null, 0, k, cb, user);
}

pub export fn dl_vector_search_corpus_version(db: ?*dl.DlDb, version: u32, corpus: ?*const DlVecCorpus, q_int8: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    if (db == null or version == 0) return -1;
    const rd = VecReader{ .db = db, .version = version, .use_version = true };
    return vectorSearchImpl(&rd, corpus orelse return -1, q_int8, null, 0, k, cb, user);
}

pub export fn dl_vector_search(db: ?*dl.DlDb, q_int8: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_search_corpus(db, &DL_VEC_CORPUS_ENTITY, q_int8, k, cb, user);
}

pub export fn dl_vector_search_version(db: ?*dl.DlDb, version: u32, q_int8: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_search_corpus_version(db, version, &DL_VEC_CORPUS_ENTITY, q_int8, k, cb, user);
}

// ─── Gated search (the hybrid path: lexical gate -> semantic rank) ────────
//
// Same ranking path as dl_vector_search, plus ONE extra membership test per
// entity: a candidate must be in the caller's sym gate as well as live.  The
// emitted set is therefore the top k OF THE INTERSECTION, exact at any k —
// this is what removes vhybrid's k*multiplier truncation artifact (an entity
// in both tiers could rank below the truncated scan and be missed).
//
// gate_syms is a SET, in any order (the engine copies, sorts and dedups it).
// gate_syms == NULL is "no gate" and behaves exactly like dl_vector_search;
// gate_syms != NULL with n_gate == 0 is the EMPTY intersection -> 0 results.

pub export fn dl_vector_search_gated_corpus(db: ?*dl.DlDb, corpus: ?*const DlVecCorpus, q_int8: ?[*]const u32, gate_syms: ?[*]const u32, n_gate: usize, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    const rd = VecReader{ .db = db, .version = 0, .use_version = false };
    return vectorSearchImpl(&rd, corpus orelse return -1, q_int8, gate_syms, n_gate, k, cb, user);
}

pub export fn dl_vector_search_gated_corpus_version(db: ?*dl.DlDb, version: u32, corpus: ?*const DlVecCorpus, q_int8: ?[*]const u32, gate_syms: ?[*]const u32, n_gate: usize, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    if (db == null or version == 0) return -1;
    const rd = VecReader{ .db = db, .version = version, .use_version = true };
    return vectorSearchImpl(&rd, corpus orelse return -1, q_int8, gate_syms, n_gate, k, cb, user);
}

pub export fn dl_vector_search_gated(db: ?*dl.DlDb, q_int8: ?[*]const u32, gate_syms: ?[*]const u32, n_gate: usize, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_search_gated_corpus(db, &DL_VEC_CORPUS_ENTITY, q_int8, gate_syms, n_gate, k, cb, user);
}

pub export fn dl_vector_search_gated_version(db: ?*dl.DlDb, version: u32, q_int8: ?[*]const u32, gate_syms: ?[*]const u32, n_gate: usize, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_search_gated_corpus_version(db, version, &DL_VEC_CORPUS_ENTITY, q_int8, gate_syms, n_gate, k, cb, user);
}

// ─── Rank an explicit candidate set (LIVE only) ──────────────────────────

/// Load the stored int8 vector (96 chunks) for one entity into a buffer.
const VecLoad = struct {
    v: [VEC_D]i8,
    present: bool,
};

fn loadVecCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const l: *VecLoad = @ptrCast(@alignCast(user.?));
    const chunk = cols.?[1];
    const packed_bits = cols.?[2];
    if (chunk >= VEC_IVEC_WORDS) return 0; // malformed, ignore
    l.present = true;
    l.v[chunk * 4 + 0] = @bitCast(@as(u8, @truncate(packed_bits & 0xFF)));
    l.v[chunk * 4 + 1] = @bitCast(@as(u8, @truncate((packed_bits >> 8) & 0xFF)));
    l.v[chunk * 4 + 2] = @bitCast(@as(u8, @truncate((packed_bits >> 16) & 0xFF)));
    l.v[chunk * 4 + 3] = @bitCast(@as(u8, @truncate((packed_bits >> 24) & 0xFF)));
    return 0;
}

pub export fn dl_vector_rerank_corpus(db: ?*dl.DlDb, corpus: ?*const DlVecCorpus, q_int8: ?[*]const u32, cand_syms: ?[*]const u32, n_cand: c_int, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    var q: [VEC_D]i32 = undefined;
    var err = false;

    if (db == null or corpus == null or q_int8 == null or cand_syms == null or n_cand <= 0 or k <= 0 or cb == null)
        return -1;

    unpackQuery(q_int8.?, &q);

    const rc_raw = c.calloc(@intCast(n_cand), @sizeOf(RankCand)) orelse return -1;
    const rc: [*]RankCand = @ptrCast(@alignCast(rc_raw));
    defer c.free(rc_raw);

    var n_valid: usize = 0;
    var i: usize = 0;
    while (i < @as(usize, @intCast(n_cand)) and !err) : (i += 1) {
        const sym = cand_syms.?[i];
        const leading = [1]u32{sym};
        var l = std.mem.zeroes(VecLoad);
        var dot: i64 = 0;
        var norm2: i64 = 0;
        var d: usize = 0;

        const n = dl.dl_prefix(db, corpus.?.vec_rel, &leading, 1, loadVecCb, &l);
        if (n < 0) {
            err = true;
            break;
        }
        if (!l.present) continue; // no vector on record: skip candidate

        while (d < VEC_D) : (d += 1) {
            const v: i32 = l.v[d];
            dot += @as(i64, v) * q[d];
            norm2 += @as(i64, v) * @as(i64, v);
        }
        rc[n_valid] = .{
            .sym = sym,
            .dot = @truncate(dot),
            .norm2 = @truncate(@as(u64, @bitCast(norm2))),
            .norm = isqrtU64(@bitCast(norm2)),
        };
        n_valid += 1;
    }

    if (err) return -1;

    std.mem.sort(RankCand, rc[0..n_valid], {}, rankCandLt);

    var emitted: c_long = 0;
    i = 0;
    while (i < n_valid and emitted < k) : (i += 1) {
        emitted += 1;
        if (cb.?(rc[i].sym, @intCast(rc[i].dot), user) != 0) break;
    }
    return emitted;
}

pub export fn dl_vector_rerank(db: ?*dl.DlDb, q_int8: ?[*]const u32, cand_syms: ?[*]const u32, n_cand: c_int, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_rerank_corpus(db, &DL_VEC_CORPUS_ENTITY, q_int8, cand_syms, n_cand, k, cb, user);
}

// ─── Tests ────────────────────────────────────────────────────────────────

const testing = std.testing;

fn collectSymCb(sym: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int {
    const out: *std.ArrayList(u64) = @ptrCast(@alignCast(user.?));
    const packed_hit = (@as(u64, sym) << 32) | @as(u64, @as(u32, @bitCast(score)));
    out.append(testing.allocator, packed_hit) catch return 1;
    return 0;
}

/// Pack 4 int8 values into a u32 (little-endian, the encoder's convention).
fn pack4(a: i8, b: i8, c0: i8, d: i8) u32 {
    return @as(u32, @as(u8, @bitCast(a))) |
        (@as(u32, @as(u8, @bitCast(b))) << 8) |
        (@as(u32, @as(u8, @bitCast(c0))) << 16) |
        (@as(u32, @as(u8, @bitCast(d))) << 24);
}

/// rm -rf `path` (bounded: test paths only).  dl_open(NULL) is an error in
/// the oracle too, so tests use real dirs (fresh_db pattern).
extern "c" fn system(cmd: [*:0]const u8) c_int;

fn testRmRf(path: [*:0]const u8) void {
    var buf: [256]u8 = undefined;
    const cmd = std.fmt.bufPrintZ(&buf, "rm -rf {s}", .{std.mem.span(path)}) catch return;
    _ = system(cmd.ptr);
}

fn testOpenDb(path: [*:0]const u8) *dl.DlDb {
    testRmRf(path);
    return dl.dl_open(path) orelse @panic("dl_open failed");
}

fn testCloseDb(db: *dl.DlDb, path: [*:0]const u8) void {
    dl.dl_close(db);
    testRmRf(path);
}

test "dl_vector_search: scan-and-rank order, live filter, k cap" {
    const path = "/tmp/datalog_zig_u14_vec_search";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "entity", 2));
    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "__vec_q__", 3));

    // Live entities 10, 11; entity 99 has a stored vector but is NOT in the
    // liveness relation -> it must not be scanned into the ranking.
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 10, 1 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 11, 1 }, 2));

    // v(10) = [+8, 0, 0, ...]; v(11) = [-8, 0, 0, ...]; v(99) = [+127, ...]
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 10, 0, pack4(8, 0, 0, 0) }, 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 11, 0, pack4(-8, 0, 0, 0) }, 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 99, 0, pack4(127, 0, 0, 0) }, 3));

    // q = [+1, 0, 0, ...]: cosine(q,10) = +1, cosine(q,11) = -1.
    var q_int8: [VEC_IVEC_WORDS]u32 = [_]u32{0} ** VEC_IVEC_WORDS;
    q_int8[0] = pack4(1, 0, 0, 0);

    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search(db, &q_int8, 10, collectSymCb, &got));
    try testing.expectEqual(@as(u64, 10), got.items[0] >> 32);
    try testing.expectEqual(@as(u32, 8), @as(u32, @truncate(got.items[0]))); // score = dot
    try testing.expectEqual(@as(u64, 11), got.items[1] >> 32);
    try testing.expectEqual(@as(u32, 0xfffffff8), @as(u32, @truncate(got.items[1]))); // dot = -8

    // k caps emission (best cosine first).
    var got_k: std.ArrayList(u64) = .empty;
    defer got_k.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 1), dl_vector_search(db, &q_int8, 1, collectSymCb, &got_k));
    try testing.expectEqual(@as(u64, 10), got_k.items[0] >> 32);

    // Error contract: k <= 0, NULL db, NULL cb, version 0.
    var sink: std.ArrayList(u64) = .empty;
    defer sink.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, -1), dl_vector_search(db, &q_int8, 0, collectSymCb, &sink));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search(null, &q_int8, 10, collectSymCb, &sink));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search(db, &q_int8, 10, null, &sink));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search_version(db, 0, &q_int8, 10, collectSymCb, &sink));
}

test "dl_vector_search: ranking is cosine, not raw dot" {
    const path = "/tmp/datalog_zig_u14_vec_cosine";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "entity", 2));
    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "__vec_q__", 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 1, 1 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 2, 1 }, 2));

    // Both vectors are parallel to q = [+1, 0, ...] -> cosine +1 for both, so
    // the RANKING must fall through to the sym tiebreak, not to |dot|:
    //   v(1) = [+1, 0, ...]  (dot 1,   |v| 1)
    //   v(2) = [+8, 0, ...]  (dot 8,   |v| 8)   <- larger dot, same cosine
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 1, 0, pack4(1, 0, 0, 0) }, 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 2, 0, pack4(8, 0, 0, 0) }, 3));

    var q_int8: [VEC_IVEC_WORDS]u32 = [_]u32{0} ** VEC_IVEC_WORDS;
    q_int8[0] = pack4(1, 0, 0, 0);

    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search(db, &q_int8, 10, collectSymCb, &got));
    try testing.expectEqual(@as(u64, 1), got.items[0] >> 32); // tie -> sym asc
    try testing.expectEqual(@as(u64, 2), got.items[1] >> 32);
}

test "dl_vector_search_gated: the rank set is the gate ∩ live" {
    const path = "/tmp/datalog_zig_u14_vec_gated";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "entity", 2));
    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "__vec_q__", 3));

    // Four live entities 10..13 with v_i = [100, i-10, 0, ...]: cosine vs
    // q = [+127, 0, ...] is 12700/(127*sqrt(100^2 + (i-10)^2)), strictly
    // DECREASING in i — so the ranking is 10, 11, 12, 13 by construction.
    var i: u32 = 10;
    while (i < 14) : (i += 1) {
        try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ i, 1 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ i, 0, pack4(100, @intCast(i - 10), 0, 0) }, 3));
    }
    // 99 has a vector but is not live; 13 is live but rank 4.
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 99, 0, pack4(127, 0, 0, 0) }, 3));

    var q_int8: [VEC_IVEC_WORDS]u32 = [_]u32{0} ** VEC_IVEC_WORDS;
    q_int8[0] = pack4(127, 0, 0, 0);

    // Unsorted, with a dead sym (99) and a duplicate (13 twice) in the gate.
    const gate = [4]u32{ 13, 99, 11, 13 };

    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search_gated(db, &q_int8, &gate, gate.len, 10, collectSymCb, &got));
    try testing.expectEqual(@as(u64, 11), got.items[0] >> 32); // higher cosine first
    try testing.expectEqual(@as(u64, 13), got.items[1] >> 32);
    try testing.expectEqual(@as(usize, 2), got.items.len); // duplicates deduped

    // k caps the emission, still best-first.
    var got_k: std.ArrayList(u64) = .empty;
    defer got_k.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 1), dl_vector_search_gated(db, &q_int8, &gate, gate.len, 1, collectSymCb, &got_k));
    try testing.expectEqual(@as(u64, 11), got_k.items[0] >> 32);

    // An EMPTY gate is the empty intersection (0 results) — distinct from no
    // gate, which must reproduce dl_vector_search exactly (4 live entities).
    var got_empty: std.ArrayList(u64) = .empty;
    defer got_empty.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 0), dl_vector_search_gated(db, &q_int8, &gate, 0, 10, collectSymCb, &got_empty));
    var un: std.ArrayList(u64) = .empty;
    defer un.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 4), dl_vector_search_gated(db, &q_int8, null, 0, 10, collectSymCb, &un));
    try testing.expectEqual(@as(u64, 10), un.items[0] >> 32);

    // Error contract (the dl_vector_search one, plus version 0).
    var sink: std.ArrayList(u64) = .empty;
    defer sink.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, -1), dl_vector_search_gated(db, &q_int8, &gate, gate.len, 0, collectSymCb, &sink));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search_gated(null, &q_int8, &gate, gate.len, 10, collectSymCb, &sink));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search_gated(db, null, &gate, gate.len, 10, collectSymCb, &sink));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search_gated(db, &q_int8, &gate, gate.len, 10, null, &sink));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search_gated_version(db, 0, &q_int8, &gate, gate.len, 10, collectSymCb, &sink));
}

test "dl_vector_rerank: int8 cosine order via integer cross-multiply" {
    const path = "/tmp/datalog_zig_u14_vec_rerank";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "__vec_q__", 3));

    // Two int8 vectors, 4 dims (chunk 0 only; the rest absent = zero).
    //   v(10) = [+8, 0, 0, 0]   -> packed chunk0 = 0x00000008
    //   v(11) = [-8, 0, 0, 0]   -> packed chunk0 = 0x000000f8 (LE int8s)
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 10, 0, 0x00000008 }, 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 11, 0, 0x000000f8 }, 3));

    // Query q = [+1, 0, 0, 0]: dot(q,v10)=8, dot(q,v11)=-8 -> 10 first.
    var q_int8: [VEC_IVEC_WORDS]u32 = [_]u32{0} ** VEC_IVEC_WORDS;
    q_int8[0] = 0x00000001;

    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    const cands = [2]u32{ 11, 10 }; // deliberately reversed input
    const emitted = dl_vector_rerank(db, &q_int8, &cands, 2, 10, collectSymCb, &got);
    try testing.expectEqual(@as(c_long, 2), emitted);
    try testing.expectEqual(@as(u64, 10), got.items[0] >> 32);
    try testing.expectEqual(@as(u32, 8), @as(u32, @truncate(got.items[0]))); // score = dot
    try testing.expectEqual(@as(u64, 11), got.items[1] >> 32);
    try testing.expectEqual(@as(u32, 0xfffffff8), @as(u32, @truncate(got.items[1]))); // dot = -8

    // k caps emission.
    var got_k: std.ArrayList(u64) = .empty;
    defer got_k.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 1), dl_vector_rerank(db, &q_int8, &cands, 2, 1, collectSymCb, &got_k));
    try testing.expectEqual(@as(u64, 10), got_k.items[0] >> 32);

    // Missing vector -> candidate skipped.
    const cands3 = [3]u32{ 10, 42, 11 };
    var got3: std.ArrayList(u64) = .empty;
    defer got3.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_rerank(db, &q_int8, &cands3, 3, 10, collectSymCb, &got3));

    // Error contract.
    try testing.expectEqual(@as(c_long, -1), dl_vector_rerank(db, &q_int8, &cands, 0, 10, collectSymCb, &got));
    try testing.expectEqual(@as(c_long, -1), dl_vector_rerank(db, &q_int8, &cands, 2, 0, collectSymCb, &got));
    try testing.expectEqual(@as(c_long, -1), dl_vector_rerank(null, &q_int8, &cands, 2, 10, collectSymCb, &got));
}

// ─── Arena invalidation tests (the cache is only worth having if a STALE one
// can never answer: each test below FAILS if the invalidation is removed) ───

/// The installed arena for `rel` (test-only reach-in: the arena is a private
/// detail of this file, so its proof lives here rather than at the ABI).
fn testArenaFor(db: ?*dl.DlDb, rel: [*:0]const u8) ?*VecArena {
    const vrel = dl.relFixedByName(db, rel) orelse return null;
    const p = vrel.cache orelse return null;
    return @ptrCast(@alignCast(p));
}

test "arena: an ADD after materialisation is visible to the next query" {
    const path = "/tmp/datalog_zig_u19_vec_arena_add";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "entity", 2));
    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "__vec_q__", 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 10, 1 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 11, 1 }, 2));
    // Off-axis vectors: cosines are DISTINCT (10 > 11), so the assertions
    // below test the ranking rather than the sym tiebreak.
    // (the integer ranking uses isqrt(NORM), so the off-axis component must be
    // big enough that truncation does not collapse the two cosines into a tie)
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 10, 0, pack4(8, 9, 0, 0) }, 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 11, 0, pack4(4, 9, 0, 0) }, 3));

    var q: [VEC_IVEC_WORDS]u32 = [_]u32{0} ** VEC_IVEC_WORDS;
    q[0] = pack4(1, 0, 0, 0);

    const builds0 = arena_builds;

    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search(db, &q, 10, collectSymCb, &got));
    try testing.expectEqual(@as(u64, 10), got.items[0] >> 32);

    // MATERIALISED, and the snapshot is the relation's: 2 vectors, tagged with
    // the relation's current rev.
    try testing.expectEqual(builds0 + 1, arena_builds);
    const a0 = testArenaFor(db, "__vec_q__") orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(usize, 2), a0.n);
    const rev0 = a0.rev;
    try testing.expectEqual(relation.rel_rev(dl.relFixedByName(db, "__vec_q__")), rev0);

    // A second query with the relation untouched REUSES it (no rebuild).
    var got2: std.ArrayList(u64) = .empty;
    defer got2.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search(db, &q, 10, collectSymCb, &got2));
    try testing.expectEqual(builds0 + 1, arena_builds);

    // MUTATE: add a third vector AFTER the arena exists.  Without rev-gating
    // the next query answers from the stale snapshot and misses sym 12 (the
    // whole point of the test).
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 12, 1 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 12, 0, pack4(127, 0, 0, 0) }, 3));

    var got3: std.ArrayList(u64) = .empty;
    defer got3.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 3), dl_vector_search(db, &q, 10, collectSymCb, &got3));
    try testing.expectEqual(@as(u64, 12), got3.items[0] >> 32); // cosine 1.0
    try testing.expectEqual(@as(u64, 10), got3.items[1] >> 32);
    try testing.expectEqual(@as(u64, 11), got3.items[2] >> 32);

    // The REBUILD is real, not decorative: a fresh snapshot with one more
    // entry, tagged with the NEW rev.
    try testing.expectEqual(builds0 + 2, arena_builds);
    const a1 = testArenaFor(db, "__vec_q__") orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(usize, 3), a1.n);
    try testing.expect(a1.rev != rev0);
}

test "arena: a DELETE rebuilds from the shrunken relation (never reads past its end)" {
    const path = "/tmp/datalog_zig_u19_vec_arena_del";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "entity", 2));
    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "__vec_q__", 3));

    // Three live entities, one chunk row each (a row is the whole vector here).
    var sym: u32 = 20;
    while (sym < 23) : (sym += 1) {
        try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ sym, 1 }, 2));
        // [10, k] with k = sym-19: cosine 10/sqrt(100+k^2) is strictly
        // DECREASING in sym, so the ranking is 20, 21, 22 (no tiebreak).
        try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ sym, 0, pack4(10, @intCast(sym - 19), 0, 0) }, 3));
    }

    var q: [VEC_IVEC_WORDS]u32 = [_]u32{0} ** VEC_IVEC_WORDS;
    q[0] = pack4(1, 0, 0, 0);

    const builds0 = arena_builds;
    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 3), dl_vector_search(db, &q, 10, collectSymCb, &got));
    try testing.expectEqual(@as(u64, 20), got.items[0] >> 32);
    try testing.expectEqual(@as(u64, 22), got.items[2] >> 32);
    try testing.expectEqual(@as(usize, 3), (testArenaFor(db, "__vec_q__") orelse return error.TestUnexpectedResult).n);

    // Delete the WHOLE vector of 22 (its only row): the relation shrinks.
    try testing.expectEqual(@as(c_int, 1), dl.dl_delete_fact(db, "__vec_q__", &[_]u32{ 22, 0, pack4(10, 3, 0, 0) }, 3));

    var got2: std.ArrayList(u64) = .empty;
    defer got2.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search(db, &q, 10, collectSymCb, &got2));
    try testing.expectEqual(@as(u64, 20), got2.items[0] >> 32);
    try testing.expectEqual(@as(u64, 21), got2.items[1] >> 32);

    const a = testArenaFor(db, "__vec_q__") orelse return error.TestUnexpectedResult;
    try testing.expectEqual(builds0 + 2, arena_builds); // rebuilt after the delete
    try testing.expectEqual(@as(usize, 2), a.n); // and re-derived, not truncated

    // A liveness-only change must NOT rebuild (the vector payload is identical;
    // the filter is applied per query, so the cache survives).
    try testing.expectEqual(@as(c_int, 1), dl.dl_delete_fact(db, "entity", &[_]u32{ 21, 1 }, 2));
    var got3: std.ArrayList(u64) = .empty;
    defer got3.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 1), dl_vector_search(db, &q, 10, collectSymCb, &got3));
    try testing.expectEqual(@as(u64, 20), got3.items[0] >> 32);
    try testing.expectEqual(builds0 + 2, arena_builds); // no rebuild: filters, not cache
}

test "arena: VERSION reads never materialise or consult it" {
    const path = "/tmp/datalog_zig_u19_vec_arena_version";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "entity", 2));
    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "__vec_q__", 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 10, 1 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 11, 1 }, 2));
    // [8,5] > [4,5] > (absent), and the added [127,0] outranks both.
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 10, 0, pack4(8, 5, 0, 0) }, 3));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 11, 0, pack4(4, 5, 0, 0) }, 3));

    try testing.expectEqual(@as(c_int, 0), dl.dl_publish_snapshot(db));
    var vers: [8]u32 = undefined;
    const nv = dl.dl_snapshot_versions(db, &vers, 8);
    try testing.expect(nv >= 1);
    const V = vers[@intCast(nv - 1)];

    var q: [VEC_IVEC_WORDS]u32 = [_]u32{0} ** VEC_IVEC_WORDS;
    q[0] = pack4(1, 0, 0, 0);

    // Version reads FIRST: they must leave the cache slot empty — the arena is
    // a LIVE-mode structure and the version path does not touch it.
    const builds0 = arena_builds;
    var vgot: std.ArrayList(u64) = .empty;
    defer vgot.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search_version(db, V, &q, 10, collectSymCb, &vgot));
    try testing.expectEqual(@as(usize, 2), vgot.items.len);
    try testing.expect(testArenaFor(db, "__vec_q__") == null);
    try testing.expectEqual(builds0, arena_builds);

    // A LIVE query materialises it...
    var lgot: std.ArrayList(u64) = .empty;
    defer lgot.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search(db, &q, 10, collectSymCb, &lgot));
    try testing.expect(testArenaFor(db, "__vec_q__") != null);

    // ...and a later VERSION read of the OLD version still answers from the
    // version, not from the (mutable) live snapshot: mutate live, then the
    // version must neither see the added vector nor rebuild the arena.
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 12, 1 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__vec_q__", &[_]u32{ 12, 0, pack4(127, 0, 0, 0) }, 3));

    var vgot2: std.ArrayList(u64) = .empty;
    defer vgot2.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 2), dl_vector_search_version(db, V, &q, 10, collectSymCb, &vgot2));
    try testing.expectEqual(@as(u64, 10), vgot2.items[0] >> 32);

    // The LIVE search does see sym 12 — and that is where the arena rebuild
    // happens (the version reads above neither rebuilt nor consumed it).
    const builds1 = arena_builds;
    var lgot2: std.ArrayList(u64) = .empty;
    defer lgot2.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 3), dl_vector_search(db, &q, 10, collectSymCb, &lgot2));
    try testing.expectEqual(@as(u64, 12), lgot2.items[0] >> 32);
    try testing.expectEqual(builds1 + 1, arena_builds);
}

test "corpus layout gate: struct matches vector.h, constants survive" {
    // Layout gate: the corpus struct must match vector.h byte-for-byte.
    try testing.expectEqual(@sizeOf(cv.struct_dl_vec_corpus), @sizeOf(DlVecCorpus));
    try testing.expectEqualStrings("entity", std.mem.span(DL_VEC_CORPUS_ENTITY.filter_rel.?));
    try testing.expectEqualStrings("__vec_q__", std.mem.span(DL_VEC_CORPUS_ENTITY.vec_rel.?));
    try testing.expectEqualStrings("observation", std.mem.span(DL_VEC_CORPUS_OBSERVATION_CONTENT.filter_rel.?));
    try testing.expectEqualStrings("__vec_obs__", std.mem.span(DL_VEC_CORPUS_OBSERVATION_CONTENT.vec_rel.?));
    try testing.expectEqual(@as(u8, 1), DL_VEC_CORPUS_OBSERVATION_CONTENT.filter_col);
    // The encoder contract: 384 int8 lanes = 96 packed u32 chunks.
    try testing.expectEqual(@as(usize, 96), VEC_IVEC_WORDS);
}
