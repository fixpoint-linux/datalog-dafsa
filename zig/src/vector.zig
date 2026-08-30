//! vector.zig — port of src/vector.c (vector tier query path, S2).
//!
//! MIH candidate retrieval (dl_vector_search / dl_vector_search_version and
//! the corpus-parameterized forms) plus integer int8 cosine re-ranking
//! (dl_vector_rerank / dl_vector_rerank_corpus).
//!
//! LIVE vs VERSION read discipline (F1/F2, the load-bearing invariant):
//!   - dl_prefix / dl_lookup are LIVE-ONLY (dl.zig) — they never route to the
//!     snapshot view.  VERSION reads MUST use dl_query_bound_version.
//!   - LIVE search reads BOTH sig_j and entity via dl_prefix.
//!   - VERSION search reads BOTH via dl_query_bound_version.
//! The two search entry points share ONE implementation that branches ONLY on
//! the read primitive (mirrors dl_search / dl_search_version in index.zig),
//! so a publish between a sig_j read and an entity read can never produce an
//! inconsistent candidate set within a single mode.
//!
//! int8 re-rank is pure integer arithmetic (F5): no float, no sqrtf.  Integer
//! dot + integer norm, ranked by exact cosine order via a SIGNED int64
//! cross-multiply dot_a*|b| vs dot_b*|a| (integer isqrt for |v|).
//!
//! Strangler-hybrid ABI: `struct dl_vec_corpus` is a CONCRETE extern struct
//! still constructed/dereferenced by C (the retained dl_cli oracle and the
//! vector test suites), so it is defined here byte-identical to vector.h and
//! comptime-gated against @cImport("vector.h")'s translate-c layout.  dl_db
//! is dl.zig's DlDb; dl_prefix / dl_query_bound_version come from dl.zig.
//!
//! Oracle: src/vector.c (never modified).  All allocation through raw libc.

const std = @import("std");
const c = std.c;

const dl = @import("dl.zig");
const relation = @import("relation.zig");

// vector.h: struct dl_vec_corpus + the compile-time layout constants shared
// verbatim with embed.py (reference for the comptime layout gate only — the
// implementation below mirrors the C oracle).
const cv = @cImport({
    @cInclude("vector.h");
});

// libc decl not in std.c (precedent: dl.zig).
extern "c" fn snprintf(buf: [*c]u8, size: usize, fmt: [*c]const u8, ...) c_int;

// ─── Compile-time layout constants (mirror src/vector.h) ──────────────────

const VEC_D = 384; // embedding dim (bge-small)
const VEC_C = 256; // ITQ bit-code length (c bits)
const VEC_M = 16; // MIH bands == __sig0__..__sig15__
const VEC_W = 16; // bits/band = ceil(VEC_C/VEC_M)
const VEC_SIG_WORDS = VEC_C / 32; // 8 u32, MSB-first
const VEC_IVEC_WORDS = VEC_D / 4; // 96 u32, 4 int8 packed each
const VEC_ENTITY_REL = "entity"; // arity-2, name sym-id in col 0

// ─── Public C-ABI struct (src/vector.h, byte-for-byte) ────────────────────

/// struct dl_vec_corpus — the vector tier is shared by multiple corpora; each
/// corpus keeps SEPARATE liveness / sig / vec relations.  ENTITY is the
/// historical path (byte-compatible); OBSERVATION_CONTENT indexes observation
/// CONTENT text (col 1 of `observation`).
pub const DlVecCorpus = extern struct {
    filter_rel: ?[*:0]const u8, // liveness relation
    filter_col: u8, // which col holds the sym
    sig_rel_fmt: ?[*:0]const u8, // e.g. "__sig%d__" / "__obssig%d__"
    vec_rel: ?[*:0]const u8, // e.g. "__vec_q__" / "__vec_obs__"
    basis_suffix: ?[*:0]const u8, // "" / "_obs" for npy+metadata
};

// Comptime gate: our extern layout must be byte-identical to the C header.
comptime {
    std.debug.assert(@sizeOf(DlVecCorpus) == @sizeOf(cv.struct_dl_vec_corpus));
    std.debug.assert(@offsetOf(DlVecCorpus, "filter_col") == @offsetOf(cv.struct_dl_vec_corpus, "filter_col"));
    std.debug.assert(@offsetOf(DlVecCorpus, "vec_rel") == @offsetOf(cv.struct_dl_vec_corpus, "vec_rel"));
    std.debug.assert(@offsetOf(DlVecCorpus, "basis_suffix") == @offsetOf(cv.struct_dl_vec_corpus, "basis_suffix"));
}

/// DL_VEC_CORPUS_ENTITY — the macro as a const instance.
pub const DL_VEC_CORPUS_ENTITY = DlVecCorpus{
    .filter_rel = VEC_ENTITY_REL,
    .filter_col = 0,
    .sig_rel_fmt = "__sig%d__",
    .vec_rel = "__vec_q__",
    .basis_suffix = "",
};

/// DL_VEC_CORPUS_OBSERVATION_CONTENT — the macro as a const instance.
pub const DL_VEC_CORPUS_OBSERVATION_CONTENT = DlVecCorpus{
    .filter_rel = "observation",
    .filter_col = 1,
    .sig_rel_fmt = "__obssig%d__",
    .vec_rel = "__vec_obs__",
    .basis_suffix = "_obs",
};

/// Callback type: typedef int (*dl_vec_cb)(uint32_t entity_sym, int score,
/// void *user).
pub const DlVecCb = ?*const fn (entity_sym: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int;

/// dl_tuple_cb == rel_enum_cb (shared type identity with dl.zig's DlTupleCb).
const DlTupleCb = relation.RelEnumCb;

// ─── Band slicing (C1: MUST match embed.py's slice formula) ──────────────

/// Band j of a c-bit code.  Word layout MSB-first (sig[0] = bits 255..224).
/// VEC_W=16 divides 32, so band j sits in one word: even j -> high 16 of word
/// j/2 (shift 16), odd j -> low 16 (shift 0).  Band 0 = high 16 of sig[0].
fn bandSlice(sig: [*]const u32, j: usize) u32 {
    return (sig[j / 2] >> @truncate((1 - (j % 2)) * 16)) & 0xFFFF;
}

fn sigRelName(corpus: *const DlVecCorpus, j: usize, out: *[16]u8) void {
    _ = snprintf(out, 16, corpus.sig_rel_fmt, @as(c_int, @intCast(j)));
}

// ─── Variant probing ─────────────────────────────────────────────────────

/// Collect cb: record cols[1] (entity sym) into the per-band set (dedup).
const AddEntityCtx = struct {
    set: *VecCandSet,
    error_occurred: bool,
};

fn addEntityCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ac: *AddEntityCtx = @ptrCast(@alignCast(user.?));
    if (csEnsure(ac.set, cols.?[1]) == null) {
        ac.error_occurred = true;
        return 1;
    }
    return 0;
}

const VariantCtx = struct {
    rd: *const VecReader,
    corpus: *const DlVecCorpus,
    rel: [16]u8,
    per_band: *VecCandSet,
    error_occurred: bool,
};

/// One variant of a query band: prefix-probe sig_j and collect its entities.
/// Returns true if the variant loop should stop early.
fn variantProbeCb(variant: u32, v: *VariantCtx) bool {
    const leading = [1]u32{variant};
    var ac = AddEntityCtx{ .set = v.per_band, .error_occurred = false };
    const n = vecReadPrefix(v.rd, &v.rel, &leading, 1, addEntityCb, &ac, &v.error_occurred);
    if (ac.error_occurred) v.error_occurred = true; // propagate OOM to caller
    if (v.error_occurred or ac.error_occurred or n < 0) return true; // stop the variant loop
    return false;
}

/// Every VEC_W-bit value within Hamming `budget` of `sub`, streamed through
/// variantProbeCb.  Popcount scan (2^16 = 65536 iters) is trivially correct
/// at w=16; a combinatorial generator is a later optimization, not a
/// correctness change.  Returns true if the cb requested an early stop.
fn variantsWithin(sub: u32, budget: c_int, v: *VariantCtx) bool {
    var mask: u32 = 0;
    while (mask < (@as(u32, 1) << VEC_W)) : (mask += 1) {
        if (@as(c_int, @popCount(mask)) <= budget)
            if (variantProbeCb(sub ^ mask, v)) return true;
    }
    return false;
}

// ─── Candidate set: (entity sym, band-match count), dedup ────────────────

const VecCand = struct { sym: u32, bands: c_int };

const VecCandSet = struct {
    d: ?[*]VecCand = null,
    n: usize = 0,
    cap: usize = 0,
};

fn csFree(s: *VecCandSet) void {
    c.free(@ptrCast(s.d));
    s.* = .{};
}

fn csFind(s: *VecCandSet, sym: u32) ?*VecCand {
    var i: usize = 0;
    while (i < s.n) : (i += 1)
        if (s.d.?[i].sym == sym) return &s.d.?[i];
    return null;
}

/// Ensure `sym` has an entry; returns a pointer at it (new entries have
/// bands = 0), or null on OOM.
fn csEnsure(s: *VecCandSet, sym: u32) ?*VecCand {
    if (csFind(s, sym)) |existing| return existing;
    if (s.n >= s.cap) {
        const nc: usize = if (s.cap != 0) s.cap * 2 else 64;
        const nd: [*]VecCand = @ptrCast(@alignCast(c.realloc(@ptrCast(s.d), nc * @sizeOf(VecCand)) orelse return null));
        s.d = nd;
        s.cap = nc;
    }
    const cand = &s.d.?[s.n];
    s.n += 1;
    cand.* = .{ .sym = sym, .bands = 0 };
    return cand;
}

/// coarse relevance: most matched bands first, sym asc as the tiebreak.
fn candLt(_: void, a: VecCand, b: VecCand) bool {
    if (a.bands != b.bands) return a.bands > b.bands;
    return a.sym < b.sym;
}

// ─── Liveness filter set ──────────────────────────────────────────────────

/// One scan of the corpus liveness relation collects every live sym into a
/// growable array (realloc-doubling, mirroring vec_cand_set), then sort+dedup
/// so per-candidate membership is a bsearch.  Empty corpus -> empty set ->
/// the search yields 0 candidates (correct).
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

fn noopCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = cols;
    _ = arity;
    _ = user;
    return 0;
}

// ─── Shared search body (LIVE / VERSION differ only in the read primitive) ──

fn vectorSearchImpl(rd: *const VecReader, corpus: *const DlVecCorpus, q_sig: ?[*]const u32, k: c_int, r_in: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    var r = r_in;
    var err = false;

    if (rd.db == null or q_sig == null or k <= 0 or cb == null) return -1;
    if (r < 0) r = 0;

    // VERSION mode up-front gate: the liveness relation must be present in the
    // version (relation-absent-from-version -> -1, per the error contract).
    // leading=[0] touches no row if the relation exists (even empty).
    if (rd.use_version) {
        const probe = [1]u32{0};
        if (vecReadPrefix(rd, corpus.filter_rel, &probe, 1, noopCb, null, &err) < 0)
            return -1;
    }

    var cand = VecCandSet{};
    var band = VecCandSet{};

    var budget = @divTrunc(r, VEC_M); // pigeonhole budget per band
    if (budget > VEC_W) budget = VEC_W;

    var j: usize = 0;
    while (j < VEC_M and !err) : (j += 1) {
        var vc = VariantCtx{
            .rd = rd,
            .corpus = corpus,
            .rel = undefined,
            .per_band = &band,
            .error_occurred = false,
        };
        sigRelName(corpus, j, &vc.rel);
        csFree(&band);
        band = .{};
        _ = variantsWithin(bandSlice(q_sig.?, j), budget, &vc);
        if (vc.error_occurred) {
            err = true;
            break;
        }
        // merge per-band (distinct within the band) into the global set.
        var i: usize = 0;
        while (i < band.n and !err) : (i += 1) {
            const g = csEnsure(&cand, band.d.?[i].sym) orelse {
                err = true;
                break;
            };
            g.bands += 1;
        }
    }
    csFree(&band);
    if (err) {
        csFree(&cand);
        return -1;
    }

    // live-entity filter: one scan of the corpus liveness relation collects
    // every live sym (sort+dedup), then drop candidates not in the set.
    // Empty corpus -> empty set -> 0 candidates (correct).
    var live = VecFilterSet{ .col = corpus.filter_col };
    if (vecReadPrefix(rd, corpus.filter_rel, null, 0, collectFilterCb, &live, &err) < 0 or live.error_occurred) {
        if (!err) err = true;
        fsFree(&live);
        csFree(&cand);
        return -1;
    }
    fsSortDedup(&live);
    {
        var w: usize = 0;
        var i: usize = 0;
        while (i < cand.n) : (i += 1) {
            if (fsContains(&live, cand.d.?[i].sym)) {
                cand.d.?[w] = cand.d.?[i];
                w += 1;
            }
        }
        cand.n = w;
    }
    fsFree(&live);

    // coarse relevance: most matched bands first, sym asc as the tiebreak.
    // (cand.d may be NULL when no candidate survived — qsort(NULL, 0, ...) in
    // the C oracle was a tolerated no-op; skip instead of unwrapping null.)
    if (cand.n > 0) std.mem.sort(VecCand, cand.d.?[0..cand.n], {}, candLt);

    // emit at most k (count before the cb so an early stop still counts).
    var emitted: c_long = 0;
    var i: usize = 0;
    while (i < cand.n and emitted < k) : (i += 1) {
        emitted += 1;
        if (cb.?(cand.d.?[i].sym, cand.d.?[i].bands, user) != 0) break;
    }
    csFree(&cand);
    return emitted;
}

pub export fn dl_vector_search_corpus(db: ?*dl.DlDb, corpus: ?*const DlVecCorpus, q_sig: ?[*]const u32, k: c_int, r: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    const rd = VecReader{ .db = db, .version = 0, .use_version = false };
    return vectorSearchImpl(&rd, corpus orelse return -1, q_sig, k, r, cb, user);
}

pub export fn dl_vector_search_corpus_version(db: ?*dl.DlDb, version: u32, corpus: ?*const DlVecCorpus, q_sig: ?[*]const u32, k: c_int, r: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    if (db == null or version == 0) return -1;
    const rd = VecReader{ .db = db, .version = version, .use_version = true };
    return vectorSearchImpl(&rd, corpus orelse return -1, q_sig, k, r, cb, user);
}

pub export fn dl_vector_search(db: ?*dl.DlDb, q_sig: ?[*]const u32, k: c_int, r: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_search_corpus(db, &DL_VEC_CORPUS_ENTITY, q_sig, k, r, cb, user);
}

pub export fn dl_vector_search_version(db: ?*dl.DlDb, version: u32, q_sig: ?[*]const u32, k: c_int, r: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_search_corpus_version(db, version, &DL_VEC_CORPUS_ENTITY, q_sig, k, r, cb, user);
}

// ─── Integer int8 cosine re-rank ─────────────────────────────────────────

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

pub export fn dl_vector_rerank_corpus(db: ?*dl.DlDb, corpus: ?*const DlVecCorpus, q_int8: ?[*]const u32, cand_syms: ?[*]const u32, n_cand: c_int, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    var q: [VEC_D]i32 = undefined;
    var err = false;
    var n_valid: usize = 0;

    if (db == null or corpus == null or q_int8 == null or cand_syms == null or n_cand <= 0 or k <= 0 or cb == null)
        return -1;

    // unpack the query int8 vector (96 u32, 4 int8 little-endian packed).
    var i: usize = 0;
    while (i < VEC_D) : (i += 1)
        q[i] = @as(i8, @bitCast(@as(u8, @truncate(q_int8.?[i / 4] >> @truncate(8 * (i % 4))))));

    const rc_raw = c.calloc(@intCast(n_cand), @sizeOf(RankCand)) orelse return -1;
    const rc: [*]RankCand = @ptrCast(@alignCast(rc_raw));

    i = 0;
    while (i < @as(usize, @intCast(n_cand)) and !err) : (i += 1) {
        const sym = cand_syms.?[i];
        const leading = [1]u32{sym};
        var l = std.mem.zeroes(VecLoad);
        var d: usize = 0;
        var dot: i64 = 0;
        var norm2: i64 = 0;

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
        rc[n_valid].sym = sym;
        rc[n_valid].dot = @truncate(dot);
        rc[n_valid].norm2 = @truncate(@as(u64, @bitCast(norm2)));
        rc[n_valid].norm = isqrtU64(@bitCast(norm2));
        n_valid += 1;
    }

    if (err) {
        c.free(rc_raw);
        return -1;
    }

    std.mem.sort(RankCand, rc[0..n_valid], {}, rankCandLt);

    var emitted: c_long = 0;
    i = 0;
    while (i < n_valid and emitted < k) : (i += 1) {
        emitted += 1;
        if (cb.?(rc[i].sym, @intCast(rc[i].dot), user) != 0) break;
    }
    c.free(rc_raw);
    return emitted;
}

pub export fn dl_vector_rerank(db: ?*dl.DlDb, q_int8: ?[*]const u32, cand_syms: ?[*]const u32, n_cand: c_int, k: c_int, cb: DlVecCb, user: ?*anyopaque) c_long {
    return dl_vector_rerank_corpus(db, &DL_VEC_CORPUS_ENTITY, q_int8, cand_syms, n_cand, k, cb, user);
}

// ─── Tests ────────────────────────────────────────────────────────────────

const testing = std.testing;

/// Encode a 256-bit code into the VEC_SIG_WORDS u32 MSB-first layout
/// (embed.py's convention: bit i of the code lives at word i/32, bit 31-(i%32)).
fn encodeSig(code: *const [VEC_C]u8, sig: *[VEC_SIG_WORDS]u32) void {
    sig.* = [_]u32{0} ** VEC_SIG_WORDS;
    for (code, 0..) |bit, i| {
        if (bit != 0)
            sig[i / 32] |= @as(u32, 1) << @truncate(31 - (i % 32));
    }
}

fn collectSymCb(sym: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int {
    const out: *std.ArrayList(u64) = @ptrCast(@alignCast(user.?));
    const packed_hit = (@as(u64, sym) << 32) | @as(u64, @as(u32, @bitCast(score)));
    out.append(testing.allocator, packed_hit) catch return 1;
    return 0;
}

fn bandValue(sig: *const [VEC_SIG_WORDS]u32, j: usize) u16 {
    return @truncate(bandSlice(sig, j));
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

test "dl_vector_search: MIH bands, live filter, k cap (r=0 exact band)" {
    const path = "/tmp/datalog_zig_u14_vec_search";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    // entity liveness + all 16 sig relations (search probes each; a missing
    // one is an error).
    try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, "entity", 2));
    var j: usize = 0;
    while (j < VEC_M) : (j += 1) {
        var name: [16]u8 = undefined;
        sigRelName(&DL_VEC_CORPUS_ENTITY, j, &name);
        try testing.expectEqual(@as(c_int, 0), dl.dl_declare_relation(db, &name, 2));
    }

    // Live entities 10, 11.  Entity 99 has postings but is NOT in the
    // liveness relation -> dropped by the live filter.
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 10, 1 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "entity", &[_]u32{ 11, 1 }, 2));

    // Query code: all zeros.  Entity 10 matches band 0 exactly (band value 0
    // stored in __sig0__); entity 11 additionally matches band 1.
    const q_code = [_]u8{0} ** VEC_C;
    var q_sig: [VEC_SIG_WORDS]u32 = undefined;
    encodeSig(&q_code, &q_sig);

    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__sig0__", &[_]u32{ 0, 10 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__sig0__", &[_]u32{ 0, 11 }, 2));
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__sig1__", &[_]u32{ 0, 11 }, 2));
    // dead entity posting: same band-0 match but not live -> dropped.
    try testing.expectEqual(@as(c_int, 1), dl.dl_add_fact(db, "__sig0__", &[_]u32{ 0, 99 }, 2));

    var got: std.ArrayList(u64) = .empty;
    defer got.deinit(testing.allocator);
    const emitted = dl_vector_search(db, &q_sig, 10, 0, collectSymCb, &got);
    try testing.expectEqual(@as(c_long, 2), emitted);
    // 11 (2 bands) before 10 (1 band).
    try testing.expectEqual(@as(u64, 11), got.items[0] >> 32);
    try testing.expectEqual(@as(u32, 2), @as(u32, @truncate(got.items[0])));
    try testing.expectEqual(@as(u64, 10), got.items[1] >> 32);
    try testing.expectEqual(@as(u32, 1), @as(u32, @truncate(got.items[1])));

    // k caps the emission (highest-band-match first).
    var got_k: std.ArrayList(u64) = .empty;
    defer got_k.deinit(testing.allocator);
    try testing.expectEqual(@as(c_long, 1), dl_vector_search(db, &q_sig, 1, 0, collectSymCb, &got_k));
    try testing.expectEqual(@as(u64, 11), got_k.items[0] >> 32);

    // Error contract: k <= 0, NULL db, NULL cb.
    try testing.expectEqual(@as(c_long, -1), dl_vector_search(db, &q_sig, 0, 0, collectSymCb, &got));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search(null, &q_sig, 10, 0, collectSymCb, &got));
    try testing.expectEqual(@as(c_long, -1), dl_vector_search(db, &q_sig, 10, 0, null, &got));
    // Version 0 -> -1.
    try testing.expectEqual(@as(c_long, -1), dl_vector_search_version(db, 0, &q_sig, 10, 0, collectSymCb, &got));
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

test "bandSlice: MSB-first 16-bit band extraction" {
    // sig[0] = 0xAABBCCDD: band 0 = AABB (high 16), band 1 = CCDD (low 16).
    const sig = [VEC_SIG_WORDS]u32{ 0xAABBCCDD, 0x11223344, 0, 0, 0, 0, 0, 0 };
    try testing.expectEqual(@as(u16, 0xAABB), bandValue(&sig, 0));
    try testing.expectEqual(@as(u16, 0xCCDD), bandValue(&sig, 1));
    try testing.expectEqual(@as(u16, 0x1122), bandValue(&sig, 2));
    try testing.expectEqual(@as(u16, 0x3344), bandValue(&sig, 3));

    // Layout gate: the corpus struct must match vector.h byte-for-byte.
    try testing.expectEqual(@sizeOf(cv.struct_dl_vec_corpus), @sizeOf(DlVecCorpus));
    try testing.expectEqualStrings("entity", std.mem.span(DL_VEC_CORPUS_ENTITY.filter_rel.?));
    try testing.expectEqualStrings("observation", std.mem.span(DL_VEC_CORPUS_OBSERVATION_CONTENT.filter_rel.?));
    try testing.expectEqual(@as(u8, 1), DL_VEC_CORPUS_OBSERVATION_CONTENT.filter_col);
}
