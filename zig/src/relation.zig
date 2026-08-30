//! relation.zig — port of src/relation.c (per-relation DAFSA with fixed-width
//! u32BE column encoding + per-relation WAL).
//!
//! Key encoding (the linchpin, architecture §2.3):
//!   | col0 u32BE | col1 u32BE | ... | col_{a-1} u32BE | \0 |
//!   Total: 4*a + 1 bytes, NO inter-column separators.
//!
//! Strangler-hybrid ABI: `struct relation` is OPAQUE in relation.h, so the
//! implementation is a native Zig struct; every non-static C function is an
//! `export fn` with the exact C name/signature/return semantics.  The dafsa
//! engine stays C in the hybrid .so (vendor/dafsa/*.c) and is addressed via
//! @cImport("dafsa_internal.h") — this port REACHES INTO dafsa internals
//! (struct dafsa states/initial/subtree_valid, State/Edge, trans_find) exactly
//! like the C oracle does; it does NOT reimplement them.  regex_dfa_walk and
//! symset_contains come from the ported zig/src/regexwalk.zig (U6) — imported
//! directly.  crc32_compute comes from dafsa_internal.h.
//!
//! Oracle: src/relation.c (never modified).

const std = @import("std");
const c = std.c;
const tupleset = @import("tupleset.zig"); // pub tuple_set type only

// ts_* are `export fn`s in tupleset.zig (C ABI); link against them directly.
extern "c" fn ts_init(ts: ?*tupleset.tuple_set, arity: u8) c_int;
extern "c" fn ts_free(ts: ?*tupleset.tuple_set) void;
extern "c" fn ts_add(ts: ?*tupleset.tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_sort(ts: ?*tupleset.tuple_set) void;

// dafsa_internal.h (struct dafsa/State/Edge/dafsa_wal, trans_find,
// crc32_compute, dafsa_* externs).  Include path set by build.zig.
const dc = @cImport({
    @cInclude("dafsa_internal.h");
});

// regexwalk.zig (U6): regex_dfa, sym_set, regex_dfa_walk, symset_contains.
const regexwalk = @import("regexwalk.zig");

const MAX_ARITY: usize = 8;
const MAX_KEY_LEN: usize = MAX_ARITY * 4 + 1; // 33

/// struct relation — opaque to C; native Zig layout.
pub const Relation = struct {
    d: [*c]dc.dafsa, // VIEW = base ∪ derived; all reads enumerate this
    base: [*c]dc.dafsa, // BASE = durable EDB facts; base == d for EDB-only
    arity: u8,
    wal: [*c]dc.dafsa_wal, // per-relation WAL handle, or NULL
    wal_path: [*c]u8, // path to WAL file (owned, strdup'd)
    dirty: c_int, // 1 if mutated since open (rel_is_dirty)
};

/// typedef int (*rel_enum_cb)(const uint32_t *cols, uint8_t arity, void *user)
pub const RelEnumCb = ?*const fn (cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int;

/// regexwalk.h types, re-exported for vrelation.zig (single source of truth so
/// the two modules share the exact same struct identity).
pub const regex_dfa = regexwalk.regex_dfa;
pub const sym_set = regexwalk.sym_set;

// ─── Key encoding helpers ─────────────────────────────────────────────────

/// trans_arr_c from dafsa_internal.h, reimplemented locally: the translate-c
/// output for the static-inline accessor fails to compile under Zig 0.16
/// (@ptrCast discarding const).  Byte-for-byte identical logic:
///   s->trans_heap ? s->trans_heap->edges : s->trans
fn transArrC(s: [*c]const dc.State) [*c]const dc.Edge {
    const heap = s.*.trans_heap;
    if (heap != null) {
        return @ptrCast(@alignCast(&heap.*._edges));
    }
    return @ptrCast(&s.*.trans);
}

/// Write arity u32 columns as big-endian into buf (4*arity bytes).
/// Does NOT write the trailing \0 — caller must add it.
fn writeColsBe(buf: [*]u8, cols: [*]const u32, arity: u8) void {
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        const v = cols[i];
        buf[4 * @as(usize, i)] = @truncate(v >> 24);
        buf[4 * @as(usize, i) + 1] = @truncate(v >> 16);
        buf[4 * @as(usize, i) + 2] = @truncate(v >> 8);
        buf[4 * @as(usize, i) + 3] = @truncate(v);
    }
}

/// Read arity u32 columns from big-endian buf.
fn readColsBe(cols: [*]u32, buf: [*]const u8, arity: u8) void {
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        cols[i] = (@as(u32, buf[4 * @as(usize, i)]) << 24) |
            (@as(u32, buf[4 * @as(usize, i) + 1]) << 16) |
            (@as(u32, buf[4 * @as(usize, i) + 2]) << 8) |
            (@as(u32, buf[4 * @as(usize, i) + 3]));
    }
}

/// Build the full key (4*arity + 1 bytes) in buf.  buf must be >= MAX_KEY_LEN.
fn encodeKey(buf: [*]u8, cols: [*]const u32, arity: u8) usize {
    writeColsBe(buf, cols, arity);
    buf[4 * @as(usize, arity)] = 0x00; // trailing guard
    return 4 * @as(usize, arity) + 1;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

/// relation *rel_create(uint8_t arity)
pub export fn rel_create(arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);

    rel.d = dc.dafsa_create();
    if (rel.d == null) {
        c.free(mem);
        return null;
    }

    rel.base = rel.d; // EDB-only: base aliases view
    rel.arity = arity;
    return rel;
}

/// relation *rel_open(const char *path, uint8_t arity)
pub export fn rel_open(path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);

    rel.d = dc.dafsa_load(path);
    if (rel.d == null) {
        // File doesn't exist or is corrupt — start fresh
        rel.d = dc.dafsa_create();
        if (rel.d == null) {
            c.free(mem);
            return null;
        }
    }

    rel.base = rel.d; // EDB-only: base aliases view
    rel.arity = arity;
    return rel;
}

/// int rel_save(const relation *rel, const char *path)
pub export fn rel_save(rel: ?*const Relation, path: [*c]const u8) c_int {
    const r = rel orelse return -1;
    if (path == null) return -1;
    return dc.dafsa_save(r.d, path);
}

/// int rel_save_base(const relation *rel, const char *path)
pub export fn rel_save_base(rel: ?*const Relation, path: [*c]const u8) c_int {
    const r = rel orelse return -1;
    if (path == null) return -1;
    return dc.dafsa_save(r.base, path);
}

/// void rel_free(relation *rel)
pub export fn rel_free(rel: ?*Relation) void {
    const r = rel orelse return;
    if (r.base != r.d) dc.dafsa_free(r.base);
    dc.dafsa_free(r.d);
    if (r.wal != null) dc.dafsa_wal_close(r.wal);
    if (r.wal_path != null) c.free(r.wal_path);
    c.free(@ptrCast(r));
}

/// uint8_t rel_arity(const relation *rel)
pub export fn rel_arity(rel: ?*const Relation) u8 {
    const r = rel orelse return 0;
    return r.arity;
}

/// uint64_t rel_count(const relation *rel)
pub export fn rel_count(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    if (r.d == null) return 0;
    var st: dc.dafsa_stats_out = std.mem.zeroes(dc.dafsa_stats_out);
    dc.dafsa_stats(r.d, &st);
    return @as(u64, st.n_final);
}

/// const dafsa *rel_dafsa(const relation *rel)
pub export fn rel_dafsa(rel: ?*const Relation) [*c]const dc.dafsa {
    const r = rel orelse return null;
    return r.d;
}

// ─── Order statistics (Tier-2) ────────────────────────────────────────────

/// uint64_t rel_rank(const relation *rel, const uint32_t *cols)
pub export fn rel_rank(rel: ?*const Relation, cols: ?[*]const u32) u64 {
    const r = rel orelse return std.math.maxInt(u64);
    if (r.d == null or cols == null) return std.math.maxInt(u64);
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, r.arity);
    return dc.dafsa_rank_n(r.d, &key, key_len);
}

/// int rel_select(const relation *rel, uint64_t k, uint32_t *cols_out)
pub export fn rel_select(rel: ?*const Relation, k: u64, cols_out: ?[*]u32) c_int {
    const r = rel orelse return -1;
    if (r.d == null or cols_out == null) return -1;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = dc.dafsa_select_n(r.d, k, &key, MAX_KEY_LEN);
    if (key_len < 0) return -1;
    readColsBe(cols_out.?, &key, r.arity);
    return 0;
}

/// uint64_t rel_range_count(const relation *rel, const uint32_t *lo,
///                          const uint32_t *hi)
pub export fn rel_range_count(rel: ?*const Relation, lo: ?[*]const u32, hi: ?[*]const u32) u64 {
    const r = rel orelse return std.math.maxInt(u64);
    if (r.d == null or lo == null or hi == null) return std.math.maxInt(u64);
    var lo_key: [MAX_KEY_LEN]u8 = undefined;
    var hi_key: [MAX_KEY_LEN]u8 = undefined;
    const lo_len = encodeKey(&lo_key, lo.?, r.arity);
    const hi_len = encodeKey(&hi_key, hi.?, r.arity);
    return dc.dafsa_range_count_n(r.d, &lo_key, lo_len, &hi_key, hi_len);
}

/// uint64_t rel_count_subtree(const relation *rel)
pub export fn rel_count_subtree(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    if (r.d == null) return 0;
    const n = dc.dafsa_ensure_subtree(r.d);
    if (r.d.*.subtree_valid == 0) return 0; // OOM during build: degrade to 0
    return n;
}

// ─── Prefix-bound order statistics (Tier-2 follow-up #1) ──────────────────

/// int rel_prefix_state(const relation *rel, const uint32_t *leading,
///                      uint8_t k)
/// Walk the k*4-byte prefix of `leading` (u32BE each) from the root via
/// trans_find to the DAFSA state representing that bound.  Returns the state
/// index, or -1 if the prefix is not a prefix of any key.
pub export fn rel_prefix_state(rel: ?*const Relation, leading: ?[*]const u32, k: u8) c_int {
    const r = rel orelse return -1;
    if (r.d == null) return -1;
    if (r.arity == 0) return -1;
    var current: c_uint = r.d.*.initial;
    var i: u8 = 0;
    while (i < k) : (i += 1) {
        const v = leading.?[i];
        var col_be: [4]u8 = undefined;
        col_be[0] = @truncate(v >> 24);
        col_be[1] = @truncate(v >> 16);
        col_be[2] = @truncate(v >> 8);
        col_be[3] = @truncate(v);
        var b: u8 = 0;
        while (b < 4) : (b += 1) {
            const tr = dc.trans_find(r.d.*.states + current, col_be[b]);
            if (tr < 0) return -1; // bound prefix not present in any key
            current = (transArrC(r.d.*.states + current) + @as(usize, @intCast(tr))).*.target;
        }
    }
    return @intCast(current);
}

/// uint64_t rel_rank_bound(const relation *rel, const uint32_t *leading,
///                         uint8_t k, const uint32_t *cols)
pub export fn rel_rank_bound(rel: ?*const Relation, leading: ?[*]const u32, k: u8, cols: ?[*]const u32) u64 {
    const r = rel orelse return std.math.maxInt(u64);
    if (r.d == null or cols == null) return std.math.maxInt(u64);
    if (k > r.arity) return std.math.maxInt(u64);
    if (k > 0 and leading == null) return std.math.maxInt(u64);
    const s = rel_prefix_state(r, leading, k);
    if (s < 0) return 0; // bound absent: rank 0 within the (empty) bound
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.? + k, @intCast(r.arity - k));
    return dc.dafsa_rank_from(r.d, @intCast(s), &key, key_len);
}

/// int rel_select_bound(const relation *rel, const uint32_t *leading,
///                      uint8_t k, uint64_t idx, uint32_t *cols_out)
pub export fn rel_select_bound(rel: ?*const Relation, leading: ?[*]const u32, k: u8, idx: u64, cols_out: ?[*]u32) c_int {
    const r = rel orelse return -1;
    if (r.d == null or cols_out == null) return -1;
    if (k > r.arity) return -1;
    if (k > 0 and leading == null) return -1;
    const s = rel_prefix_state(r, leading, k);
    if (s < 0) return -1; // bound absent
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = dc.dafsa_select_from(r.d, @intCast(s), idx, &key, MAX_KEY_LEN);
    if (key_len < 0) return -1;
    if (k > 0) {
        @memcpy(cols_out.?[0..k], leading.?[0..k]);
    }
    readColsBe(cols_out.? + k, &key, @intCast(r.arity - k));
    return 0;
}

/// uint64_t rel_range_count_bound(const relation *rel, const uint32_t *leading,
///                                uint8_t k, const uint32_t *lo,
///                                const uint32_t *hi)
pub export fn rel_range_count_bound(rel: ?*const Relation, leading: ?[*]const u32, k: u8, lo: ?[*]const u32, hi: ?[*]const u32) u64 {
    const r = rel orelse return std.math.maxInt(u64);
    if (r.d == null or lo == null or hi == null) return std.math.maxInt(u64);
    if (k > r.arity) return std.math.maxInt(u64);
    if (k > 0 and leading == null) return std.math.maxInt(u64);
    const s = rel_prefix_state(r, leading, k);
    if (s < 0) return 0; // bound absent
    var lo_key: [MAX_KEY_LEN]u8 = undefined;
    var hi_key: [MAX_KEY_LEN]u8 = undefined;
    const lo_len = encodeKey(&lo_key, lo.? + k, @intCast(r.arity - k));
    const hi_len = encodeKey(&hi_key, hi.? + k, @intCast(r.arity - k));
    return dc.dafsa_range_count_from(r.d, @intCast(s), &lo_key, lo_len, &hi_key, hi_len);
}

// ─── Fact operations ──────────────────────────────────────────────────────

fn relAddD(rel: *Relation, d: [*c]dc.dafsa, cols: ?[*]const u32) c_int {
    if (d == null or cols == null) return -1;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, rel.arity);
    return dc.dafsa_add_n(d, &key, key_len);
}

/// int rel_add(relation *rel, const uint32_t *cols)
pub export fn rel_add(rel: ?*Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return -1;
    const rc = relAddD(r, r.d, cols);
    if (rc > 0) r.dirty = 1;
    return rc;
}

/// int rel_add_base(relation *rel, const uint32_t *cols)
pub export fn rel_add_base(rel: ?*Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return -1;
    const rc = relAddD(r, r.base, cols);
    if (rc > 0) r.dirty = 1;
    return rc;
}

fn relExactD(rel: *const Relation, d: [*c]const dc.dafsa, cols: ?[*]const u32) c_int {
    if (d == null or cols == null) return 0;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, rel.arity);
    return dc.dafsa_lookup_n(d, &key, key_len);
}

/// int rel_exact(const relation *rel, const uint32_t *cols)
pub export fn rel_exact(rel: ?*const Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return 0;
    return relExactD(r, r.d, cols);
}

/// int rel_exact_base(const relation *rel, const uint32_t *cols)
pub export fn rel_exact_base(rel: ?*const Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return 0;
    return relExactD(r, r.base, cols);
}

fn relDeleteD(rel: *Relation, d: [*c]dc.dafsa, cols: ?[*]const u32) c_int {
    if (d == null or cols == null) return -1;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, rel.arity);
    return dc.dafsa_delete_n(d, &key, key_len);
}

/// int rel_delete(relation *rel, const uint32_t *cols)
pub export fn rel_delete(rel: ?*Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return -1;
    const rc = relDeleteD(r, r.d, cols);
    if (rc > 0) r.dirty = 1;
    return rc;
}

/// int rel_delete_base(relation *rel, const uint32_t *cols)
pub export fn rel_delete_base(rel: ?*Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return -1;
    const rc = relDeleteD(r, r.base, cols);
    if (rc > 0) r.dirty = 1;
    return rc;
}

// ─── Prefix enumeration ───────────────────────────────────────────────────

/// Context passed through the DFS recursion
const PrefixCtx = struct {
    leading: ?[*]const u32, // bound leading column values
    k: u8, // how many leading columns are bound
    arity: u8, // total arity of the relation
    cb: RelEnumCb, // user callback
    user: ?*anyopaque,
    count: c_long, // running count
};

/// Recursive DFS from `state`, accumulating bytes into `buf`.
/// At each final state, reconstruct the full tuple and call the user cb.
/// Returns non-zero to stop early (propagated from cb).
fn prefixDfs(d: [*c]const dc.dafsa, state: c_uint, buf: [*]u8, depth: usize, ctx: *PrefixCtx) c_int {
    const s = d.*.states + state;
    if (s.*.is_final != 0) {
        // Payload is `depth` bytes: the remaining columns + trailing \0.
        const n_rem: u8 = ctx.arity - ctx.k;
        var full_tuple: [MAX_ARITY]u32 = undefined;

        // Copy leading columns
        var i: u8 = 0;
        while (i < ctx.k) : (i += 1) {
            full_tuple[i] = ctx.leading.?[i];
        }

        // Parse remaining columns from buf (skip trailing \0)
        if (n_rem > 0 and depth >= @as(usize, n_rem) * 4) {
            readColsBe(full_tuple[ctx.k..].ptr, buf, n_rem);
        }

        ctx.count +%= 1;
        if (ctx.cb.?(&full_tuple, ctx.arity, ctx.user) != 0)
            return 1;
    }

    if (depth >= MAX_KEY_LEN) return 0;

    var j: c_uint = 0;
    while (j < s.*.ntrans) : (j += 1) {
        const e = transArrC(s) + j;
        buf[depth] = e.*.sym;
        if (prefixDfs(d, e.*.target, buf, depth + 1, ctx) != 0)
            return 1;
    }
    return 0;
}

fn relPrefixD(rel: *const Relation, d: [*c]const dc.dafsa, leading: ?[*]const u32, k: u8, cb: RelEnumCb, user: ?*anyopaque) c_long {
    if (d == null or cb == null) return -1;
    if (k > rel.arity) return -1;
    if (k > 0 and leading == null) return -1;

    // Walk the k*4 prefix bytes
    var current: c_uint = d.*.initial;
    var i: u8 = 0;
    while (i < k) : (i += 1) {
        const v = leading.?[i];
        var col_be: [4]u8 = undefined;
        col_be[0] = @truncate(v >> 24);
        col_be[1] = @truncate(v >> 16);
        col_be[2] = @truncate(v >> 8);
        col_be[3] = @truncate(v);
        var b: u8 = 0;
        while (b < 4) : (b += 1) {
            const tr = dc.trans_find(d.*.states + current, col_be[b]);
            if (tr < 0) return 0; // prefix not found
            current = (transArrC(d.*.states + current) + @as(usize, @intCast(tr))).*.target;
        }
    }

    // Set up context and DFS from current state
    var ctx = PrefixCtx{
        .leading = leading,
        .k = k,
        .arity = rel.arity,
        .cb = cb,
        .user = user,
        .count = 0,
    };
    var buf: [MAX_KEY_LEN]u8 = undefined;

    _ = prefixDfs(d, current, &buf, 0, &ctx);
    return ctx.count;
}

/// long rel_prefix(const relation *rel, const uint32_t *leading, uint8_t k,
///                 rel_enum_cb cb, void *user)
pub export fn rel_prefix(rel: ?*const Relation, leading: ?[*]const u32, k: u8, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    return relPrefixD(r, r.d, leading, k, cb, user);
}

/// long rel_prefix_base(const relation *rel, const uint32_t *leading, uint8_t k,
///                      rel_enum_cb cb, void *user)
pub export fn rel_prefix_base(rel: ?*const Relation, leading: ?[*]const u32, k: u8, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    return relPrefixD(r, r.base, leading, k, cb, user);
}

// ─── Leading-column range enumeration ─────────────────────────────────────

/// int rel_has_col0(const relation *rel, uint32_t x)
pub export fn rel_has_col0(rel: ?*const Relation, x: u32) c_int {
    const xp: [*]const u32 = @ptrCast(&x);
    return if (rel_prefix_state(rel, xp, 1) >= 0) 1 else 0;
}

/// long rel_range_each(const relation *rel, uint32_t lo, uint32_t hi,
///                     rel_enum_cb cb, void *user)
pub export fn rel_range_each(rel: ?*const Relation, lo: u32, hi: u32, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    if (r.d == null or cb == null) return -1;
    if (hi <= lo) return 0;

    var lo_key: [MAX_ARITY]u32 = std.mem.zeroes([MAX_ARITY]u32);
    var hi_key: [MAX_ARITY]u32 = std.mem.zeroes([MAX_ARITY]u32);
    lo_key[0] = lo;
    hi_key[0] = hi;

    const r0 = rel_rank(r, &lo_key);
    const r1 = rel_rank(r, &hi_key);
    if (r0 == std.math.maxInt(u64) or r1 == std.math.maxInt(u64)) return -1;
    if (r1 <= r0) return 0;

    var count: u64 = 0;
    var k: u64 = r0;
    while (k < r1) : (k += 1) {
        var cols: [MAX_ARITY]u32 = undefined;
        if (rel_select(r, k, &cols) != 0) return -1;
        count +%= 1;
        if (cb.?(&cols, r.arity, user) != 0)
            return @intCast(count); // early stop requested
    }
    return @intCast(count);
}

// ─── Bulk build from tuple set ────────────────────────────────────────────

/// int ts_sink_cb(const uint32_t *cols, uint8_t arity, void *user)
pub export fn ts_sink_cb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) c_int {
    _ = arity;
    const ts: *tupleset.tuple_set = @ptrCast(@alignCast(user orelse return -1));
    return if (ts_add(ts, cols) < 0) -1 else 0;
}

/// Build a fresh minimal DAFSA from a SORTED, DEDUPLICATED tuple_set.
/// Returns NULL on error/OOM.
fn dafsaBuildFromTs(ts: *const tupleset.tuple_set, arity: u8) [*c]dc.dafsa {
    const n = ts.count;
    if (n == 0)
        return dc.dafsa_create();

    const keys_mem = c.calloc(@as(usize, @intCast(n)), @sizeOf([*c]u8));
    const lens_mem = c.calloc(@as(usize, @intCast(n)), @sizeOf(usize));
    if (keys_mem == null or lens_mem == null) {
        if (keys_mem) |k| c.free(k);
        if (lens_mem) |l| c.free(l);
        return null;
    }
    const keys: [*c][*c]u8 = @ptrCast(@alignCast(keys_mem.?));
    const lens: [*c]usize = @ptrCast(@alignCast(lens_mem.?));

    var i: c_long = 0;
    while (i < n) : (i += 1) {
        const cols = ts.data.? + @as(usize, @intCast(i)) * @as(usize, ts.arity);
        const km = c.malloc(MAX_KEY_LEN);
        if (km == null) break;
        const key: [*]u8 = @ptrCast(km);
        keys[@intCast(i)] = @ptrCast(key);
        lens[@intCast(i)] = encodeKey(key, cols, arity);
    }

    var new_d: [*c]dc.dafsa = null;
    if (i == n) {
        new_d = dc.dafsa_build_sorted(@ptrCast(keys), lens, @as(usize, @intCast(n)));
    }

    // Free every key (calloc'd NULLs for the unfilled tail are free(NULL)).
    var j: c_long = 0;
    while (j < n) : (j += 1) {
        if (keys[@intCast(j)] != null) c.free(keys[@intCast(j)]);
    }
    c.free(keys_mem.?);
    c.free(lens_mem.?);
    return new_d;
}

/// int rel_build_from_tupleset(relation *rel, const struct tuple_set *ts)
pub export fn rel_build_from_tupleset(rel: ?*Relation, ts: ?*const tupleset.tuple_set) c_int {
    const r = rel orelse return -1;
    const t = ts orelse return -1;
    if (t.arity != r.arity) return -1;

    const new_d = dafsaBuildFromTs(t, r.arity);
    if (new_d == null) return -1;

    const aliased = (r.base == r.d);
    dc.dafsa_free(r.d);
    r.d = new_d;
    if (aliased) r.base = r.d; // keep base aliased for EDB rels
    r.dirty = 1;
    return 0;
}

/// int rel_build_base_from_tupleset(relation *rel, const struct tuple_set *ts)
pub export fn rel_build_base_from_tupleset(rel: ?*Relation, ts: ?*const tupleset.tuple_set) c_int {
    const r = rel orelse return -1;
    const t = ts orelse return -1;
    if (t.arity != r.arity) return -1;

    const new_b = dafsaBuildFromTs(t, r.arity);
    if (new_b == null) return -1;

    const aliased = (r.base == r.d);
    dc.dafsa_free(r.base);
    r.base = new_b;
    if (aliased) r.d = r.base; // keep view aliased for EDB rels
    r.dirty = 1;
    return 0;
}

/// Copy the facts of `src` into a fresh minimal DAFSA (via enumeration +
/// bulk build).
fn dafsaCopyFrom(rel: *const Relation, src: [*c]const dc.dafsa) [*c]dc.dafsa {
    if (src == null) return null;
    var ts: tupleset.tuple_set = undefined;
    if (ts_init(&ts, rel.arity) != 0) return null;
    if (relPrefixD(rel, src, null, 0, ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        return null;
    }
    ts_sort(&ts);
    const out = dafsaBuildFromTs(&ts, rel.arity);
    ts_free(&ts);
    return out;
}

// ─── Base/view partition (IVM Slice 0) ────────────────────────────────────

/// int rel_is_idb(const relation *rel)
pub export fn rel_is_idb(rel: ?*const Relation) c_int {
    const r = rel orelse return 0;
    return if (r.base != r.d) 1 else 0;
}

/// int rel_is_dirty(const relation *rel)
pub export fn rel_is_dirty(rel: ?*const Relation) c_int {
    const r = rel orelse return 0;
    return if (r.dirty != 0) 1 else 0;
}

/// int rel_reset_view(relation *rel)
pub export fn rel_reset_view(rel: ?*Relation) c_int {
    const r = rel orelse return -1;

    if (r.base == r.d) {
        // First evaluation as a derived relation: SPLIT base from view.
        const nb = dafsaCopyFrom(r, r.d);
        if (nb == null) return -1;
        r.base = nb;
        r.dirty = 1; // view is about to be re-derived by the VM
        return 0;
    }

    // Already split: drop stale derived facts — view = copy of base.
    {
        const nv = dafsaCopyFrom(r, r.base);
        if (nv == null) return -1;
        dc.dafsa_free(r.d);
        r.d = nv;
    }
    r.dirty = 1; // view reset for re-derivation
    return 0;
}

// ─── WAL operations (M7) ──────────────────────────────────────────────────

/// WAL replay callback: apply ADD or DEL to the in-memory DAFSA.
const WalReplayCtx = struct {
    rel: ?*Relation,
    ok: c_int, // becomes -1 on first error
};

fn walReplayCb(op: u8, key: [*c]const u8, key_len: u32, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *WalReplayCtx = @ptrCast(@alignCast(user orelse return 0));

    if (ctx.ok != 0) return 0; // already failed, skip

    if (op == dc.DAFSA_WAL_OP_ADD) {
        const rc = dc.dafsa_add_n(ctx.rel.?.base, key, key_len);
        if (rc < 0) ctx.ok = -1;
    } else if (op == dc.DAFSA_WAL_OP_DEL) {
        const rc = dc.dafsa_delete_n(ctx.rel.?.base, key, key_len);
        if (rc < 0) ctx.ok = -1;
    }
    return 0;
}

/// int rel_wal_replay_into(relation *rel)
pub export fn rel_wal_replay_into(rel: ?*Relation) c_int {
    const r = rel orelse return -1;
    if (r.wal == null) return -1;

    var ctx = WalReplayCtx{ .rel = r, .ok = 0 };

    if (dc.dafsa_wal_replay(r.wal, walReplayCb, &ctx) != 0)
        return -1;

    return ctx.ok;
}

/// int rel_wal_append_add(relation *rel, const unsigned char *key, uint32_t key_len)
pub export fn rel_wal_append_add(rel: ?*Relation, key: [*c]const u8, key_len: u32) c_int {
    const r = rel orelse return -1;
    if (r.wal == null or key == null) return -1;
    if (dc.dafsa_wal_append_add(r.wal, key, key_len) != 0) return -1;
    if (dc.dafsa_wal_sync(r.wal) != 0) return -1;
    return 0;
}

/// int rel_wal_append_del(relation *rel, const unsigned char *key, uint32_t key_len)
pub export fn rel_wal_append_del(rel: ?*Relation, key: [*c]const u8, key_len: u32) c_int {
    const r = rel orelse return -1;
    if (r.wal == null or key == null) return -1;
    if (dc.dafsa_wal_append_del(r.wal, key, key_len) != 0) return -1;
    if (dc.dafsa_wal_sync(r.wal) != 0) return -1;
    return 0;
}

/// fsync helper: fsync the directory containing a path.
/// NOTE: this replicates relation.c's own fsync_parent_dir (takes a path,
/// fsyncs its containing directory) — distinct from util.fsync_dir_of_path.
fn fsyncParentDir(path: [*c]const u8) c_int {
    const slash = dc.strrchr(path, '/');
    var ret: c_int = -1;
    const path_addr = @intFromPtr(path);

    if (slash == null or @intFromPtr(slash) == path_addr) {
        const fd = c.open("/", .{ .ACCMODE = .RDONLY, .DIRECTORY = true });
        if (fd >= 0) {
            ret = c.fsync(fd);
            _ = c.close(fd);
        }
        return ret;
    }

    const dir = dc.strndup(path, @intFromPtr(slash) - path_addr);
    if (dir == null) return -1;
    const fd = c.open(dir, .{ .ACCMODE = .RDONLY, .DIRECTORY = true });
    if (fd >= 0) {
        ret = c.fsync(fd);
        _ = c.close(fd);
    }
    c.free(dir);
    return ret;
}

/// int rel_compact(relation *rel, const char *dafsa_path)
pub export fn rel_compact(rel: ?*Relation, dafsa_path: [*c]const u8) c_int {
    const r = rel orelse return -1;
    if (r.wal == null or dafsa_path == null) return -1;

    // 1. Save the BASE DAFSA atomically.
    if (dc.dafsa_save(r.base, dafsa_path) != 0) return -1;

    // 2. ftruncate WAL to 16 bytes (header-only) and fsync.
    if (c.ftruncate(r.wal.*.fd, 16) != 0) return -1;
    if (c.fsync(r.wal.*.fd) != 0) return -1;
    r.wal.*.size = 16;

    // 3. fsync the directory containing the WAL.
    if (fsyncParentDir(r.wal_path) != 0) return -1;

    return 0;
}

/// uint64_t rel_wal_size(const relation *rel)
pub export fn rel_wal_size(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    if (r.wal == null) return 0;
    return dc.dafsa_wal_size(r.wal);
}

/// uint64_t rel_dafsa_size(const relation *rel)
pub export fn rel_dafsa_size(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    if (r.base == null) return 0;
    var st: dc.dafsa_stats_out = std.mem.zeroes(dc.dafsa_stats_out);
    dc.dafsa_stats(r.base, &st);
    // Rough byte estimate based on state/transition counts (25% compaction
    // threshold).
    const est = @as(u64, st.n_states_reachable) * 64 + @as(u64, st.n_trans) * 8;
    return est;
}

// ─── Open with WAL ────────────────────────────────────────────────────────

/// relation *rel_open_writable(const char *dafsa_path, const char *wal_path,
///                             uint8_t arity)
pub export fn rel_open_writable(dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);

    // Load base DAFSA
    rel.d = dc.dafsa_load(dafsa_path);
    if (rel.d == null) {
        rel.d = dc.dafsa_create();
        if (rel.d == null) {
            c.free(mem);
            return null;
        }
    }
    rel.base = rel.d; // EDB-only: base aliases view
    rel.arity = arity;

    // Open WAL (rw) — auto-repairs torn tail
    var wal_exists: c_int = 0;
    {
        var st: dc.struct_stat = undefined;
        wal_exists = if (dc.stat(wal_path, &st) == 0 and st.st_size > 16) 1 else 0;
    }

    rel.wal = dc.dafsa_wal_open_rw(wal_path);
    if (rel.wal == null) {
        dc.dafsa_free(rel.d);
        c.free(mem);
        return null;
    }

    rel.wal_path = dc.strdup(wal_path);
    if (rel.wal_path == null) {
        dc.dafsa_wal_close(rel.wal);
        dc.dafsa_free(rel.d);
        c.free(mem);
        return null;
    }

    // If WAL had records > header, replay them into in-memory DAFSA
    if (wal_exists != 0) {
        if (rel_wal_replay_into(rel) != 0) {
            rel_free(rel);
            return null;
        }
        // Compact immediately: save DAFSA + truncate WAL
        if (rel_compact(rel, dafsa_path) != 0) {
            rel_free(rel);
            return null;
        }
    }

    return rel;
}

/// relation *rel_open_writable_idb(const char *base_path, const char *dafsa_path,
///                                 const char *wal_path, uint8_t arity)
pub export fn rel_open_writable_idb(base_path: [*c]const u8, dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);

    // Load base DAFSA
    rel.base = dc.dafsa_load(base_path);
    if (rel.base == null) {
        rel.base = dc.dafsa_create();
        if (rel.base == null) {
            c.free(mem);
            return null;
        }
    }

    // Load view DAFSA (base ∪ derived, as last materialized)
    rel.d = dc.dafsa_load(dafsa_path);
    if (rel.d == null) {
        rel.d = dc.dafsa_create();
        if (rel.d == null) {
            dc.dafsa_free(rel.base);
            c.free(mem);
            return null;
        }
    }
    rel.arity = arity;

    // Open WAL (rw) — auto-repairs torn tail
    var wal_exists: c_int = 0;
    {
        var st: dc.struct_stat = undefined;
        wal_exists = if (dc.stat(wal_path, &st) == 0 and st.st_size > 16) 1 else 0;
    }

    rel.wal = dc.dafsa_wal_open_rw(wal_path);
    if (rel.wal == null) {
        dc.dafsa_free(rel.d);
        dc.dafsa_free(rel.base);
        c.free(mem);
        return null;
    }

    rel.wal_path = dc.strdup(wal_path);
    if (rel.wal_path == null) {
        dc.dafsa_wal_close(rel.wal);
        dc.dafsa_free(rel.d);
        dc.dafsa_free(rel.base);
        c.free(mem);
        return null;
    }

    // Replay WAL into BASE and compact immediately.
    if (wal_exists != 0) {
        if (rel_wal_replay_into(rel) != 0) {
            rel_free(rel);
            return null;
        }
        if (rel_compact(rel, base_path) != 0) {
            rel_free(rel);
            return null;
        }
    }

    return rel;
}

/// relation *rel_open_readonly(const char *dafsa_path, const char *wal_path,
///                             uint8_t arity)
pub export fn rel_open_readonly(dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);

    rel.d = dc.dafsa_load(dafsa_path);
    if (rel.d == null) {
        rel.d = dc.dafsa_create();
        if (rel.d == null) {
            c.free(mem);
            return null;
        }
    }
    rel.base = rel.d; // EDB-only: base aliases view
    rel.arity = arity;

    if (wal_path != null) {
        rel.wal = dc.dafsa_wal_open_ro(wal_path);
        if (rel.wal != null and rel_wal_replay_into(rel) != 0) {
            rel_free(rel);
            return null;
        }
        // wal_path stays NULL: only rel_compact reads it and RO never compacts.
    }

    return rel;
}

/// relation *rel_open_readonly_idb(const char *base_path, const char *dafsa_path,
///                                 const char *wal_path, uint8_t arity)
pub export fn rel_open_readonly_idb(base_path: [*c]const u8, dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);

    rel.base = dc.dafsa_load(base_path);
    if (rel.base == null) {
        rel.base = dc.dafsa_create();
        if (rel.base == null) {
            c.free(mem);
            return null;
        }
    }

    rel.d = dc.dafsa_load(dafsa_path);
    if (rel.d == null) {
        rel.d = dc.dafsa_create();
        if (rel.d == null) {
            dc.dafsa_free(rel.base);
            c.free(mem);
            return null;
        }
    }
    rel.arity = arity;

    if (wal_path != null) {
        rel.wal = dc.dafsa_wal_open_ro(wal_path);
        if (rel.wal != null and rel_wal_replay_into(rel) != 0) {
            rel_free(rel);
            return null;
        }
    }

    return rel;
}

// ─── Regex pattern walk ───────────────────────────────────────────────────

const PatCtx = struct {
    arity: u8,
    cb: RelEnumCb,
    user: ?*anyopaque,
    count: c_long,
};

fn patCb(key_bytes: [*c]const u8, key_len: usize, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *PatCtx = @ptrCast(@alignCast(user orelse return 0));
    var cols: [MAX_ARITY]u32 = undefined;

    // Key is 4*arity+1 bytes. Decode columns from buf.
    if (key_len != @as(usize, ctx.arity) * 4 + 1) return 0;

    var i: u8 = 0;
    while (i < ctx.arity) : (i += 1) {
        cols[i] = (@as(u32, key_bytes[4 * @as(usize, i)]) << 24) |
            (@as(u32, key_bytes[4 * @as(usize, i) + 1]) << 16) |
            (@as(u32, key_bytes[4 * @as(usize, i) + 2]) << 8) |
            (@as(u32, key_bytes[4 * @as(usize, i) + 3]));
    }

    ctx.count +%= 1;
    return ctx.cb.?(&cols, ctx.arity, ctx.user);
}

/// long rel_pattern(const relation *rel, const struct regex_dfa *dfa,
///                  rel_enum_cb cb, void *user)
pub export fn rel_pattern(rel: ?*const Relation, dfa: [*c]const regexwalk.regex_dfa, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    if (dfa == null or cb == null) return -1;

    var ctx = PatCtx{
        .arity = r.arity,
        .cb = cb,
        .user = user,
        .count = 0,
    };

    // r.d is a [*c]dc.dafsa (this module's @cImport); regexwalk's dc is a
    // separate @cImport, so cast the opaque struct pointer across.
    const n = regexwalk.regex_dfa_walk(@ptrCast(r.d), dfa, patCb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

// ─── Filter callback ──────────────────────────────────────────────────────

const FilterCtx = struct {
    col: u8,
    set: [*c]const regexwalk.sym_set,
    cb: RelEnumCb,
    user: ?*anyopaque,
    count: c_long,
};

fn filterCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *FilterCtx = @ptrCast(@alignCast(user orelse return 0));
    const cols_p = cols orelse return 0;
    if (ctx.col < arity and regexwalk.symset_contains(ctx.set, cols_p[ctx.col]) != 0) {
        ctx.count +%= 1;
        return ctx.cb.?(cols, arity, ctx.user);
    }
    return 0;
}

/// long rel_filter_col(const relation *rel, uint8_t col, const struct sym_set *set,
///                     rel_enum_cb cb, void *user)
pub export fn rel_filter_col(rel: ?*const Relation, col: u8, set: [*c]const regexwalk.sym_set, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    if (set == null or cb == null) return -1;
    if (col >= r.arity) return 0; // out of range: no matches

    var ctx = FilterCtx{
        .col = col,
        .set = set,
        .cb = cb,
        .user = user,
        .count = 0,
    };

    // Enumerate all tuples and filter by column membership
    const n = rel_prefix(r, null, 0, filterCb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

// ─── Tests ────────────────────────────────────────────────────────────────

test "key encode/decode roundtrip" {
    var buf: [MAX_KEY_LEN]u8 = undefined;
    const cols = [_]u32{ 0xDEADBEEF, 0x00000001, 0xFFFFFFFF, 0x01020304 };
    const len = encodeKey(&buf, &cols, 4);
    try std.testing.expectEqual(@as(usize, 17), len);
    try std.testing.expectEqual(@as(u8, 0x00), buf[16]);
    var out: [4]u32 = undefined;
    readColsBe(&out, &buf, 4);
    try std.testing.expectEqualSlices(u32, &cols, &out);
    // Byte-exact big-endian layout.
    try std.testing.expectEqual(@as(u8, 0xDE), buf[0]);
    try std.testing.expectEqual(@as(u8, 0xAD), buf[1]);
    try std.testing.expectEqual(@as(u8, 0xBE), buf[2]);
    try std.testing.expectEqual(@as(u8, 0xEF), buf[3]);
}

test "rel_create/add/exact/prefix/rank/select/range_count roundtrip" {
    const r = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(r);

    const facts = [_]u32{ 1, 2, 1, 3, 2, 1, 5, 5 };
    var i: usize = 0;
    while (i < 4) : (i += 1) {
        try std.testing.expectEqual(@as(c_int, 1), rel_add(r, facts[i * 2 ..][0..2].ptr));
    }
    try std.testing.expectEqual(@as(c_int, 0), rel_add(r, facts[0..2].ptr)); // dup
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(r, facts[2..4].ptr));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact(r, &.{ 9, 9 }));
    // rel_count = dafsa n_final = merged final STATES (identical right
    // languages collapse), not words.  count_subtree is the word count.
    try std.testing.expectEqual(@as(u64, 1), rel_count(r));
    try std.testing.expectEqual(@as(u64, 4), rel_count_subtree(r));

    // lex order (u32BE): (1,2) (1,3) (2,1) (5,5)
    try std.testing.expectEqual(@as(u64, 0), rel_rank(r, &.{ 1, 2 }));
    try std.testing.expectEqual(@as(u64, 2), rel_rank(r, &.{ 2, 0 }));
    try std.testing.expectEqual(@as(u64, 4), rel_rank(r, &.{ 9, 9 }));

    var out: [2]u32 = undefined;
    try std.testing.expectEqual(@as(c_int, 0), rel_select(r, 2, &out));
    try std.testing.expectEqualSlices(u32, &.{ 2, 1 }, &out);
    try std.testing.expectEqual(@as(c_int, -1), rel_select(r, 4, &out));

    // key-lexicographic range: (1,2),(1,3),(2,1) fall in [(1,0)..(2,2)].
    try std.testing.expectEqual(@as(u64, 3), rel_range_count(r, &.{ 1, 0 }, &.{ 2, 2 }));

    // prefix walk
    var count: c_long = 0;
    try std.testing.expectEqual(@as(c_long, 4), rel_prefix(r, null, 0, countCb, &count));
    try std.testing.expectEqual(@as(c_long, 4), count);
    count = 0;
    try std.testing.expectEqual(@as(c_long, 2), rel_prefix(r, &.{1}, 1, countCb, &count));
    try std.testing.expectEqual(@as(c_long, 2), count);
    count = 0;
    try std.testing.expectEqual(@as(c_long, 1), rel_prefix(r, &.{ 2, 1 }, 2, countCb, &count));
    try std.testing.expectEqual(@as(c_long, 1), count);
    count = 0;
    try std.testing.expectEqual(@as(c_long, 0), rel_prefix(r, &.{ 7 }, 1, countCb, &count));

    // prefix-bound order stats
    try std.testing.expectEqual(@as(u64, 1), rel_rank_bound(r, &.{1}, 1, &.{ 1, 3 }));
    var b2: [2]u32 = undefined;
    try std.testing.expectEqual(@as(c_int, 0), rel_select_bound(r, &.{1}, 1, 1, &b2));
    try std.testing.expectEqualSlices(u32, &.{ 1, 3 }, &b2);
    // bound range: within the prefix-{1} subtree, tail in [0..0] — none of
    // (1,2),(1,3) qualifies (oracle-verified: 0).
    try std.testing.expectEqual(@as(u64, 0), rel_range_count_bound(r, &.{1}, 1, &.{ 1, 0 }, &.{ 2, 0 }));

    // delete
    try std.testing.expectEqual(@as(c_int, 1), rel_delete(r, facts[0..2].ptr));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact(r, facts[0..2].ptr));
    // (1,3),(2,1),(5,5) remain; their final states merge to one.
    try std.testing.expectEqual(@as(u64, 1), rel_count(r));
}

fn countCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = cols;
    _ = arity;
    const counter: *c_long = @ptrCast(@alignCast(user orelse return 0));
    counter.* +%= 1;
    return 0;
}

test "rel_reset_view split + build_from_tupleset" {
    const r = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(r);

    // EDB: base == view
    try std.testing.expectEqual(@as(c_int, 0), rel_is_idb(r));

    // First reset SPLITs base from view.
    try std.testing.expectEqual(@as(c_int, 0), rel_reset_view(r));
    try std.testing.expectEqual(@as(c_int, 1), rel_is_idb(r));

    // Add a derived fact to the view only.
    try std.testing.expectEqual(@as(c_int, 1), rel_add(r, &.{ 9, 9 }));
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(r, &.{ 9, 9 }));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact_base(r, &.{ 9, 9 }));

    // Reset again: view = copy of base, dropping derived facts.
    try std.testing.expectEqual(@as(c_int, 0), rel_reset_view(r));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact(r, &.{ 9, 9 }));
    try std.testing.expectEqual(@as(c_int, 1), rel_is_idb(r));
}

test "rel_build_from_tupleset bulk build" {
    const r = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(r);

    var ts: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), ts_init(&ts, 2));
    defer ts_free(&ts);

    _ = ts_add(&ts, &.{ 3, 1 });
    _ = ts_add(&ts, &.{ 1, 2 });
    _ = ts_add(&ts, &.{ 2, 2 });
    ts_sort(&ts);

    try std.testing.expectEqual(@as(c_int, 0), rel_build_from_tupleset(r, &ts));
    // Merged final states (see roundtrip test above), not word count.
    try std.testing.expectEqual(@as(u64, 1), rel_count(r));
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(r, &.{ 3, 1 }));
    try std.testing.expectEqual(@as(c_int, 0), rel_is_idb(r)); // still aliased
}
