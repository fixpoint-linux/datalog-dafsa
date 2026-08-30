//! magic.zig — port of src/magic.c (magic-sets AST→AST adornment rewrite,
//! M8 v2 multi-predicate slice / SIPS body reordering / negation+aggregate
//! slice / adornment-closure fixpoint slice).
//!
//! Strangler-hybrid ABI: `magic_adornment`, `magic_decl` and `magic_program`
//! are CONCRETE C structs — still-C src/dl.c dereferences `prog.decls[d].name`,
//! `prog.decls[d].arity`, `prog.adorned_goal`, `prog.rules`, `prog.n_rules`,
//! `prog.n_decls` and calls magic_program_free — so they are `extern struct`s
//! that MUST stay byte-for-byte identical to src/magic.h.  The AST node types
//! (token/expr/atom/rule) come from parser.zig (already ported, extern
//! structs); the synthesized program is built with the SAME allocation
//! discipline as parser.c (calloc'd atom/rule, strdup'd pred/text, one token*
//! per arg) so the output is rule_free()-compatible with C callers.
//!
//! The transform is a PURE AST→AST rewrite: no dl_db access, no globals.
//! Constants are already interned and copied opaquely as tokens.  The `ir`
//! and `vals` parameters are unused (validated only), matching the oracle.
//!
//! Oracle: src/magic.c (never modified).

const std = @import("std");
const c = std.c;

const parser = @import("parser.zig");

// glibc strdup — not re-exported by std.c; the C oracle uses it for every
// owned AST string.
extern "c" fn strdup(s: [*:0]const u8) ?[*:0]u8;

// parser.zig exports (rule_free/expr_free/expr_clone are `export fn`, not
// `pub`, so reach them through raw extern bindings like vm.zig does).
extern "c" fn rule_free(r: ?*parser.rule) void;
extern "c" fn expr_free(e: ?*parser.expr) void;
extern "c" fn expr_clone(e: ?*const parser.expr) ?*parser.expr;

// ─── Public C-ABI structs (src/magic.h) ────────────────────────────────────

/// typedef struct { char pred[64]; uint8_t arity; char adorn[9]; }
pub const magic_adornment = extern struct {
    pred: [64]u8,
    arity: u8,
    adorn: [9]u8,
};

/// typedef struct { char name[64]; uint8_t arity; }
pub const magic_decl = extern struct {
    name: [64]u8,
    arity: u8,
};

/// typedef struct { rule **rules; int n_rules; magic_decl *decls;
///                   int n_decls; char adorned_goal[80]; uint8_t goal_arity; }
pub const magic_program = extern struct {
    rules: ?[*]?*parser.rule,
    n_rules: c_int,
    decls: ?[*]magic_decl,
    n_decls: c_int,
    adorned_goal: [80]u8,
    goal_arity: u8,
};

// Comptime gate: our extern layouts must be byte-identical to the C header
// (translate-c produces the C struct, so size/offset equality proves it).
const mx = @cImport({
    @cInclude("magic.h");
});

comptime {
    std.debug.assert(@sizeOf(magic_decl) == @sizeOf(mx.magic_decl));
    std.debug.assert(@offsetOf(magic_decl, "arity") == @offsetOf(mx.magic_decl, "arity"));
    std.debug.assert(@sizeOf(magic_adornment) == @sizeOf(mx.magic_adornment));
    std.debug.assert(@offsetOf(magic_adornment, "arity") == @offsetOf(mx.magic_adornment, "arity"));
    std.debug.assert(@offsetOf(magic_adornment, "adorn") == @offsetOf(mx.magic_adornment, "adorn"));
    std.debug.assert(@sizeOf(magic_program) == @sizeOf(mx.magic_program));
    std.debug.assert(@offsetOf(magic_program, "n_rules") == @offsetOf(mx.magic_program, "n_rules"));
    std.debug.assert(@offsetOf(magic_program, "decls") == @offsetOf(mx.magic_program, "decls"));
    std.debug.assert(@offsetOf(magic_program, "n_decls") == @offsetOf(mx.magic_program, "n_decls"));
    std.debug.assert(@offsetOf(magic_program, "adorned_goal") == @offsetOf(mx.magic_program, "adorned_goal"));
    std.debug.assert(@offsetOf(magic_program, "goal_arity") == @offsetOf(mx.magic_program, "goal_arity"));
}

// ─── Constants ──────────────────────────────────────────────────────────────

const MAX_ADORN_VARIANTS = 64;
const MAX_RELS = 64; // dl_internal.h (the only symbol magic.c pulls from it)

// ─── Small string helpers (snprintf "%s" / strcmp semantics) ───────────────

fn span64(arr: *const [64]u8) []const u8 {
    return std.mem.span(@as([*:0]const u8, @ptrCast(arr)));
}
fn span9(arr: *const [9]u8) []const u8 {
    return std.mem.span(@as([*:0]const u8, @ptrCast(arr)));
}
fn span80(arr: *const [80]u8) []const u8 {
    return std.mem.span(@as([*:0]const u8, @ptrCast(arr)));
}
fn span86(arr: *const [86]u8) []const u8 {
    return std.mem.span(@as([*:0]const u8, @ptrCast(arr)));
}

/// strcmp(s, lit) == 0 for a NUL-terminated C string vs a literal.
fn cstrEq(s: [*:0]const u8, lit: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(s), lit);
}

/// strcmp(s, arr) == 0 for a C string vs a NUL-terminated [64]u8 array.
fn cstrEqArr(s: [*:0]const u8, arr: *const [64]u8) bool {
    return std.mem.eql(u8, std.mem.span(s), span64(arr));
}

/// Mimic `snprintf(buf, buf.len, "%s", s)`: truncate + NUL-terminate, return
/// the would-be length (strlen).  buf.len must be > 0.
fn snprintStr(buf: []u8, s: ?[*:0]const u8) c_int {
    const n: usize = if (s) |sp| std.mem.span(sp).len else 0;
    if (buf.len > 0) {
        const to_copy = @min(n, buf.len - 1);
        if (s) |sp| @memcpy(buf[0..to_copy], sp[0..to_copy]);
        buf[to_copy] = 0;
    }
    return @intCast(n);
}

/// snprintf into the caller's reject buffer with C-style truncation.
fn rejectFmt(reason: ?[*]u8, sz: usize, comptime fmt: []const u8, args: anytype) c_int {
    if (reason != null and sz > 0) {
        var tmp: [512]u8 = undefined;
        const s = std.fmt.bufPrint(&tmp, fmt, args) catch tmp[0..0];
        const n = @min(s.len, sz - 1);
        @memcpy(reason.?[0..n], s[0..n]);
        reason.?[n] = 0;
    }
    return -1;
}

/// Set the reject reason to "out of memory" (snprintf semantics).
fn setOom(err: ?[*]u8, errsz: usize) void {
    if (err != null and errsz > 0) {
        _ = snprintStr(err.?[0..errsz], "out of memory");
    }
}

// ─── Builtin atom classification (mirror compiler.c) ───────────────────────

fn isComparisonAtom(A: ?*const parser.atom) bool {
    const a = A orelse return false;
    const p = a.pred orelse return false;
    return cstrEq(p, "<") or cstrEq(p, "<=") or cstrEq(p, ">") or
        cstrEq(p, ">=") or cstrEq(p, "!=");
}

fn isArithAtom(A: ?*const parser.atom) bool {
    const a = A orelse return false;
    const p = a.pred orelse return false;
    return cstrEq(p, "=") and a.arith != null;
}

fn isEqualityAtom(A: ?*const parser.atom) bool {
    const a = A orelse return false;
    const p = a.pred orelse return false;
    return cstrEq(p, "=") and a.nargs == 2 and a.arith == null;
}

fn isStrProducingAtom(A: ?*const parser.atom) bool {
    const a = A orelse return false;
    const p = a.pred orelse return false;
    return cstrEq(p, "concat") or cstrEq(p, "length") or
        cstrEq(p, "lower") or cstrEq(p, "upper");
}

fn isStrFilterAtom(A: ?*const parser.atom) bool {
    const a = A orelse return false;
    const p = a.pred orelse return false;
    return cstrEq(p, "prefix") or cstrEq(p, "suffix") or cstrEq(p, "contains");
}

// ─── Growable buffers ───────────────────────────────────────────────────────

const rule_vec = struct { v: ?[*]?*parser.rule, n: c_int, cap: c_int };
const decl_vec = struct { v: ?[*]magic_decl, n: c_int, cap: c_int };
const name_set = struct { v: ?[*]?[*:0]const u8, n: c_int, cap: c_int }; // borrowed var names

fn rvPush(v: *rule_vec, r: ?*parser.rule) c_int {
    if (v.n >= v.cap) {
        const nc: c_int = if (v.cap != 0) v.cap * 2 else 8;
        const mem = c.realloc(@ptrCast(v.v), @as(usize, @intCast(nc)) * @sizeOf(?*parser.rule));
        if (mem == null) return -1;
        v.v = @ptrCast(@alignCast(mem));
        v.cap = nc;
    }
    v.v.?[@intCast(v.n)] = r;
    v.n += 1;
    return 0;
}

fn dvPush(v: *decl_vec, name: ?[*:0]const u8, arity: u8) c_int {
    if (v.n >= v.cap) {
        const nc: c_int = if (v.cap != 0) v.cap * 2 else 4;
        const mem = c.realloc(if (v.v) |p| p else null, @as(usize, @intCast(nc)) * @sizeOf(magic_decl));
        if (mem == null) return -1;
        v.v = @ptrCast(@alignCast(mem));
        v.cap = nc;
    }
    _ = snprintStr(v.v.?[@intCast(v.n)].name[0..], name);
    v.v.?[@intCast(v.n)].arity = arity;
    v.n += 1;
    return 0;
}

fn dvContains(v: *const decl_vec, name: ?[*:0]const u8) c_int {
    if (name == null) return 0;
    var i: c_int = 0;
    while (i < v.n) : (i += 1) {
        if (std.mem.eql(u8, span64(&v.v.?[@intCast(i)].name), std.mem.span(name.?))) return 1;
    }
    return 0;
}

fn nsContains(s: *const name_set, n: ?[*:0]const u8) c_int {
    if (n == null) return 0;
    var i: c_int = 0;
    while (i < s.n) : (i += 1) {
        if (std.mem.eql(u8, std.mem.span(s.v.?[@intCast(i)].?), std.mem.span(n.?))) return 1;
    }
    return 0;
}

fn nsAdd(s: *name_set, n: ?[*:0]const u8) c_int {
    if (nsContains(s, n) != 0) return 0;
    if (s.n >= s.cap) {
        const nc: c_int = if (s.cap != 0) s.cap * 2 else 8;
        const mem = c.realloc(@ptrCast(s.v), @as(usize, @intCast(nc)) * @sizeOf(?[*:0]const u8));
        if (mem == null) return -1;
        s.v = @ptrCast(@alignCast(mem));
        s.cap = nc;
    }
    s.v.?[@intCast(s.n)] = n;
    s.n += 1;
    return 0;
}

// Count bound/unbound variable operands of an arithmetic expr tree (mirrors
// sips_atom_score's score/n_new semantics for arithmetic atoms).
fn exprOperandVars(e: ?*const parser.expr, bound: *const name_set, s: *c_int, nn: *c_int) void {
    const ep = e orelse return;
    switch (ep.kind) {
        parser.EX_VAR => {
            if (nsContains(bound, ep.@"var") != 0) s.* += 1 else nn.* += 1;
        },
        parser.EX_BINOP => {
            exprOperandVars(ep.l, bound, s, nn);
            exprOperandVars(ep.r, bound, s, nn);
        },
        else => {},
    }
}

// Score the VARIABLE operands of a string builtin (mirror expr_operand_vars/
// sips_atom_score semantics).
fn strOperandVars(A: ?*const parser.atom, start: c_int, bound: *const name_set, s: *c_int, nn: *c_int) void {
    const a = A orelse return;
    var j: c_int = start;
    while (j < a.nargs) : (j += 1) {
        const t = a.args.?[@intCast(j)].?;
        if (t.kind != parser.TOK_VAR) continue;
        if (nsContains(bound, t.text) != 0) s.* += 1 else nn.* += 1;
    }
}

// ─── AST allocation helpers (mirror parser.c discipline) ───────────────────

fn tokFreeLocal(t: ?*parser.token) void {
    const tp = t orelse return;
    if (tp.children) |ch| {
        var i: c_int = 0;
        while (i < tp.nchildren) : (i += 1) tokFreeLocal(ch[@intCast(i)]);
        c.free(@ptrCast(ch));
    }
    tokFreeLocal(tp.tail);
    if (tp.text) |tx| c.free(@ptrCast(tx));
    c.free(tp);
}

// Deep-copy a token.  NOTE: unlike parser.zig's tokDup, the oracle's tok_dup
// does NOT copy off/line/col (they stay 0 from calloc).
fn tokDup(t: *const parser.token) ?*parser.token {
    const n: *parser.token = @ptrCast(@alignCast(c.calloc(1, @sizeOf(parser.token)) orelse return null));
    n.kind = t.kind;
    n.ival = t.ival;
    if (t.text) |tx| {
        n.text = strdup(tx) orelse {
            c.free(n);
            return null;
        };
    }
    if (t.nchildren > 0) {
        n.children = @ptrCast(@alignCast(c.calloc(@intCast(t.nchildren), @sizeOf(?*parser.token))));
        if (n.children == null) {
            if (n.text) |tx2| c.free(@ptrCast(tx2));
            c.free(n);
            return null;
        }
        n.nchildren = t.nchildren;
        var i: c_int = 0;
        while (i < t.nchildren) : (i += 1) {
            n.children.?[@intCast(i)] = tokDup(t.children.?[@intCast(i)].?);
            if (n.children.?[@intCast(i)] == null) {
                tokFreeLocal(n);
                return null;
            }
        }
    }
    if (t.tail) |tl| {
        n.tail = tokDup(tl);
        if (n.tail == null) {
            tokFreeLocal(n);
            return null;
        }
    }
    return n;
}

// Reclaim an atom exactly like parser.c's static atom_free (agg_op freed only
// when aggregate is set, matching the oracle AND parser.zig's atomFree).
fn atomFreeLocal(a: ?*parser.atom) void {
    const ap = a orelse return;
    if (ap.pred) |pd| c.free(@ptrCast(pd));
    if (ap.pattern) |pt| c.free(@ptrCast(pt));
    if (ap.args) |ar| {
        var i: c_int = 0;
        while (i < ap.nargs) : (i += 1) tokFreeLocal(ar[@intCast(i)]);
        c.free(@ptrCast(ar));
    }
    if (ap.aggregate != 0)
        tokFreeLocal(ap.agg_op);
    expr_free(ap.arith);
    c.free(ap);
}

fn atomsFree(arr: ?[*]?*parser.atom, n: c_int) void {
    const a = arr orelse return;
    var i: c_int = 0;
    while (i < n) : (i += 1) atomFreeLocal(a[@intCast(i)]);
    c.free(@ptrCast(a));
}

fn tokensFree(arr: ?[*]?*parser.token, n: c_int) void {
    const a = arr orelse return;
    var i: c_int = 0;
    while (i < n) : (i += 1) tokFreeLocal(a[@intCast(i)]);
    c.free(@ptrCast(a));
}

// Deep-copy an atom, optionally renaming its predicate.  NOTE: off/line/col
// are NOT copied (stay 0), matching the oracle's atom_copy.
fn atomCopy(src: *const parser.atom, new_pred: ?[*:0]const u8) ?*parser.atom {
    const a: *parser.atom = @ptrCast(@alignCast(c.calloc(1, @sizeOf(parser.atom)) orelse return null));
    const np: [*:0]const u8 = (new_pred orelse src.pred).?;
    a.pred = strdup(np) orelse {
        c.free(a);
        return null;
    };
    a.negated = src.negated;
    a.aggregate = src.aggregate;
    if (src.pattern) |pt| {
        a.pattern = strdup(pt) orelse {
            atomFreeLocal(a);
            return null;
        };
    }
    a.pattern_col = src.pattern_col;
    if (src.agg_op) |op| {
        a.agg_op = tokDup(op) orelse {
            atomFreeLocal(a);
            return null;
        };
    }
    if (src.arith) |ar| {
        a.arith = expr_clone(ar) orelse {
            atomFreeLocal(a);
            return null;
        };
    }
    if (src.nargs > 0) {
        a.args = @ptrCast(@alignCast(c.calloc(@intCast(src.nargs), @sizeOf(?*parser.token))));
        if (a.args == null) {
            atomFreeLocal(a);
            return null;
        }
        a.nargs = src.nargs;
        var i: c_int = 0;
        while (i < src.nargs) : (i += 1) {
            a.args.?[@intCast(i)] = tokDup(src.args.?[@intCast(i)].?);
            if (a.args.?[@intCast(i)] == null) {
                atomFreeLocal(a);
                return null;
            }
        }
    }
    return a;
}

// Build a fresh atom "pred" whose args are the 'b' positions of `src` under
// `adorn`.  Used for magic guards and magic-rule heads.
fn buildBoundAtom(src: *const parser.atom, pred: [*:0]const u8, adorn: [*:0]const u8) ?*parser.atom {
    var nb: c_int = 0;
    var i: c_int = 0;
    while (i < src.nargs) : (i += 1) {
        if (adorn[@intCast(i)] == 'b') nb += 1;
    }

    const a: *parser.atom = @ptrCast(@alignCast(c.calloc(1, @sizeOf(parser.atom)) orelse return null));
    a.pred = strdup(pred) orelse {
        c.free(a);
        return null;
    };

    if (nb > 0) {
        const args_raw = c.calloc(@intCast(nb), @sizeOf(?*parser.token));
        if (args_raw == null) {
            if (a.pred) |p| c.free(@ptrCast(p));
            c.free(a);
            return null;
        }
        const args: [*]?*parser.token = @ptrCast(@alignCast(args_raw));
        var k: c_int = 0;
        var ii: c_int = 0;
        while (ii < src.nargs) : (ii += 1) {
            if (adorn[@intCast(ii)] == 'b') {
                args[@intCast(k)] = tokDup(src.args.?[@intCast(ii)].?);
                if (args[@intCast(k)] == null) {
                    tokensFree(args, k);
                    if (a.pred) |p| c.free(@ptrCast(p));
                    c.free(a);
                    return null;
                }
                k += 1;
            }
        }
        a.args = args;
        a.nargs = nb;
    }
    return a;
}

fn makeRule(head: ?*parser.atom, body: ?[*]?*parser.atom, nbody: c_int, has_neg: c_int, has_agg: c_int) ?*parser.rule {
    const r: *parser.rule = @ptrCast(@alignCast(c.calloc(1, @sizeOf(parser.rule)) orelse return null));
    r.head = head;
    r.body = body;
    r.nbody = nbody;
    r.has_negation = has_neg;
    r.has_aggregate = has_agg;
    return r;
}

// ─── Adornment helpers ──────────────────────────────────────────────────────

// Write "b"*k ++ "f"*(arity-k) into buf (buf >= arity+1).
fn makeAdornment(buf: [*]u8, arity: u8, k: u8) void {
    var i: u8 = 0;
    while (i < k) : (i += 1) buf[i] = 'b';
    while (i < arity) : (i += 1) buf[i] = 'f';
    buf[arity] = 0;
}

// Compute the left-to-right SIPS adornment of atom A given the current bound
// variable set.  Constants are 'b'; vars in bound are 'b'.
fn atomAdornment(A: *const parser.atom, bound: *const name_set, beta: [*]u8) void {
    var j: c_int = 0;
    while (j < A.nargs) : (j += 1) {
        const t = A.args.?[@intCast(j)].?;
        if (t.kind == parser.TOK_INT or t.kind == parser.TOK_IDENT) {
            beta[@intCast(j)] = 'b';
        } else if (t.kind == parser.TOK_VAR and nsContains(bound, t.text) != 0) {
            beta[@intCast(j)] = 'b';
        } else {
            beta[@intCast(j)] = 'f';
        }
    }
    beta[@intCast(A.nargs)] = 0;
}

// ─── IDB predicate table (name + arity) ────────────────────────────────────

const pred_info = extern struct { pred: [64]u8, arity: u8 };
const pred_vec = struct { v: ?[*]pred_info, n: c_int, cap: c_int };

fn predFind(v: *const pred_vec, pred: ?[*:0]const u8) c_int {
    if (pred == null) return -1;
    var i: c_int = 0;
    while (i < v.n) : (i += 1) {
        if (std.mem.eql(u8, span64(&v.v.?[@intCast(i)].pred), std.mem.span(pred.?))) return i;
    }
    return -1;
}

fn predPush(v: *pred_vec, pred: ?[*:0]const u8, arity: u8) c_int {
    if (predFind(v, pred) >= 0) return 0;
    if (v.n >= v.cap) {
        const nc: c_int = if (v.cap != 0) v.cap * 2 else 8;
        const mem = c.realloc(if (v.v) |p| p else null, @as(usize, @intCast(nc)) * @sizeOf(pred_info));
        if (mem == null) return -1;
        v.v = @ptrCast(@alignCast(mem));
        v.cap = nc;
    }
    v.v.?[@intCast(v.n)] = std.mem.zeroes(pred_info);
    _ = snprintStr(v.v.?[@intCast(v.n)].pred[0..], pred);
    v.v.?[@intCast(v.n)].arity = arity;
    v.n += 1;
    return 0;
}

// ─── Adornment-variant multimap (predicate, adornment) → names ─────────────

const pred_adorn_variant = extern struct {
    pred: [64]u8, // original predicate name
    arity: u8, // natural arity
    adorn: [9]u8, // this variant's adornment
    adorned_name: [80]u8, // "<pred>__<adorn>"
    magic_name: [86]u8, // "magic_<pred>__<adorn>"
    is_goal_variant: c_int, // 1 if this is the goal's user-requested variant
};

const pa_vec = struct { v: ?[*]pred_adorn_variant, n: c_int, cap: c_int };

// Fill in the derived adorned/magic predicate names; -1 on overflow.
fn setPredNames(p: *pred_adorn_variant) c_int {
    const pred = span64(&p.pred);
    const adorn = span9(&p.adorn);

    const n1 = pred.len + 2 + adorn.len;
    if (n1 >= p.adorned_name.len) return -1;
    var i: usize = 0;
    @memcpy(p.adorned_name[0..pred.len], pred);
    i = pred.len;
    p.adorned_name[i] = '_';
    p.adorned_name[i + 1] = '_';
    i += 2;
    @memcpy(p.adorned_name[i .. i + adorn.len], adorn);
    i += adorn.len;
    p.adorned_name[i] = 0;

    const n2 = 6 + pred.len + 2 + adorn.len; // "magic_" is 6 chars
    if (n2 >= p.magic_name.len) return -1;
    @memcpy(p.magic_name[0..6], "magic_");
    @memcpy(p.magic_name[6 .. 6 + pred.len], pred);
    i = 6 + pred.len;
    p.magic_name[i] = '_';
    p.magic_name[i + 1] = '_';
    i += 2;
    @memcpy(p.magic_name[i .. i + adorn.len], adorn);
    i += adorn.len;
    p.magic_name[i] = 0;
    return 0;
}

fn paFind(v: *const pa_vec, pred: ?[*:0]const u8, adorn: ?[*:0]const u8) c_int {
    if (pred == null or adorn == null) return -1;
    var i: c_int = 0;
    while (i < v.n) : (i += 1) {
        if (std.mem.eql(u8, span64(&v.v.?[@intCast(i)].pred), std.mem.span(pred.?)) and
            std.mem.eql(u8, span9(&v.v.?[@intCast(i)].adorn), std.mem.span(adorn.?))) return i;
    }
    return -1;
}

fn paFindAny(v: *const pa_vec, pred: ?[*:0]const u8) c_int {
    if (pred == null) return 0;
    var i: c_int = 0;
    while (i < v.n) : (i += 1) {
        if (std.mem.eql(u8, span64(&v.v.?[@intCast(i)].pred), std.mem.span(pred.?))) return 1;
    }
    return 0;
}

fn paPush(v: *pa_vec, pred: ?[*:0]const u8, arity: u8, adorn: ?[*:0]const u8) c_int {
    const qi = paFind(v, pred, adorn);
    if (qi >= 0) return qi;
    if (v.n >= v.cap) {
        const nc: c_int = if (v.cap != 0) v.cap * 2 else 8;
        const mem = c.realloc(if (v.v) |p| p else null, @as(usize, @intCast(nc)) * @sizeOf(pred_adorn_variant));
        if (mem == null) return -1;
        v.v = @ptrCast(@alignCast(mem));
        v.cap = nc;
    }
    v.v.?[@intCast(v.n)] = std.mem.zeroes(pred_adorn_variant);
    const entry = &v.v.?[@intCast(v.n)];
    _ = snprintStr(entry.pred[0..], pred);
    entry.arity = arity;
    _ = snprintStr(entry.adorn[0..], adorn);
    if (setPredNames(entry) != 0) return -1;
    const ret = v.n;
    v.n += 1;
    return ret;
}

// Adorned name for a body atom, or its own pred if it has no variant (EDB
// atom, negated atom, aggregate atom).  See renamed_pred in the oracle.
fn renamedPred(A: ?*const parser.atom, beta: ?[*:0]const u8, adorns: *const pa_vec) ?[*:0]const u8 {
    const a = A orelse return null;
    if (a.negated != 0 or a.aggregate != 0) return a.pred;
    if (beta == null or beta.?[0] == 0) return a.pred;
    const qi = paFind(adorns, a.pred, beta.?);
    if (qi < 0) return a.pred;
    return @as([*:0]const u8, @ptrCast(&adorns.v.?[@intCast(qi)].adorned_name));
}

// ─── Predicate dependency graph + acyclic-SCC check ────────────────────────

const dep_edge = extern struct { from: c_int, to: c_int };
const edge_vec = struct { v: ?[*]dep_edge, n: c_int, cap: c_int };

fn evPush(v: *edge_vec, from: c_int, to: c_int) c_int {
    if (v.n >= v.cap) {
        const nc: c_int = if (v.cap != 0) v.cap * 2 else 16;
        const mem = c.realloc(if (v.v) |p| p else null, @as(usize, @intCast(nc)) * @sizeOf(dep_edge));
        if (mem == null) return -1;
        v.v = @ptrCast(@alignCast(mem));
        v.cap = nc;
    }
    v.v.?[@intCast(v.n)].from = from;
    v.v.?[@intCast(v.n)].to = to;
    v.n += 1;
    return 0;
}

const scc_ctx = struct {
    n: c_int,
    adj: ?[*]?[*]u8,
    index: ?[*]c_int,
    low: ?[*]c_int,
    onstack: ?[*]c_int,
    stack: ?[*]c_int,
    idx: c_int,
    top: c_int,
    has_big: c_int,
};

fn sccVisit(cctx: *scc_ctx, u: c_int) void {
    cctx.index.?[@intCast(u)] = cctx.idx;
    cctx.low.?[@intCast(u)] = cctx.idx;
    cctx.idx += 1;
    cctx.stack.?[@intCast(cctx.top)] = u;
    cctx.top += 1;
    cctx.onstack.?[@intCast(u)] = 1;
    var v: c_int = 0;
    while (v < cctx.n) : (v += 1) {
        if (cctx.adj.?[@intCast(u)].?[@intCast(v)] == 0) continue;
        if (cctx.index.?[@intCast(v)] == -1) {
            sccVisit(cctx, v);
            if (cctx.low.?[@intCast(v)] < cctx.low.?[@intCast(u)]) cctx.low.?[@intCast(u)] = cctx.low.?[@intCast(v)];
        } else if (cctx.onstack.?[@intCast(v)] != 0) {
            if (cctx.index.?[@intCast(v)] < cctx.low.?[@intCast(u)]) cctx.low.?[@intCast(u)] = cctx.index.?[@intCast(v)];
        }
    }
    if (cctx.low.?[@intCast(u)] == cctx.index.?[@intCast(u)]) {
        var size: c_int = 0;
        while (true) {
            cctx.top -= 1;
            const w = cctx.stack.?[@intCast(cctx.top)];
            cctx.onstack.?[@intCast(w)] = 0;
            size += 1;
            if (w == u) break;
        }
        if (size > 1) cctx.has_big = 1;
    }
}

// 1 if the predicate dep graph has an SCC with >1 node, 0 if every SCC is
// size 1 (self-loops allowed), -1 on allocation failure.
fn depHasMultiNodeScc(n: c_int, E: *const edge_vec) c_int {
    if (n <= 1) return 0;

    var cctx = std.mem.zeroes(scc_ctx);
    cctx.n = n;
    cctx.adj = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(n)), @sizeOf(?[*]u8))));
    cctx.index = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(n)) * @sizeOf(c_int))));
    cctx.low = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(n)) * @sizeOf(c_int))));
    cctx.onstack = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(n)), @sizeOf(c_int))));
    cctx.stack = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(n)) * @sizeOf(c_int))));

    var rc: c_int = 0;
    if (cctx.adj == null or cctx.index == null or cctx.low == null or cctx.onstack == null or cctx.stack == null) {
        rc = -1;
    } else {
        var i: c_int = 0;
        while (i < n) : (i += 1) {
            cctx.adj.?[@intCast(i)] = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(n)), 1)));
            if (cctx.adj.?[@intCast(i)] == null) {
                rc = -1;
                break;
            }
            cctx.index.?[@intCast(i)] = -1;
        }
        if (rc == 0) {
            var e: c_int = 0;
            while (e < E.n) : (e += 1) {
                cctx.adj.?[@intCast(E.v.?[@intCast(e)].from)].?[@intCast(E.v.?[@intCast(e)].to)] = 1;
            }
            var j: c_int = 0;
            while (j < n) : (j += 1) {
                if (cctx.index.?[@intCast(j)] == -1) sccVisit(&cctx, j);
            }
            rc = cctx.has_big;
        }
    }

    if (cctx.adj) |ad| {
        var i: c_int = 0;
        while (i < n) : (i += 1) c.free(@ptrCast(ad[@intCast(i)]));
        c.free(@ptrCast(ad));
    }
    c.free(@ptrCast(cctx.index));
    c.free(@ptrCast(cctx.low));
    c.free(@ptrCast(cctx.onstack));
    c.free(@ptrCast(cctx.stack));
    return rc;
}

// ─── SIPS body ordering (deterministic greedy) ─────────────────────────────

fn sipsAtomScore(A: *const parser.atom, bound: *const name_set, score: *c_int, n_new: *c_int) void {
    var s: c_int = 0;
    var nn: c_int = 0;
    var j: c_int = 0;
    while (j < A.nargs) : (j += 1) {
        const t = A.args.?[@intCast(j)].?;
        if (t.kind == parser.TOK_VAR) {
            if (nsContains(bound, t.text) != 0) s += 1 else nn += 1;
        } else if (t.kind == parser.TOK_INT or t.kind == parser.TOK_IDENT) {
            s += 1;
        }
    }
    score.* = s;
    n_new.* = nn;
}

fn sipsAtomBetter(score: c_int, n_new: c_int, edb: c_int, idx: c_int, b_score: c_int, b_new: c_int, b_edb: c_int, b_idx: c_int) c_int {
    if (score != b_score) return @intFromBool(score > b_score);
    const z: c_int = @intFromBool(n_new == 0);
    const bz: c_int = @intFromBool(b_new == 0);
    if (z != bz) return @intFromBool(z > bz);
    if (edb != b_edb) return @intFromBool(edb > b_edb);
    return @intFromBool(idx < b_idx);
}

fn sipsBodyOrder(R: *const parser.rule, alpha: [*:0]const u8, preds: *const pred_vec, body_order_out: [*]c_int, err: ?[*]u8, errsz: usize) c_int {
    var bound = std.mem.zeroes(name_set);
    const n = R.nbody;
    var out_n: c_int = 0;

    if (n <= 0) return 0;

    const placed_raw = c.calloc(@as(usize, @intCast(n)), 1) orelse {
        setOom(err, errsz);
        return -1;
    };
    const placed: [*]u8 = @ptrCast(@alignCast(placed_raw));
    defer c.free(@ptrCast(placed));
    defer c.free(@ptrCast(bound.v));

    // Init bound from head 'b'-position TOK_VAR args (constants don't
    // propagate — they're positional).
    var j: c_int = 0;
    while (j < R.head.?.nargs) : (j += 1) {
        if (alpha[@intCast(j)] == 'b' and R.head.?.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
            if (nsAdd(&bound, R.head.?.args.?[@intCast(j)].?.text) != 0) {
                setOom(err, errsz);
                return -1;
            }
        }
    }

    while (out_n < n) {
        var fired: c_int = 0;

        // (1) Fire any equality with one side already bound.
        var i: c_int = 0;
        while (i < n) : (i += 1) {
            const A = R.body.?[@intCast(i)];
            if (placed[@intCast(i)] != 0 or A == null or A.?.pred == null) continue;
            if (!isEqualityAtom(A)) continue;
            if (A.?.nargs == 2 and
                A.?.args.?[0].?.kind == parser.TOK_VAR and
                A.?.args.?[1].?.kind == parser.TOK_VAR)
            {
                const b0 = nsContains(&bound, A.?.args.?[0].?.text);
                const b1 = nsContains(&bound, A.?.args.?[1].?.text);
                if (b0 != 0 or b1 != 0) {
                    if (b0 != 0 and b1 == 0) {
                        if (nsAdd(&bound, A.?.args.?[1].?.text) != 0) {
                            setOom(err, errsz);
                            return -1;
                        }
                    } else if (b1 != 0 and b0 == 0) {
                        if (nsAdd(&bound, A.?.args.?[0].?.text) != 0) {
                            setOom(err, errsz);
                            return -1;
                        }
                    }
                    body_order_out[@intCast(out_n)] = i;
                    out_n += 1;
                    placed[@intCast(i)] = 1;
                    fired = 1;
                }
            }
        }
        if (fired != 0) continue; // re-sweep: propagation may enable another '='

        // (2) Score remaining non-equality atoms; pick the best.
        {
            var best: c_int = -1;
            var best_score: c_int = 0;
            var best_new: c_int = 0;
            var best_edb: c_int = 0;
            var best_nonprop: c_int = 0;
            var ii: c_int = 0;
            while (ii < n) : (ii += 1) {
                const A = R.body.?[@intCast(ii)];
                if (placed[@intCast(ii)] != 0 or A == null or A.?.pred == null) continue;
                if (isEqualityAtom(A)) continue; // none fireable now
                const is_ar = isArithAtom(A);
                const is_cp = isComparisonAtom(A);
                const is_sp = isStrProducingAtom(A);
                const is_sf = isStrFilterAtom(A);
                var score: c_int = 0;
                var n_new: c_int = 0;
                var nonprop: c_int = 0;
                if (is_ar) {
                    exprOperandVars(A.?.arith, &bound, &score, &n_new);
                    nonprop = 0;
                    if (n_new > 0) continue; // defer until operands bound
                } else if (is_sp) {
                    strOperandVars(A, 1, &bound, &score, &n_new);
                    nonprop = 0;
                    if (n_new > 0) continue;
                } else if (is_cp) {
                    sipsAtomScore(A.?, &bound, &score, &n_new);
                    nonprop = 1;
                    if (n_new > 0) continue; // defer
                } else if (is_sf) {
                    strOperandVars(A, 0, &bound, &score, &n_new);
                    nonprop = 1;
                    if (n_new > 0) continue;
                } else {
                    nonprop = @intFromBool(A.?.aggregate != 0 or A.?.negated != 0);
                    sipsAtomScore(A.?, &bound, &score, &n_new);
                    if (nonprop != 0 and n_new > 0) continue;
                }
                const edb: c_int = @intFromBool(predFind(preds, A.?.pred) < 0);
                if (best < 0 or sipsAtomBetter(score, n_new, edb, ii, best_score, best_new, best_edb, best) != 0) {
                    best = ii;
                    best_score = score;
                    best_new = n_new;
                    best_edb = edb;
                    best_nonprop = nonprop;
                }
            }
            if (best >= 0) {
                const A = R.body.?[@intCast(best)];
                body_order_out[@intCast(out_n)] = best;
                out_n += 1;
                placed[@intCast(best)] = 1;
                if (isArithAtom(A)) {
                    if (A.?.nargs >= 1 and A.?.args.?[0].?.kind == parser.TOK_VAR) {
                        if (nsAdd(&bound, A.?.args.?[0].?.text) != 0) {
                            setOom(err, errsz);
                            return -1;
                        }
                    }
                } else if (isStrProducingAtom(A)) {
                    if (A.?.nargs >= 1 and A.?.args.?[0].?.kind == parser.TOK_VAR) {
                        if (nsAdd(&bound, A.?.args.?[0].?.text) != 0) {
                            setOom(err, errsz);
                            return -1;
                        }
                    }
                } else if (best_nonprop == 0) {
                    var j2: c_int = 0;
                    while (j2 < A.?.nargs) : (j2 += 1) {
                        if (A.?.args.?[@intCast(j2)].?.kind == parser.TOK_VAR) {
                            if (nsAdd(&bound, A.?.args.?[@intCast(j2)].?.text) != 0) {
                                setOom(err, errsz);
                                return -1;
                            }
                        }
                    }
                }
                continue;
            }
        }

        // (3) Defensive fallback: place the remainder in original order.
        var ik: c_int = 0;
        while (ik < n) : (ik += 1) {
            if (placed[@intCast(ik)] == 0) {
                body_order_out[@intCast(out_n)] = ik;
                out_n += 1;
            }
        }
        break;
    }
    return 0;
}

// ─── Per-(rule, variant) SIPS cache ────────────────────────────────────────

const sips_entry = struct { rule_idx: c_int, variant_idx: c_int, order: ?[*]c_int };
const sips_cache = struct { v: ?[*]sips_entry, n: c_int, cap: c_int };

fn sipsGet(sc: *sips_cache, rule_idx: c_int, variant_idx: c_int, R: *const parser.rule, alpha: [*:0]const u8, preds: *const pred_vec, err: ?[*]u8, errsz: usize) ?[*]c_int {
    var i: c_int = 0;
    while (i < sc.n) : (i += 1) {
        if (sc.v.?[@intCast(i)].rule_idx == rule_idx and sc.v.?[@intCast(i)].variant_idx == variant_idx)
            return sc.v.?[@intCast(i)].order;
    }
    if (sc.n >= sc.cap) {
        const nc: c_int = if (sc.cap != 0) sc.cap * 2 else 8;
        const mem = c.realloc(if (sc.v) |p| p else null, @as(usize, @intCast(nc)) * @sizeOf(sips_entry));
        if (mem == null) {
            setOom(err, errsz);
            return null;
        }
        sc.v = @ptrCast(@alignCast(mem));
        sc.cap = nc;
    }
    const nbody: c_int = if (R.nbody > 0) R.nbody else 1;
    const ord_raw = c.malloc(@as(usize, @intCast(nbody)) * @sizeOf(c_int)) orelse {
        setOom(err, errsz);
        return null;
    };
    const ord: [*]c_int = @ptrCast(@alignCast(ord_raw));
    if (sipsBodyOrder(R, alpha, preds, ord, err, errsz) != 0) {
        c.free(@ptrCast(ord));
        return null;
    }
    sc.v.?[@intCast(sc.n)].rule_idx = rule_idx;
    sc.v.?[@intCast(sc.n)].variant_idx = variant_idx;
    sc.v.?[@intCast(sc.n)].order = ord;
    sc.n += 1;
    return ord;
}

fn sipsCacheFree(sc: *sips_cache) void {
    var i: c_int = 0;
    while (i < sc.n) : (i += 1) c.free(@ptrCast(sc.v.?[@intCast(i)].order));
    c.free(@ptrCast(sc.v));
}

// ─── Bound-set walk (adornment capture) ────────────────────────────────────

fn computeBetas(R: *const parser.rule, alpha: [*:0]const u8, preds: *const pred_vec, body_order: ?[*]const c_int, betas: [*][9]u8, err: ?[*]u8, errsz: usize) c_int {
    var bound = std.mem.zeroes(name_set);
    defer c.free(@ptrCast(bound.v));

    var j: c_int = 0;
    while (j < R.nbody) : (j += 1) betas[@intCast(j)][0] = 0;

    j = 0;
    while (j < R.head.?.nargs) : (j += 1) {
        if (alpha[@intCast(j)] == 'b' and R.head.?.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
            if (nsAdd(&bound, R.head.?.args.?[@intCast(j)].?.text) != 0) {
                setOom(err, errsz);
                return -1;
            }
        }
    }

    j = 0;
    while (j < R.nbody) : (j += 1) {
        const bj: c_int = if (body_order != null) body_order.?[@intCast(j)] else j;
        const A = R.body.?[@intCast(bj)];
        if (A == null) continue;

        if (isEqualityAtom(A)) {
            if (A.?.nargs == 2 and
                A.?.args.?[0].?.kind == parser.TOK_VAR and
                A.?.args.?[1].?.kind == parser.TOK_VAR)
            {
                const a0 = A.?.args.?[0].?.text;
                const a1 = A.?.args.?[1].?.text;
                const b0 = nsContains(&bound, a0);
                const b1 = nsContains(&bound, a1);
                if (b0 != 0 and b1 == 0) {
                    if (nsAdd(&bound, a1) != 0) {
                        setOom(err, errsz);
                        return -1;
                    }
                } else if (b1 != 0 and b0 == 0) {
                    if (nsAdd(&bound, a0) != 0) {
                        setOom(err, errsz);
                        return -1;
                    }
                }
            }
            continue;
        }

        if (isArithAtom(A)) {
            if (A.?.nargs >= 1 and A.?.args.?[0].?.kind == parser.TOK_VAR) {
                if (nsAdd(&bound, A.?.args.?[0].?.text) != 0) {
                    setOom(err, errsz);
                    return -1;
                }
            }
            continue;
        }

        if (isStrProducingAtom(A)) {
            if (A.?.nargs >= 1 and A.?.args.?[0].?.kind == parser.TOK_VAR) {
                if (nsAdd(&bound, A.?.args.?[0].?.text) != 0) {
                    setOom(err, errsz);
                    return -1;
                }
            }
            continue;
        }

        if (isComparisonAtom(A)) continue;
        if (isStrFilterAtom(A)) continue;

        // Negated and aggregate atoms neither propagate adornment nor add vars
        // to bound (SIPS schedules them only once all their var args are bound).
        if (A.?.negated != 0 or A.?.aggregate != 0) continue;

        if (A.?.pred != null and predFind(preds, A.?.pred) >= 0)
            atomAdornment(A.?, &bound, @ptrCast(&betas[@intCast(bj)]));

        var a: c_int = 0;
        while (a < A.?.nargs) : (a += 1) {
            if (A.?.args.?[@intCast(a)].?.kind == parser.TOK_VAR) {
                if (nsAdd(&bound, A.?.args.?[@intCast(a)].?.text) != 0) {
                    setOom(err, errsz);
                    return -1;
                }
            }
        }
    }
    return 0;
}

// ─── Rule ordering for single-pass non-recursive evaluation ────────────────

fn topoSortRules(v: *rule_vec) void {
    const n = v.n;
    if (n <= 1) return;

    const adj_raw = c.calloc(@as(usize, @intCast(n)), @sizeOf(?[*]u8));
    const indeg_raw = c.calloc(@as(usize, @intCast(n)), @sizeOf(c_int));
    const q_raw = c.malloc(@as(usize, @intCast(n)) * @sizeOf(c_int));
    if (adj_raw == null or indeg_raw == null or q_raw == null) {
        if (adj_raw) |a| c.free(@ptrCast(a));
        if (indeg_raw) |a| c.free(@ptrCast(a));
        if (q_raw) |a| c.free(@ptrCast(a));
        return;
    }
    const adj: [*]?[*]u8 = @ptrCast(@alignCast(adj_raw));
    const indeg: [*]c_int = @ptrCast(@alignCast(indeg_raw));
    const q: [*]c_int = @ptrCast(@alignCast(q_raw));

    var rows_ok = true;
    var i: c_int = 0;
    while (i < n) : (i += 1) {
        adj[@intCast(i)] = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(n)), 1)));
        if (adj[@intCast(i)] == null) {
            rows_ok = false;
            break;
        }
    }
    if (!rows_ok) {
        var k0: c_int = 0;
        while (k0 < n) : (k0 += 1) c.free(@ptrCast(adj[@intCast(k0)]));
        c.free(@ptrCast(adj));
        c.free(@ptrCast(indeg));
        c.free(@ptrCast(q));
        return;
    }

    // Edge r1 -> r2 when r1's head predicate appears in r2's body.
    i = 0;
    while (i < n) : (i += 1) {
        const r2 = v.v.?[@intCast(i)].?;
        var j: c_int = 0;
        while (j < r2.nbody) : (j += 1) {
            const bp = r2.body.?[@intCast(j)].?.pred;
            if (bp == null) continue;
            var k: c_int = 0;
            while (k < n) : (k += 1) {
                if (k == i) continue; // ignore self-reference
                if (std.mem.eql(u8, std.mem.span(v.v.?[@intCast(k)].?.head.?.pred.?), std.mem.span(bp.?)) and
                    adj[@intCast(k)].?[@intCast(i)] == 0)
                {
                    adj[@intCast(k)].?[@intCast(i)] = 1;
                    indeg[@intCast(i)] += 1;
                }
            }
        }
    }

    // Kahn's algorithm.
    var qh: c_int = 0;
    var qt: c_int = 0;
    i = 0;
    while (i < n) : (i += 1) {
        if (indeg[@intCast(i)] == 0) {
            q[@intCast(qt)] = i;
            qt += 1;
        }
    }
    const sorted_raw = c.malloc(@as(usize, @intCast(n)) * @sizeOf(?*parser.rule));
    if (sorted_raw == null) {
        var k1: c_int = 0;
        while (k1 < n) : (k1 += 1) c.free(@ptrCast(adj[@intCast(k1)]));
        c.free(@ptrCast(adj));
        c.free(@ptrCast(indeg));
        c.free(@ptrCast(q));
        return;
    }
    const sorted: [*]?*parser.rule = @ptrCast(@alignCast(sorted_raw));
    var sorted_n: c_int = 0;
    while (qh < qt) {
        const u = q[@intCast(qh)];
        qh += 1;
        sorted[@intCast(sorted_n)] = v.v.?[@intCast(u)];
        sorted_n += 1;
        var w: c_int = 0;
        while (w < n) : (w += 1) {
            if (adj[@intCast(u)].?[@intCast(w)] != 0) {
                indeg[@intCast(w)] -= 1;
                if (indeg[@intCast(w)] == 0) {
                    q[@intCast(qt)] = w;
                    qt += 1;
                }
            }
        }
    }

    if (sorted_n == n) {
        var m: c_int = 0;
        while (m < n) : (m += 1) v.v.?[@intCast(m)] = sorted[@intCast(m)];
    }
    c.free(@ptrCast(sorted));

    var k2: c_int = 0;
    while (k2 < n) : (k2 += 1) c.free(@ptrCast(adj[@intCast(k2)]));
    c.free(@ptrCast(adj));
    c.free(@ptrCast(indeg));
    c.free(@ptrCast(q));
}

// ─── The transformation ────────────────────────────────────────────────────

pub export fn magic_transform(
    ast_rules: ?[*]const ?*const parser.rule,
    n_ast: c_int,
    goal_pred: ?[*:0]const u8,
    goal_arity: u8,
    leading: ?[*]const u32,
    k: u8,
    src_nrels: usize,
    ir: ?*anyopaque,
    out: ?*magic_program,
    reject_reason: ?[*]u8,
    reject_sz: usize,
) c_int {
    var adorn: [9]u8 = undefined;
    if (reject_reason != null and reject_sz > 0) reject_reason.?[0] = 0;
    if (goal_arity > 8) {
        _ = rejectFmt(reject_reason, reject_sz, "goal arity {d} out of range 1..8", .{goal_arity});
        if (out) |o| o.* = std.mem.zeroes(magic_program);
        return -1;
    }
    if (k > goal_arity) {
        _ = rejectFmt(reject_reason, reject_sz, "k={d} exceeds goal arity {d}", .{k, goal_arity});
        if (out) |o| o.* = std.mem.zeroes(magic_program);
        return -1;
    }
    makeAdornment(@ptrCast(&adorn), goal_arity, k);
    return magic_transform_adorn(ast_rules, n_ast, goal_pred, goal_arity,
        @as([*:0]const u8, @ptrCast(&adorn)), leading, k, src_nrels, ir, out,
        reject_reason, reject_sz);
}

pub export fn magic_transform_adorn(
    ast_rules: ?[*]const ?*const parser.rule,
    n_ast: c_int,
    goal_pred: ?[*:0]const u8,
    goal_arity: u8,
    adorn: ?[*:0]const u8,
    vals: ?[*]const u32,
    nvals: u8,
    src_nrels: usize,
    ir: ?*anyopaque,
    out: ?*magic_program,
    reject_reason: ?[*]u8,
    reject_sz: usize,
) c_int {
    var preds = std.mem.zeroes(pred_vec);
    var adorns = std.mem.zeroes(pa_vec);
    var edges = std.mem.zeroes(edge_vec);
    var modrules = std.mem.zeroes(rule_vec);
    var decls = std.mem.zeroes(decl_vec);
    var sips = std.mem.zeroes(sips_cache);
    var queue: ?[*]c_int = null;

    _ = ir;
    _ = vals;

    const o = out orelse return -1;
    o.* = std.mem.zeroes(magic_program);
    if (reject_reason != null and reject_sz > 0) reject_reason.?[0] = 0;

    var success = false;
    defer {
        if (!success) {
            var x: c_int = 0;
            while (x < modrules.n) : (x += 1) rule_free(modrules.v.?[@intCast(x)]);
            c.free(@ptrCast(modrules.v));
            c.free(@ptrCast(decls.v));
        }
    }
    defer c.free(@ptrCast(edges.v));
    defer c.free(@ptrCast(preds.v));
    defer c.free(@ptrCast(adorns.v));
    defer sipsCacheFree(&sips);
    defer {
        if (queue) |q| c.free(@ptrCast(q));
    }

    var goal_vi: c_int = -1;

    if (ast_rules == null or n_ast <= 0) return rejectFmt(reject_reason, reject_sz, "no rules loaded", .{});
    if (goal_pred == null) return rejectFmt(reject_reason, reject_sz, "null goal predicate", .{});
    if (adorn == null) return rejectFmt(reject_reason, reject_sz, "null goal adornment", .{});
    if (goal_arity == 0 or goal_arity > 8)
        return rejectFmt(reject_reason, reject_sz, "goal arity {d} out of range 1..8", .{goal_arity});
    {
        const alen = std.mem.len(adorn.?);
        var nb: c_int = 0;
        if (alen != goal_arity)
            return rejectFmt(reject_reason, reject_sz, "adornment length {d} != goal arity {d}", .{ alen, goal_arity });
        var xi: usize = 0;
        while (xi < alen) : (xi += 1) {
            if (adorn.?[xi] != 'b' and adorn.?[xi] != 'f')
                return rejectFmt(reject_reason, reject_sz, "adornment char '{c}' (not 'b'/'f')", .{adorn.?[xi]});
            if (adorn.?[xi] == 'b') nb += 1;
        }
        if (nvals == 0) return rejectFmt(reject_reason, reject_sz, "nvals==0 not supported (route to dl_query)", .{});
        if (nvals > goal_arity)
            return rejectFmt(reject_reason, reject_sz, "nvals={d} exceeds goal arity {d}", .{ nvals, goal_arity });
        if (nb != @as(c_int, nvals))
            return rejectFmt(reject_reason, reject_sz, "nvals={d} != count_b(adorn)={d}", .{ nvals, nb });
    }

    // ── Collect the IDB head set (with natural arity) ──
    var i: c_int = 0;
    while (i < n_ast) : (i += 1) {
        const r = ast_rules.?[@intCast(i)];
        if (r == null or r.?.head == null or r.?.head.?.pred == null) continue;
        if (predPush(&preds, r.?.head.?.pred, @intCast(r.?.head.?.nargs)) != 0)
            return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
    }

    // The goal must be an IDB predicate (EDB goals degenerate to prefix).
    if (predFind(&preds, goal_pred) < 0)
        return rejectFmt(reject_reason, reject_sz, "goal '{s}' is not a rule head (EDB goal: use prefix lookup)", .{std.mem.span(goal_pred.?)});

    // Seed the goal's user-requested variant.
    goal_vi = paPush(&adorns, goal_pred, goal_arity, adorn.?);
    if (goal_vi < 0)
        return rejectFmt(reject_reason, reject_sz, "adorned predicate name for '{s}' too long", .{std.mem.span(goal_pred.?)});
    adorns.v.?[@intCast(goal_vi)].is_goal_variant = 1;

    // ── Phase A: predicate dependency graph (body IDB -> head) ──
    i = 0;
    while (i < n_ast) : (i += 1) {
        const r = ast_rules.?[@intCast(i)];
        if (r == null or r.?.head == null or r.?.head.?.pred == null) continue;
        const hi = predFind(&preds, r.?.head.?.pred);
        var j: c_int = 0;
        while (j < r.?.nbody) : (j += 1) {
            const A = r.?.body.?[@intCast(j)];
            if (A == null or A.?.pred == null) continue;
            if (cstrEq(A.?.pred.?, "=")) continue;
            if (isStrProducingAtom(A) or isStrFilterAtom(A)) continue;
            const bi = predFind(&preds, A.?.pred);
            if (bi >= 0) // IDB body atom → edge bi -> hi
                if (evPush(&edges, bi, hi) != 0)
                    return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
        }
    }

    // Reject cross-predicate mutual recursion (SCC size > 1).
    {
        const s = depHasMultiNodeScc(preds.n, &edges);
        if (s < 0) return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
        if (s > 0)
            return rejectFmt(reject_reason, reject_sz, "cross-predicate mutual recursion (dependency cycle among IDB predicates) not supported", .{});
    }
    c.free(@ptrCast(edges.v));
    edges.v = null;

    // ── Phase B: adornment-closure fixpoint ──
    queue = @ptrCast(@alignCast(c.malloc(@as(usize, MAX_ADORN_VARIANTS) * @sizeOf(c_int))));
    if (queue == null) return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
    {
        var qh: c_int = 0;
        var qt: c_int = 0;
        var pal: [9]u8 = undefined;
        queue.?[@intCast(qt)] = goal_vi;
        qt += 1;
        while (qh < qt) {
            const vi = queue.?[@intCast(qh)];
            qh += 1;
            const V = &adorns.v.?[@intCast(vi)];
            _ = snprintStr(pal[0..], @as([*:0]const u8, @ptrCast(&V.adorn)));
            var ii: c_int = 0;
            while (ii < n_ast) : (ii += 1) {
                const R = ast_rules.?[@intCast(ii)];
                if (R == null or R.?.head == null or R.?.head.?.pred == null) continue;
                if (!cstrEqArr(R.?.head.?.pred.?, &V.pred)) continue;
                const ord = sipsGet(&sips, ii, vi, R.?, @as([*:0]const u8, @ptrCast(&pal)), &preds, reject_reason, reject_sz);
                if (ord == null) return -1;
                const nbetas: usize = @intCast(if (R.?.nbody > 0) R.?.nbody else 1);
                const betas_raw = c.calloc(nbetas, @sizeOf([9]u8));
                if (betas_raw == null) return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
                const betas: [*][9]u8 = @ptrCast(@alignCast(betas_raw));
                if (computeBetas(R.?, @as([*:0]const u8, @ptrCast(&pal)), &preds, ord, betas, reject_reason, reject_sz) != 0) {
                    c.free(@ptrCast(betas));
                    return -1;
                }
                var a: c_int = 0;
                while (a < R.?.nbody) : (a += 1) {
                    const A = R.?.body.?[@intCast(a)];
                    if (betas[@intCast(a)][0] == 0) continue;
                    if (A == null or A.?.pred == null or cstrEq(A.?.pred.?, "=")) continue;
                    if (A.?.negated != 0 or A.?.aggregate != 0) continue; // defensive
                    var nvi = paFind(&adorns, A.?.pred, @as([*:0]const u8, @ptrCast(&betas[@intCast(a)])));
                    if (nvi < 0) {
                        nvi = paPush(&adorns, A.?.pred, @intCast(A.?.nargs), @as([*:0]const u8, @ptrCast(&betas[@intCast(a)])));
                        if (nvi < 0) {
                            c.free(@ptrCast(betas));
                            return rejectFmt(reject_reason, reject_sz, "adorned predicate name for '{s}' too long", .{std.mem.span(A.?.pred.?)});
                        }
                        if (adorns.n > MAX_ADORN_VARIANTS) {
                            c.free(@ptrCast(betas));
                            return rejectFmt(reject_reason, reject_sz, "adornment closure exceeds {d} variants (predicate lattice blow-up)", .{MAX_ADORN_VARIANTS});
                        }
                        queue.?[@intCast(qt)] = nvi;
                        qt += 1;
                    }
                }
                c.free(@ptrCast(betas));
            }
        }
    }
    c.free(@ptrCast(queue.?));
    queue = null;

    // ── Negation / aggregate soundness post-pass ──
    i = 0;
    while (i < n_ast) : (i += 1) {
        const R = ast_rules.?[@intCast(i)];
        if (R == null or R.?.head == null or R.?.head.?.pred == null) continue;
        if (paFindAny(&adorns, R.?.head.?.pred) == 0) continue; // unreachable
        var jj: c_int = 0;
        while (jj < R.?.nbody) : (jj += 1) {
            const A = R.?.body.?[@intCast(jj)];
            if (A == null or A.?.pred == null) continue;
            if (A.?.negated != 0) {
                if (paFindAny(&adorns, A.?.pred) != 0)
                    return rejectFmt(reject_reason, reject_sz, "negation on adorned-closure predicate '{s}' not supported", .{std.mem.span(A.?.pred.?)});
            } else if (A.?.aggregate != 0) {
                var a2: c_int = 0;
                while (a2 < R.?.nbody) : (a2 += 1) {
                    const B = R.?.body.?[@intCast(a2)];
                    if (B == null or B.?.pred == null) continue;
                    if (B.?.negated != 0 or B.?.aggregate != 0) continue;
                    if (cstrEq(B.?.pred.?, "=")) continue;
                    if (paFindAny(&adorns, B.?.pred) != 0)
                        return rejectFmt(reject_reason, reject_sz, "aggregate in rule with adorned-closure body atom '{s}' not supported", .{std.mem.span(B.?.pred.?)});
                }
            }
        }
    }

    // ── MAX_RELS budget: src_nrels aliased + 2 fresh rels per variant ──
    {
        const total = src_nrels + 2 * @as(usize, @intCast(adorns.n));
        if (total > MAX_RELS)
            return rejectFmt(reject_reason, reject_sz,
                "adorned closure needs {d} aliased + {d} fresh relations ({d} variants x 2) = {d} total, exceeding MAX_RELS={d}",
                .{ src_nrels, 2 * @as(usize, @intCast(adorns.n)), adorns.n, total, MAX_RELS });
    }

    // ── Phase C: per-(P,alpha) synthesis of all 3 rule classes ──
    i = 0;
    while (i < adorns.n) : (i += 1) {
        const P = &adorns.v.?[@intCast(i)];
        var r: c_int = 0;
        while (r < n_ast) : (r += 1) {
            const R = ast_rules.?[@intCast(r)];
            if (R == null or R.?.head == null or R.?.head.?.pred == null) continue;
            if (!cstrEqArr(R.?.head.?.pred.?, &P.pred)) continue;

            const ord = sipsGet(&sips, r, i, R.?, @as([*:0]const u8, @ptrCast(&P.adorn)), &preds, reject_reason, reject_sz);
            if (ord == null) return -1;
            const nbetas: usize = @intCast(if (R.?.nbody > 0) R.?.nbody else 1);
            const betas_raw = c.calloc(nbetas, @sizeOf([9]u8));
            if (betas_raw == null) return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
            const betas: [*][9]u8 = @ptrCast(@alignCast(betas_raw));
            if (computeBetas(R.?, @as([*:0]const u8, @ptrCast(&P.adorn)), &preds, ord, betas, reject_reason, reject_sz) != 0) {
                c.free(@ptrCast(betas));
                return -1;
            }

            // (a) adorned rule: P^a :- magic_P^a(bound head), body'
            const guard = buildBoundAtom(R.?.head.?, @as([*:0]const u8, @ptrCast(&P.magic_name)), @as([*:0]const u8, @ptrCast(&P.adorn)));
            if (guard == null) {
                c.free(@ptrCast(betas));
                return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
            }
            const total: c_int = 1 + R.?.nbody;
            const mbody_raw = c.calloc(@as(usize, @intCast(total)), @sizeOf(?*parser.atom));
            if (mbody_raw == null) {
                atomFreeLocal(guard);
                c.free(@ptrCast(betas));
                return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
            }
            const mbody: [*]?*parser.atom = @ptrCast(@alignCast(mbody_raw));
            mbody[0] = guard;
            var j: c_int = 0;
            while (j < R.?.nbody) : (j += 1) {
                const bj: c_int = ord.?[@intCast(j)];
                mbody[@as(usize, @intCast(1 + j))] = atomCopy(R.?.body.?[@intCast(bj)].?, renamedPred(R.?.body.?[@intCast(bj)], @as([*:0]const u8, @ptrCast(&betas[@intCast(bj)])), &adorns));
                if (mbody[@as(usize, @intCast(1 + j))] == null) {
                    c.free(@ptrCast(betas));
                    atomsFree(mbody, 1 + j);
                    return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
                }
            }
            const mhead = atomCopy(R.?.head.?, @as([*:0]const u8, @ptrCast(&P.adorned_name)));
            if (mhead == null) {
                c.free(@ptrCast(betas));
                atomsFree(mbody, total);
                return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
            }
            const mr = makeRule(mhead, mbody, total, R.?.has_negation, R.?.has_aggregate);
            if (mr == null) {
                atomFreeLocal(mhead);
                c.free(@ptrCast(betas));
                atomsFree(mbody, total);
                return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
            }
            if (rvPush(&modrules, mr) != 0) {
                c.free(@ptrCast(betas));
                rule_free(mr);
                return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
            }

            // (b)/(c) magic rules for each IDB body atom.
            j = 0;
            while (j < R.?.nbody) : (j += 1) {
                const A = R.?.body.?[@intCast(ord.?[@intCast(j)])];
                const aorig: c_int = ord.?[@intCast(j)];
                if (A == null or A.?.pred == null) continue;
                if (A.?.negated != 0 or A.?.aggregate != 0) continue;
                if (betas[@intCast(aorig)][0] == 0) continue; // EDB body atom
                const qi = paFind(&adorns, A.?.pred, @as([*:0]const u8, @ptrCast(&betas[@intCast(aorig)])));
                if (qi < 0) continue; // defensive: no such variant

                const target_magic: [*:0]const u8 = @ptrCast(&adorns.v.?[@intCast(qi)].magic_name);
                const target_adorn: [*:0]const u8 = @ptrCast(&adorns.v.?[@intCast(qi)].adorn);

                const mghead = buildBoundAtom(A.?, target_magic, target_adorn);
                if (mghead == null) {
                    c.free(@ptrCast(betas));
                    return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
                }

                const total2: c_int = 1 + j; // magic guard + prefix atoms A0..A_{j-1}
                const mgbody_raw = c.calloc(@as(usize, @intCast(total2)), @sizeOf(?*parser.atom));
                if (mgbody_raw == null) {
                    atomFreeLocal(mghead);
                    c.free(@ptrCast(betas));
                    return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
                }
                const mgbody: [*]?*parser.atom = @ptrCast(@alignCast(mgbody_raw));
                mgbody[0] = buildBoundAtom(R.?.head.?, @as([*:0]const u8, @ptrCast(&P.magic_name)), @as([*:0]const u8, @ptrCast(&P.adorn)));
                if (mgbody[0] == null) {
                    atomFreeLocal(mghead);
                    c.free(@ptrCast(mgbody));
                    c.free(@ptrCast(betas));
                    return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
                }
                var mg_neg: c_int = 0;
                var mg_agg: c_int = 0;
                var jj: c_int = 0;
                while (jj < j) : (jj += 1) {
                    const bjj: c_int = ord.?[@intCast(jj)];
                    const B = R.?.body.?[@intCast(bjj)];
                    if (B == null) continue;
                    if (B.?.negated != 0) mg_neg = 1;
                    if (B.?.aggregate != 0) mg_agg = 1;
                    mgbody[@as(usize, @intCast(1 + jj))] = atomCopy(B.?, renamedPred(B, @as([*:0]const u8, @ptrCast(&betas[@intCast(bjj)])), &adorns));
                    if (mgbody[@as(usize, @intCast(1 + jj))] == null) {
                        atomFreeLocal(mghead);
                        atomsFree(mgbody, 1 + jj);
                        c.free(@ptrCast(betas));
                        return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
                    }
                }
                const mgr = makeRule(mghead, mgbody, total2, mg_neg, mg_agg);
                if (mgr == null) {
                    atomFreeLocal(mghead);
                    atomsFree(mgbody, total2);
                    c.free(@ptrCast(betas));
                    return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
                }
                if (rvPush(&modrules, mgr) != 0) {
                    rule_free(mgr);
                    c.free(@ptrCast(betas));
                    return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
                }
            }
            c.free(@ptrCast(betas));
        }
    }

    // ── Phase D: decls — 2 per variant ──
    i = 0;
    while (i < adorns.n) : (i += 1) {
        const P = &adorns.v.?[@intCast(i)];
        var nb: u8 = 0;
        const alen = std.mem.len(@as([*:0]const u8, @ptrCast(&P.adorn)));
        var j: c_int = 0;
        while (j < @as(c_int, @intCast(alen))) : (j += 1) {
            if (P.adorn[@intCast(j)] == 'b') nb += 1;
        }

        if (dvContains(&decls, @as([*:0]const u8, @ptrCast(&P.adorned_name))) != 0)
            return rejectFmt(reject_reason, reject_sz, "adorned/magic predicate name collision '{s}'", .{span80(&P.adorned_name)});
        if (dvPush(&decls, @as([*:0]const u8, @ptrCast(&P.adorned_name)), P.arity) != 0)
            return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
        if (dvContains(&decls, @as([*:0]const u8, @ptrCast(&P.magic_name))) != 0)
            return rejectFmt(reject_reason, reject_sz, "adorned/magic predicate name collision '{s}'", .{span86(&P.magic_name)});
        if (dvPush(&decls, @as([*:0]const u8, @ptrCast(&P.magic_name)), nb) != 0)
            return rejectFmt(reject_reason, reject_sz, "out of memory", .{});
    }

    topoSortRules(&modrules);

    o.rules = modrules.v;
    o.n_rules = modrules.n;
    o.decls = decls.v;
    o.n_decls = decls.n;
    _ = snprintStr(o.adorned_goal[0..], @as([*:0]const u8, @ptrCast(&adorns.v.?[@intCast(goal_vi)].adorned_name)));
    o.goal_arity = goal_arity;

    success = true;
    return 0;
}

pub export fn magic_program_free(p: ?*magic_program) void {
    const pp = p orelse return;
    var i: c_int = 0;
    while (i < pp.n_rules) : (i += 1) rule_free(pp.rules.?[@intCast(i)]);
    c.free(@ptrCast(pp.rules));
    c.free(@ptrCast(pp.decls));
    pp.* = std.mem.zeroes(magic_program);
}

// ─── Tests ──────────────────────────────────────────────────────────────────

const testing = std.testing;

extern "c" fn parse_create(source: [*:0]const u8) ?*anyopaque;
extern "c" fn parse_rules(p: ?*anyopaque, n_rules: ?*c_int) ?[*]?*parser.rule;
extern "c" fn parse_free(p: ?*anyopaque) void;

fn testFreeRules(rules: ?[*]?*parser.rule, n: c_int) void {
    const arr = rules orelse return;
    var i: c_int = 0;
    while (i < n) : (i += 1) rule_free(arr[@intCast(i)]);
    c.free(@ptrCast(arr));
}

test "magic_transform_adorn: TC bf adorn produces adorned+magic rules" {
    const p = parse_create("tc(X,Y):-edge(X,Y).\ntc(X,Y):-edge(X,Z),tc(Z,Y).\n") orelse return error.TestUnexpectedResult;
    defer parse_free(p);
    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse return error.TestUnexpectedResult;
    defer testFreeRules(rules, n);
    try testing.expectEqual(@as(c_int, 2), n);

    var prog = std.mem.zeroes(magic_program);
    var reject: [256]u8 = undefined;
    const vals = [1]u32{42};
    const rc = magic_transform_adorn(
        @as(?[*]const ?*const parser.rule, @ptrCast(rules)), n,
        "tc", 2, "bf", &vals, 1, 1, null, &prog, &reject, reject.len);
    try testing.expectEqual(@as(c_int, 0), rc);
    defer magic_program_free(&prog);

    try testing.expectEqual(@as(c_int, 3), prog.n_rules);
    try testing.expectEqual(@as(c_int, 2), prog.n_decls);
    try testing.expectEqualStrings("tc__bf", span80(&prog.adorned_goal));
    try testing.expectEqual(@as(u8, 2), prog.goal_arity);
    try testing.expectEqualStrings("tc__bf", span64(&prog.decls.?[0].name));
    try testing.expectEqual(@as(u8, 2), prog.decls.?[0].arity);
    try testing.expectEqualStrings("magic_tc__bf", span64(&prog.decls.?[1].name));
    try testing.expectEqual(@as(u8, 1), prog.decls.?[1].arity);
}

test "magic_transform_adorn: rejects k==0 and cross-predicate mutual recursion" {
    // k==0 / nvals==0.
    {
        const p = parse_create("tc(X,Y):-edge(X,Y).\n") orelse return error.TestUnexpectedResult;
        defer parse_free(p);
        var n: c_int = 0;
        const rules = parse_rules(p, &n) orelse return error.TestUnexpectedResult;
        defer testFreeRules(rules, n);
        var prog = std.mem.zeroes(magic_program);
        var reject: [256]u8 = undefined;
        const rc = magic_transform_adorn(
            @as(?[*]const ?*const parser.rule, @ptrCast(rules)), n,
            "tc", 2, "ff", null, 0, 1, null, &prog, &reject, reject.len);
        try testing.expectEqual(@as(c_int, -1), rc);
    }
    // Cross-predicate mutual recursion.
    {
        const p = parse_create("p(X,Y):-q(X,Y).\nq(X,Y):-p(X,Y).\n") orelse return error.TestUnexpectedResult;
        defer parse_free(p);
        var n: c_int = 0;
        const rules = parse_rules(p, &n) orelse return error.TestUnexpectedResult;
        defer testFreeRules(rules, n);
        var prog = std.mem.zeroes(magic_program);
        var reject: [256]u8 = undefined;
        const vals = [1]u32{1};
        const rc = magic_transform_adorn(
            @as(?[*]const ?*const parser.rule, @ptrCast(rules)), n,
            "p", 2, "bf", &vals, 1, 1, null, &prog, &reject, reject.len);
        try testing.expectEqual(@as(c_int, -1), rc);
        try testing.expect(std.mem.indexOf(u8, reject[0..], "mutual recursion") != null);
    }
}
