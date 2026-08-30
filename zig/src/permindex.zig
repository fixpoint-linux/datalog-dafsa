//! permindex.zig — port of src/permindex.c (permutation index builder, M6).
//!
//! Builds perm-π DAFSA indices from base relation facts.  Each perm index
//! re-encodes tuples: c_{π(0)} c_{π(1)} ... c_{π(a-1)} \0 so a join on
//! original columns {π(0)..π(k-1)} becomes a leading-k prefix lookup.
//!
//! Strangler-hybrid ABI: `struct dl_db` and `perm_index_entry` are EXPOSED
//! concrete structs still dereferenced by still-C dl.c — consumed via
//! @cImport("dl_internal.h"), NOT redefined here.  db_rel_at_arity_ro stays
//! in still-C dl.c and is reached through the same @cImport's extern decl.
//! relation*/tuple_set pointers cross into the ported relation.zig /
//! tupleset.zig via @ptrCast (opaque-C-layout <-> native-Zig-struct).

const std = @import("std");
const c = std.c;

const relation = @import("relation.zig");
const tupleset = @import("tupleset.zig");

// dl_internal.h: dl_db, perm_index_entry (via permindex.h), db_rel_at_arity_ro.
const dx = @cImport({
    @cInclude("dl_internal.h");
});

// ts_* are `export fn`s in tupleset.zig (C ABI); link against them directly.
extern "c" fn ts_init(ts: ?*tupleset.tuple_set, arity: u8) c_int;
extern "c" fn ts_free(ts: ?*tupleset.tuple_set) void;
extern "c" fn ts_add(ts: ?*tupleset.tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_sort(ts: ?*tupleset.tuple_set) void;

// ─── Build a single permutation index ─────────────────────────────────────

/// int permindex_build(dl_db *db, int rel_id, int perm_id)
pub export fn permindex_build(db: ?*dx.dl_db, rel_id: c_int, perm_id: c_int) c_int {
    const d = db orelse return -1;
    if (perm_id < 0 or perm_id >= d.n_perms) return -1;
    const pe: *dx.perm_index_entry = &d.perms[@intCast(perm_id)];
    if (pe.*.rel_id != rel_id) return -1;

    // v2: a perm index is per-arity-variant by construction (dl_db_declare_
    // perm keys on rel_id + arity + perm, and every atom's nargs is its
    // static arity) — resolve the VARIANT the perm belongs to.
    const base_rel_c = dx.db_rel_at_arity_ro(db, rel_id, pe.*.arity);
    if (base_rel_c == null) {
        // Fixed relation of a different arity, or a variadic variant that
        // does not exist: build an EMPTY index (an absent variant reads as
        // an empty relation everywhere else too).
        if (pe.*.pidx_rel) |old| relation.rel_free(@ptrCast(@alignCast(old)));
        pe.*.pidx_rel = @ptrCast(relation.rel_create(pe.*.arity));
        pe.*.dirty = 0;
        return if (pe.*.pidx_rel != null) 0 else -1;
    }
    const base_rel: *const relation.Relation = @ptrCast(@alignCast(base_rel_c));

    const ar: u8 = pe.*.arity;

    // Collect base facts
    var ts: tupleset.tuple_set = undefined;
    if (ts_init(&ts, ar) != 0) return -1;

    if (relation.rel_prefix(base_rel, null, 0, relation.ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        return -1;
    }

    if (ts.count == 0) {
        // Empty relation: create empty perm index
        if (pe.*.pidx_rel) |old| relation.rel_free(@ptrCast(@alignCast(old)));
        pe.*.pidx_rel = @ptrCast(relation.rel_create(ar));
        pe.*.dirty = 0;
        ts_free(&ts);
        return 0;
    }

    // Re-encode each tuple via permutation
    {
        var pts: tupleset.tuple_set = undefined;

        if (ts_init(&pts, ar) != 0) {
            ts_free(&ts);
            return -1;
        }

        var ci: c_long = 0;
        while (ci < ts.count) : (ci += 1) {
            const row = ts.data.? + @as(usize, @intCast(ci)) * @as(usize, ar);
            var prow: [8]u32 = undefined;
            var j: usize = 0;
            while (j < ar) : (j += 1)
                prow[j] = row[pe.*.perm[j]];
            _ = ts_add(&pts, &prow);
        }

        // Sort and bulk-build minimal DAFSA
        ts_sort(&pts);

        if (pe.*.pidx_rel) |old| relation.rel_free(@ptrCast(@alignCast(old)));

        pe.*.pidx_rel = @ptrCast(relation.rel_create(ar));
        if (pe.*.pidx_rel == null) {
            ts_free(&pts);
            ts_free(&ts);
            return -1;
        }

        if (relation.rel_build_from_tupleset(@ptrCast(@alignCast(pe.*.pidx_rel)), &pts) != 0) {
            relation.rel_free(@ptrCast(@alignCast(pe.*.pidx_rel)));
            pe.*.pidx_rel = null;
            ts_free(&pts);
            ts_free(&ts);
            return -1;
        }

        ts_free(&pts);
    }

    ts_free(&ts);
    pe.*.dirty = 0;
    return 0;
}

// ─── Build all dirty permutation indices ──────────────────────────────────

/// int permindex_build_dirty(struct dl_db *db)
pub export fn permindex_build_dirty(db: ?*dx.dl_db) c_int {
    const d = db orelse return 0;

    var i: c_int = 0;
    while (i < d.n_perms) : (i += 1) {
        if (d.perms[@intCast(i)].dirty != 0) {
            if (permindex_build(d, d.perms[@intCast(i)].rel_id, i) != 0)
                return -1;
        }
    }
    return 0;
}

// ─── Mark permutation indices dirty for a relation ────────────────────────

/// void permindex_mark_dirty(struct dl_db *db, int rel_id)
pub export fn permindex_mark_dirty(db: ?*dx.dl_db, rel_id: c_int) void {
    const d = db orelse return;

    var pi: c_int = 0;
    while (pi < d.n_perms) : (pi += 1) {
        if (d.perms[@intCast(pi)].rel_id == rel_id)
            d.perms[@intCast(pi)].dirty = 1;
    }
}

// ─── Free all permutation index relations ─────────────────────────────────

/// void permindex_free_all(struct dl_db *db)
pub export fn permindex_free_all(db: ?*dx.dl_db) void {
    const d = db orelse return;

    var i: c_int = 0;
    while (i < d.n_perms) : (i += 1) {
        if (d.perms[@intCast(i)].pidx_rel) |rel| {
            relation.rel_free(@ptrCast(@alignCast(rel)));
            d.perms[@intCast(i)].pidx_rel = null;
        }
    }
}

// ─── tests ────────────────────────────────────────────────────────────────

const testing = std.testing;

const CollectCtx = struct {
    rows: [64][8]u32 = undefined,
    n: usize = 0,

    fn cb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
        _ = arity;
        const self: *CollectCtx = @ptrCast(@alignCast(user.?));
        self.rows[self.n][0] = cols[0];
        self.rows[self.n][1] = cols[1];
        self.n += 1;
        return 0;
    }
};

test "permindex build/mark_dirty/build_dirty/free_all over a permuted base" {
    var db = std.mem.zeroes(dx.dl_db);

    // One fixed arity-2 relation "edge" {(1,2),(1,3),(2,4)}.
    var name_buf = "edge".*;
    const r = relation.rel_create(2) orelse return error.TestUnexpectedResult;
    const t1 = [2]u32{ 1, 2 };
    const t2 = [2]u32{ 1, 3 };
    const t3 = [2]u32{ 2, 4 };
    try testing.expect(relation.rel_add(r, &t1) > 0); // 1 = inserted
    try testing.expect(relation.rel_add(r, &t2) > 0);
    try testing.expect(relation.rel_add(r, &t3) > 0);

    db.rels[0].name = @ptrCast(&name_buf);
    db.rels[0].kind = dx.RELK_FIXED;
    db.rels[0].arity = 2;
    db.rels[0].rel = @ptrCast(r);
    db.nrels = 1;

    // perm index swapping the two columns: prow[j] = row[perm[j]]
    const perm = [8]u8{ 1, 0, 0, 0, 0, 0, 0, 0 };
    db.perms[0].rel_id = 0;
    db.perms[0].arity = 2;
    db.perms[0].perm = perm;
    db.perms[0].dirty = 1;
    db.n_perms = 1;

    // bounds / rel mismatch
    try testing.expectEqual(@as(c_int, -1), permindex_build(&db, 0, -1));
    try testing.expectEqual(@as(c_int, -1), permindex_build(&db, 0, 1)); // >= n_perms
    try testing.expectEqual(@as(c_int, -1), permindex_build(&db, 5, 0)); // rel_id mismatch

    try testing.expectEqual(@as(c_int, 0), permindex_build(&db, 0, 0));
    try testing.expectEqual(@as(c_int, 0), db.perms[0].dirty); // cleared by build
    try testing.expect(db.perms[0].pidx_rel != null);

    // permuted contents: (2,1),(3,1),(4,2) in sorted order
    var got = CollectCtx{};
    const pidx: *const relation.Relation = @ptrCast(@alignCast(db.perms[0].pidx_rel));
    try testing.expectEqual(@as(c_long, 3), relation.rel_prefix(pidx, null, 0, CollectCtx.cb, &got));
    try testing.expectEqual(@as(usize, 3), got.n);
    try testing.expectEqual(@as(u32, 2), got.rows[0][0]);
    try testing.expectEqual(@as(u32, 1), got.rows[0][1]);
    try testing.expectEqual(@as(u32, 3), got.rows[1][0]);
    try testing.expectEqual(@as(u32, 4), got.rows[2][0]); // (4,2)
    try testing.expectEqual(@as(u32, 2), got.rows[2][1]);

    // mark_dirty + build_dirty
    permindex_mark_dirty(&db, 0);
    try testing.expectEqual(@as(c_int, 1), db.perms[0].dirty);
    permindex_mark_dirty(&db, 9); // no such rel: no-op
    try testing.expectEqual(@as(c_int, 1), db.perms[0].dirty);
    try testing.expectEqual(@as(c_int, 0), permindex_build_dirty(&db));
    try testing.expectEqual(@as(c_int, 0), db.perms[0].dirty);

    // perm against an ABSENT variant (arity 5 has no relation): empty index
    db.perms[1].rel_id = 0;
    db.perms[1].arity = 5;
    db.perms[1].perm = perm;
    db.perms[1].dirty = 1;
    db.n_perms = 2;
    try testing.expectEqual(@as(c_int, 0), permindex_build(&db, 0, 1));
    try testing.expect(db.perms[1].pidx_rel != null);
    var got_empty = CollectCtx{};
    const pidx5: *const relation.Relation = @ptrCast(@alignCast(db.perms[1].pidx_rel));
    try testing.expectEqual(@as(c_long, 0), relation.rel_prefix(pidx5, null, 0, CollectCtx.cb, &got_empty));

    permindex_free_all(&db);
    try testing.expect(db.perms[0].pidx_rel == null);
    try testing.expect(db.perms[1].pidx_rel == null);
    permindex_free_all(&db); // idempotent
}
