//! wfs.zig — well-founded semantics (van Gelder's alternating fixpoint) as
//! a per-query evaluation strategy.
//!
//! This is the evaluation regime beside the bottom-up semi-naive fixpoint
//! (vm.zig) for programs the stratified fixpoint cannot express.
//! It exists because the existing drivers only evaluate programs whose
//! dependency graph stratifies: the compiler rejects a rule whose body
//! negates a predicate of its own recursion cycle ("unstratifiable
//! program", compiler.zig:1048/1365/1384).  The canonical case is
//!     win(X) :- move(X,Y), !win(Y).
//! whose meaning requires the 3-valued well-founded partial model.
//!
//! NULL-ISOLATION: nothing here changes any existing path.  The driver is
//! reachable ONLY through the exported entry dl_query_wfs_ro (dl.zig);
//! every other entry (dl_load_rules/dl_compile/dl_query_*)
//! keeps rejecting unstratifiable programs exactly as before — the
//! stratifier itself is untouched.
//!
//! ─── Stratified fast path ──────────────────────────────────────────────────
//! The alternating fixpoint is only needed for programs the stratifier cannot
//! order.  After the S1 subset/scope gate, the driver therefore probes the
//! ORIGINAL program with compiler.programStratifies (the compiler's own
//! stratifier, diagnosed silently): when it stratifies, the program is
//! evaluated by ONE ordinary semi-naive fixpoint (compile + vm_execute on the
//! same eval clone, exactly dl_query_rules_ro's shape) and the goal's TRUE
//! atoms are streamed — the well-founded model of a stratified program IS its
//! stratified least fixpoint (dl.h).  Only the stratifier's own rejection
//! falls through to the rewrite below; a compile failure for any other reason
//! is a loud error, never a re-reading as "non-stratified".  MEASURED (chain
//! graph, n=400): the stratified column drops from 369-394 ms to ~66 ms, which
//! is the plain-VM column.
//!
//! ─── Mechanism ────────────────────────────────────────────────────────────
//! The key design move: REWRITE AT THE AST LEVEL.  Every NEGATED body atom
//! over an IDB predicate becomes a POSITIVE atom over a fresh driver-owned
//! complement relation:
//!     H :- A, !P(x).   ==>   H :- A, wfs_c<k>_P(x).
//! The rewritten program has no negation at all, so the existing compiler
//! accepts it and the existing VM executes it with NO override channel: the
//! driver materializes the current approximation into the complement
//! relation's stored view between rounds, and ordinary SCAN/LOOKUP reads it.
//! Negation safety is checked on the ORIGINAL AST before the rewrite
//! (mirroring compiler.zig pass 5: a negated atom's variables must be bound
//! by earlier positive body atoms) — after the rewrite the compiler could
//! no longer reject an unsafe program.
//!
//! With the complements fixed, the rewritten program is positive and has a
//! least fixpoint.  For a set S of ground IDB atoms let
//!     Γ(S) := lfp(EDB ∪ rules | wfs_c_P := Active^ar(P) \ S|P)
//! (Active = the active domain; see buildDomain — datalog without function
//! symbols derives heads only from these values, so the complement's
//! content outside Active^ar is never probed: safety makes every rewritten
//! atom a bound lookup).  Γ is antitone in S, and van Gelder's alternating
//! sequence
//!     S_0 = the initial head content,     S_{k+1} = Γ(S_k)
//! converges on the finite lattice to either a FIXPOINT (S_{k+1} = S_k: the
//! program is total, every atom TRUE or FALSE) or a 2-CYCLE (S_{k+1} =
//! S_{k-1}).  In the 2-cycle case the two cycle points are comparable
//! (even/odd subsequences are monotone by antitonicity; incomparability is
//! reported as an internal error, never a silent answer); the smaller is
//! the greatest fixpoint I_* below and the larger the least fixpoint O_*
//! above, and the well-founded partial model is
//!     TRUE       = I_*      (the intersection of the cycle points)
//!     FALSE      = complement of O_*
//!     UNDEFINED  = O_* \ I_*.
//! This is the Przymusinski-style reading of van Gelder's alternating
//! fixpoint; it was hand-verified on both canonical fixtures (win/move
//! graph {a,b,c,d,e}: T={a,c} F={b,d} U={e}; even cycle {1,2}: all
//! undefined) BEFORE implementation.
//!
//! Per round the driver (a) refreshes each complement from the PREVIOUS
//! assignment: wfs_c_P := Active^ar(P) \ prev|P, (b) runs vm_execute on the
//! rewritten program from scratch (head views reset to base; the complement
//! relations are body-only and never reset), (c) snapshots every head as
//! the new assignment and tests fixpoint / 2-cycle.
//!
//! ─── Incremental complement (two-step, parity-alternated) ─────────────────
//! Active^ar(P) is a CARTESIAN PRODUCT (|Active|^arity), so re-emitting
//! `full \ prev|P` every round is the round's second-largest term.  The
//! complement differs from the previous round's by exactly the assignment's
//! delta, mirrored: an atom that ENTERED prev|P leaves the complement, one
//! that LEFT it re-enters.  So it is maintained tuple-by-tuple (rel_delete /
//! rel_add) from the symmetric difference of HEAD SNAPSHOTS the driver already
//! keeps — no extra snapshot of P is taken — and only a delta past the
//! measured crossover (WFS_REBUILD_DENOM) re-derives full \ prev|P.  `full`
//! stays precomputed: it is the fallback's input and never changes.
//!
//! The delta is taken over TWO rounds, not one, because Γ is ANTITONE: S_k and
//! S_{k+1} are incomparable, so their difference stays near half the domain
//! even when the model has nearly settled (MEASURED win/move n=400: 106,175
//! tuples over 775 rounds), while the two PARITY subsequences are monotone and
//! differ by the round's actual new information (765 tuples — ONE per round).
//! Each complement therefore has TWO materializations, alternated by round
//! parity: round k installs rels[k%2] — the one idle for two rounds, i.e.
//! reflecting S_{k-2} exactly — after applying Δ(S_{k-2}|P, S_k|P) to it.
//! Rounds 0-1 and any post-crossover round rebuild instead.
//!
//! MIRRORED-CONTENT INVARIANT: the incremental update leaves exactly the set
//! the from-scratch derivation would (inside Active^arity; outside it the
//! content is never probed and is not written).  DL_WFS_VERIFY=1 checks it per
//! round; the differential harness gates the answers byte-for-byte.
//!
//! ─── Slice 1 scope (S1) ───────────────────────────────────────────────────
//! Rules: pure relational atoms over EDB + IDB, negation restricted to IDB
//! predicates, safe negation, no aggregates/builtins/lists/regex/variadic/
//! arithmetic.  Truth mode 0 (TRUE-only) is implemented; modes 1/2/3 return
//! DL_WFS_ERR_NOT_IMPLEMENTED rather than a wrong answer.  Not implemented:
//! publish/materialization of WFS results (query-scoped, like dl_query_rules_ro).

const std = @import("std");
const c = std.c;

const parser = @import("parser.zig");
const compiler = @import("compiler.zig");
const tupleset = @import("tupleset.zig");
const relation = @import("relation.zig");
const vrelation = @import("vrelation.zig");
const dl = @import("dl.zig");
const vm = @import("vm.zig");
const permindex = @import("permindex.zig");

// ─── Public C-ABI constants (mirrored in src/dl.h) ─────────────────────────

/// dl_wfs_status (dl.h): classification of a WFS evaluation.
pub const DL_WFS_STABLE: c_int = 0;
pub const DL_WFS_UNSUPPORTED_FEATURE: c_int = 1;

/// Truth modes of dl_query_wfs_ro's `truth` parameter.
pub const DL_WFS_TRUE_ONLY: c_int = 0;
pub const DL_WFS_FALSE_ONLY: c_int = 1;
pub const DL_WFS_UNDEF_ONLY: c_int = 2;
pub const DL_WFS_ALL_TAGGED: c_int = 3;

/// Negative results of dl_query_wfs_ro (see dl.h).
pub const DL_WFS_ERR_REJECTED: c_long = -2;
pub const DL_WFS_ERR_OOM: c_long = -3;
pub const DL_WFS_ERR_NOT_IMPLEMENTED: c_long = -4;
pub const DL_WFS_ERR_NO_CONVERGE: c_long = -5;
pub const DL_WFS_ERR_INTERNAL: c_long = -6;

/// van Gelder round cap — mirrors vm.zig's FIXPOINT_ERROR_BOUND discipline:
/// fail loudly, never silently return a partial model.
const WFS_ROUND_BOUND: c_int = 10000;

/// Cap on one complement's enumeration |Active|^arity.  Exceeding it is a
/// clean DL_WFS_ERR_REJECTED: the S1 complement is materialized by
/// enumeration, so arity>2 over a large domain is out of scope, not wrong.
const WFS_COMPLEMENT_CAP: c_long = 1 << 20;

/// Incremental-complement fallback rule: a round whose symmetric difference
/// (|S_k \ S_{k-2}| + |S_{k-2} \ S_k|, restricted to P — the two-step delta)
/// exceeds |Active|^arity / WFS_REBUILD_DENOM re-derives the complement from
/// scratch instead of applying the delta tuple-by-tuple.
///
/// MEASURED, not guessed (/tmp/wfscross.c: the engine's own ts_*/rel_*
/// primitives; the relation is reset to the real `full \ prev` before every
/// timed delta, and the delta is drawn from the real complement / prev sets —
/// an earlier version of that harness got both wrong and its numbers were
/// meaningless).  Full rebuild vs rel_delete+rel_add of the delta:
///
///     arity 1 (|full| = 200 / 400 / 1024): crossover at 0.32 / 0.21 / 0.25
///     arity 2 (|full| = 10k / 40k / 160k): crossover at 0.20 / 0.20 / 0.20
///
/// (fraction of |full| at which the delta costs as much as the rebuild).  A
/// delta of 1/8 costs 5-85 % of a rebuild across those regimes — the tightest,
/// 160k arity-2, is a near wash.  So 1/8 is the largest denominator that never
/// lands past the crossover.
const WFS_REBUILD_DENOM: c_long = 8;

const MAX_ARITY: u8 = 8;
const MAX_HEADS: usize = 64; // dl.DlDb.rels is [64]
const RELK_FIXED: u8 = 0;
const RELK_VARIADIC: u8 = 1;

// ─── extern bindings (same discipline as vm.zig/compiler.zig) ──────────────

extern "c" fn strdup(s: [*:0]const u8) ?[*:0]u8;
// The INSERTING intern (intern.zig's intern_str): what the compiler's
// token_const (compiler.zig:460) uses for TOK_IDENT — buildDomain must agree
// or the complement misses constants the compiled code will probe.
extern "c" fn intern_str(ir: ?*anyopaque, str: [*c]const u8) u32;

// ─── small helpers ─────────────────────────────────────────────────────────

fn strEqZ(a: [*:0]const u8, b: [*:0]const u8) bool {
    var i: usize = 0;
    while (a[i] != 0 and a[i] == b[i]) i += 1;
    return a[i] == 0 and b[i] == 0;
}

fn strEqLit(z: [*:0]const u8, lit: []const u8) bool {
    var i: usize = 0;
    while (i < lit.len and z[i] != 0 and z[i] == lit[i]) i += 1;
    return i == lit.len and z[i] == 0;
}

/// Builtin predicate spellings the compiler treats specially
/// (compiler.zig:600-673) — such atoms never name a stored relation.
/// Over-approximation: a USER relation genuinely named e.g. "range" or
/// "contains" is rejected too.  Safe by construction (reject, never a wrong
/// answer); the escape hatch is renaming the relation.
fn isBuiltinName(name: ?[*:0]const u8) bool {
    const n = name orelse return false;
    const builtins = [_][]const u8{
        "=",      "<",    ">=",  ">",     "!=",    "<=",
        "concat", "length", "lower", "upper", "prefix", "suffix",
        "contains", "cons", "car", "cdr",   "append", "member",
        "range",
    };
    for (builtins) |b| if (strEqLit(n, b)) return true;
    return false;
}

/// Does the argument token (recursively — list patterns) contain TOK_LIST?
fn tokenHasList(t: ?*const parser.token) bool {
    const tp = t orelse return false;
    if (tp.kind == parser.TOK_LIST) return true;
    if (tp.tail) |tl| if (tokenHasList(tl)) return true;
    return false;
}

fn wfsErr(comptime fmt: []const u8, args: anytype) void {
    var buf: [1024]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch buf[0..0];
    _ = c.write(2, msg.ptr, msg.len);
}

// ─── opt-in diagnostics (DL_WFS_STATS / DL_WFS_VERIFY) ────────────────────
//
// DL_WFS_STATS=1 prints one WFSSTAT line per query (the round count, how many
// rounds took the incremental path, the per-round delta sizes, and the ms per
// phase) — the instrument the incremental complement's economics were
// measured with, kept so the split can be re-measured.
//
// DL_WFS_VERIFY=1 makes every round CHECK the two invariants the incremental
// path rests on, and return DL_WFS_ERR_INTERNAL loudly on violation: (1) the
// head snapshot equals the live P view, (2) the installed complement equals
// full \ S_k recomputed from scratch.  It costs a full re-derivation per
// round, so it is off by default; a query never changes behaviour without it.
// Both knobs are read once per process (the first WFS query).
const Timespec = extern struct { sec: isize, nsec: isize };
extern fn clock_gettime(clk_id: c_int, tp: *Timespec) c_int;
fn nowNs() u64 {
    var ts: Timespec = undefined;
    _ = clock_gettime(1, &ts); // CLOCK_MONOTONIC
    return @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
}
var st_on: bool = false;
var st_verify: bool = false;
var st_checked: bool = false;
var st_walk: u64 = 0;
var st_sort: u64 = 0;
var st_rel: u64 = 0;
var st_dsort: u64 = 0;
var st_vm: u64 = 0;
var st_snap: u64 = 0;
var st_rounds: u64 = 0;
var st_full: u64 = 0;
var st_delta: u64 = 0;
var st_deltup: u64 = 0;
var st_maxdelta: u64 = 0;
var st_fullsz: u64 = 0;
var st_psz: u64 = 0;

fn statCheck() void {
    if (st_checked) return;
    st_checked = true;
    const v = std.c.getenv("DL_WFS_STATS");
    st_on = (v != null and v.?[0] != 0);
    const vv = std.c.getenv("DL_WFS_VERIFY");
    st_verify = (vv != null and vv.?[0] != 0);
}

fn statDump() void {
    if (!st_on) return;
    wfsErr("WFSSTAT rounds={d} full={d} deltas={d} deltatuples={d} maxdelta={d} |full|={d} |P|={d} walk_ms={d} sort_ms={d} rel_ms={d} delta_ms={d} vm_ms={d} snap_ms={d}\n", .{
        st_rounds,        st_full,
        st_delta,         st_deltup,
        st_maxdelta,      st_fullsz,
        st_psz,           st_walk / 1_000_000,
        st_sort / 1_000_000, st_rel / 1_000_000,
        st_dsort / 1_000_000, st_vm / 1_000_000,
        st_snap / 1_000_000,
    });
    // Per-query figures: the next query must not inherit these totals.
    st_walk = 0;
    st_sort = 0;
    st_rel = 0;
    st_dsort = 0;
    st_vm = 0;
    st_snap = 0;
    st_rounds = 0;
    st_full = 0;
    st_delta = 0;
    st_deltup = 0;
    st_maxdelta = 0;
    st_fullsz = 0;
    st_psz = 0;
}

// ─── sorted tuple_set set operations ───────────────────────────────────────

fn tsEq(a: *const tupleset.tuple_set, b: *const tupleset.tuple_set) bool {
    if (a.count != b.count) return false;
    if (a.count == 0) return true;
    const n: usize = @as(usize, @intCast(a.count)) * a.arity;
    return std.mem.eql(u32, a.data.?[0..n], b.data.?[0..n]);
}

/// a ⊆ b (both sorted).
fn tsSubset(a: *const tupleset.tuple_set, b: *const tupleset.tuple_set) bool {
    if (a.count > b.count) return false;
    var ai: c_long = 0;
    var bi: c_long = 0;
    const ar = a.arity;
    while (ai < a.count) {
        const at = a.data.? + @as(usize, @intCast(ai)) * ar;
        var found = false;
        while (bi < b.count) {
            const bt = b.data.? + @as(usize, @intCast(bi)) * ar;
            var k: usize = 0;
            while (k < ar and at[k] == bt[k]) k += 1;
            if (k == ar) {
                found = true;
                bi += 1;
                break;
            }
            if (at[k] < bt[k]) break; // at below bt and absent — not in b
            bi += 1;
        }
        if (!found) return false;
        ai += 1;
    }
    return true;
}

/// out := a ∩ b (both sorted).  Fresh ts; caller frees.
fn tsIntersect(a: *const tupleset.tuple_set, b: *const tupleset.tuple_set) ?tupleset.tuple_set {
    var out: tupleset.tuple_set = undefined;
    if (tupleset.ts_init(&out, a.arity) != 0) return null;
    var ai: c_long = 0;
    var bi: c_long = 0;
    const ar = a.arity;
    while (ai < a.count and bi < b.count) {
        const at = a.data.? + @as(usize, @intCast(ai)) * ar;
        const bt = b.data.? + @as(usize, @intCast(bi)) * ar;
        var k: usize = 0;
        while (k < ar and at[k] == bt[k]) k += 1;
        if (k == ar) {
            if (tupleset.ts_add(&out, at) < 0) {
                tupleset.ts_free(&out);
                return null;
            }
            ai += 1;
            bi += 1;
        } else if (at[k] < bt[k]) {
            ai += 1;
        } else {
            bi += 1;
        }
    }
    tupleset.ts_sort(&out);
    return out;
}

/// Snapshot a relation's view into a fresh sorted ts.
fn snapRel(rel: ?*const relation.Relation, arity: u8) ?tupleset.tuple_set {
    var ts: tupleset.tuple_set = undefined;
    if (tupleset.ts_init(&ts, arity) != 0) return null;
    if (relation.rel_prefix(rel, null, 0, relation.ts_sink_cb, &ts) < 0) {
        tupleset.ts_free(&ts);
        return null;
    }
    tupleset.ts_sort(&ts);
    return ts;
}

// ─── eval-clone helpers (local mirrors of dl.zig's evalDb*) ────────────────

fn cloneFindRel(db: *const dl.DlDb, name: [*:0]const u8) c_int {
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (db.rels[i].name != null and strEqZ(@ptrCast(db.rels[i].name.?), name))
            return @intCast(i);
    }
    return -1;
}

/// rel_create-based in-memory declare into the clone (dir stays NULL).
fn cloneDeclareInmem(db: *dl.DlDb, name: [*:0]const u8, arity: u8) c_int {
    if (db.nrels >= MAX_HEADS) return -1;
    if (cloneFindRel(db, name) >= 0) return -1;
    const rel = relation.rel_create(arity) orelse return -1;
    const nm = strdup(name) orelse {
        relation.rel_free(rel);
        return -1;
    };
    db.rels[db.nrels].name = @ptrCast(nm);
    db.rels[db.nrels].kind = RELK_FIXED;
    db.rels[db.nrels].arity = arity;
    db.rels[db.nrels].rel = rel;
    db.rels[db.nrels].vrel = null;
    db.nrels += 1;
    return 0;
}

/// Deep-copy a relation's view into a fresh in-memory relation (a head that
/// collides with an existing relation is evaluated into the copy).
fn cloneDeepcopyView(src: ?*const relation.Relation, arity: u8) ?*relation.Relation {
    const dst = relation.rel_create(arity) orelse return null;
    var ts: tupleset.tuple_set = undefined;
    if (tupleset.ts_init(&ts, arity) != 0) {
        relation.rel_free(dst);
        return null;
    }
    if (relation.rel_prefix(src, null, 0, relation.ts_sink_cb, &ts) < 0) {
        tupleset.ts_free(&ts);
        relation.rel_free(dst);
        return null;
    }
    tupleset.ts_sort(&ts);
    if (relation.rel_build_from_tupleset(dst, &ts) != 0) {
        tupleset.ts_free(&ts);
        relation.rel_free(dst);
        return null;
    }
    tupleset.ts_free(&ts);
    return dst;
}

/// Free the eval clone: the clone's perm indexes, its fresh relations +
/// names (idx >= n_aliased), and the deep-copied collision heads (owned).
fn cloneFree(db: *dl.DlDb, n_aliased: usize, owned: *const [MAX_HEADS]u8) void {
    permindex.permindex_free_all(db);
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (i >= n_aliased) {
            if (db.rels[i].rel) |r| relation.rel_free(r);
            if (db.rels[i].name) |n| c.free(@ptrCast(n));
        } else if (owned[i] != 0) {
            if (db.rels[i].rel) |r| relation.rel_free(r);
            // name borrowed from the live db — not freed
        }
    }
    db.nrels = 0;
}

// ─── S1 subset check + negation-safety pre-check (original AST) ────────────

/// Reject anything outside the S1 subset.  0 ok / -1 reject.
/// Includes the negation-safety pass (compiler.zig pass-5 semantics): a
/// negated atom's variables must be bound by EARLIER positive body atoms of
/// the same rule — checked here because the rewrite turns the atom positive
/// and the compiler could no longer reject the unsafe program.
fn checkSubset(rules: [*]?*parser.rule, n_rules: c_int) c_int {
    var i: c_int = 0;
    while (i < n_rules) : (i += 1) {
        const r = rules[@intCast(i)] orelse return -1;
        if (r.head == null or r.head.?.pred == null) return -1;
        if (r.head.?.nargs < 1 or r.head.?.nargs > MAX_ARITY) return -1;
        if (r.has_aggregate != 0) return -1;

        var bound: [256]c_int = [_]c_int{0} ** 256;
        var vnames: [256]?[*:0]u8 = [_]?[*:0]u8{null} ** 256;
        var nv: c_int = 0;

        var j: c_int = 0;
        while (j < r.nbody) : (j += 1) {
            const a = r.body.?[@intCast(j)] orelse return -1;
            if (a.pred == null) return -1;
            if (a.aggregate != 0) return -1; // aggregate atom
            if (a.pattern != null) return -1; // regex pattern atom
            if (a.arith != null) return -1; // X = E arithmetic atom
            if (isBuiltinName(a.pred)) return -1; // builtins: '=', comparisons, str/list/range
            if (a.nargs < 1 or a.nargs > MAX_ARITY) return -1;
            var k: c_int = 0;
            while (k < a.nargs) : (k += 1) {
                if (tokenHasList(a.args.?[@intCast(k)])) return -1; // no lists in S1
            }

            // variable-index lookup (linear; n <= 256 by the cap below)
            const findVar = struct {
                fn call(names: *const [256]?[*:0]u8, n: c_int, t: *const parser.token) c_int {
                    var vi: c_int = 0;
                    while (vi < n) : (vi += 1)
                        if (names[@intCast(vi)] != null and strEqZ(names[@intCast(vi)].?, @ptrCast(t.text)))
                            return vi;
                    return -1;
                }
            }.call;

            if (a.negated != 0) {
                k = 0;
                while (k < a.nargs) : (k += 1) {
                    const t = a.args.?[@intCast(k)] orelse continue;
                    if (t.kind != parser.TOK_VAR) continue;
                    const vi = findVar(&vnames, nv, t);
                    if (vi < 0 or bound[@intCast(vi)] == 0) return -1; // unsafe negation
                }
            } else {
                k = 0;
                while (k < a.nargs) : (k += 1) {
                    const t = a.args.?[@intCast(k)] orelse continue;
                    if (t.kind != parser.TOK_VAR) continue;
                    var vi = findVar(&vnames, nv, t);
                    if (vi < 0) {
                        if (nv >= 256) return -1;
                        vnames[@intCast(nv)] = @ptrCast(t.text);
                        vi = nv;
                        nv += 1;
                    }
                    bound[@intCast(vi)] = 1;
                }
            }
        }
    }
    return 0;
}

// ─── stratified fast path + S1 negation scope ──────────────────────────────

/// 0 when every negated body atom names a rule head (an IDB predicate) —
/// S1 negates IDB predicates only.  Non-zero after the diagnostic otherwise.
/// Split out of the complement loop so it can run BEFORE the stratified fast
/// path: `p(X) :- q(X), !edb_r(X)` stratifies (and the ordinary evaluator
/// answers it happily), so a fast path placed ahead of this check would
/// silently accept an out-of-subset program.
fn checkNegatedAreHeads(
    edb: *const dl.DlDb,
    rules: [*]?*parser.rule,
    n_rules: c_int,
    heads: []const c_int,
) c_int {
    var i: c_int = 0;
    while (i < n_rules) : (i += 1) {
        const r = rules[@intCast(i)] orelse return -1;
        var j: c_int = 0;
        while (j < r.nbody) : (j += 1) {
            const a = r.body.?[@intCast(j)] orelse return -1;
            if (a.negated == 0) continue;
            const pname: [*:0]const u8 = @ptrCast(a.pred.?);
            const pidx = cloneFindRel(edb, pname);
            var is_head = false;
            for (heads) |h| {
                if (h == pidx) {
                    is_head = true;
                    break;
                }
            }
            if (!is_head) {
                wfsErr("dl_query_wfs_ro: rejected — negated atom '{s}' is not a rule head (S1 negates IDB predicates only)\n", .{pname});
                return -1;
            }
        }
    }
    return 0;
}

const FastPathResult = union(enum) {
    /// The stratified least fixpoint answered the goal; the payload is the
    /// tuple count streamed.
    answered: c_long,
    /// programStratifies rejected the ORIGINAL program as unstratifiable — the
    /// only case that belongs to the alternating fixpoint.
    unstratifiable: void,
    /// A loud failure (already diagnosed).  Never reinterpreted as
    /// "non-stratified": a compile failure for any other reason must not be
    /// re-read as a WFS problem.
    failed: void,
};

/// Evaluate the ORIGINAL (unrewritten) program on the ordinary semi-naive
/// fixpoint — the same shape dl_query_rules_ro uses (compile -> vm_execute ->
/// goal scan) on the driver's already-populated eval clone.
///
/// A stratified program's well-founded model is its stratified least fixpoint
/// (dl.h), and truth mode 0 streams that model's TRUE atoms, so the goal
/// relation vm_execute leaves behind is exactly what the alternating path
/// would compute — one fixpoint instead of a chain of them.
///
/// Distinguishing the fall-through: compiler.programStratifies runs the
/// compiler's OWN stratifier (compute_strata, diagnosed silently) BEFORE any
/// compile is attempted.  1 routes here, 0 is the unstratifiable rejection and
/// the only value that falls through, -1 (the check could not run) is a loud
/// failure.  Because the verdict is taken before compiling, a program that
/// fails to compile for any OTHER reason (unsafe rule, arity, kind, OOM) can
/// never be re-read as non-stratified: it fails in the compile below.
fn stratifiedFastPath(
    edb: *dl.DlDb,
    rules: [*]?*parser.rule,
    n_rules: c_int,
    goal_rel: [*c]const u8,
    cb: relation.RelEnumCb,
    user: ?*anyopaque,
) FastPathResult {
    var crules: ?[*]?*compiler.compiled_rule = null;
    var n_crules: c_int = 0;

    switch (compiler.programStratifies(edb, rules, n_rules)) {
        0 => return .unstratifiable, // the stratifier's own rejection
        1 => {}, // stratified: the ordinary fixpoint answers it
        else => {
            wfsErr("dl_query_wfs_ro: stratification check failed\n", .{});
            return .failed;
        },
    }

    if (compiler.compile_rules(edb, rules, n_rules, &crules, &n_crules) != 0) {
        // compile_rules publishes its out-params only on success; free
        // defensively so a partial array cannot leak either way.
        if (crules) |cr| {
            var j: c_int = 0;
            while (j < n_crules) : (j += 1) compiler.compiled_rule_free(cr[@intCast(j)]);
            c.free(@ptrCast(cr));
        }
        wfsErr("dl_query_wfs_ro: compile failed\n", .{});
        return .failed;
    }
    defer {
        var j: c_int = 0;
        while (j < n_crules) : (j += 1) compiler.compiled_rule_free(crules.?[@intCast(j)]);
        c.free(@ptrCast(crules.?));
    }

    if (vm.vm_execute(edb, crules, n_crules) != 0) {
        wfsErr("dl_query_wfs_ro: evaluation failed\n", .{});
        return .failed;
    }

    const gi = cloneFindRel(edb, goal_rel);
    if (gi < 0) {
        wfsErr("dl_query_wfs_ro: goal '{s}' not found\n", .{@as([*c]const u8, @ptrCast(goal_rel))});
        return .failed;
    }
    const e = &edb.rels[@intCast(gi)];
    if (e.kind == RELK_VARIADIC) return .{ .answered = vrelation.vrel_prefix(e.vrel, null, 0, cb, user) };
    return .{ .answered = relation.rel_prefix(e.rel, null, 0, cb, user) };
}

// ─── active domain + complement enumeration ────────────────────────────────

/// Active domain: every column value of every fixed-arity relation's
/// current view, plus every constant (symbol or integer) appearing in the
/// rules.  Symbol constants are interned with the INSERTING intern, exactly
/// as the compiler's token_const does (compiler.zig:460), so the domain is
/// independent of prior interner state.  Datalog without function symbols
/// derives heads only from these values; safety makes every rewritten
/// complement atom a bound lookup over a subset of Active^arity, so the
/// complement content outside Active^arity is never probed.
fn buildDomain(db: *const dl.DlDb, rules: [*]?*parser.rule, n_rules: c_int, dom: *tupleset.tuple_set) c_int {
    if (tupleset.ts_init(dom, 1) != 0) return -1;
    var ri: usize = 0;
    while (ri < db.nrels) : (ri += 1) {
        if (db.rels[ri].kind != RELK_FIXED) continue; // variadic values are never probed (S1)
        const rel = db.rels[ri].rel orelse continue;
        const ar = relation.rel_arity(rel);
        if (ar == 0) continue;
        var ts: tupleset.tuple_set = undefined;
        if (tupleset.ts_init(&ts, ar) != 0) return -1;
        if (relation.rel_prefix(rel, null, 0, relation.ts_sink_cb, &ts) < 0) {
            tupleset.ts_free(&ts);
            return -1;
        }
        var ti: c_long = 0;
        while (ti < ts.count) : (ti += 1) {
            const row = ts.data.? + @as(usize, @intCast(ti)) * ar;
            var k: u8 = 0;
            while (k < ar) : (k += 1) {
                const v: [*]const u32 = @ptrCast(&row[k]);
                _ = tupleset.ts_add(dom, v);
            }
        }
        tupleset.ts_free(&ts);
    }

    // Constants appearing in the rules.
    var i: c_int = 0;
    while (i < n_rules) : (i += 1) {
        const r = rules[@intCast(i)] orelse continue;
        var na: usize = 0;
        var atoms: [128]?*const parser.atom = undefined;
        if (na < atoms.len) {
            atoms[na] = r.head;
            na += 1;
        }
        var j: c_int = 0;
        while (j < r.nbody) : (j += 1) {
            if (na >= atoms.len) return -1;
            atoms[na] = r.body.?[@intCast(j)];
            na += 1;
        }
        for (atoms[0..na]) |maybe_a| {
            const a = maybe_a orelse continue;
            var k: c_int = 0;
            while (k < a.nargs) : (k += 1) {
                const t = a.args.?[@intCast(k)] orelse continue;
                var v: u32 = 0;
                if (t.kind == parser.TOK_INT) {
                    v = t.ival; // integers are stored as raw u32 ids — matches token_const (compiler.zig:449-452)
                } else if (t.kind == parser.TOK_IDENT and t.text != null) {
                    // INSERTING intern, same as the compiler's token_const
                    // (compiler.zig:460): the compiled probe interns the
                    // constant at this id regardless of prior interner
                    // state, so the complement must enumerate it.  The old
                    // non-inserting find+skip made the answer depend on
                    // invisible interner state (silent-wrong).
                    v = intern_str(db.ir, @ptrCast(t.text));
                    if (v == 0) return -1; // OOM/overlong key — loud, never skip
                } else continue;
                const vp: [*]const u32 = @ptrCast(&v);
                _ = tupleset.ts_add(dom, vp);
            }
        }
    }
    tupleset.ts_sort(dom);
    return 0;
}

/// out := Active^arity (cartesian product; odometer order, then sorted).
fn enumerateProduct(dom: *const tupleset.tuple_set, arity: u8, out: *tupleset.tuple_set) c_int {
    const n: c_long = dom.count;
    if (n == 0) return tupleset.ts_init(out, arity); // empty domain → empty complement
    var total: c_long = 1;
    var a: u8 = 0;
    while (a < arity) : (a += 1) {
        total *= n;
        if (total > WFS_COMPLEMENT_CAP) return -1; // too large — clean reject
    }
    if (tupleset.ts_init(out, arity) != 0) return -1;
    var idx: [MAX_ARITY]c_long = [_]c_long{0} ** MAX_ARITY;
    var row: [MAX_ARITY]u32 = undefined;
    var t: c_long = 0;
    while (t < total) : (t += 1) {
        a = 0;
        while (a < arity) : (a += 1)
            row[a] = dom.data.?[@intCast(idx[a])];
        if (tupleset.ts_add(out, &row) < 0) return -1;
        var d: u8 = arity; // odometer increment
        while (d > 0) {
            d -= 1;
            idx[d] += 1;
            if (idx[d] < n) break;
            idx[d] = 0;
        }
    }
    tupleset.ts_sort(out);
    return 0;
}

// ─── driver state ──────────────────────────────────────────────────────────

const CompRel = struct {
    pred_idx: c_int, // clone index of the negated predicate P
    comp_idx: c_int, // clone index of wfs_c<k>_P (the body atom's slot)
    arity: u8,
    /// Head slot of P: st.prev/prev2/prev3[slot].ts are exactly the LAST THREE
    /// assignments restricted to P — the delta source.  (P is a rule head by
    /// checkNegatedAreHeads, so the slot always exists.)
    slot: usize,
    name: [96:0]u8 = undefined,
    full: tupleset.tuple_set, // Active^arity (sorted)
    /// TWO materializations of the complement, alternated by round parity.
    /// Round k installs rels[k % 2] into the clone slot for the body atom to
    /// read; the other one is idle until round k+2.  Γ is ANTITONE, so
    /// consecutive assignments S_k and S_{k+1} are not comparable and their
    /// difference is large even when the model is nearly settled — but the
    /// two PARITY subsequences are monotone, i.e. S_k and S_{k+2} differ by
    /// the round's actual new information.  MEASURED (win/move chain,
    /// n=400): the 1-step difference is 106,175 tuples over 775 rounds
    /// (half the domain per round); the 2-step difference is 765 — ONE tuple
    /// per round.  So the relation a round has to update is the one that sat
    /// idle for two rounds, and its delta is the small one.
    rels: [2]?*relation.Relation = .{ null, null },
    /// Symmetric difference between two assignments (tuples that ENTERED /
    /// LEFT prev|P).  Scratch, reset every round; held here so the per-round
    /// delta never allocates a fresh tuple_set.
    entered: tupleset.tuple_set,
    left: tupleset.tuple_set,
};

const HeadSnap = struct {
    rel_idx: c_int,
    arity: u8,
    ts: tupleset.tuple_set,
};

const WfsState = struct {
    edb: *dl.DlDb,
    n_aliased: usize,
    owned: [MAX_HEADS]u8,
    dom: tupleset.tuple_set,
    comps: [MAX_HEADS]CompRel = undefined,
    n_comps: usize = 0,
    head_idx: [MAX_HEADS]c_int = undefined,
    n_heads: usize = 0,
    prev: [MAX_HEADS]HeadSnap = undefined,
    prev2: [MAX_HEADS]HeadSnap = undefined,
    prev3: [MAX_HEADS]HeadSnap = undefined,
    cur: [MAX_HEADS]HeadSnap = undefined,
    have_prev2: bool = false,
    have_prev3: bool = false,
    crules: ?[*]?*compiler.compiled_rule = null,
    n_crules: c_int = 0,
    dom_ok: bool = false,
    prev_ok: bool = false, // prev[0..n_heads] initialized
    prev2_ok: bool = false,
    prev3_ok: bool = false,
    cur_ok: bool = false,
    comps_ok: bool = false,
};

fn freeSnaps(snaps: []HeadSnap) void {
    for (snaps) |*s| tupleset.ts_free(&s.ts);
}

fn teardown(st: *WfsState) void {
    if (st.dom_ok) tupleset.ts_free(&st.dom);
    if (st.comps_ok) {
        var i: usize = 0;
        while (i < st.n_comps) : (i += 1) {
            const cp = &st.comps[i];
            tupleset.ts_free(&cp.full);
            tupleset.ts_free(&cp.entered);
            tupleset.ts_free(&cp.left);
            // The two complement relations are driver-owned: unhook the clone
            // slot first so cloneFree cannot free one of them twice.
            st.edb.rels[@intCast(cp.comp_idx)].rel = null;
            for (cp.rels) |maybe_r| if (maybe_r) |r| relation.rel_free(r);
        }
    }
    if (st.prev_ok) freeSnaps(st.prev[0..st.n_heads]);
    if (st.prev2_ok) freeSnaps(st.prev2[0..st.n_heads]);
    if (st.prev3_ok) freeSnaps(st.prev3[0..st.n_heads]);
    if (st.cur_ok) freeSnaps(st.cur[0..st.n_heads]);
    if (st.crules) |cr| {
        var j: c_int = 0;
        while (j < st.n_crules) : (j += 1) compiler.compiled_rule_free(cr[@intCast(j)]);
        c.free(@ptrCast(cr));
    }
    cloneFree(st.edb, st.n_aliased, &st.owned);
}

// ─── the driver ────────────────────────────────────────────────────────────

/// Evaluate `rules` against a shallow clone of `db` under the well-founded
/// semantics and stream the goal relation's TRUE tuples (truth mode 0).
///
/// `rules` are the caller's parsed AST nodes, REWRITTEN IN PLACE (negated
/// atoms become positive complement atoms); the caller frees them right
/// after (they must not be reused).
///
/// `db_opaque` is dl.zig's *DlDb passed as anyopaque — the same byte layout
/// as dl.DlDb (abi_witness.zig holds the layout gate).
pub fn wfs_eval_query(
    db_opaque: ?*anyopaque,
    rules: ?[*]?*parser.rule,
    n_rules: c_int,
    goal_rel: [*c]const u8,
    truth: c_int,
    cb: relation.RelEnumCb,
    user: ?*anyopaque,
) c_long {
    const d: *dl.DlDb = @ptrCast(@alignCast(db_opaque orelse return -1));
    const rs = rules orelse return -1;
    if (n_rules <= 0 or cb == null) return -1;

    if (truth != DL_WFS_TRUE_ONLY) {
        wfsErr("dl_query_wfs_ro: truth mode {d} is not implemented in this slice (only 0 = TRUE-only)\n", .{truth});
        return DL_WFS_ERR_NOT_IMPLEMENTED;
    }

    // S1 subset + negation safety on the ORIGINAL ast (the rewrite would
    // hide both from the compiler).
    if (checkSubset(rs, n_rules) != 0) {
        wfsErr("dl_query_wfs_ro: rejected — program outside the well-founded S1 subset (pure relational rules, negation of IDB predicates only, safe negation; no aggregates/builtins/lists/patterns/variadic/arithmetic)\n", .{});
        return DL_WFS_ERR_REJECTED;
    }

    // Variadic relations are outside S1 (heads and bodies).
    {
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) {
            const r = rs[@intCast(i)] orelse return -1;
            var j: c_int = -1;
            while (j < r.nbody) : (j += 1) {
                const a = if (j < 0) r.head.? else (r.body.?[@intCast(j)] orelse continue);
                if (a.pred == null) return -1;
                const ri = cloneFindRel(d, @ptrCast(a.pred.?));
                if (ri >= 0 and d.rels[@intCast(ri)].kind != RELK_FIXED) {
                    wfsErr("dl_query_wfs_ro: rejected — variadic relation '{s}' is outside the S1 subset\n", .{@as([*c]const u8, @ptrCast(a.pred.?))});
                    return DL_WFS_ERR_REJECTED;
                }
            }
        }
    }

    // ── shallow clone (dir NULL: the VM/compile path cannot touch disk)
    var edb: dl.DlDb = std.mem.zeroes(dl.DlDb);
    edb.ir = d.ir;
    edb.terms = d.terms;
    edb.lock_fd = -1;
    // U-homog: carry the schema so the kind check agrees with dl_load_rules
    // on schema-attached dbs (see dl.zig evalDbClone's note).
    edb.schema = d.schema;
    {
        var i: usize = 0;
        while (i < d.nrels) : (i += 1) edb.rels[i] = d.rels[i];
        edb.nrels = d.nrels;
    }

    var st = WfsState{
        .edb = &edb,
        .n_aliased = edb.nrels,
        .owned = [_]u8{0} ** MAX_HEADS,
        .dom = std.mem.zeroes(tupleset.tuple_set),
    };
    defer teardown(&st);

    // ── declare / deep-copy head relations; collect the head set
    {
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) {
            const r = rs[@intCast(i)] orelse return -1;
            const h = r.head.?;
            const name: [*:0]const u8 = @ptrCast(h.pred.?);

            var known = false;
            var k: usize = 0;
            while (k < st.n_heads) : (k += 1) {
                const e = &edb.rels[@intCast(st.head_idx[k])];
                if (e.name != null and strEqZ(@ptrCast(e.name.?), name)) {
                    known = true;
                    break;
                }
            }
            if (known) continue;

            var hi = cloneFindRel(&edb, name);
            if (hi < 0) {
                if (cloneDeclareInmem(&edb, name, @intCast(h.nargs)) != 0) {
                    wfsErr("dl_query_wfs_ro: cannot declare head '{s}'\n", .{name});
                    return -1;
                }
                hi = cloneFindRel(&edb, name);
            } else if (@as(usize, @intCast(hi)) < st.n_aliased) {
                // collision with an existing relation: evaluate into a deep
                // copy so the borrowed live relation is never written
                const e = &edb.rels[@intCast(hi)];
                const rc = cloneDeepcopyView(e.rel, e.arity) orelse {
                    wfsErr("dl_query_wfs_ro: head-copy failed for '{s}'\n", .{name});
                    return -1;
                };
                e.rel = rc;
                st.owned[@intCast(hi)] = 1;
            }
            if (hi < 0 or st.n_heads >= MAX_HEADS) return -1;
            st.head_idx[st.n_heads] = hi;
            st.n_heads += 1;
        }
    }

    // U-homog S2: int/symbol constant-kind check on the ORIGINAL ast — the
    // complement rewrite below RENAMES every negated atom, which would hide
    // its constants from compile_rules' own check.  Runs on the clone (all
    // rule heads declared above) so head-kind propagation can see them.
    // Exactly ONE site: it must run BEFORE buildDomain, which interns rule
    // symbol constants into the SHARED interner — with recorded (S2) kinds
    // that pollution is irrelevant, but the single-site placement keeps the
    // check count at one per query.
    if (compiler.checkAllRuleConstKinds(&edb, rs, n_rules) != 0) {
        wfsErr("dl_query_wfs_ro: compile of the rewritten program failed\n", .{});
        return DL_WFS_ERR_REJECTED;
    }

    // ── S1 negation scope: every negated atom names a rule head (an IDB
    // predicate).  Checked HERE, before the stratified fast path below,
    // because negating an EDB predicate is a perfectly STRATIFIABLE program
    // the ordinary evaluator would answer — taking the fast path there would
    // silently widen the documented S1 rejection contract.
    if (checkNegatedAreHeads(&edb, rs, n_rules, st.head_idx[0..st.n_heads]) != 0)
        return DL_WFS_ERR_REJECTED;

    // ── stratified fast path: a program the compiler's own stratifier
    // accepts needs no alternating fixpoint (dl.h: the well-founded model of
    // a stratified program IS its stratified least fixpoint, and truth mode 0
    // streams exactly those TRUE atoms).  Only the stratifier's rejection
    // falls through to the rewrite below.
    switch (stratifiedFastPath(&edb, rs, n_rules, goal_rel, cb, user)) {
        .answered => |n| return n,
        .unstratifiable => {},
        .failed => return -1,
    }

    // ── active domain (before any mutation of the clone's borrowed rels)
    if (buildDomain(&edb, rs, n_rules, &st.dom) != 0) {
        wfsErr("dl_query_wfs_ro: active-domain build failed\n", .{});
        return -1;
    }
    st.dom_ok = true;

    // ── complement relation for every distinct negated IDB predicate
    {
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) {
            const r = rs[@intCast(i)] orelse return -1;
            var j: c_int = 0;
            while (j < r.nbody) : (j += 1) {
                const a = r.body.?[@intCast(j)] orelse return -1;
                if (a.negated == 0) continue;
                const pname: [*:0]const u8 = @ptrCast(a.pred.?);
                const pidx = cloneFindRel(&edb, pname);
                // pidx is a head: checkNegatedAreHeads ran above, before the
                // fast path, precisely so the S1 scope rejection cannot be
                // bypassed by a stratifiable negation of an EDB predicate.

                var ci: usize = 0;
                while (ci < st.n_comps) : (ci += 1)
                    if (st.comps[ci].pred_idx == pidx) break;
                if (ci < st.n_comps) continue; // complement already created

                if (st.n_comps >= MAX_HEADS) return DL_WFS_ERR_OOM;
                // P's head slot — the delta source (st.prev/prev2/prev3).
                var slot: usize = 0;
                var have_slot = false;
                var si: usize = 0;
                while (si < st.n_heads) : (si += 1) {
                    if (st.head_idx[si] == pidx) {
                        slot = si;
                        have_slot = true;
                        break;
                    }
                }
                if (!have_slot) {
                    wfsErr("dl_query_wfs_ro: internal error — negated predicate is not a tracked head\n", .{});
                    return DL_WFS_ERR_INTERNAL;
                }
                var cr = CompRel{
                    .pred_idx = pidx,
                    .comp_idx = -1,
                    .arity = edb.rels[@intCast(pidx)].arity,
                    .slot = slot,
                    .name = undefined,
                    .full = std.mem.zeroes(tupleset.tuple_set),
                    .entered = std.mem.zeroes(tupleset.tuple_set),
                    .left = std.mem.zeroes(tupleset.tuple_set),
                };
                _ = std.fmt.bufPrintZ(&cr.name, "wfs_c{d}_{s}", .{ st.n_comps, pname }) catch return -1;
                if (cloneDeclareInmem(&edb, &cr.name, cr.arity) != 0) {
                    wfsErr("dl_query_wfs_ro: cannot declare complement '{s}'\n", .{&cr.name});
                    return -1;
                }
                cr.comp_idx = cloneFindRel(&edb, &cr.name);
                if (enumerateProduct(&st.dom, cr.arity, &cr.full) != 0) {
                    wfsErr("dl_query_wfs_ro: rejected — complement of '{s}' exceeds the S1 enumeration cap (|active domain|^{d})\n", .{ pname, cr.arity });
                    return DL_WFS_ERR_REJECTED;
                }
                if (tupleset.ts_init(&cr.entered, cr.arity) != 0 or
                    tupleset.ts_init(&cr.left, cr.arity) != 0)
                {
                    tupleset.ts_free(&cr.full);
                    return DL_WFS_ERR_OOM;
                }
                // rels[0] is the relation the clone slot was declared with;
                // rels[1] is the second parity's materialization.  Both are
                // freed by teardown (which unhooks the clone slot first).
                cr.rels[0] = edb.rels[@intCast(cr.comp_idx)].rel;
                cr.rels[1] = relation.rel_create(cr.arity) orelse {
                    tupleset.ts_free(&cr.full);
                    tupleset.ts_free(&cr.entered);
                    tupleset.ts_free(&cr.left);
                    return DL_WFS_ERR_OOM;
                };
                st.comps[st.n_comps] = cr;
                st.n_comps += 1;
                st.comps_ok = true;
            }
        }
    }

    // ── rewrite: negated atom -> positive atom over its complement
    {
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) {
            const r = rs[@intCast(i)] orelse return -1;
            var j: c_int = 0;
            while (j < r.nbody) : (j += 1) {
                const a = r.body.?[@intCast(j)] orelse return -1;
                if (a.negated == 0) continue;
                const pidx = cloneFindRel(&edb, @ptrCast(a.pred.?));
                var ci: usize = 0;
                while (ci < st.n_comps) : (ci += 1)
                    if (st.comps[ci].pred_idx == pidx) break;
                if (ci >= st.n_comps) return DL_WFS_ERR_INTERNAL; // created above
                const np = strdup(@ptrCast(&st.comps[ci].name)) orelse return DL_WFS_ERR_OOM;
                if (a.pred) |p| c.free(@ptrCast(p));
                a.pred = np;
                a.negated = 0;
            }
            r.has_negation = 0;
        }
    }

    // ── compile the positive rewrite
    if (compiler.compile_rules(&edb, rs, n_rules, &st.crules, &st.n_crules) != 0) {
        wfsErr("dl_query_wfs_ro: compile of the rewritten program failed\n", .{});
        return -1;
    }

    // ── S_0: snapshot the heads' initial (EDB-collision) content
    {
        var k: usize = 0;
        while (k < st.n_heads) : (k += 1) {
            const e = &edb.rels[@intCast(st.head_idx[k])];
            const ts = snapRel(e.rel, e.arity) orelse return DL_WFS_ERR_OOM;
            st.prev[k] = .{ .rel_idx = st.head_idx[k], .arity = e.arity, .ts = ts };
        }
        st.prev_ok = true;
    }

    // ── van Gelder rounds: S_{k+1} = Γ(S_k)
    var converged = false;
    var two_cycle = false;
    {
        statCheck();
        var round: c_int = 0;
        while (round < WFS_ROUND_BOUND) : (round += 1) {
            if (st_on) st_rounds += 1;
            // complements := Active^ar \ S_k  (materialized into the clone
            // slot the rewritten atom's SCAN/LOOKUP reads).
            //
            // INCREMENTAL, TWO-STEP: Γ is antitone, so S_k and S_{k+1} are
            // incomparable and their difference is large even close to
            // convergence — but the parity subsequences are monotone, so the
            // relation installed two rounds ago (S_{k-2}) differs from S_k by
            // the round's actual new information (MEASURED win/move n=400:
            // one tuple per round, vs half the domain for one step).  Each
            // round therefore installs rels[k%2] — the one that sat idle for
            // two rounds — after applying Δ(S_{k-2}|P, S_k|P) to it: an atom
            // that ENTERED P leaves the complement, one that LEFT P re-enters.
            // The from-scratch rebuild (full \ S_k) still covers rounds 0-1
            // (no S_{k-2} yet) and any delta past the measured crossover.
            const par: usize = @intCast(@mod(round, 2));
            var ci: usize = 0;
            while (ci < st.n_comps) : (ci += 1) {
                const cp = &st.comps[ci];
                const s_new = &st.prev[cp.slot].ts; // S_k|P — the head snapshot
                var incremental = false;

                if (round >= 2 and st.have_prev3) {
                    const t_d0 = if (st_on) nowNs() else 0;
                    const s_old = &st.prev3[cp.slot].ts; // S_{k-2}|P
                    tupleset.ts_reset(&cp.entered);
                    tupleset.ts_reset(&cp.left);
                    var di: c_long = 0;
                    while (di < s_new.count) : (di += 1) {
                        const row = s_new.data.? + @as(usize, @intCast(di)) * cp.arity;
                        if (tupleset.ts_contains(s_old, row) == 0)
                            if (tupleset.ts_add(&cp.entered, row) < 0) return DL_WFS_ERR_OOM;
                    }
                    di = 0;
                    while (di < s_old.count) : (di += 1) {
                        const row = s_old.data.? + @as(usize, @intCast(di)) * cp.arity;
                        if (tupleset.ts_contains(s_new, row) == 0)
                            if (tupleset.ts_add(&cp.left, row) < 0) return DL_WFS_ERR_OOM;
                    }
                    const nd = cp.entered.count + cp.left.count;
                    if (st_on) {
                        st_deltup += @intCast(nd);
                        if (@as(c_long, @intCast(st_maxdelta)) < nd) st_maxdelta = @intCast(nd);
                        st_fullsz = @intCast(cp.full.count);
                        st_psz = @intCast(s_new.count);
                    }

                    if (nd * WFS_REBUILD_DENOM <= cp.full.count) {
                        const inc_rel = cp.rels[par].?;
                        di = 0;
                        while (di < cp.entered.count) : (di += 1) {
                            const row = cp.entered.data.? + @as(usize, @intCast(di)) * cp.arity;
                            if (relation.rel_delete(inc_rel, row) < 0) {
                                wfsErr("dl_query_wfs_ro: complement update failed\n", .{});
                                return -1;
                            }
                        }
                        // Re-add only what Active^arity actually holds: the
                        // complement's content outside it is never probed
                        // (buildDomain's note above), and this guard keeps the
                        // incrementally maintained view byte-identical to the
                        // from-scratch one even if that invariant were violated.
                        di = 0;
                        while (di < cp.left.count) : (di += 1) {
                            const row = cp.left.data.? + @as(usize, @intCast(di)) * cp.arity;
                            if (tupleset.ts_contains(&cp.full, row) == 0) continue;
                            if (relation.rel_add(inc_rel, row) < 0) {
                                wfsErr("dl_query_wfs_ro: complement update failed\n", .{});
                                return -1;
                            }
                        }
                        incremental = true;
                        if (st_on) {
                            st_delta += 1;
                            st_dsort += nowNs() - t_d0;
                        }
                    }
                }

                if (!incremental) {
                    const t_w0 = if (st_on) nowNs() else 0;
                    const build_rel = cp.rels[par].?;
                    var rc: c_int = 0;
                    if (s_new.count == 0) {
                        // S_k|P is empty: the complement IS full — no walk.
                        rc = relation.rel_build_from_tupleset(build_rel, &cp.full);
                        if (st_on) {
                            st_walk += nowNs() - t_w0;
                            st_full += 1;
                            st_fullsz = @intCast(cp.full.count);
                            st_psz = 0;
                        }
                    } else {
                        var cts: tupleset.tuple_set = undefined;
                        if (tupleset.ts_init(&cts, cp.arity) != 0) return DL_WFS_ERR_OOM;
                        // cts := full \ s_new  (both sorted — merge walk)
                        var fi: c_long = 0;
                        var vi: c_long = 0;
                        const ar = cp.arity;
                        while (fi < cp.full.count) {
                            const ft = cp.full.data.? + @as(usize, @intCast(fi)) * ar;
                            var skip = false;
                            while (vi < s_new.count) {
                                const vt = s_new.data.? + @as(usize, @intCast(vi)) * ar;
                                var m: usize = 0;
                                while (m < ar and ft[m] == vt[m]) m += 1;
                                if (m == ar) {
                                    skip = true;
                                    vi += 1;
                                    break;
                                }
                                if (ft[m] > vt[m]) {
                                    vi += 1;
                                    continue;
                                }
                                break; // ft < vt — difference element
                            }
                            if (!skip) _ = tupleset.ts_add(&cts, ft);
                            fi += 1;
                        }
                        const t_w1 = if (st_on) nowNs() else 0;
                        tupleset.ts_sort(&cts);
                        const t_w2 = if (st_on) nowNs() else 0;
                        rc = relation.rel_build_from_tupleset(build_rel, &cts);
                        tupleset.ts_free(&cts);
                        if (st_on) {
                            const t_w3 = nowNs();
                            st_walk += t_w1 - t_w0; // merge walk + ts_add
                            st_sort += t_w2 - t_w1; // ts_sort
                            st_rel += t_w3 - t_w2; // rel_build_from_tupleset
                            st_full += 1;
                            st_fullsz = @intCast(cp.full.count);
                            st_psz = @intCast(s_new.count);
                        }
                    }
                    if (rc != 0) {
                        wfsErr("dl_query_wfs_ro: complement materialization failed\n", .{});
                        return -1;
                    }
                }

                // install this parity's materialization for the body atom
                edb.rels[@intCast(cp.comp_idx)].rel = cp.rels[par].?;

                if (st_verify) {
                    // (1) the substitution: the head snapshot must equal the
                    // live relation content the old code re-snapshotted.
                    var live = snapRel(edb.rels[@intCast(cp.pred_idx)].rel, cp.arity) orelse return DL_WFS_ERR_OOM;
                    if (!tsEq(&live, s_new)) {
                        wfsErr("WFSVERIFY round {d}: st.prev|P != live P view ({d} vs {d})\n", .{ round, live.count, s_new.count });
                        tupleset.ts_free(&live);
                        return DL_WFS_ERR_INTERNAL;
                    }
                    tupleset.ts_free(&live);
                    // (2) the complement content: the installed materialization
                    // must equal full \ S_k byte for byte.
                    var want: tupleset.tuple_set = undefined;
                    if (tupleset.ts_init(&want, cp.arity) != 0) return DL_WFS_ERR_OOM;
                    var fi2: c_long = 0;
                    var vi2: c_long = 0;
                    while (fi2 < cp.full.count) {
                        const ft = cp.full.data.? + @as(usize, @intCast(fi2)) * cp.arity;
                        var skip = false;
                        while (vi2 < s_new.count) {
                            const vt = s_new.data.? + @as(usize, @intCast(vi2)) * cp.arity;
                            var m: usize = 0;
                            while (m < cp.arity and ft[m] == vt[m]) m += 1;
                            if (m == cp.arity) {
                                skip = true;
                                vi2 += 1;
                                break;
                            }
                            if (ft[m] > vt[m]) {
                                vi2 += 1;
                                continue;
                            }
                            break;
                        }
                        if (!skip) _ = tupleset.ts_add(&want, ft);
                        fi2 += 1;
                    }
                    tupleset.ts_sort(&want);
                    var got = snapRel(cp.rels[par].?, cp.arity) orelse return DL_WFS_ERR_OOM;
                    if (!tsEq(&got, &want)) {
                        wfsErr("WFSVERIFY round {d} comp {d}: complement != full \\ S_k ({d} vs {d} tuples, incremental={any})\n", .{ round, ci, got.count, want.count, incremental });
                        tupleset.ts_free(&got);
                        tupleset.ts_free(&want);
                        return DL_WFS_ERR_INTERNAL;
                    }
                    tupleset.ts_free(&got);
                    tupleset.ts_free(&want);
                }
            }

            // Γ(S_k) from scratch: vm_execute resets head views to base
            const t_v0 = if (st_on) nowNs() else 0;
            if (vm.vm_execute(&edb, st.crules, st.n_crules) != 0) {
                wfsErr("dl_query_wfs_ro: evaluation failed\n", .{});
                return -1;
            }
            const t_v1 = if (st_on) nowNs() else 0;

            // snapshot S_{k+1}
            {
                var k: usize = 0;
                while (k < st.n_heads) : (k += 1) {
                    const e = &edb.rels[@intCast(st.head_idx[k])];
                    const ts = snapRel(e.rel, e.arity) orelse return DL_WFS_ERR_OOM;
                    st.cur[k] = .{ .rel_idx = st.head_idx[k], .arity = e.arity, .ts = ts };
                }
                st.cur_ok = true;
            }
            if (st_on) {
                const t_v2 = nowNs();
                st_vm += t_v1 - t_v0;
                st_snap += t_v2 - t_v1;
            }

            // fixpoint?  S_{k+1} == S_k
            var eq_prev = true;
            {
                var k: usize = 0;
                while (k < st.n_heads) : (k += 1) {
                    if (!tsEq(&st.cur[k].ts, &st.prev[k].ts)) {
                        eq_prev = false;
                        break;
                    }
                }
            }
            if (eq_prev) {
                converged = true;
                break;
            }

            // 2-cycle?  S_{k+1} == S_{k-1}
            if (st.have_prev2) {
                var eq_p2 = true;
                var k: usize = 0;
                while (k < st.n_heads) : (k += 1) {
                    if (!tsEq(&st.cur[k].ts, &st.prev2[k].ts)) {
                        eq_p2 = false;
                        break;
                    }
                }
                if (eq_p2) {
                    two_cycle = true;
                    break;
                }
            }

            // rotate S_{k-2} <- S_{k-1} <- S_k <- S_{k+1}
            if (st.have_prev3) freeSnaps(st.prev3[0..st.n_heads]);
            st.prev3 = st.prev2;
            st.prev3_ok = st.prev2_ok;
            st.have_prev3 = st.prev2_ok;
            st.prev2 = st.prev;
            st.prev2_ok = st.prev_ok;
            st.prev = st.cur;
            st.prev_ok = true;
            st.have_prev2 = true;
            st.cur_ok = false; // slots re-snapshotted next round
        }
        statDump();
        if (!converged and !two_cycle) {
            wfsErr("dl_query_wfs_ro: alternating fixpoint did not converge within {d} rounds\n", .{WFS_ROUND_BOUND});
            return DL_WFS_ERR_NO_CONVERGE;
        }
    }

    // ── truth classification + streaming (mode 0: TRUE-only)
    // TRUE = I_* = the smaller fixpoint: S_k itself at a fixpoint, and
    // min(S_k, S_{k+1}) (their intersection — they are comparable) at a
    // 2-cycle.
    {
        var k: usize = 0;
        while (k < st.n_heads) : (k += 1) {
            if (two_cycle and !tsSubset(&st.cur[k].ts, &st.prev[k].ts) and !tsSubset(&st.prev[k].ts, &st.cur[k].ts)) {
                wfsErr("dl_query_wfs_ro: internal error — alternating cycle points are incomparable\n", .{});
                return DL_WFS_ERR_INTERNAL;
            }
        }

        const gi = cloneFindRel(&edb, @ptrCast(goal_rel));
        if (gi < 0) {
            wfsErr("dl_query_wfs_ro: goal '{s}' not found\n", .{@as([*c]const u8, @ptrCast(goal_rel))});
            return -1;
        }

        var is_head = false;
        var slot: usize = 0;
        k = 0;
        while (k < st.n_heads) : (k += 1) {
            if (st.head_idx[k] == gi) {
                is_head = true;
                slot = k;
            }
        }

        var emitted: c_long = 0;
        if (!is_head) {
            // non-head goal: an existing relation's stored facts — the VM
            // never writes it; every stored tuple is plainly TRUE.
            const e = &edb.rels[@intCast(gi)];
            emitted = relation.rel_prefix(e.rel, null, 0, cb, user);
        } else {
            var out_ts: tupleset.tuple_set = undefined;
            var out_owned = false;
            if (two_cycle) {
                out_ts = tsIntersect(&st.prev[slot].ts, &st.cur[slot].ts) orelse return DL_WFS_ERR_OOM;
                out_owned = true;
            } else {
                // fixpoint: cur == prev — stream the final snapshot
                out_ts = st.cur[slot].ts;
                st.cur[slot].ts = std.mem.zeroes(tupleset.tuple_set); // moved
            }
            var ti: c_long = 0;
            while (ti < out_ts.count) : (ti += 1) {
                const t = out_ts.data.? + @as(usize, @intCast(ti)) * out_ts.arity;
                emitted += 1;
                if (cb.?(t, out_ts.arity, user) != 0) break;
            }
            if (out_owned) tupleset.ts_free(&out_ts);
        }
        return emitted;
    }
}

// ─── unit tests ────────────────────────────────────────────────────────────

test "tsEq / tsSubset / tsIntersect basics" {
    var a: tupleset.tuple_set = undefined;
    var b: tupleset.tuple_set = undefined;
    try std.testing.expect(tupleset.ts_init(&a, 1) == 0);
    try std.testing.expect(tupleset.ts_init(&b, 1) == 0);
    _ = tupleset.ts_add(&a, &[_]u32{2});
    _ = tupleset.ts_add(&a, &[_]u32{3});
    tupleset.ts_sort(&a);
    _ = tupleset.ts_add(&b, &[_]u32{3});
    tupleset.ts_sort(&b);
    try std.testing.expect(!tsEq(&a, &b));
    try std.testing.expect(tsSubset(&b, &a));
    try std.testing.expect(!tsSubset(&a, &b));
    var inter = tsIntersect(&a, &b).?;
    try std.testing.expectEqual(@as(c_long, 1), inter.count);
    try std.testing.expectEqual(@as(u32, 3), inter.data.?[0]);
    tupleset.ts_free(&a);
    tupleset.ts_free(&b);
    tupleset.ts_free(&inter);
}

test "enumerateProduct content" {
    var dom: tupleset.tuple_set = undefined;
    try std.testing.expect(tupleset.ts_init(&dom, 1) == 0);
    _ = tupleset.ts_add(&dom, &[_]u32{7});
    _ = tupleset.ts_add(&dom, &[_]u32{9});
    tupleset.ts_sort(&dom);
    var p2: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), enumerateProduct(&dom, 2, &p2));
    try std.testing.expectEqual(@as(c_long, 4), p2.count);
    tupleset.ts_free(&p2);
    tupleset.ts_free(&dom);
}
