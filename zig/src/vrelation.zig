//! vrelation.zig — port of src/vrelation.c (variable-arity relation dispatch).
//!
//! A variadic relation is a family of fixed-width `relation` variants, one per
//! arity a in [1..MAX_VAR_ARITY].  Every operation delegates VERBATIM to the
//! fixed-width relation.c code on the chosen variant — this file contains no
//! DAFSA logic of its own.
//!
//! Strangler-hybrid ABI: `struct vrelation` is OPAQUE in vrelation.h, so the
//! implementation is a native Zig struct; every non-static C function is an
//! `export fn` with the exact C name/signature/return semantics.  Dispatch
//! calls go through the relation.zig `pub export fn`s.
//!
//! Oracle: src/vrelation.c (never modified).

const std = @import("std");
const c = std.c;
const relation = @import("relation.zig");

const MAX_VAR_ARITY: usize = 8;

/// struct vrelation — opaque to C; native Zig layout.
pub const Vrelation = struct {
    variants: [MAX_VAR_ARITY + 1]?*relation.Relation, // [0] unused
};

/// typedef void (*vrel_iter_cb)(relation *variant, uint8_t arity, void *user)
const VrelIterCb = ?*const fn (variant: ?*relation.Relation, arity: u8, user: ?*anyopaque) callconv(.c) void;

/// vrelation *vrel_create(void)
pub export fn vrel_create() ?*Vrelation {
    const mem = c.calloc(1, @sizeOf(Vrelation)) orelse return null;
    const v: *Vrelation = @ptrCast(@alignCast(mem));
    v.* = std.mem.zeroes(Vrelation);
    return v;
}

/// void vrel_free(vrelation *v)
pub export fn vrel_free(v: ?*Vrelation) void {
    const vv = v orelse return;
    var a: u8 = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        relation.rel_free(vv.variants[a]);
    }
    c.free(@ptrCast(vv));
}

/// relation *vrel_variant_or_null(const vrelation *v, uint8_t arity)
pub export fn vrel_variant_or_null(v: ?*const Vrelation, arity: u8) ?*relation.Relation {
    const vv = v orelse return null;
    if (arity == 0 or arity > MAX_VAR_ARITY) return null;
    return vv.variants[arity];
}

/// relation *vrel_variant(vrelation *v, uint8_t arity)
pub export fn vrel_variant(v: ?*Vrelation, arity: u8) ?*relation.Relation {
    const vv = v orelse return null;
    if (arity == 0 or arity > MAX_VAR_ARITY) return null;
    if (vv.variants[arity] == null) {
        vv.variants[arity] = relation.rel_create(arity);
        // On OOM the slot stays NULL and the caller sees failure.
    }
    return vv.variants[arity];
}

/// int vrel_attach(vrelation *v, uint8_t arity, relation *r)
pub export fn vrel_attach(v: ?*Vrelation, arity: u8, r: ?*relation.Relation) c_int {
    const vv = v orelse return -1;
    if (r == null or arity == 0 or arity > MAX_VAR_ARITY) return -1;
    if (vv.variants[arity] != null) return -1; // already present
    vv.variants[arity] = r;
    return 0;
}

/// void vrel_foreach(vrelation *v, vrel_iter_cb cb, void *user)
pub export fn vrel_foreach(v: ?*Vrelation, cb: VrelIterCb, user: ?*anyopaque) void {
    const vv = v orelse return;
    if (cb == null) return;
    var a: u8 = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        if (vv.variants[a] != null)
            cb.?(vv.variants[a], a, user);
    }
}

/// int vrel_any_idb(const vrelation *v)
pub export fn vrel_any_idb(v: ?*const Vrelation) c_int {
    const vv = v orelse return 0;
    var a: u8 = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        if (vv.variants[a] != null and relation.rel_is_idb(vv.variants[a]) != 0)
            return 1;
    }
    return 0;
}

/// int vrel_reset_views(vrelation *v)
pub export fn vrel_reset_views(v: ?*Vrelation) c_int {
    const vv = v orelse return -1;
    var a: u8 = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        if (vv.variants[a] == null) continue;
        if (relation.rel_reset_view(vv.variants[a]) != 0)
            return -1; // earlier variants stay reset; loud failure
    }
    return 0;
}

/// int vrel_exact(const vrelation *v, const uint32_t *cols, uint8_t arity)
pub export fn vrel_exact(v: ?*const Vrelation, cols: ?[*]const u32, arity: u8) c_int {
    const r = vrel_variant_or_null(v, arity);
    return if (r != null) relation.rel_exact(r, cols) else 0;
}

/// int vrel_exact_base(const vrelation *v, const uint32_t *cols, uint8_t arity)
pub export fn vrel_exact_base(v: ?*const Vrelation, cols: ?[*]const u32, arity: u8) c_int {
    const r = vrel_variant_or_null(v, arity);
    return if (r != null) relation.rel_exact_base(r, cols) else 0;
}

/// Shared fan-out: rel_prefix or rel_prefix_base over every present variant
/// a >= max(k,1).  Early-stop semantics best-effort (keep walking, total sums).
fn vrelPrefixImpl(v: *const Vrelation, leading: ?[*]const u32, k: u8, use_base: c_int, cb: relation.RelEnumCb, user: ?*anyopaque) c_long {
    if (cb == null) return -1;
    if (k > MAX_VAR_ARITY) return -1;
    if (k > 0 and leading == null) return -1;

    var total: c_long = 0;
    var a: u8 = if (k > 0) k else 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        const r = v.variants[a];
        if (r == null) continue;
        const n = if (use_base != 0)
            relation.rel_prefix_base(r, leading, k, cb, user)
        else
            relation.rel_prefix(r, leading, k, cb, user);
        if (n < 0) return -1;
        total += n;
    }
    return total;
}

/// long vrel_prefix(const vrelation *v, const uint32_t *leading, uint8_t k,
///                  rel_enum_cb cb, void *user)
pub export fn vrel_prefix(v: ?*const Vrelation, leading: ?[*]const u32, k: u8, cb: relation.RelEnumCb, user: ?*anyopaque) c_long {
    const vv = v orelse return -1;
    return vrelPrefixImpl(vv, leading, k, 0, cb, user);
}

/// long vrel_prefix_base(const vrelation *v, const uint32_t *leading, uint8_t k,
///                       rel_enum_cb cb, void *user)
pub export fn vrel_prefix_base(v: ?*const Vrelation, leading: ?[*]const u32, k: u8, cb: relation.RelEnumCb, user: ?*anyopaque) c_long {
    const vv = v orelse return -1;
    return vrelPrefixImpl(vv, leading, k, 1, cb, user);
}

/// long vrel_pattern(const vrelation *v, const struct regex_dfa *dfa,
///                   rel_enum_cb cb, void *user)
pub export fn vrel_pattern(v: ?*const Vrelation, dfa: [*c]const relation.regex_dfa, cb: relation.RelEnumCb, user: ?*anyopaque) c_long {
    const vv = v orelse return -1;
    if (dfa == null or cb == null) return -1;

    var total: c_long = 0;
    var a: u8 = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        const r = vv.variants[a];
        if (r == null) continue;
        const n = relation.rel_pattern(r, dfa, cb, user);
        if (n < 0) return -1;
        total += n;
    }
    return total;
}

/// long vrel_filter_col(const vrelation *v, uint8_t col, const struct sym_set *set,
///                      rel_enum_cb cb, void *user)
pub export fn vrel_filter_col(v: ?*const Vrelation, col: u8, set: [*c]const relation.sym_set, cb: relation.RelEnumCb, user: ?*anyopaque) c_long {
    const vv = v orelse return -1;
    if (set == null or cb == null) return -1;

    var total: c_long = 0;
    var a: u8 = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        const r = vv.variants[a];
        if (r == null) continue;
        // Only filter if the variant has enough columns
        if (col < relation.rel_arity(r)) {
            const n = relation.rel_filter_col(r, col, set, cb, user);
            if (n < 0) return -1;
            total += n;
        }
    }
    return total;
}

/// uint64_t vrel_count(const vrelation *v)
pub export fn vrel_count(v: ?*const Vrelation) u64 {
    const vv = v orelse return 0;
    var total: u64 = 0;
    var a: u8 = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        if (vv.variants[a] != null)
            total += relation.rel_count(vv.variants[a]);
    }
    return total;
}

// ─── Tests ────────────────────────────────────────────────────────────────

fn countCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = cols;
    _ = arity;
    const counter: *c_long = @ptrCast(@alignCast(user orelse return 0));
    counter.* +%= 1;
    return 0;
}

test "vrel variant get-or-create + exact + prefix + count" {
    const v = vrel_create() orelse return error.OutOfMemory;
    defer vrel_free(v);

    try std.testing.expect(vrel_variant_or_null(v, 2) == null);
    const r2 = vrel_variant(v, 2) orelse return error.OutOfMemory;
    try std.testing.expectEqual(@as(c_int, 1), relation.rel_add(r2, &.{ 1, 2 }));
    try std.testing.expectEqual(@as(c_int, 1), relation.rel_add(r2, &.{ 1, 3 }));

    const r1 = vrel_variant(v, 1) orelse return error.OutOfMemory;
    try std.testing.expectEqual(@as(c_int, 1), relation.rel_add(r1, &.{5}));

    try std.testing.expectEqual(@as(c_int, 1), vrel_exact(v, &.{ 1, 2 }, 2));
    try std.testing.expectEqual(@as(c_int, 0), vrel_exact(v, &.{ 1, 2 }, 3)); // absent variant
    try std.testing.expectEqual(@as(c_int, 1), vrel_exact(v, &.{5}, 1));

    // Sum of variant n_final (merged final states): arity-2 {(1,2),(1,3)}
    // collapses to 1, arity-1 {(5)} is 1 — oracle-verified total 2, while
    // vrel_prefix below still enumerates 3 words.
    try std.testing.expectEqual(@as(u64, 2), vrel_count(v));

    var count: c_long = 0;
    // k=0 fans out over both variants: 2 tuples in the arity-2 + 1 in arity-1.
    try std.testing.expectEqual(@as(c_long, 3), vrel_prefix(v, null, 0, countCb, &count));
    try std.testing.expectEqual(@as(c_long, 3), count);
    count = 0;
    // leading={1},k=1 matches only the arity-2 tuples (1,2),(1,3).
    try std.testing.expectEqual(@as(c_long, 2), vrel_prefix(v, &.{1}, 1, countCb, &count));
    try std.testing.expectEqual(@as(c_long, 2), count);

    try std.testing.expectEqual(@as(c_int, 0), vrel_any_idb(v));
    try std.testing.expectEqual(@as(c_int, 0), vrel_reset_views(v));
    try std.testing.expectEqual(@as(c_int, 1), vrel_any_idb(v));
}
