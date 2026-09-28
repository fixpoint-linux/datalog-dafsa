//! wfs.zig — well-founded semantics (van Gelder's alternating fixpoint) as
//! a per-query evaluation strategy.
//!
//! This is the THIRD evaluation regime beside the bottom-up semi-naive
//! fixpoint (vm.zig), magic-sets (magic.zig) and topdown/QSQ (topdown.zig).
//! It exists because the existing drivers only evaluate programs whose
//! dependency graph stratifies: the compiler rejects a rule whose body
//! negates a predicate of its own recursion cycle ("unstratifiable
//! program", compiler.zig:1048/1365/1384).  The canonical case is
//!     win(X) :- move(X,Y), !win(Y).
//! whose meaning requires the 3-valued well-founded partial model.
//!
//! NULL-ISOLATION: nothing here changes any existing path.  The driver is
//! reachable ONLY through the exported entry dl_query_wfs_ro (dl.zig);
//! every other entry (dl_load_rules/dl_compile/dl_query_*/magic/topdown)
//! keeps rejecting unstratifiable programs exactly as before — the
//! stratifier itself is untouched.
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
//! Per round the driver (a) rebuilds each complement from the PREVIOUS
//! assignment: wfs_c_P := Active^ar(P) \ prev|P, (b) runs vm_execute on the
//! rewritten program from scratch (head views reset to base; the complement
//! relations are body-only and never reset), (c) snapshots every head as
//! the new assignment and tests fixpoint / 2-cycle.
//!
//! ─── Slice 1 scope (S1) ───────────────────────────────────────────────────
//! Rules: pure relational atoms over EDB + IDB, negation restricted to IDB
//! predicates, safe negation, no aggregates/builtins/lists/regex/variadic/
//! arithmetic.  Truth mode 0 (TRUE-only) is implemented; modes 1/2/3 return
//! DL_WFS_ERR_NOT_IMPLEMENTED rather than a wrong answer.  Not implemented:
//! publish/materialization of WFS results (query-scoped, like magic/topdown).

const std = @import("std");
const c = std.c;

const parser = @import("parser.zig");
const compiler = @import("compiler.zig");
const tupleset = @import("tupleset.zig");

// dl_internal.zig: the shared C-layout mirror of dl_db — the same import
// vm.zig/topdown.zig use (each module's view of the C struct is a distinct
// Zig type over identical bytes; dl.zig holds the comptime layout gate).
const dx = @import("dl_internal.zig");

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

const MAX_ARITY: u8 = 8;
const MAX_HEADS: usize = 64; // dx.dl_db.rels is [64]
const RELK_FIXED: u8 = 0;
const RELK_VARIADIC: u8 = 1;

// ─── extern bindings (same discipline as vm.zig/topdown.zig) ───────────────

// tupleset.zig export fns (not `pub`).
extern "c" fn ts_init(ts: ?*tupleset.tuple_set, arity: u8) c_int;
extern "c" fn ts_free(ts: ?*tupleset.tuple_set) void;
extern "c" fn ts_add(ts: ?*tupleset.tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_sort(ts: ?*tupleset.tuple_set) void;

// compiler.zig / vm.zig / permindex.zig export fns against dx.dl_db.
extern "c" fn compile_rules(
    db: ?*dx.dl_db,
    rules: ?[*]?*parser.rule,
    n_rules: c_int,
    out_rules: ?*?[*]?*compiler.compiled_rule,
    out_n: ?*c_int,
) c_int;
extern "c" fn vm_execute(db: ?*dx.dl_db, rules: ?[*]?*compiler.compiled_rule, n_rules: c_int) c_int;
extern "c" fn permindex_free_all(db: ?*dx.dl_db) void;

// relation.zig export fns reached through dx's opaque relation type.
extern "c" fn rel_create(arity: u8) ?*dx.relation;
extern "c" fn rel_free(rel: ?*dx.relation) void;
extern "c" fn rel_arity(rel: ?*const dx.relation) u8;
extern "c" fn rel_prefix(
    rel: ?*const dx.relation,
    leading: ?[*]const u32,
    k: u8,
    cb: dx.rel_enum_cb,
    user: ?*anyopaque,
) c_long;
extern "c" fn ts_sink_cb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) c_int;
extern "c" fn rel_build_from_tupleset(rel: ?*dx.relation, ts: ?*const tupleset.tuple_set) c_int;

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
    if (ts_init(&out, a.arity) != 0) return null;
    var ai: c_long = 0;
    var bi: c_long = 0;
    const ar = a.arity;
    while (ai < a.count and bi < b.count) {
        const at = a.data.? + @as(usize, @intCast(ai)) * ar;
        const bt = b.data.? + @as(usize, @intCast(bi)) * ar;
        var k: usize = 0;
        while (k < ar and at[k] == bt[k]) k += 1;
        if (k == ar) {
            if (ts_add(&out, at) < 0) {
                ts_free(&out);
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
    ts_sort(&out);
    return out;
}

/// Snapshot a relation's view into a fresh sorted ts.
fn snapRel(rel: ?*const dx.relation, arity: u8) ?tupleset.tuple_set {
    var ts: tupleset.tuple_set = undefined;
    if (ts_init(&ts, arity) != 0) return null;
    if (rel_prefix(rel, null, 0, ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        return null;
    }
    ts_sort(&ts);
    return ts;
}

// ─── eval-clone helpers (local mirrors of dl.zig's evalDb*) ────────────────

fn cloneFindRel(db: *const dx.dl_db, name: [*:0]const u8) c_int {
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (db.rels[i].name != null and strEqZ(@ptrCast(db.rels[i].name.?), name))
            return @intCast(i);
    }
    return -1;
}

/// rel_create-based in-memory declare into the clone (dir stays NULL).
fn cloneDeclareInmem(db: *dx.dl_db, name: [*:0]const u8, arity: u8) c_int {
    if (db.nrels >= MAX_HEADS) return -1;
    if (cloneFindRel(db, name) >= 0) return -1;
    const rel = rel_create(arity) orelse return -1;
    const nm = strdup(name) orelse {
        rel_free(rel);
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
fn cloneDeepcopyView(src: ?*const dx.relation, arity: u8) ?*dx.relation {
    const dst = rel_create(arity) orelse return null;
    var ts: tupleset.tuple_set = undefined;
    if (ts_init(&ts, arity) != 0) {
        rel_free(dst);
        return null;
    }
    if (rel_prefix(src, null, 0, ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        rel_free(dst);
        return null;
    }
    ts_sort(&ts);
    if (rel_build_from_tupleset(dst, &ts) != 0) {
        ts_free(&ts);
        rel_free(dst);
        return null;
    }
    ts_free(&ts);
    return dst;
}

/// Free the eval clone: the clone's perm indexes, its fresh relations +
/// names (idx >= n_aliased), and the deep-copied collision heads (owned).
fn cloneFree(db: *dx.dl_db, n_aliased: usize, owned: *const [MAX_HEADS]u8) void {
    permindex_free_all(db);
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (i >= n_aliased) {
            if (db.rels[i].rel) |r| rel_free(r);
            if (db.rels[i].name) |n| c.free(@ptrCast(n));
        } else if (owned[i] != 0) {
            if (db.rels[i].rel) |r| rel_free(r);
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

// ─── active domain + complement enumeration ────────────────────────────────

/// Active domain: every column value of every fixed-arity relation's
/// current view, plus every constant (symbol or integer) appearing in the
/// rules.  Symbol constants are interned with the INSERTING intern, exactly
/// as the compiler's token_const does (compiler.zig:460), so the domain is
/// independent of prior interner state.  Datalog without function symbols
/// derives heads only from these values; safety makes every rewritten
/// complement atom a bound lookup over a subset of Active^arity, so the
/// complement content outside Active^arity is never probed.
fn buildDomain(db: *const dx.dl_db, rules: [*]?*parser.rule, n_rules: c_int, dom: *tupleset.tuple_set) c_int {
    if (ts_init(dom, 1) != 0) return -1;
    var ri: usize = 0;
    while (ri < db.nrels) : (ri += 1) {
        if (db.rels[ri].kind != RELK_FIXED) continue; // variadic values are never probed (S1)
        const rel = db.rels[ri].rel orelse continue;
        const ar = rel_arity(rel);
        if (ar == 0) continue;
        var ts: tupleset.tuple_set = undefined;
        if (ts_init(&ts, ar) != 0) return -1;
        if (rel_prefix(rel, null, 0, ts_sink_cb, &ts) < 0) {
            ts_free(&ts);
            return -1;
        }
        var ti: c_long = 0;
        while (ti < ts.count) : (ti += 1) {
            const row = ts.data.? + @as(usize, @intCast(ti)) * ar;
            var k: u8 = 0;
            while (k < ar) : (k += 1) {
                const v: [*]const u32 = @ptrCast(&row[k]);
                _ = ts_add(dom, v);
            }
        }
        ts_free(&ts);
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
                _ = ts_add(dom, vp);
            }
        }
    }
    ts_sort(dom);
    return 0;
}

/// out := Active^arity (cartesian product; odometer order, then sorted).
fn enumerateProduct(dom: *const tupleset.tuple_set, arity: u8, out: *tupleset.tuple_set) c_int {
    const n: c_long = dom.count;
    if (n == 0) return ts_init(out, arity); // empty domain → empty complement
    var total: c_long = 1;
    var a: u8 = 0;
    while (a < arity) : (a += 1) {
        total *= n;
        if (total > WFS_COMPLEMENT_CAP) return -1; // too large — clean reject
    }
    if (ts_init(out, arity) != 0) return -1;
    var idx: [MAX_ARITY]c_long = [_]c_long{0} ** MAX_ARITY;
    var row: [MAX_ARITY]u32 = undefined;
    var t: c_long = 0;
    while (t < total) : (t += 1) {
        a = 0;
        while (a < arity) : (a += 1)
            row[a] = dom.data.?[@intCast(idx[a])];
        if (ts_add(out, &row) < 0) return -1;
        var d: u8 = arity; // odometer increment
        while (d > 0) {
            d -= 1;
            idx[d] += 1;
            if (idx[d] < n) break;
            idx[d] = 0;
        }
    }
    ts_sort(out);
    return 0;
}

// ─── driver state ──────────────────────────────────────────────────────────

const CompRel = struct {
    pred_idx: c_int, // clone index of the negated predicate P
    comp_idx: c_int, // clone index of wfs_c<k>_P
    arity: u8,
    name: [96:0]u8 = undefined,
    full: tupleset.tuple_set, // Active^arity (sorted)
};

const HeadSnap = struct {
    rel_idx: c_int,
    arity: u8,
    ts: tupleset.tuple_set,
};

const WfsState = struct {
    edb: *dx.dl_db,
    n_aliased: usize,
    owned: [MAX_HEADS]u8,
    dom: tupleset.tuple_set,
    comps: [MAX_HEADS]CompRel = undefined,
    n_comps: usize = 0,
    head_idx: [MAX_HEADS]c_int = undefined,
    n_heads: usize = 0,
    prev: [MAX_HEADS]HeadSnap = undefined,
    prev2: [MAX_HEADS]HeadSnap = undefined,
    cur: [MAX_HEADS]HeadSnap = undefined,
    have_prev2: bool = false,
    crules: ?[*]?*compiler.compiled_rule = null,
    n_crules: c_int = 0,
    dom_ok: bool = false,
    prev_ok: bool = false, // prev[0..n_heads] initialized
    prev2_ok: bool = false,
    cur_ok: bool = false,
    comps_ok: bool = false,
};

fn freeSnaps(snaps: []HeadSnap) void {
    for (snaps) |*s| ts_free(&s.ts);
}

fn teardown(st: *WfsState) void {
    if (st.dom_ok) ts_free(&st.dom);
    if (st.comps_ok) {
        var i: usize = 0;
        while (i < st.n_comps) : (i += 1) ts_free(&st.comps[i].full);
    }
    if (st.prev_ok) freeSnaps(st.prev[0..st.n_heads]);
    if (st.prev2_ok) freeSnaps(st.prev2[0..st.n_heads]);
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
/// as dx.dl_db (dl.zig holds the comptime gate).
pub fn wfs_eval_query(
    db_opaque: ?*anyopaque,
    rules: ?[*]?*parser.rule,
    n_rules: c_int,
    goal_rel: [*c]const u8,
    truth: c_int,
    cb: dx.dl_tuple_cb,
    user: ?*anyopaque,
) c_long {
    const d: *dx.dl_db = @ptrCast(@alignCast(db_opaque orelse return -1));
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
    var edb: dx.dl_db = std.mem.zeroes(dx.dl_db);
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

                var is_head = false;
                var k: usize = 0;
                while (k < st.n_heads) : (k += 1)
                    if (st.head_idx[k] == pidx) {
                        is_head = true;
                        break;
                    };
                if (!is_head) {
                    wfsErr("dl_query_wfs_ro: rejected — negated atom '{s}' is not a rule head (S1 negates IDB predicates only)\n", .{pname});
                    return DL_WFS_ERR_REJECTED;
                }

                var ci: usize = 0;
                while (ci < st.n_comps) : (ci += 1)
                    if (st.comps[ci].pred_idx == pidx) break;
                if (ci < st.n_comps) continue; // complement already created

                if (st.n_comps >= MAX_HEADS) return DL_WFS_ERR_OOM;
                var cr = CompRel{
                    .pred_idx = pidx,
                    .comp_idx = -1,
                    .arity = edb.rels[@intCast(pidx)].arity,
                    .name = undefined,
                    .full = std.mem.zeroes(tupleset.tuple_set),
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
    if (compile_rules(&edb, rs, n_rules, &st.crules, &st.n_crules) != 0) {
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
        var round: c_int = 0;
        while (round < WFS_ROUND_BOUND) : (round += 1) {
            // complements := Active^ar \ prev|P  (materialized into the
            // stored view — what the rewritten atom's SCAN/LOOKUP reads)
            var ci: usize = 0;
            while (ci < st.n_comps) : (ci += 1) {
                const cp = &st.comps[ci];
                const pv_snap = snapRel(edb.rels[@intCast(cp.pred_idx)].rel, cp.arity) orelse return DL_WFS_ERR_OOM;
                var pv: tupleset.tuple_set = pv_snap;
                var cts: tupleset.tuple_set = undefined;
                if (ts_init(&cts, cp.arity) != 0) {
                    ts_free(&pv);
                    return DL_WFS_ERR_OOM;
                }
                // cts := full \ pv  (both sorted — merge walk)
                var fi: c_long = 0;
                var vi: c_long = 0;
                const ar = cp.arity;
                while (fi < cp.full.count) {
                    const ft = cp.full.data.? + @as(usize, @intCast(fi)) * ar;
                    var skip = false;
                    while (vi < pv.count) {
                        const vt = pv.data.? + @as(usize, @intCast(vi)) * ar;
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
                    if (!skip) _ = ts_add(&cts, ft);
                    fi += 1;
                }
                ts_sort(&cts);
                const rc = rel_build_from_tupleset(edb.rels[@intCast(cp.comp_idx)].rel, &cts);
                ts_free(&cts);
                ts_free(&pv);
                if (rc != 0) {
                    wfsErr("dl_query_wfs_ro: complement materialization failed\n", .{});
                    return -1;
                }
            }

            // Γ(S_k) from scratch: vm_execute resets head views to base
            if (vm_execute(&edb, st.crules, st.n_crules) != 0) {
                wfsErr("dl_query_wfs_ro: evaluation failed\n", .{});
                return -1;
            }

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

            // rotate S_{k-1} <- S_k <- S_{k+1}
            if (st.have_prev2) freeSnaps(st.prev2[0..st.n_heads]);
            st.prev2 = st.prev;
            st.prev2_ok = st.prev_ok;
            st.prev = st.cur;
            st.prev_ok = true;
            st.have_prev2 = true;
            st.cur_ok = false; // slots re-snapshotted next round
        }
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
            emitted = rel_prefix(e.rel, null, 0, cb, user);
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
            if (out_owned) ts_free(&out_ts);
        }
        return emitted;
    }
}

// ─── unit tests ────────────────────────────────────────────────────────────

test "tsEq / tsSubset / tsIntersect basics" {
    var a: tupleset.tuple_set = undefined;
    var b: tupleset.tuple_set = undefined;
    try std.testing.expect(ts_init(&a, 1) == 0);
    try std.testing.expect(ts_init(&b, 1) == 0);
    _ = ts_add(&a, &[_]u32{2});
    _ = ts_add(&a, &[_]u32{3});
    ts_sort(&a);
    _ = ts_add(&b, &[_]u32{3});
    ts_sort(&b);
    try std.testing.expect(!tsEq(&a, &b));
    try std.testing.expect(tsSubset(&b, &a));
    try std.testing.expect(!tsSubset(&a, &b));
    var inter = tsIntersect(&a, &b).?;
    try std.testing.expectEqual(@as(c_long, 1), inter.count);
    try std.testing.expectEqual(@as(u32, 3), inter.data.?[0]);
    ts_free(&a);
    ts_free(&b);
    ts_free(&inter);
}

test "enumerateProduct content" {
    var dom: tupleset.tuple_set = undefined;
    try std.testing.expect(ts_init(&dom, 1) == 0);
    _ = ts_add(&dom, &[_]u32{7});
    _ = ts_add(&dom, &[_]u32{9});
    ts_sort(&dom);
    var p2: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), enumerateProduct(&dom, 2, &p2));
    try std.testing.expectEqual(@as(c_long, 4), p2.count);
    ts_free(&p2);
    ts_free(&dom);
}
