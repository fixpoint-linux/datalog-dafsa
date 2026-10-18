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

// ─── Shared search body (LIVE / VERSION differ only in the read primitive) ──

fn vectorSearchImpl(rd: *const VecReader, corpus: *const DlVecCorpus, q_in: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    var err = false;

    if (rd.db == null or q_in == null or k <= 0 or cb == null) return -1;

    var q: [VEC_D]i32 = undefined;
    unpackQuery(q_in.?, &q);

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

    // scan: score every stored vector in ONE pass.
    var acc = std.AutoHashMap(u32, VecAccum).init(std.heap.c_allocator);
    defer acc.deinit();
    var sc = ScanCtx{ .q = &q, .m = &acc };
    if (vecReadPrefix(rd, corpus.vec_rel, null, 0, scanChunkCb, &sc, &err) < 0 or sc.error_occurred)
        return -1;

    // rank: one RankCand per LIVE entity that had at least one chunk (the map
    // may also hold dead syms — append-only vector rows — so it is an upper
    // bound on the candidate count).
    const n_scanned = acc.count();
    if (n_scanned == 0) return 0;
    const rc_raw = c.calloc(n_scanned, @sizeOf(RankCand)) orelse return -1;
    const rc: [*]RankCand = @ptrCast(@alignCast(rc_raw));
    defer c.free(rc_raw);

    var n_cand: usize = 0;
    var it = acc.iterator();
    while (it.next()) |e| {
        if (!fsContains(&live, e.key_ptr.*)) continue;
        const norm2 = e.value_ptr.norm2;
        rc[n_cand] = .{
            .sym = e.key_ptr.*,
            .dot = e.value_ptr.dot,
            .norm2 = norm2,
            .norm = isqrtU64(norm2),
        };
        n_cand += 1;
    }
    if (n_cand == 0) return 0;

    std.mem.sort(RankCand, rc[0..n_cand], {}, rankCandLt);

    // emit at most k, highest cosine first (counted before the cb so an early
    // stop still counts the row it saw).
    var emitted: c_long = 0;
    var i: usize = 0;
    while (i < n_cand and emitted < k) : (i += 1) {
        emitted += 1;
        if (cb.?(rc[i].sym, @intCast(rc[i].dot), user) != 0) break;
    }
    return emitted;
}

pub export fn dl_vector_search_corpus(db: ?*dl.DlDb, corpus: ?*const DlVecCorpus, q_int8: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    const rd = VecReader{ .db = db, .version = 0, .use_version = false };
    return vectorSearchImpl(&rd, corpus orelse return -1, q_int8, k, cb, user);
}

pub export fn dl_vector_search_corpus_version(db: ?*dl.DlDb, version: u32, corpus: ?*const DlVecCorpus, q_int8: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    if (db == null or version == 0) return -1;
    const rd = VecReader{ .db = db, .version = version, .use_version = true };
    return vectorSearchImpl(&rd, corpus orelse return -1, q_int8, k, cb, user);
}

pub export fn dl_vector_search(db: ?*dl.DlDb, q_int8: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_search_corpus(db, &DL_VEC_CORPUS_ENTITY, q_int8, k, cb, user);
}

pub export fn dl_vector_search_version(db: ?*dl.DlDb, version: u32, q_int8: ?[*]const u32, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_search_corpus_version(db, version, &DL_VEC_CORPUS_ENTITY, q_int8, k, cb, user);
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
