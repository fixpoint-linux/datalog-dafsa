//! reactive.zig — fired-event observation over the EXISTING delta dispatch
//! (Capability 2 slice R1: a NON-recursive trigger facility).
//!
//! A reactive rule set denotes the same model as today; what this module adds
//! is OBSERVATION: a caller marks which derived (rule-head) relations to
//! watch, commits deltas, then steps the engine and receives exactly the
//! head tuples that became newly true ("fired") — the edge-triggered view an
//! external driver currently re-derives by hand.
//!
//! ─── Mechanism ────────────────────────────────────────────────────────────
//! ZERO vm/compiler surgery (the plan's diff-capture design): a step
//!   1. snapshots the CURRENT content of every watched head view
//!      (rel_prefix over DAFSA ∪ overlay),
//!   2. runs the engine's OWN maintenance cascade — dl.zig's ONE
//!      consolidateInner (prologue + dispatch + epilogue) via
//!      dl.consolidateForReactive with an observation hook, so the step
//!      IS a full consolidation and the engine's choice among
//!      full-reeval / aggregate-maintenance / DRed / IVM is never
//!      re-decided here (one extra enumeration of the watched relations
//!      per step is the whole cost, the right trade for a scheduler
//!      facility),
//!   3. re-enumerates the watched views and reports ADDED tuples through the
//!      caller's dl_fired_cb, then tuples REMOVED by the same step.
//!
//! ─── Isolation / default-off ──────────────────────────────────────────────
//! State is keyed by the dl_db POINTER and only exists between
//! dl_fired_init and dl_fired_clear.  With no observation session open (the
//! default, and the state of every existing program) no reactive code runs
//! anywhere except one table lookup in dl_close's forget call: caller-visible
//! behaviour is bit-identical to today, which the unchanged suites gate.
//! Session shape: dl_fired_init -> dl_set_reactive* -> dl_fired_step* ->
//! dl_fired_clear.
//!
//! ─── R1 boundary (stated, not hidden) ─────────────────────────────────────
//! R1 covers edge-triggered ADDITIONS derived by the existing chain over an
//! IVM-eligible program (the vm_execute_ivm / vm_propagate_deltas / DRed
//! branches).  When a step falls outside that class — IVM/DRed both
//! ineligible (OP_WALK / OP_HASH_JOIN / …), ineligible aggregates, a pending
//! full re-eval, or a delete against recursive rules (DRed rejects
//! recursion) — the engine's cascade takes its vm_execute full-recompute
//! branch; the step still reports the CORRECT diff but ORs
//! DL_REACTIVE_FALLBACK into the returned count (never silent).  Removals
//! are reported as DL_REACTIVE_REMOVED events (a diff boundary: first-class
//! retraction FIRING is slice R2 — captureStep is structured so an R2
//! delta-capturing variant slots in beside it, not inside it).
//!
//! Reported events are RULE-DERIVED head changes: dl_set_reactive rejects a
//! non-head (EDB) relation loudly at set time — the plan's mitigation for
//! direct-EDB-write aliasing (a diff over a directly-written relation would
//! conflate caller writes with derived tuples).

const std = @import("std");
const c = std.c;

const tupleset = @import("tupleset.zig");
const dx = @import("dl_internal.zig");

// ─── Public C-ABI constants (mirrored in src/dl.h) ─────────────────────────

/// dl_fired_cb `event` values: the watched head's tuples ADDED by this step.
pub const DL_REACTIVE_ADDED: c_int = 0;
/// dl_fired_cb `event` value: tuples REMOVED by this step (delete-driven
/// step; R1 reports them as a diff — retraction firing is R2).
pub const DL_REACTIVE_REMOVED: c_int = 1;

/// OR-ed into dl_fired_step's returned count when the dispatch took the
/// engine's full re-evaluation fallback (the diff is still correct).
pub const DL_REACTIVE_FALLBACK: c_long = 0x40000000;

/// dl_fired_step / dl_fired_init / dl_fired_clear negative results
/// (distinct from a fired-tuple count, which is >= 0).
pub const DL_REACTIVE_ERR_ARGS: c_long = -1;
pub const DL_REACTIVE_ERR_NOT_INIT: c_long = -2;
pub const DL_REACTIVE_ERR_OOM: c_long = -3;
pub const DL_REACTIVE_ERR_DISPATCH: c_long = -4;
pub const DL_REACTIVE_ERR_CONFLICT: c_long = -5;
pub const DL_REACTIVE_ERR_INTERNAL: c_long = -6;

/// dl_fired_cb — event-streaming callback (dl.h).  Return non-zero to stop
/// the step early; the count returned by dl_fired_step covers only the
/// events already delivered.
pub const FiredCb = ?*const fn (event: c_int, cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int;

// ─── extern bindings (same discipline as wfs.zig) ──────────────────────────

// tupleset.zig export fns (not `pub`).
extern "c" fn ts_init(ts: ?*tupleset.tuple_set, arity: u8) c_int;
extern "c" fn ts_free(ts: ?*tupleset.tuple_set) void;
extern "c" fn ts_add(ts: ?*tupleset.tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_sort(ts: ?*tupleset.tuple_set) void;

// ─── unit tests ────────────────────────────────────────────────────────────
// reactive.zig's correctness-critical primitives, tested directly (the C
// suite exercises the whole step; these pin the pieces a diff is built of).

fn mk2(rows: []const [2]u32) !tupleset.tuple_set {
    var ts: tupleset.tuple_set = undefined;
    try std.testing.expect(ts_init(&ts, 2) == 0);
    // ts_add: 1 = inserted, 0 = duplicate (inputs here are distinct), -1 = error.
    for (rows) |r| try std.testing.expect(ts_add(&ts, &r) >= 0);
    ts_sort(&ts);
    return ts;
}

fn expectTs2(ts: *const tupleset.tuple_set, rows: []const [2]u32) !void {
    try std.testing.expectEqual(@as(c_long, @intCast(rows.len)), ts.count);
    try std.testing.expectEqual(@as(u8, 2), ts.arity);
    for (rows, 0..) |r, i| {
        const got = (ts.data.? + i * 2)[0..2];
        try std.testing.expectEqualSlices(u32, &r, got);
    }
}

test "tsMinus: set difference over sorted tuple sets" {
    // Disjoint sides: everything survives.
    {
        var a = try mk2(&.{ .{ 1, 2 }, .{ 3, 4 } });
        var b = try mk2(&.{.{ 9, 9 }});
        var d = tsMinus(&a, &b).?;
        try expectTs2(&d, &.{ .{ 1, 2 }, .{ 3, 4 } });
        ts_free(&d);
        ts_free(&b);
        ts_free(&a);
    }
    // Full overlap: nothing survives.
    {
        var a = try mk2(&.{ .{ 1, 2 }, .{ 3, 4 } });
        var b = try mk2(&.{ .{ 1, 2 }, .{ 3, 4 } });
        var d = tsMinus(&a, &b).?;
        try expectTs2(&d, &.{});
        ts_free(&d);
        ts_free(&b);
        ts_free(&a);
    }
    // Interleaved: first-difference compare must walk BOTH sides.
    {
        var a = try mk2(&.{ .{ 1, 2 }, .{ 2, 9 }, .{ 5, 5 }, .{ 7, 1 } });
        var b = try mk2(&.{ .{ 1, 2 }, .{ 3, 0 }, .{ 5, 6 }, .{ 7, 1 } });
        var d = tsMinus(&a, &b).?;
        try expectTs2(&d, &.{ .{ 2, 9 }, .{ 5, 5 } });
        ts_free(&d);
        // Asymmetry: the same pair the other way round keeps b's extras.
        var d2 = tsMinus(&b, &a).?;
        try expectTs2(&d2, &.{ .{ 3, 0 }, .{ 5, 6 } });
        ts_free(&d2);
        ts_free(&b);
        ts_free(&a);
    }
    // First-exhausts tail dump: b's lone tuple sorts before all of a's —
    // the walk runs off b, then must dump a's whole tail (the early-stop
    // path relies on this shape).
    {
        var a = try mk2(&.{ .{ 1, 1 }, .{ 2, 2 }, .{ 3, 3 }, .{ 4, 4 } });
        var b = try mk2(&.{.{ 0, 0 }});
        var d = tsMinus(&a, &b).?;
        try expectTs2(&d, &.{ .{ 1, 1 }, .{ 2, 2 }, .{ 3, 3 }, .{ 4, 4 } });
        ts_free(&d);
        // Mirror: b's (0,0) is absent from a, so it survives.
        var d2 = tsMinus(&b, &a).?;
        try expectTs2(&d2, &.{.{ 0, 0 }});
        ts_free(&d2);
        ts_free(&b);
        ts_free(&a);
    }
    // First exhausts INSIDE second (first is a mid-sorted subset): every
    // first-tuple is consumed by matches and the result is empty.
    {
        var sub = try mk2(&.{.{ 3, 3 }});
        var sup = try mk2(&.{ .{ 1, 1 }, .{ 2, 2 }, .{ 3, 3 }, .{ 4, 4 } });
        var d = tsMinus(&sub, &sup).?;
        try expectTs2(&d, &.{});
        ts_free(&d);
        ts_free(&sup);
        ts_free(&sub);
    }
    // Both empty.
    {
        var a = try mk2(&.{});
        var b = try mk2(&.{});
        var d = tsMinus(&a, &b).?;
        try expectTs2(&d, &.{});
        ts_free(&d);
        ts_free(&b);
        ts_free(&a);
    }
    // Lexicographic compare is COLUMN-wise on u32 values: (1, 0x20000000)
    // before (1, 0x90000000), difference keeps the low one.
    {
        var a = try mk2(&.{ .{ 1, 0x20000000 }, .{ 1, 0x90000000 } });
        var b = try mk2(&.{.{ 1, 0x90000000 }});
        var d = tsMinus(&a, &b).?;
        try expectTs2(&d, &.{.{ 1, 0x20000000 }});
        ts_free(&d);
        ts_free(&b);
        ts_free(&a);
    }
    // Arity mismatch never reaches tsMinus (both sides come from the SAME
    // relation); an arity-1 pair guards the single-column walk.
    {
        var a: tupleset.tuple_set = undefined;
        var b: tupleset.tuple_set = undefined;
        try std.testing.expect(ts_init(&a, 1) == 0);
        try std.testing.expect(ts_init(&b, 1) == 0);
        try std.testing.expect(ts_add(&a, &[_]u32{5}) == 1);
        try std.testing.expect(ts_add(&a, &[_]u32{7}) == 1);
        try std.testing.expect(ts_add(&b, &[_]u32{7}) == 1);
        ts_sort(&a);
        ts_sort(&b);
        var d = tsMinus(&a, &b).?;
        try std.testing.expectEqual(@as(c_long, 1), d.count);
        try std.testing.expectEqual(@as(u32, 5), d.data.?[0]);
        ts_free(&d);
        ts_free(&b);
        ts_free(&a);
    }
}

test "ABI constants round-trip the C header (dl.h)" {
    // dl.h is the ABI contract; these values are pinned by tests/test_reactive.c
    // AND by every caller that OR-tests the flag / switches on the errors.
    try std.testing.expectEqual(@as(c_long, 0x40000000), DL_REACTIVE_FALLBACK);
    try std.testing.expectEqual(@as(c_int, 0), DL_REACTIVE_ADDED);
    try std.testing.expectEqual(@as(c_int, 1), DL_REACTIVE_REMOVED);
    // Distinct error codes, none colliding with a count (>= 0) or the flag.
    const errs = [_]c_long{
        DL_REACTIVE_ERR_ARGS,     DL_REACTIVE_ERR_NOT_INIT, DL_REACTIVE_ERR_OOM,
        DL_REACTIVE_ERR_DISPATCH, DL_REACTIVE_ERR_CONFLICT, DL_REACTIVE_ERR_INTERNAL,
    };
    for (errs, 0..) |e, i| {
        try std.testing.expect(e < 0);
        for (errs[i + 1 ..]) |o| try std.testing.expect(e != o);
    }
    // The flag must not eat into the count bits a caller masks back out.
    try std.testing.expect(DL_REACTIVE_FALLBACK & 0x0FFFFFFF == 0);
    try std.testing.expect((DL_REACTIVE_FALLBACK | @as(c_long, 123)) & ~DL_REACTIVE_FALLBACK == 123);
}

// dl.zig provides consolidateForReactive (the shared consolidate cascade with
// an observation hook); the mutual import is lazy (each side only references
// the other's fn inside function bodies), matching how std breaks import
// cycles.
const dl = @import("dl.zig");

// relation.zig export fns against dx's opaque relation type.
extern "c" fn rel_prefix(
    rel: ?*const dx.relation,
    leading: [*c]const u32,
    k: u8,
    cb: dx.rel_enum_cb,
    user: ?*anyopaque,
) c_long;
extern "c" fn rel_arity(rel: ?*const dx.relation) u8;
extern "c" fn rel_is_idb(rel: ?*const dx.relation) c_int;

// ─── small helpers ─────────────────────────────────────────────────────────

fn strEq(a: [*c]const u8, b: [*c]const u8) bool {
    var i: usize = 0;
    while (a[i] != 0 and a[i] == b[i]) : (i += 1) {}
    return a[i] == b[i];
}

fn reactiveErr(comptime fmt: []const u8, args: anytype) void {
    var buf: [1024]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch buf[0..0];
    _ = c.write(2, msg.ptr, msg.len);
}

/// Snapshot one relation's view (DAFSA ∪ overlay) into a fresh SORTED
/// tuple_set.  Caller frees.
fn snapshotRel(rel: ?*const dx.relation) ?tupleset.tuple_set {
    var ts: tupleset.tuple_set = undefined;
    if (ts_init(&ts, rel_arity(rel)) != 0) return null;
    if (rel_prefix(rel, null, 0, tsSink, &ts) < 0) {
        ts_free(&ts);
        return null;
    }
    ts_sort(&ts);
    return ts;
}

fn tsSink(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ts: *tupleset.tuple_set = @ptrCast(@alignCast(user orelse return -1));
    return if (ts_add(ts, cols) < 0) -1 else 0;
}

/// `first` ⊖ `second`: tuples of `first` absent from `second` (both sorted,
/// same arity).  Fresh sorted ts; caller frees.  null on OOM.
fn tsMinus(first: *const tupleset.tuple_set, second: *const tupleset.tuple_set) ?tupleset.tuple_set {
    var out: tupleset.tuple_set = undefined;
    if (ts_init(&out, first.arity) != 0) return null;
    var fi: c_long = 0;
    var si: c_long = 0;
    const ar = first.arity;
    while (fi < first.count and si < second.count) {
        const ft = first.data.? + @as(usize, @intCast(fi)) * ar;
        const st = second.data.? + @as(usize, @intCast(si)) * ar;
        var k: usize = 0;
        while (k < ar and ft[k] == st[k]) k += 1;
        if (k == ar) {
            fi += 1;
            si += 1;
        } else if (ft[k] < st[k]) {
            if (ts_add(&out, ft) < 0) {
                ts_free(&out);
                return null;
            }
            fi += 1;
        } else {
            si += 1;
        }
    }
    while (fi < first.count) : (fi += 1) {
        if (ts_add(&out, first.data.? + @as(usize, @intCast(fi)) * ar) < 0) {
            ts_free(&out);
            return null;
        }
    }
    ts_sort(&out);
    return out;
}

// ─── per-db reactive state (ptr-keyed; alive only between init/clear) ──────

// dx.dl_db.rels is [64] (MAX_RELS).
const MAX_WATCHED: usize = 64;

const Watch = struct {
    rel_id: usize,
    before: tupleset.tuple_set, // snapshot from init (or the previous step)
};

const ReactiveState = struct {
    db: *dx.dl_db,
    watch: [MAX_WATCHED]?Watch = [_]?Watch{null} ** MAX_WATCHED,
    n_watch: usize = 0,
    fallback: bool = false, // last step's dispatch used full re-eval
};

// Session table: a linear array of db-pointer keys, scanned linearly.
// Sessions are per-process FEW (one per live db being observed), so a scan
// is cheap — and deletion (swap-with-last or null-out) can NEVER disturb
// another db's entry, which an open-addressing table without tombstones
// silently could (a nulled slot mid-probe-run broke probe4: clearing one
// db's session made sibling sessions unreachable AND leaked them).  Linear
// keys also survive key deletion by construction; dl_close's reactiveForget
// removes a freed pointer so a reused address cannot alias a stale session.
// NOT thread-safe: reactive sessions are single-threaded per process (one
// thread may observe its own db; two threads observing two dbs must
// serialize the session calls externally).  Stated here, not hidden.
var tbl: [MAX_SESSIONS]?*ReactiveState = [_]?*ReactiveState{null} ** MAX_SESSIONS;
const MAX_SESSIONS: usize = 64;

fn stateFor(db: *dx.dl_db) ?*ReactiveState {
    for (&tbl) |e| {
        const st = e orelse continue;
        if (st.db == db) return st;
    }
    return null;
}

fn stateInsert(st: *ReactiveState) bool {
    for (&tbl) |*e| {
        if (e.* == null) {
            e.* = st;
            return true;
        }
    }
    return false; // table full (MAX_SESSIONS live sessions)
}

fn stateTake(db: *dx.dl_db) ?*ReactiveState {
    for (&tbl, 0..) |e, i| {
        const st = e orelse continue;
        if (st.db != db) continue;
        tbl[i] = null;
        return st;
    }
    return null;
}

/// Free the session for `db` if one is open (dl_close safety: never leave a
/// freed db pointer keyed).
pub fn reactiveForget(db: *dx.dl_db) void {
    if (stateTake(db)) |st| {
        freeWatched(st);
        c.free(st);
    }
}

fn freeWatched(st: *ReactiveState) void {
    var i: usize = 0;
    while (i < MAX_WATCHED) : (i += 1) {
        if (st.watch[i]) |*w| ts_free(&w.before);
        st.watch[i] = null;
    }
    st.n_watch = 0;
}

/// Resolve a watched entry's live relation from the CURRENT db layout.
/// Relations are append-only (dl_declare_relation never moves earlier
/// entries), so rel_id stays valid for the session.
fn watchedRel(st: *ReactiveState, w: *Watch) ?*dx.relation {
    if (w.rel_id >= st.db.nrels) return null;
    return st.db.rels[w.rel_id].rel;
}

/// Is rel_id the head of at least one loaded rule?  (dx.dl_db.crules holds
/// the compiled rules; head_rel_id is the relation index.)
fn isRuleHeadRel(d: *const dx.dl_db, rel_id: usize) bool {
    var i: c_int = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        if (@as(usize, cc.*.head_rel_id) == rel_id) return true;
    }
    return false;
}

// ─── the capture step ──────────────────────────────────────────────────────

fn captureStep(st: *ReactiveState, cb: FiredCb, user: ?*anyopaque) c_long {
    st.fallback = false;
    // The EXISTING dispatch cascade, executed by dl.zig — reactive.zig never
    // re-decides an evaluation strategy.  This is the SAME cascade (prologue
    // + dispatch + epilogue) dl_consolidate runs, with the observation hook
    // armed; there is and shall be no second copy of it anywhere.
    var cw: dl.CascadeWatch = .{};
    const rc = dl.consolidateForReactive(@ptrCast(st.db), &cw);
    if (rc == -2) {
        st.fallback = true; // the cascade fell back to full re-evaluation
    } else if (rc != 0) {
        return DL_REACTIVE_ERR_DISPATCH;
    }

    var fired: c_long = 0;
    var i: usize = 0;
    while (i < MAX_WATCHED) : (i += 1) {
        const w = &(st.watch[i] orelse continue);
        const rel = watchedRel(st, w) orelse {
            // Unreachable: rel_id indexed a relation that existed at
            // registration and declaration is append-only.
            reactiveErr("dl_fired_step: watched relation vanished — internal error\n", .{});
            return DL_REACTIVE_ERR_INTERNAL;
        };
        var after_o: ?tupleset.tuple_set = snapshotRel(rel) orelse return DL_REACTIVE_ERR_OOM;
        const after = &after_o.?;

        // Newly true = after ⊖ before.
        var added_o: ?tupleset.tuple_set = tsMinus(after, &w.before);
        if (added_o == null) {
            ts_free(after);
            return DL_REACTIVE_ERR_OOM;
        }
        const added = &added_o.?;
        if (added.count > 0) {
            const em = emitTuples(cb, DL_REACTIVE_ADDED, added, user);
            fired += em.delivered;
            if (em.aborted) {
                ts_free(added);
                ts_free(&w.before); // the baseline advances to after: free the OLD one first
                w.before = after_o.?;
                return fired; // callback aborted the step
            }
        }
        ts_free(added);

        // R1 boundary: a delete-driven step reports disappearances as
        // REMOVED events; first-class retraction firing is slice R2.
        var removed_o: ?tupleset.tuple_set = tsMinus(&w.before, after);
        if (removed_o == null) {
            ts_free(after);
            return DL_REACTIVE_ERR_OOM;
        }
        const removed = &removed_o.?;
        if (removed.count > 0) {
            const em = emitTuples(cb, DL_REACTIVE_REMOVED, removed, user);
            fired += em.delivered;
            if (em.aborted) {
                ts_free(removed);
                ts_free(&w.before); // the baseline advances to after: free the OLD one first
                w.before = after_o.?;
                return fired; // callback aborted the step
            }
        }
        ts_free(removed);
        ts_free(&w.before);
        w.before = after_o.?; // becomes the next step's baseline
    }
    return fired;
}

/// Deliver every tuple of `ts` as one event kind.  Returns how many tuples
/// were delivered and whether the callback stopped the stream early (the
/// caller still counts the delivered ones — the step's returned count must
/// cover every event already delivered, per the dl.h contract).
const EmitResult = struct { delivered: c_long, aborted: bool };

fn emitTuples(cb: FiredCb, event: c_int, ts: *const tupleset.tuple_set, user: ?*anyopaque) EmitResult {
    const f = cb orelse return .{ .delivered = 0, .aborted = false };
    var i: c_long = 0;
    while (i < ts.count) : (i += 1) {
        const cols = ts.data.? + @as(usize, @intCast(i)) * @as(usize, @intCast(ts.arity));
        if (f(event, cols, @intCast(ts.arity), user) != 0)
            // The aborting event WAS delivered (the callback saw it and
            // stopped after processing it) — the count must cover it.
            return .{ .delivered = i + 1, .aborted = true };
    }
    return .{ .delivered = ts.count, .aborted = false };
}

// ─── C entry points (exported from dl.zig; declared in src/dl.h) ───────────

// rel_entry.kind value (dl_internal.zig RELK_VARIADIC); the C layout stores
// it as u8, so compare against this rather than the c_int constant.
const RELK_VARIADIC: u8 = 1;

/// int dl_set_reactive(dl_db *db, const char *rel_name, int on)
pub fn dlSetReactive(db: ?*dx.dl_db, rel_name: [*c]const u8, on: c_int) c_int {
    const d = db orelse return -1;
    if (rel_name == null) return -1;
    if (d.read_only != 0) return -1;

    const st = stateFor(d) orelse {
        // Watches are a property of the observation session (they carry the
        // "before" baseline); register after dl_fired_init.
        reactiveErr("dl_set_reactive: no observation session — call dl_fired_init first\n", .{});
        return -1;
    };

    var i: usize = 0;
    while (i < d.nrels and i < MAX_WATCHED) : (i += 1) {
        if (!strEq(d.rels[i].name, rel_name)) continue;
        // A variadic relation's rel_entry.rel is null by construction
        // (dl.zig declares it that way), so arming one would snapshot an
        // arity-0 empty set and fail with a bare -1.  R1 does not support
        // variadic heads — refuse LOUDLY, distinct from the EDB refusal.
        if (d.rels[i].kind == RELK_VARIADIC) {
            reactiveErr("dl_set_reactive: '{s}' is variadic — R1 does not support variadic heads\n", .{rel_name});
            return -1;
        }
        // EDB heads are rejected loudly: a diff over a directly-written
        // relation would conflate direct writes with derived tuples
        // (plan risk R1).  Accept a relation that is ALREADY a derived view
        // or is the head of a loaded rule (pre-compile registration).
        if (rel_is_idb(d.rels[i].rel) == 0 and !isRuleHeadRel(d, i)) {
            reactiveErr("dl_set_reactive: '{s}' is not a rule head (EDB) — refusing\n", .{rel_name});
            return -1;
        }
        if (on == 0) {
            if (st.watch[i]) |*w| {
                ts_free(&w.before);
                st.watch[i] = null;
                st.n_watch -= 1;
            }
            return 0; // idempotent disarm
        }
        if (st.watch[i] != null) return 0; // idempotent re-arm
        const before = snapshotRel(d.rels[i].rel) orelse return -1;
        st.watch[i] = .{ .rel_id = i, .before = before };
        st.n_watch += 1;
        return 0;
    }
    reactiveErr("dl_set_reactive: unknown relation '{s}'\n", .{rel_name});
    return -1;
}

/// long dl_fired_init(dl_db *db)
pub fn dlFiredInit(db: ?*dx.dl_db) c_long {
    const d = db orelse return DL_REACTIVE_ERR_ARGS;
    if (d.read_only != 0) return DL_REACTIVE_ERR_ARGS;
    if (stateFor(d) != null) return DL_REACTIVE_ERR_CONFLICT; // session already open

    const mem = c.calloc(1, @sizeOf(ReactiveState)) orelse return DL_REACTIVE_ERR_OOM;
    const st: *ReactiveState = @ptrCast(@alignCast(mem));
    st.db = d;
    if (!stateInsert(st)) {
        c.free(st);
        return DL_REACTIVE_ERR_CONFLICT; // session table full (MAX_SESSIONS)
    }
    return 0;
}

/// long dl_fired_step(dl_db *db, dl_fired_cb cb, void *user)
pub fn dlFiredStep(db: ?*dx.dl_db, cb: FiredCb, user: ?*anyopaque) c_long {
    const d = db orelse return DL_REACTIVE_ERR_ARGS;
    if (cb == null) return DL_REACTIVE_ERR_ARGS;
    const st = stateFor(d) orelse return DL_REACTIVE_ERR_NOT_INIT;

    const fired = captureStep(st, cb, user);
    if (fired < 0) return fired;
    return if (st.fallback) fired | DL_REACTIVE_FALLBACK else fired;
}

/// long dl_fired_clear(dl_db *db)
pub fn dlFiredClear(db: ?*dx.dl_db) c_long {
    const d = db orelse return DL_REACTIVE_ERR_ARGS;
    const st = stateTake(d) orelse return DL_REACTIVE_ERR_NOT_INIT;
    freeWatched(st);
    c.free(st);
    return 0;
}
