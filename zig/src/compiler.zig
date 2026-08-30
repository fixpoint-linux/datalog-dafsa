//! compiler.zig — port of src/compiler.c (rule compiler: AST → VM bytecode).
//!
//! 26-opcode rule compiler with binding-table slot assignment, negation
//! safety, SCC stratification (Kosaraju + strict-stratification fixpoint),
//! M9 arithmetic/comparison/string builtins, v2 list/range builtins, M6
//! perm-select join planning, and BUSHY binary-tree join plans.
//!
//! Strangler-hybrid ABI: `vm_instr`, `var_info` and `compiled_rule` are
//! CONCRETE structs still dereferenced by the still-C vm.c / dl.c / tests, so
//! they are `extern struct`s that MUST stay byte-for-byte identical to
//! src/compiler.h (verified at comptime against the @cImport'd layout).
//! `dl_db`/`rel_entry` are consumed via @cImport("dl_internal.h"), NOT
//! redefined here.  rel_count/rel_arity/rel_count_subtree/intern_str/term_cons
//! are reached through the same @cImport's extern decls (relation.c/intern.c/
//! termstore.c are ported, but their symbols still link through the .so).
//!
//! Oracle: src/compiler.c (never modified).
//!
//! The four g_* compile-time toggles are `export var`s — plain writable DATA
//! globals the tests flip directly before dl_compile (test_bushy.c,
//! test_m14_permsel.c).

const std = @import("std");
const builtin = @import("builtin");
const c = std.c;

const parser = @import("parser.zig");
const regexwalk = @import("regexwalk.zig");

// dl_internal.h pulls in dl.h/intern.h/relation.h/vrelation.h/snapshot.h/
// permindex.h/termstore.h/compiler.h — so this one @cImport supplies dl_db,
// rel_entry, RELK_VARIADIC, the vm_instr/compiled_rule reference layouts, and
// extern decls for dl_declare_relation / dl_ensure_variant /
// dl_db_declare_perm / dl_db_find_perm / rel_* / vrel_count / intern_str /
// term_cons.
const dx = @cImport({
    @cInclude("dl_internal.h");
});

// glibc strdup (not re-exported by std.c; parser.zig declares its own too).
extern "c" fn strdup(s: [*c]const u8) ?[*:0]u8;

// ─── Constants (mirror src/compiler.h / src/parser.h) ─────────────────────

const MAX_VARS = 64;
const MAX_ARITY = 8;

const OP_HALT: u8 = 0;
const OP_SCAN: u8 = 1;
const OP_LOOKUP: u8 = 2;
const OP_EQ: u8 = 3;
const OP_EQ_CONST: u8 = 4;
const OP_PROJECT: u8 = 5;
const OP_OPEN_REL: u8 = 6;
const OP_NEG_CHECK: u8 = 7;
const OP_AGG_ACC: u8 = 8;
const OP_AGG_EMIT: u8 = 9;
const OP_WALK: u8 = 10;
const OP_LOOKUP_PERM: u8 = 11;
const OP_HASH_JOIN: u8 = 12;
const OP_CMP: u8 = 13;
const OP_ARITH: u8 = 14;
const OP_STR_FILTER: u8 = 15;
const OP_STR_LEN: u8 = 16;
const OP_STR_BIND: u8 = 17;
const OP_MAT_BEGIN: u8 = 18;
const OP_MAT_JOIN: u8 = 19;
const OP_LIST_CONS: u8 = 20;
const OP_LIST_CAR: u8 = 21;
const OP_LIST_CDR: u8 = 22;
const OP_LIST_APPEND: u8 = 23;
const OP_LIST_MEMBER: u8 = 24;
const OP_RANGE: u8 = 25;

const TERM_NIL: u32 = 0x80000000;

// ─── Public C-ABI structs (src/compiler.h) ────────────────────────────────

/// typedef struct { uint8_t op,a,b,c; uint32_t imm; uint8_t slots[8];
///                    uint8_t body_idx; } vm_instr
pub const vm_instr = extern struct {
    op: u8,
    a: u8,
    b: u8,
    c: u8,
    imm: u32,
    slots: [8]u8,
    body_idx: u8,
};

/// typedef struct { char *name; uint8_t slot; } var_info
pub const var_info = extern struct {
    name: ?[*:0]u8,
    slot: u8,
};

/// typedef struct { ... } compiled_rule — patterns is regex_dfa**; we point
/// it at the ported regexwalk.zig type (identical layout).
pub const compiled_rule = extern struct {
    head_pred: ?[*:0]u8,
    head_rel_id: u8,
    n_vars: u8,
    vars: ?[*]var_info,
    n_instrs: c_int,
    instrs: ?[*]vm_instr,
    stratum: c_int,
    is_recursive: c_int,
    has_aggregate: c_int,
    n_patterns: c_int,
    patterns: ?[*]?*regexwalk.regex_dfa,
};

// Comptime gate: our extern layouts must be byte-identical to the C header
// (translate-c produces the C struct, so size/offset equality proves it).
comptime {
    std.debug.assert(@sizeOf(vm_instr) == @sizeOf(dx.vm_instr));
    std.debug.assert(@offsetOf(vm_instr, "imm") == @offsetOf(dx.vm_instr, "imm"));
    std.debug.assert(@offsetOf(vm_instr, "slots") == @offsetOf(dx.vm_instr, "slots"));
    std.debug.assert(@offsetOf(vm_instr, "body_idx") == @offsetOf(dx.vm_instr, "body_idx"));
    std.debug.assert(@sizeOf(var_info) == @sizeOf(dx.var_info));
    std.debug.assert(@offsetOf(var_info, "slot") == @offsetOf(dx.var_info, "slot"));
    std.debug.assert(@sizeOf(compiled_rule) == @sizeOf(dx.compiled_rule));
    std.debug.assert(@offsetOf(compiled_rule, "stratum") == @offsetOf(dx.compiled_rule, "stratum"));
    std.debug.assert(@offsetOf(compiled_rule, "has_aggregate") == @offsetOf(dx.compiled_rule, "has_aggregate"));
    std.debug.assert(@offsetOf(compiled_rule, "n_patterns") == @offsetOf(dx.compiled_rule, "n_patterns"));
    std.debug.assert(@offsetOf(compiled_rule, "patterns") == @offsetOf(dx.compiled_rule, "patterns"));
}

// ─── Compile-time toggles (plain writable DATA globals; see compiler.h) ────

export var g_bushy: c_int = 1;
export var g_reorder: c_int = 1;
export var g_perm_select: c_int = 1;
export var g_perm_card_threshold: c_int = 4;

// C exes take R_X86_64_COPY on these data globals: they get their own .bss
// copy and the loader only redirects GOT-based references, so a direct
// reference binds to this .so's local storage and never sees the exe's copy
// (exe-set toggles would be ignored). In shared-lib builds every access goes
// through the accessors below, which load the symbol address via GOTPCREL —
// the same thing -fPIC refs in dl.c do. Test/exe builds bind direct (one
// storage domain); Debug shared-lib builds take the @extern fallback (the
// self-hosted backend emits true extern refs, and its inline-asm parser
// rejects GOTPCREL templates).
const use_got_refs = builtin.output_mode == .Lib and builtin.mode != .Debug and
    builtin.cpu.arch == .x86_64 and builtin.os.tag == .linux;

inline fn gotDataRef(comptime name: []const u8, comptime T: type) *T {
    if (use_got_refs) {
        return asm ("movq " ++ name ++ "@GOTPCREL(%rip), %[p]"
            : [p] "={rax}" (-> *T)
        );
    }
    return @extern(*T, .{ .name = name });
}

inline fn gBushyRef() *c_int {
    return gotDataRef("g_bushy", c_int);
}
inline fn gReorderRef() *c_int {
    return gotDataRef("g_reorder", c_int);
}
inline fn gPermSelectRef() *c_int {
    return gotDataRef("g_perm_select", c_int);
}
inline fn gPermCardThresholdRef() *c_int {
    return gotDataRef("g_perm_card_threshold", c_int);
}

// ─── LSP compile-error sink (parallel capture; stderr text unchanged) ──────

var compile_err_off: c_uint = 0;
var compile_err_msg: [256]u8 = undefined;
var compile_has_err: c_int = 0;

/// Byte offset of the rule currently being compiled by compile_one.
var g_rule_off: c_uint = 0;

// ─── Small C-string helpers ───────────────────────────────────────────────

fn strEq(a: [*c]const u8, b: [*c]const u8) bool {
    var i: usize = 0;
    while (a[i] != 0 and a[i] == b[i]) : (i += 1) {}
    return a[i] == b[i];
}

fn strLen(s: [*c]const u8) usize {
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {}
    return i;
}

/// Unwrap an optional C string for a `%s` slot (glibc prints "(null)" for
/// NULL; none of the compile-error formats pass NULL in practice).
fn cs(p: ?[*c]const u8) [*c]const u8 {
    return p orelse "(null)";
}

/// Write a compile error to stderr and capture the FIRST one (message + offset).
/// Mirrors the C cerr(): vfprintf(stderr, fmt) is never truncated; the captured
/// vsnprintf(compile_err_msg, 256, fmt) is truncated to 255 + NUL.
fn cerr(off: c_uint, comptime fmt: []const u8, args: anytype) void {
    const msg = std.fmt.allocPrint(std.heap.c_allocator, fmt, args) catch return;
    defer std.heap.c_allocator.free(msg);
    std.debug.print("{s}", .{msg});
    if (compile_has_err == 0) {
        compile_has_err = 1;
        compile_err_off = off;
        const n: usize = @min(msg.len, compile_err_msg.len - 1);
        @memcpy(compile_err_msg[0..n], msg[0..n]);
        compile_err_msg[n] = 0;
    }
}

/// const char *compile_last_error(uint32_t *off)
pub export fn compile_last_error(off: ?*c_uint) ?[*:0]const u8 {
    if (off) |o| o.* = compile_err_off;
    if (compile_has_err != 0) return @ptrCast(&compile_err_msg);
    return null;
}

// ─── dl_db internals access (authoritative layout in dl_internal.h) ───────

fn db_find_rel(db: *dx.dl_db, name: [*c]const u8) c_int {
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (strEq(db.rels[i].name, name)) return @intCast(i);
    }
    return -1;
}

fn db_rel_arity(db: *dx.dl_db, idx: c_int) u8 {
    if (idx < 0 or @as(usize, @intCast(idx)) >= db.nrels) return 0;
    return dx.rel_arity(db.rels[@intCast(idx)].rel);
}

fn db_rel_name(db: *dx.dl_db, idx: c_int) ?[*c]const u8 {
    if (idx < 0 or @as(usize, @intCast(idx)) >= db.nrels) return null;
    return db.rels[@intCast(idx)].name;
}

fn db_rel_count(db: *dx.dl_db) usize {
    return db.nrels;
}

fn db_get_interner(db: *dx.dl_db) ?*dx.interner {
    return db.ir;
}

/// M6-permsel: distinct-tuple estimate for the perm-selection cost gate.
fn db_rel_card_est(db: *dx.dl_db, idx: c_int) u64 {
    if (idx < 0 or @as(usize, @intCast(idx)) >= db.nrels) return 0;
    if (db.rels[@intCast(idx)].kind == dx.RELK_VARIADIC)
        return dx.vrel_count(db.rels[@intCast(idx)].vrel);
    return dx.rel_count_subtree(db.rels[@intCast(idx)].rel);
}

fn db_rel_card(db: *dx.dl_db, idx: c_int) u64 {
    if (idx < 0 or @as(usize, @intCast(idx)) >= db.nrels) return 0;
    if (db.rels[@intCast(idx)].kind == dx.RELK_VARIADIC)
        return dx.vrel_count(db.rels[@intCast(idx)].vrel);
    return dx.rel_count(db.rels[@intCast(idx)].rel);
}

fn db_rel_is_variadic(db: *dx.dl_db, idx: c_int) c_int {
    if (idx < 0 or @as(usize, @intCast(idx)) >= db.nrels) return 0;
    return if (db.rels[@intCast(idx)].kind == dx.RELK_VARIADIC) 1 else 0;
}

// ─── Variable slot tracking ────────────────────────────────────────────────

const v_entry = struct {
    name: ?[*:0]u8,
    slot: u8,
};

const v_tab = struct {
    e: ?[*]v_entry,
    n: c_int,
    cap: c_int,
    err: c_int,
};

fn v_init(t: *v_tab) void {
    t.* = std.mem.zeroes(v_tab);
}

fn v_free(t: *v_tab) void {
    var i: c_int = 0;
    while (i < t.n) : (i += 1) c.free(@ptrCast(t.e.?[@intCast(i)].name));
    c.free(@ptrCast(t.e));
}

fn v_find(t: *const v_tab, n: [*c]const u8) c_int {
    var i: c_int = 0;
    while (i < t.n) : (i += 1) {
        if (strEq(cs(t.e.?[@intCast(i)].name), n)) return i;
    }
    return -1;
}

fn v_add(t: *v_tab, n: [*c]const u8) c_int {
    const i = v_find(t, n);
    if (i >= 0) return i;
    if (t.n >= MAX_VARS) {
        t.err = 1;
        return -1;
    }
    if (t.n >= t.cap) {
        const nc: c_int = if (t.cap != 0) t.cap * 2 else 8;
        const ne = c.realloc(if (t.e) |ee| ee else null, @as(usize, @intCast(nc)) * @sizeOf(v_entry));
        if (ne == null) {
            t.err = 1;
            return -1;
        }
        t.e = @ptrCast(@alignCast(ne));
        t.cap = nc;
    }
    t.e.?[@intCast(t.n)].name = strdup(n);
    t.e.?[@intCast(t.n)].slot = @intCast(t.n);
    if (t.e.?[@intCast(t.n)].name == null) {
        t.err = 1;
        return -1;
    }
    const r = t.n;
    t.n += 1;
    return r;
}

/// Generate a fresh reserved slot name "__X%d" that does NOT collide with any
/// existing variable.  Writes into `buf` (size `cap`) and returns it.
fn v_fresh_name(t: *v_tab, buf: [*]u8, cap: usize, counter: *c_int, kind: u8) [*:0]u8 {
    while (true) {
        const n: c_int = counter.*;
        counter.* += 1;
        const written = std.fmt.bufPrint(buf[0..cap], "__{c}{d}", .{ kind, n }) catch
            buf[0..cap];
        buf[written.len] = 0;
        if (v_find(t, @ptrCast(buf)) < 0) return @ptrCast(buf);
    }
}

// ─── Instruction buffer ────────────────────────────────────────────────────

const i_buf = struct {
    b: ?[*]vm_instr,
    n: c_int,
    cap: c_int,
    err: c_int,
};

fn i_init(b: *i_buf) void {
    b.* = std.mem.zeroes(i_buf);
}

fn i_free(b: *i_buf) void {
    c.free(@ptrCast(b.b));
}

fn i_emit(b: *i_buf) ?*vm_instr {
    if (b.n >= b.cap) {
        const nc: c_int = if (b.cap != 0) b.cap * 2 else 16;
        const nb = c.realloc(if (b.b) |bb| bb else null, @as(usize, @intCast(nc)) * @sizeOf(vm_instr));
        if (nb == null) {
            b.err = 1;
            return null;
        }
        b.b = @ptrCast(@alignCast(nb));
        b.cap = nc;
    }
    b.b.?[@intCast(b.n)] = std.mem.zeroes(vm_instr);
    const r = &b.b.?[@intCast(b.n)];
    b.n += 1;
    return r;
}

// ─── v2-lists: list-pattern helpers ────────────────────────────────────────

fn list_is_pattern(t: ?*const parser.token) c_int {
    const tt = t orelse return 0;
    if (tt.kind != parser.TOK_LIST) return 0;
    if (tt.tail != null) return 1;
    var i: c_int = 0;
    while (i < tt.nchildren) : (i += 1) {
        const e = tt.children.?[@intCast(i)] orelse return 0;
        if (e.kind == parser.TOK_VAR) return 1;
        if (list_is_pattern(e) != 0) return 1;
    }
    return 0;
}

fn mask_token_vars(m: *u64, t: ?*const parser.token, vt: *v_tab) void {
    const tt = t orelse return;
    if (tt.kind == parser.TOK_VAR) {
        const vi = v_find(vt, cs(tt.text));
        if (vi >= 0 and vi < 64) m.* |= (@as(u64, 1) << @intCast(vi));
        return;
    }
    if (tt.kind == parser.TOK_LIST) {
        var i: c_int = 0;
        while (i < tt.nchildren) : (i += 1) mask_token_vars(m, tt.children.?[@intCast(i)], vt);
        mask_token_vars(m, tt.tail, vt);
    }
}

fn token_contains_var(t: ?*const parser.token, name: [*c]const u8) c_int {
    const tt = t orelse return 0;
    if (tt.kind == parser.TOK_VAR) {
        return if (tt.text != null and strEq(cs(tt.text), name)) 1 else 0;
    }
    if (tt.kind == parser.TOK_LIST) {
        var i: c_int = 0;
        while (i < tt.nchildren) : (i += 1) {
            if (token_contains_var(tt.children.?[@intCast(i)], name) != 0) return 1;
        }
        if (token_contains_var(tt.tail, name) != 0) return 1;
    }
    return 0;
}

fn collect_token_vars(t: ?*const parser.token, vt: *v_tab) c_int {
    const tt = t orelse return 0;
    if (tt.kind == parser.TOK_VAR) {
        return if (v_add(vt, cs(tt.text)) < 0) -1 else 0;
    }
    if (tt.kind == parser.TOK_LIST) {
        var i: c_int = 0;
        while (i < tt.nchildren) : (i += 1) {
            if (collect_token_vars(tt.children.?[@intCast(i)], vt) < 0) return -1;
        }
        if (collect_token_vars(tt.tail, vt) < 0) return -1;
    }
    return 0;
}

fn mark_token_vars_bound(t: ?*const parser.token, vt: *v_tab, bound_vars: []c_int) void {
    const tt = t orelse return;
    if (tt.kind == parser.TOK_VAR) {
        const vi = v_find(vt, cs(tt.text));
        if (vi >= 0) bound_vars[@intCast(vi)] = 1;
        return;
    }
    if (tt.kind == parser.TOK_LIST) {
        var i: c_int = 0;
        while (i < tt.nchildren) : (i += 1) mark_token_vars_bound(tt.children.?[@intCast(i)], vt, bound_vars);
        mark_token_vars_bound(tt.tail, vt, bound_vars);
    }
}

// ─── Parse a constant from a token ─────────────────────────────────────────

fn token_const(db: *dx.dl_db, t: ?*const parser.token, out: *u32) c_int {
    const tt = t orelse return -1;
    if (tt.kind == parser.TOK_INT) {
        out.* = tt.ival;
        return 0;
    }
    if (tt.kind == parser.TOK_IDENT or tt.kind == parser.TOK_STRING) {
        out.* = dx.intern_str(db_get_interner(db), cs(tt.text));
        if (out.* == 0) {
            var brief: [48]u8 = undefined;
            const tl = strLen(cs(tt.text));
            var pos: usize = 0;
            if (tl >= brief.len) {
                const keep = brief.len - 14;
                @memcpy(brief[0..keep], cs(tt.text)[0..keep]);
                pos = keep;
                @memcpy(brief[pos..][0..5], "... (");
                pos += 5;
            } else {
                @memcpy(brief[0..tl], cs(tt.text)[0..tl]);
                pos = tl;
            }
            var rev: [16]u8 = undefined;
            var x = tl;
            var dd: usize = 0;
            if (x == 0) {
                rev[dd] = '0';
                dd += 1;
            }
            while (x != 0) {
                rev[dd] = @intCast('0' + x % 10);
                dd += 1;
                x /= 10;
            }
            var i: usize = 0;
            while (i < dd) : (i += 1) brief[pos + i] = rev[dd - 1 - i];
            pos += dd;
            @memcpy(brief[pos..][0..7], " bytes)");
            brief[pos + 7] = 0;
            pos += 8;
            brief[pos] = 0;

            cerr(g_rule_off, "compile error: failed to intern string constant '{s}' (out of memory, or string exceeds the {d}-byte interner key limit)\n", .{ brief[0..pos], 4096 });
            return -1;
        }
        return 0;
    }
    if (tt.kind == parser.TOK_LIST) {
        if (list_is_pattern(tt) != 0) {
            cerr(g_rule_off, "compile error: internal: list pattern reached a constant position (nested list patterns are not supported)\n", .{});
            return -1;
        }
        var acc: u32 = TERM_NIL;
        var i: c_int = tt.nchildren - 1;
        while (i >= 0) : (i -= 1) {
            var eh: u32 = 0;
            if (token_const(db, tt.children.?[@intCast(i)], &eh) != 0) return -1;
            acc = dx.term_cons(db.terms, eh, acc);
            if (acc == 0) {
                cerr(g_rule_off, "compile error: out of memory interning a list literal (term store)\n", .{});
                return -1;
            }
        }
        out.* = acc;
        return 0;
    }
    cerr(g_rule_off, "compile error: internal: unexpected token kind {d} in a constant position\n", .{tt.kind});
    return -1;
}

// ─── List-pattern destructuring emission ───────────────────────────────────

fn emit_pattern(db: *dx.dl_db, pat: *const parser.token, src_slot: u8, vt: *v_tab, ib: *i_buf, cc: *c_int, head_pred: [*c]const u8) c_int {
    var cur: u8 = src_slot;
    var i: c_int = 0;
    while (i < pat.nchildren) : (i += 1) {
        const e = pat.children.?[@intCast(i)] orelse return -1;
        if (e.kind == parser.TOK_VAR) {
            const vi = v_find(vt, cs(e.text));
            if (vi < 0) return -1;
            const op = i_emit(ib) orelse return -1;
            op.op = OP_LIST_CAR;
            op.a = cur;
            op.c = vt.e.?[@intCast(vi)].slot;
        } else {
            var cname: [16]u8 = undefined;
            var cv: u32 = 0;
            const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, cc, 'p'));
            if (vi < 0) return -1;
            if (token_const(db, e, &cv) != 0) return -1;
            const op = i_emit(ib) orelse return -1;
            op.op = OP_LIST_CAR;
            op.a = cur;
            op.c = vt.e.?[@intCast(vi)].slot;
            const eq = i_emit(ib) orelse return -1;
            eq.op = OP_EQ_CONST;
            eq.a = vt.e.?[@intCast(vi)].slot;
            eq.imm = cv;
        }
        {
            var cname: [16]u8 = undefined;
            const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, cc, 'p'));
            if (vi < 0) return -1;
            const op = i_emit(ib) orelse return -1;
            op.op = OP_LIST_CDR;
            op.a = cur;
            op.c = vt.e.?[@intCast(vi)].slot;
            cur = vt.e.?[@intCast(vi)].slot;
        }
    }
    if (pat.tail != null) {
        const vi = v_find(vt, cs(pat.tail.?.text));
        if (vi < 0) return -1;
        const eq = i_emit(ib) orelse return -1;
        eq.op = OP_EQ;
        eq.a = vt.e.?[@intCast(vi)].slot;
        eq.b = cur;
    } else {
        const eq = i_emit(ib) orelse return -1;
        eq.op = OP_EQ_CONST;
        eq.a = cur;
        eq.imm = TERM_NIL;
    }
    _ = head_pred;
    return 0;
}

/// Emit post-join arg handling for atom a: each non-var arg is a constant
/// filter (OP_EQ_CONST) or a list pattern (destructured from slots[j]).
fn emit_const_or_pattern(db: *dx.dl_db, a: *const parser.atom, slots: [*]const u8, vt: *v_tab, ib: *i_buf, cc: *c_int, head_pred: [*c]const u8) c_int {
    var j: c_int = 0;
    while (j < a.nargs) : (j += 1) {
        const t = a.args.?[@intCast(j)] orelse return 0;
        if (t.kind == parser.TOK_VAR) continue;
        if (t.kind == parser.TOK_LIST and list_is_pattern(t) != 0) {
            if (emit_pattern(db, t, slots[@intCast(j)], vt, ib, cc, head_pred) != 0) return -1;
            continue;
        }
        var cv: u32 = 0;
        if (token_const(db, t, &cv) != 0) return -1;
        const eq = i_emit(ib) orelse return -1;
        eq.op = OP_EQ_CONST;
        eq.a = slots[@intCast(j)];
        eq.imm = cv;
    }
    return 0;
}

// ─── M9: builtin atom classification ───────────────────────────────────────

fn is_comparison(a: ?*const parser.atom) bool {
    const aa = a orelse return false;
    if (aa.pred == null) return false;
    const p = cs(aa.pred);
    return strEq(p, "<") or strEq(p, "<=") or strEq(p, ">") or strEq(p, ">=") or strEq(p, "!=");
}

fn is_arith(a: ?*const parser.atom) bool {
    const aa = a orelse return false;
    return aa.pred != null and strEq(cs(aa.pred), "=") and aa.arith != null;
}

fn is_equality(a: ?*const parser.atom) bool {
    const aa = a orelse return false;
    return aa.pred != null and strEq(cs(aa.pred), "=") and aa.nargs == 2 and aa.arith == null;
}

fn is_list_assign(a: ?*const parser.atom) bool {
    if (!is_equality(a)) return false;
    return a.?.args.?[0] != null and a.?.args.?[0].?.kind == parser.TOK_LIST;
}

fn is_str_producing(a: ?*const parser.atom) bool {
    const aa = a orelse return false;
    if (aa.pred == null) return false;
    const p = cs(aa.pred);
    return strEq(p, "concat") or strEq(p, "length") or strEq(p, "lower") or strEq(p, "upper");
}

fn is_str_filter(a: ?*const parser.atom) bool {
    const aa = a orelse return false;
    if (aa.pred == null) return false;
    const p = cs(aa.pred);
    return strEq(p, "prefix") or strEq(p, "suffix") or strEq(p, "contains");
}

fn is_str_builtin(a: ?*const parser.atom) bool {
    return is_str_producing(a) or is_str_filter(a);
}

fn is_list_producing(a: ?*const parser.atom) bool {
    const aa = a orelse return false;
    if (aa.pred == null) return false;
    const p = cs(aa.pred);
    return strEq(p, "cons") or strEq(p, "car") or strEq(p, "cdr") or strEq(p, "append");
}

fn is_list_filter(a: ?*const parser.atom) bool {
    const aa = a orelse return false;
    return aa.pred != null and strEq(cs(aa.pred), "member");
}

fn is_list_builtin(a: ?*const parser.atom) bool {
    return is_list_producing(a) or is_list_filter(a);
}

fn is_range_builtin(a: ?*const parser.atom) bool {
    const aa = a orelse return false;
    return aa.pred != null and strEq(cs(aa.pred), "range");
}

fn is_builtin_pred(a: ?*const parser.atom) bool {
    return is_equality(a) or is_comparison(a) or is_arith(a) or is_str_builtin(a) or is_list_builtin(a) or is_range_builtin(a);
}

fn is_reserved_builtin_name(name: [*c]const u8) bool {
    return strEq(name, "member") or strEq(name, "car") or strEq(name, "cons") or
        strEq(name, "cdr") or strEq(name, "append") or strEq(name, "concat") or
        strEq(name, "length") or strEq(name, "lower") or strEq(name, "upper") or
        strEq(name, "prefix") or strEq(name, "suffix") or strEq(name, "contains") or
        strEq(name, "range");
}

fn str_operand_ok(t: ?*const parser.token) bool {
    const tt = t orelse return false;
    return tt.kind == parser.TOK_VAR or tt.kind == parser.TOK_IDENT;
}

fn str_builtin_valid(a: *const parser.atom) c_int {
    if (a.pred == null) return 0;
    var i: c_int = 0;
    var need: c_int = 0;
    if (is_str_producing(a)) {
        need = if (strEq(cs(a.pred), "concat")) 3 else 2;
        if (a.nargs != need) return 0;
        if (a.args.?[0].?.kind != parser.TOK_VAR) return 0;
        i = 1;
        while (i < need) : (i += 1) {
            const t = a.args.?[@intCast(i)] orelse return 0;
            if (strEq(cs(a.pred), "length") and t.kind == parser.TOK_LIST) {
                if (list_is_pattern(t) != 0) return 0;
                continue;
            }
            if (!str_operand_ok(t)) return 0;
        }
        return 1;
    }
    if (is_str_filter(a)) {
        if (a.nargs != 2) return 0;
        i = 0;
        while (i < 2) : (i += 1) {
            if (!str_operand_ok(a.args.?[@intCast(i)])) return 0;
        }
        return 1;
    }
    return 0;
}

fn list_operand_ok(t: ?*const parser.token) bool {
    const tt = t orelse return false;
    if (tt.kind == parser.TOK_VAR or tt.kind == parser.TOK_INT or
        tt.kind == parser.TOK_IDENT or tt.kind == parser.TOK_STRING) return true;
    if (tt.kind == parser.TOK_LIST) return list_is_pattern(tt) == 0;
    return false;
}

fn list_builtin_valid(a: *const parser.atom) c_int {
    if (a.pred == null) return 0;
    var i: c_int = 0;
    var need: c_int = 0;
    if (is_list_filter(a)) {
        if (a.nargs != 2) return 0;
        if (a.args.?[0].?.kind != parser.TOK_VAR) return 0;
        if (!list_operand_ok(a.args.?[1])) return 0;
        return 1;
    }
    need = if (strEq(cs(a.pred), "cons") or strEq(cs(a.pred), "append")) 3 else 2;
    if (a.nargs != need) return 0;
    if (a.args.?[0].?.kind != parser.TOK_VAR) return 0;
    i = 1;
    while (i < need) : (i += 1) {
        if (!list_operand_ok(a.args.?[@intCast(i)])) return 0;
    }
    return 1;
}

fn range_operand_ok(t: ?*const parser.token) bool {
    const tt = t orelse return false;
    return tt.kind == parser.TOK_VAR or tt.kind == parser.TOK_INT;
}

fn range_builtin_valid(db: *dx.dl_db, a: *const parser.atom) c_int {
    if (a.pred == null) return 0;
    if (a.nargs != 4) return 0;
    if (a.args.?[0].?.kind != parser.TOK_VAR) return 0;
    if (a.args.?[1].?.kind != parser.TOK_IDENT) return 0;
    if (!range_operand_ok(a.args.?[2])) return 0;
    if (!range_operand_ok(a.args.?[3])) return 0;
    const ri = db_find_rel(db, cs(a.args.?[1].?.text));
    if (ri < 0) return 0;
    if (db_rel_is_variadic(db, ri) != 0) return 0;
    if (db_rel_arity(db, ri) < 1) return 0;
    return 1;
}

fn str_filter_code(pred: [*c]const u8) c_int {
    if (strEq(pred, "prefix")) return 0;
    if (strEq(pred, "suffix")) return 1;
    return 2;
}

fn str_bind_imm(pred: [*c]const u8) c_int {
    if (strEq(pred, "concat")) return 0;
    if (strEq(pred, "lower")) return 1;
    if (strEq(pred, "upper")) return 2;
    return -1;
}

fn cmp_op_code(pred: [*c]const u8) c_int {
    if (strEq(pred, "<")) return 0;
    if (strEq(pred, "<=")) return 1;
    if (strEq(pred, ">")) return 2;
    if (strEq(pred, ">=")) return 3;
    return 4;
}

fn arith_op_code(op: u8) c_int {
    return switch (op) {
        '+' => 0,
        '-' => 1,
        '*' => 2,
        '/' => 3,
        else => 4,
    };
}

fn collect_expr_vars(e: ?*const parser.expr, vt: *v_tab) void {
    const ee = e orelse return;
    if (ee.kind == parser.EX_VAR) {
        _ = v_add(vt, cs(ee.@"var"));
    } else if (ee.kind == parser.EX_BINOP) {
        collect_expr_vars(ee.l, vt);
        collect_expr_vars(ee.r, vt);
    }
}

fn expr_has_div0(e: ?*const parser.expr) c_int {
    const ee = e orelse return 0;
    if (ee.kind == parser.EX_BINOP) {
        if ((ee.op == '/' or ee.op == '%') and ee.r != null and
            ee.r.?.kind == parser.EX_INT and ee.r.?.ival == 0) return 1;
        return if (expr_has_div0(ee.l) != 0 or expr_has_div0(ee.r) != 0) 1 else 0;
    }
    return 0;
}

fn expr_vars_bound(e: ?*const parser.expr, vt: *v_tab, bound_vars: []const c_int, head_pred: [*c]const u8) c_int {
    const ee = e orelse return 1;
    if (ee.kind == parser.EX_VAR) {
        const vi = v_find(vt, cs(ee.@"var"));
        if (vi < 0 or bound_vars[@intCast(vi)] == 0) {
            cerr(g_rule_off, "compile error: ungrounded arithmetic operand — variable '{s}' is not bound by a positive body atom (rule '{s}')\n", .{ cs(ee.@"var"), head_pred });
            return 0;
        }
        return 1;
    }
    if (ee.kind == parser.EX_BINOP)
        return if (expr_vars_bound(ee.l, vt, bound_vars, head_pred) != 0 and
            expr_vars_bound(ee.r, vt, bound_vars, head_pred) != 0) 1 else 0;
    return 1;
}

/// Lower an expr tree postorder into bytecode.  Returns the slot index.
fn lower_expr(db: *dx.dl_db, e: ?*const parser.expr, vt: *v_tab, ib: *i_buf, cc: *c_int, tc: *c_int, body_idx: c_int) c_int {
    var cname: [16]u8 = undefined;
    const ee = e orelse return -1;
    switch (ee.kind) {
        parser.EX_INT => {
            const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, cc, 'k'));
            if (vi < 0) return -1;
            const in = i_emit(ib) orelse return -1;
            in.op = OP_EQ_CONST;
            in.a = vt.e.?[@intCast(vi)].slot;
            in.imm = ee.ival;
            return vt.e.?[@intCast(vi)].slot;
        },
        parser.EX_VAR => {
            const v = v_find(vt, cs(ee.@"var"));
            return if (v >= 0) vt.e.?[@intCast(v)].slot else -1;
        },
        parser.EX_BINOP => {
            const ls = lower_expr(db, ee.l, vt, ib, cc, tc, body_idx);
            const rs = lower_expr(db, ee.r, vt, ib, cc, tc, body_idx);
            if (ls < 0 or rs < 0) return -1;
            const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, tc, 't'));
            if (vi < 0) return -1;
            const in = i_emit(ib) orelse return -1;
            in.op = OP_ARITH;
            in.a = @intCast(ls);
            in.b = @intCast(rs);
            in.c = vt.e.?[@intCast(vi)].slot;
            in.imm = @intCast(arith_op_code(ee.op));
            in.body_idx = @intCast(body_idx);
            return vt.e.?[@intCast(vi)].slot;
        },
        else => return -1,
    }
}

/// Resolve a comparison operand to a slot.
fn cmp_operand_slot(db: *dx.dl_db, t: ?*const parser.token, vt: *v_tab, ib: *i_buf, cc: *c_int, allow_list: c_int) c_int {
    const tt = t orelse return -1;
    if (tt.kind == parser.TOK_LIST and allow_list == 0) {
        cerr(g_rule_off, "compile error: list literal not allowed in this position (lists have no meaningful order; use = / != via variables)\n", .{});
        return -1;
    }
    if (tt.kind == parser.TOK_VAR) {
        const vi = v_find(vt, cs(tt.text));
        return if (vi >= 0) vt.e.?[@intCast(vi)].slot else -1;
    }
    var cname: [16]u8 = undefined;
    var cv: u32 = 0;
    const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, cc, 'k'));
    if (vi < 0) return -1;
    if (token_const(db, tt, &cv) != 0) return -1;
    const eq = i_emit(ib) orelse return -1;
    eq.op = OP_EQ_CONST;
    eq.a = vt.e.?[@intCast(vi)].slot;
    eq.imm = cv;
    return vt.e.?[@intCast(vi)].slot;
}

fn str_producing_operands_bound(a: *const parser.atom, vt: *v_tab, bound_vars: []const c_int, head_pred: [*c]const u8) c_int {
    var j: c_int = 1;
    while (j < a.nargs) : (j += 1) {
        const t = a.args.?[@intCast(j)] orelse return 0;
        if (t.kind != parser.TOK_VAR) continue;
        const vi = v_find(vt, cs(t.text));
        if (vi < 0 or bound_vars[@intCast(vi)] == 0) {
            cerr(g_rule_off, "compile error: ungrounded string operand — variable '{s}' in '{s}' is not bound by a positive body atom (rule '{s}')\n", .{ cs(t.text), cs(a.pred), head_pred });
            return 0;
        }
    }
    return 1;
}

fn list_producing_operands_bound(a: *const parser.atom, vt: *v_tab, bound_vars: []const c_int, head_pred: [*c]const u8) c_int {
    var j: c_int = 1;
    while (j < a.nargs) : (j += 1) {
        const t = a.args.?[@intCast(j)] orelse return 0;
        if (t.kind != parser.TOK_VAR) continue;
        const vi = v_find(vt, cs(t.text));
        if (vi < 0 or bound_vars[@intCast(vi)] == 0) {
            cerr(g_rule_off, "compile error: ungrounded list operand — variable '{s}' in '{s}' is not bound by a positive body atom (rule '{s}')\n", .{ cs(t.text), cs(a.pred), head_pred });
            return 0;
        }
    }
    return 1;
}

fn member_operand_bound(a: *const parser.atom, vt: *v_tab, bound_vars: []const c_int, head_pred: [*c]const u8) c_int {
    const t = a.args.?[1] orelse return 0;
    if (t.kind != parser.TOK_VAR) return 1;
    const vi = v_find(vt, cs(t.text));
    if (vi < 0 or bound_vars[@intCast(vi)] == 0) {
        cerr(g_rule_off, "compile error: ungrounded member operand — list variable '{s}' in 'member' is not bound by a positive body atom (rule '{s}')\n", .{ cs(t.text), head_pred });
        return 0;
    }
    return 1;
}

fn range_operands_bound(a: *const parser.atom, vt: *v_tab, bound_vars: []const c_int, head_pred: [*c]const u8) c_int {
    var j: c_int = 2;
    while (j <= 3) : (j += 1) {
        const t = a.args.?[@intCast(j)] orelse return 0;
        if (t.kind != parser.TOK_VAR) continue;
        const vi = v_find(vt, cs(t.text));
        if (vi < 0 or bound_vars[@intCast(vi)] == 0) {
            cerr(g_rule_off, "compile error: ungrounded range bound — variable '{s}' in 'range' is not bound by a positive body atom (rule '{s}')\n", .{ cs(t.text), head_pred });
            return 0;
        }
    }
    return 1;
}

fn str_filter_operands_bound(a: *const parser.atom, vt: *v_tab, bound_vars: []const c_int, head_pred: [*c]const u8) c_int {
    var j: c_int = 0;
    while (j < a.nargs) : (j += 1) {
        const t = a.args.?[@intCast(j)] orelse return 0;
        if (t.kind != parser.TOK_VAR) continue;
        const vi = v_find(vt, cs(t.text));
        if (vi < 0 or bound_vars[@intCast(vi)] == 0) {
            cerr(g_rule_off, "compile error: ungrounded string filter — variable '{s}' in '{s}' is not bound by a positive body atom (rule '{s}')\n", .{ cs(t.text), cs(a.pred), head_pred });
            return 0;
        }
    }
    return 1;
}

// ─── Stratification (Kosaraju SCC + strict-stratification fixpoint) ────────

const Edge = struct {
    from: c_int,
    to: c_int,
    is_neg: c_int,
};

fn compute_strata(db: *dx.dl_db, rules: [*]?*parser.rule, n_rules: c_int, out_strata: [*]c_int, out_recursive: [*]c_int, out_max: c_int) c_int {
    const nrels = db_rel_count(db);
    if (nrels == 0 or out_max < 1) return 0;

    const stratum: [*]c_int = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse return -1));

    var edges: ?[*]Edge = null;
    var n_edges: c_int = 0;
    var edges_cap: c_int = 0;

    const add_edge = struct {
        fn call(e: *?[*]Edge, ne: *c_int, cap: *c_int, f: c_int, t: c_int, neg: c_int) bool {
            if (ne.* >= cap.*) {
                const nc: c_int = if (cap.* != 0) cap.* * 2 else 32;
                const arr = c.realloc(if (e.*) |ee| ee else null, @as(usize, @intCast(nc)) * @sizeOf(Edge));
                if (arr == null) return false;
                e.* = @ptrCast(@alignCast(arr));
                cap.* = nc;
            }
            e.*.?[@intCast(ne.*)] = .{ .from = f, .to = t, .is_neg = neg };
            ne.* += 1;
            return true;
        }
    }.call;

    const self_loop: [*]c_int = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse {
        c.free(@ptrCast(stratum));
        return -1;
    }));

    var i: c_int = 0;
    while (i < n_rules) : (i += 1) {
        const r = rules[@intCast(i)] orelse continue;
        const head_ri = db_find_rel(db, cs(r.head.?.pred));
        if (head_ri < 0) continue;
        var bi: c_int = 0;
        while (bi < r.nbody) : (bi += 1) {
            const ba = r.body.?[@intCast(bi)] orelse continue;
            if (is_range_builtin(ba) and ba.nargs >= 2 and
                ba.args.?[1] != null and ba.args.?[1].?.kind == parser.TOK_IDENT) {
                const rel_ri = db_find_rel(db, cs(ba.args.?[1].?.text));
                if (rel_ri >= 0) {
                    if (!add_edge(&edges, &n_edges, &edges_cap, rel_ri, head_ri, 1)) {
                        c.free(@ptrCast(self_loop));
                        c.free(@ptrCast(stratum));
                        return -1;
                    }
                }
                continue;
            }
            const body_ri = db_find_rel(db, cs(ba.pred));
            if (body_ri < 0) continue;
            if (!add_edge(&edges, &n_edges, &edges_cap, body_ri, head_ri, if (ba.negated != 0) 1 else 0)) {
                c.free(@ptrCast(self_loop));
                c.free(@ptrCast(stratum));
                return -1;
            }
            if (body_ri == head_ri) self_loop[@intCast(head_ri)] = 1;
        }
    }

    // Iterative fixpoint stratum assignment.
    var iteration: c_int = 0;
    var changed: c_int = 0;
    while (true) {
        changed = 0;
        i = 0;
        while (i < n_edges) : (i += 1) {
            const e = edges.?[@intCast(i)];
            const from_s = stratum[@intCast(e.from)];
            const to_s = stratum[@intCast(e.to)];
            var needed: c_int = undefined;
            if (e.is_neg != 0) {
                needed = from_s + 1;
                if (needed > 1000000) needed = 1000000;
            } else {
                needed = from_s;
            }
            if (to_s < needed) {
                stratum[@intCast(e.to)] = needed;
                changed = 1;
            }
        }
        iteration += 1;
        if (changed == 0 or iteration >= 10000) break;
    }

    if (changed != 0) {
        cerr(0, "compile error: unstratifiable program — negation (or range) through a recursion cycle is not supported; only stratified negation is (negate a recursive predicate from a strictly-higher stratum)\n", .{});
        c.free(@ptrCast(self_loop));
        c.free(@ptrCast(edges));
        c.free(@ptrCast(stratum));
        return -1;
    }

    // Kosaraju SCC on the positive subgraph.
    var comp: ?[*]c_int = null;
    {
        const out_deg: [*]c_int = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse {
            c.free(@ptrCast(self_loop));
            c.free(@ptrCast(edges));
            c.free(@ptrCast(stratum));
            return -1;
        }));
        const rev_deg: [*]c_int = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse {
            c.free(@ptrCast(out_deg));
            c.free(@ptrCast(self_loop));
            c.free(@ptrCast(edges));
            c.free(@ptrCast(stratum));
            return -1;
        }));

        var ri: usize = 0;
        i = 0;
        while (i < n_edges) : (i += 1) {
            const e = edges.?[@intCast(i)];
            if (e.is_neg == 0) {
                out_deg[@intCast(e.from)] += 1;
                rev_deg[@intCast(e.to)] += 1;
            }
        }

        var adj: ?[*]?[*]c_int = null;
        var rev_adj: ?[*]?[*]c_int = null;
        var adj_cap: ?[*]c_int = null;
        var rev_cap: ?[*]c_int = null;
        var visited: ?[*]c_int = null;
        var order: ?[*]c_int = null;
        var scc_failed = false;

        adj = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(?[*]c_int)) orelse {
            c.free(@ptrCast(out_deg));
            c.free(@ptrCast(rev_deg));
            c.free(@ptrCast(self_loop));
            c.free(@ptrCast(edges));
            c.free(@ptrCast(stratum));
            return -1;
        }));
        rev_adj = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(?[*]c_int)) orelse {
            c.free(@ptrCast(adj));
            c.free(@ptrCast(out_deg));
            c.free(@ptrCast(rev_deg));
            c.free(@ptrCast(self_loop));
            c.free(@ptrCast(edges));
            c.free(@ptrCast(stratum));
            return -1;
        }));
        adj_cap = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse {
            c.free(@ptrCast(rev_adj));
            c.free(@ptrCast(adj));
            c.free(@ptrCast(out_deg));
            c.free(@ptrCast(rev_deg));
            c.free(@ptrCast(self_loop));
            c.free(@ptrCast(edges));
            c.free(@ptrCast(stratum));
            return -1;
        }));
        rev_cap = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse {
            c.free(@ptrCast(adj_cap));
            c.free(@ptrCast(rev_adj));
            c.free(@ptrCast(adj));
            c.free(@ptrCast(out_deg));
            c.free(@ptrCast(rev_deg));
            c.free(@ptrCast(self_loop));
            c.free(@ptrCast(edges));
            c.free(@ptrCast(stratum));
            return -1;
        }));

        ri = 0;
        while (ri < nrels) : (ri += 1) {
            if (out_deg[@intCast(ri)] > 0) {
                adj.?[@intCast(ri)] = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(out_deg[@intCast(ri)])) * @sizeOf(c_int)) orelse {
                    scc_failed = true;
                    break;
                }));
                adj_cap.?[@intCast(ri)] = out_deg[@intCast(ri)];
                out_deg[@intCast(ri)] = 0;
            }
            if (rev_deg[@intCast(ri)] > 0) {
                rev_adj.?[@intCast(ri)] = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(rev_deg[@intCast(ri)])) * @sizeOf(c_int)) orelse {
                    scc_failed = true;
                    break;
                }));
                rev_cap.?[@intCast(ri)] = rev_deg[@intCast(ri)];
                rev_deg[@intCast(ri)] = 0;
            }
        }

        if (!scc_failed) {
            i = 0;
            while (i < n_edges) : (i += 1) {
                const e = edges.?[@intCast(i)];
                if (e.is_neg == 0) {
                    const f = e.from;
                    const t = e.to;
                    adj.?[@intCast(f)].?[@intCast(out_deg[@intCast(f)])] = t;
                    out_deg[@intCast(f)] += 1;
                    rev_adj.?[@intCast(t)].?[@intCast(rev_deg[@intCast(t)])] = f;
                    rev_deg[@intCast(t)] += 1;
                }
            }

            visited = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse blk: {
                 scc_failed = true;
                break :blk null;
                }));
            if (!scc_failed) {
                order = @ptrCast(@alignCast(c.malloc(nrels * @sizeOf(c_int)) orelse blk: {
                     scc_failed = true;
                    break :blk null;
                    }));
            }
            if (!scc_failed) {
                comp = @ptrCast(@alignCast(c.malloc(nrels * @sizeOf(c_int)) orelse blk: {
                     scc_failed = true;
                    break :blk null;
                    }));
            }

            if (!scc_failed) {
                var order_n: c_int = 0;
                // First pass: iterative DFS on forward graph for postorder.
                {
                    const stack_node: [*]c_int = @ptrCast(@alignCast(c.malloc((nrels + 1) * @sizeOf(c_int)) orelse blk: {
                         scc_failed = true;
                        break :blk null;
                        }));
                    const stack_idx: [*]c_int = @ptrCast(@alignCast(c.malloc((nrels + 1) * @sizeOf(c_int)) orelse blk: {
                        c.free(@ptrCast(stack_node));
                        scc_failed = true;
                        break :blk null;
                    }));
                    if (!scc_failed) {
                        var sp: c_int = 0;
                        ri = 0;
                        while (ri < nrels) : (ri += 1) {
                            if (visited.?[@intCast(ri)] != 0) continue;
                            visited.?[@intCast(ri)] = 1;
                            stack_node[@intCast(sp)] = @intCast(ri);
                            stack_idx[@intCast(sp)] = 0;
                            sp += 1;
                            while (sp > 0) {
                                const node = stack_node[@intCast(sp - 1)];
                                var idx = stack_idx[@intCast(sp - 1)];
                                var found: c_int = 0;
                                while (idx < adj_cap.?[@intCast(node)]) {
                                    const nb = adj.?[@intCast(node)].?[@intCast(idx)];
                                    stack_idx[@intCast(sp - 1)] = idx + 1;
                                    if (visited.?[@intCast(nb)] == 0) {
                                        visited.?[@intCast(nb)] = 1;
                                        stack_node[@intCast(sp)] = nb;
                                        stack_idx[@intCast(sp)] = 0;
                                        sp += 1;
                                        found = 1;
                                        break;
                                    }
                                    idx = stack_idx[@intCast(sp - 1)];
                                }
                                if (found == 0) {
                                    order.?[@intCast(order_n)] = node;
                                    order_n += 1;
                                    sp -= 1;
                                }
                            }
                        }
                        c.free(@ptrCast(stack_node));
                        c.free(@ptrCast(stack_idx));
                    }
                }

                if (!scc_failed) {
                    // Second pass: DFS on reverse graph in reverse postorder.
                    @memset(visited.?[0..nrels], 0);
                    const stack_node: [*]c_int = @ptrCast(@alignCast(c.malloc((nrels + 1) * @sizeOf(c_int)) orelse blk: {
                         scc_failed = true;
                        break :blk null;
                        }));
                    const stack_idx: [*]c_int = @ptrCast(@alignCast(c.malloc((nrels + 1) * @sizeOf(c_int)) orelse blk: {
                        c.free(@ptrCast(stack_node));
                        scc_failed = true;
                        break :blk null;
                    }));
                    if (!scc_failed) {
                        i = order_n - 1;
                        while (i >= 0) : (i -= 1) {
                            const root = order.?[@intCast(i)];
                            if (visited.?[@intCast(root)] != 0) continue;
                            var scc_size: c_int = 0;
                            visited.?[@intCast(root)] = 1;
                            var sp: c_int = 0;
                            stack_node[@intCast(sp)] = root;
                            stack_idx[@intCast(sp)] = 0;
                            sp += 1;
                            while (sp > 0) {
                                const node = stack_node[@intCast(sp - 1)];
                                var idx = stack_idx[@intCast(sp - 1)];
                                var found: c_int = 0;
                                while (idx < rev_cap.?[@intCast(node)]) {
                                    const nb = rev_adj.?[@intCast(node)].?[@intCast(idx)];
                                    stack_idx[@intCast(sp - 1)] = idx + 1;
                                    if (visited.?[@intCast(nb)] == 0) {
                                        visited.?[@intCast(nb)] = 1;
                                        stack_node[@intCast(sp)] = nb;
                                        stack_idx[@intCast(sp)] = 0;
                                        sp += 1;
                                        found = 1;
                                        break;
                                    }
                                    idx = stack_idx[@intCast(sp - 1)];
                                }
                                if (found == 0) {
                                    comp.?[@intCast(node)] = root;
                                    scc_size += 1;
                                    sp -= 1;
                                }
                            }
                            if (scc_size > 1) {
                                ri = 0;
                                while (ri < nrels) : (ri += 1) {
                                    if (comp.?[@intCast(ri)] == root) out_recursive[@intCast(ri)] = 1;
                                }
                            }
                        }
                        c.free(@ptrCast(stack_node));
                        c.free(@ptrCast(stack_idx));
                    }
                }
            }
        }

        // Self-loops also make a predicate recursive.
        ri = 0;
        while (ri < nrels) : (ri += 1) {
            if (self_loop[@intCast(ri)] != 0) out_recursive[@intCast(ri)] = 1;
        }

        // Cleanup adjacency structures.
        ri = 0;
        while (ri < nrels) : (ri += 1) {
            c.free(@ptrCast(adj.?[@intCast(ri)]));
            c.free(@ptrCast(rev_adj.?[@intCast(ri)]));
        }
        c.free(@ptrCast(adj));
        c.free(@ptrCast(rev_adj));
        c.free(@ptrCast(adj_cap));
        c.free(@ptrCast(rev_cap));
        c.free(@ptrCast(out_deg));
        c.free(@ptrCast(rev_deg));
        c.free(@ptrCast(visited));
        c.free(@ptrCast(order));

        if (scc_failed) {
            c.free(@ptrCast(comp));
            c.free(@ptrCast(self_loop));
            c.free(@ptrCast(edges));
            c.free(@ptrCast(stratum));
            return -1;
        }
    }

    // Seed recursion from ALREADY-COMPILED rules (carry recursion across
    // dl_load_rules batches).
    if (db.crules != null) {
        // translate-c maps `compiled_rule **` to `[*c][*c]compiled_rule`; cast
        // to a many-item pointer of optional single-item pointers for indexing.
        const crules: [*]?*dx.compiled_rule = @ptrCast(db.crules);
        i = 0;
        while (i < db.n_crules) : (i += 1) {
            const pcr = crules[@intCast(i)] orelse continue;
            if (pcr.is_recursive != 0 and pcr.head_rel_id < @as(u8, @truncate(nrels)))
                out_recursive[@intCast(pcr.head_rel_id)] = 1;
        }
    }

    {
        var iter2: c_int = 0;
        while (true) {
            changed = 0;
            i = 0;
            while (i < n_edges) : (i += 1) {
                const e = edges.?[@intCast(i)];
                const from_s = stratum[@intCast(e.from)];
                const to_s = stratum[@intCast(e.to)];
                var needed: c_int = undefined;
                if (e.is_neg != 0) {
                    needed = from_s + 1;
                    if (needed > 1000000) needed = 1000000;
                } else if (out_recursive[@intCast(e.from)] != 0 and comp != null and
                    comp.?[@intCast(e.from)] != comp.?[@intCast(e.to)]) {
                    needed = from_s + 1;
                    if (needed > 1000000) needed = 1000000;
                } else {
                    needed = from_s;
                }
                if (to_s < needed) {
                    stratum[@intCast(e.to)] = needed;
                    changed = 1;
                }
            }
            iter2 += 1;
            if (changed == 0 or iter2 >= 10000) break;
        }

        if (changed != 0) {
            cerr(0, "compile error: unstratifiable program — negation (or range) through a recursion cycle is not supported; only stratified negation is (negate a recursive predicate from a strictly-higher stratum)\n", .{});
            c.free(@ptrCast(comp));
            comp = null;
            c.free(@ptrCast(self_loop));
            c.free(@ptrCast(edges));
            c.free(@ptrCast(stratum));
            return -1;
        }
    }

    c.free(@ptrCast(comp));
    comp = null;

    // Check for unstratifiable (after both fixpoint passes).
    i = 0;
    while (i < n_edges) : (i += 1) {
        const e = edges.?[@intCast(i)];
        if (e.is_neg != 0) {
            if (stratum[@intCast(e.to)] <= stratum[@intCast(e.from)]) {
                cerr(0, "compile error: unstratifiable program — negation through recursion: '{s}' depends negatively on '{s}' in the same SCC\n", .{ cs(db_rel_name(db, e.from)), cs(db_rel_name(db, e.to)) });
                c.free(@ptrCast(self_loop));
                c.free(@ptrCast(edges));
                c.free(@ptrCast(stratum));
                return -1;
            }
        }
    }

    c.free(@ptrCast(self_loop));
    c.free(@ptrCast(edges));

    {
        var ri: usize = 0;
        while (ri < nrels) : (ri += 1) out_strata[@intCast(ri)] = stratum[@intCast(ri)];
    }

    c.free(@ptrCast(stratum));
    return 0;
}

// ─── BUSHY (v2): natural-partition join planning ───────────────────────────

const BUSHY_MAX_CUT = 2;
const BUSHY_MAX_ATOMS = 16;

const emit_ctx = struct {
    db: *dx.dl_db,
    r: *const parser.rule,
    bri: [*]const c_int,
    pat_idx: [*]const u8,
    pat_col: [*]const u8,
    vt: *v_tab,
    ib: *i_buf,
    cc: *c_int,
    mask: [*]const u64,
    recursive: ?[*]const c_int,
    do_bushy: c_int,
    next_buf: c_int,
};

fn popcount64(x: u64) c_int {
    return @intCast(@popCount(x));
}

fn atom_var_mask(r: *const parser.rule, bi: c_int, vt: *v_tab) u64 {
    const a = r.body.?[@intCast(bi)] orelse return 0;
    var m: u64 = 0;
    var j: c_int = 0;
    while (j < a.nargs) : (j += 1) mask_token_vars(&m, a.args.?[@intCast(j)], vt);
    return m;
}

fn atoms_connected(atoms: [*]const c_int, n: c_int, mask: [*]const u64) c_int {
    var visited = [_]u8{0} ** 64;
    var stack: [64]c_int = undefined;
    var sp: c_int = 0;
    if (n <= 1) return 1;
    visited[0] = 1;
    stack[@intCast(sp)] = 0;
    sp += 1;
    while (sp > 0) {
        sp -= 1;
        const u = stack[@intCast(sp)];
        var v: c_int = 0;
        while (v < n) : (v += 1) {
            if (visited[@intCast(v)] != 0) continue;
            if ((mask[@intCast(atoms[@intCast(u)])] & mask[@intCast(atoms[@intCast(v)])]) != 0) {
                visited[@intCast(v)] = 1;
                stack[@intCast(sp)] = v;
                sp += 1;
            }
        }
    }
    var i: c_int = 0;
    while (i < n) : (i += 1) {
        if (visited[@intCast(i)] == 0) return 0;
    }
    return 1;
}

fn min_cut_split(atoms: [*]const c_int, n: c_int, mask: [*]const u64, best_l: *u32, best_r: *u32, best_w: *c_int) c_int {
    const total: u32 = if (n >= 32) 0xFFFFFFFF else (@as(u32, 1) << @intCast(n)) - 1;
    var found: c_int = 0;
    var best: c_int = 1 << 20;
    var bL: u32 = 0;
    var bR: u32 = 0;
    var sub: u32 = 1;
    while (sub < total) : (sub += 1) {
        const comp: u32 = total & ~sub;
        const nl = popcount64(sub);
        const nr = n - nl;
        var L: [64]c_int = undefined;
        var R: [64]c_int = undefined;
        var il: c_int = 0;
        var ir: c_int = 0;
        var mL: u64 = 0;
        var mR: u64 = 0;
        if (nl < 2 or nr < 2) continue;
        var i: c_int = 0;
        while (i < n) : (i += 1) {
            if ((sub & (@as(u32, 1) << @intCast(i))) != 0) {
                L[@intCast(il)] = atoms[@intCast(i)];
                il += 1;
                mL |= mask[@intCast(atoms[@intCast(i)])];
            } else {
                R[@intCast(ir)] = atoms[@intCast(i)];
                ir += 1;
                mR |= mask[@intCast(atoms[@intCast(i)])];
            }
        }
        if (atoms_connected(&L, il, mask) == 0) continue;
        if (atoms_connected(&R, ir, mask) == 0) continue;
        const w = popcount64(mL & mR);
        if (w < best) {
            best = w;
            bL = sub;
            bR = comp;
            found = 1;
        }
    }
    if (found == 0) return 0;
    best_l.* = bL;
    best_r.* = bR;
    best_w.* = best;
    return 1;
}

fn mask_to_slots(m: u64, out: [*]u8) c_int {
    var n: c_int = 0;
    var s: u8 = 0;
    while (s < 64) : (s += 1) {
        if (m & (@as(u64, 1) << @intCast(s)) != 0) {
            out[@intCast(n)] = s;
            n += 1;
        }
    }
    return n;
}

fn reorder_pos_atoms(db: *dx.dl_db, bri: [*]const c_int, mask: [*]const u64, order: [*]c_int, n: c_int) void {
    const m = if (n > 0) @as(usize, @intCast(n)) else 1;
    const used: [*]c_int = @ptrCast(@alignCast(c.calloc(m, @sizeOf(c_int)) orelse return));
    const res: [*]c_int = @ptrCast(@alignCast(c.malloc(m * @sizeOf(c_int)) orelse {
        c.free(@ptrCast(used));
        return;
    }));
    var placed_mask: u64 = 0;
    var out: c_int = 0;
    while (out < n) : (out += 1) {
        var best: c_int = -1;
        var bc: u64 = 0;
        var bsh: c_int = -1;
        var k: c_int = 0;
        while (k < n) : (k += 1) {
            if (used[@intCast(k)] != 0) continue;
            const cnt = db_rel_card(db, bri[@intCast(order[@intCast(k)])]);
            const s = popcount64(mask[@intCast(order[@intCast(k)])] & placed_mask);
            if (best < 0 or cnt < bc or (cnt == bc and s > bsh)) {
                best = k;
                bc = cnt;
                bsh = s;
            }
        }
        used[@intCast(best)] = 1;
        res[@intCast(out)] = order[@intCast(best)];
        placed_mask |= mask[@intCast(order[@intCast(best)])];
    }
    @memcpy(order[0..@intCast(n)], res[0..@intCast(n)]);
    c.free(@ptrCast(used));
    c.free(@ptrCast(res));
}

fn pack_perm(perm: [*]const u8, arity: u8) u32 {
    var p: u32 = 0;
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        p |= @as(u32, perm[i] & 7) << @intCast(3 * @as(u32, i));
    }
    return p;
}

fn emit_nonleading_join(db: *dx.dl_db, r: *const parser.rule, curr: *const parser.atom, rel_id: c_int, sc: [*]const c_int, recursive: ?[*]const c_int, vt: *v_tab, ib: *i_buf, cc: *c_int, ip: *vm_instr, bi: c_int) c_int {
    var perm_arr: [8]u8 = undefined;
    var n_join: u8 = 0;
    var n_other: u8 = 0;
    var join_cols: [8]u8 = undefined;
    var other_cols: [8]u8 = undefined;
    var j: c_int = 0;
    while (j < curr.nargs) : (j += 1) {
        if (sc[@intCast(j)] != 0) {
            join_cols[@intCast(n_join)] = @intCast(j);
            n_join += 1;
        } else {
            other_cols[@intCast(n_other)] = @intCast(j);
            n_other += 1;
        }
    }
    var pj: u8 = 0;
    while (pj < n_join) : (pj += 1) perm_arr[@intCast(pj)] = join_cols[@intCast(pj)];
    pj = 0;
    while (pj < n_other) : (pj += 1) perm_arr[@intCast(n_join + pj)] = other_cols[@intCast(pj)];

    var use_perm: c_int = 0;
    var perm_id: c_int = -1;

    if (recursive != null and recursive.?[@intCast(rel_id)] != 0) {
        perm_id = dx.dl_db_declare_perm(db, rel_id, @intCast(curr.nargs), &perm_arr);
        if (perm_id < 0) {
            cerr(r.off, "compile error: too many permutation indices (rule '{s}')\n", .{cs(r.head.?.pred)});
            return -1;
        }
        use_perm = 1;
    } else if (gPermSelectRef().* == 0) {
        use_perm = 0;
    } else {
        const existing = dx.dl_db_find_perm(db, rel_id, @intCast(curr.nargs), &perm_arr);
        if (existing >= 0) {
            perm_id = existing;
            use_perm = 1;
        } else if (db_rel_card_est(db, rel_id) >= @as(u64, @intCast(gPermCardThresholdRef().*))) {
            perm_id = dx.dl_db_declare_perm(db, rel_id, @intCast(curr.nargs), &perm_arr);
            if (perm_id >= 0) use_perm = 1;
        }
    }

    ip.op = if (use_perm != 0) OP_LOOKUP_PERM else OP_HASH_JOIN;
    ip.a = @intCast(rel_id);
    ip.b = n_join;
    ip.c = @intCast(curr.nargs);
    ip.body_idx = @intCast(bi);
    ip.imm = if (use_perm != 0) @intCast(perm_id) else pack_perm(&perm_arr, @intCast(curr.nargs));

    j = 0;
    while (j < curr.nargs) : (j += 1) {
        if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
            const vi = v_find(vt, cs(curr.args.?[@intCast(j)].?.text));
            ip.slots[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
        } else {
            var cname: [16]u8 = undefined;
            const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, cc, 'k'));
            ip.slots[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
        }
    }
    if (emit_const_or_pattern(db, curr, &ip.slots, vt, ib, cc, cs(r.head.?.pred)) != 0) return -1;
    return 0;
}

fn emit_pos_atom(db: *dx.dl_db, r: *const parser.rule, bi: c_int, bri: [*]const c_int, pat_idx: [*]const u8, pat_col: [*]const u8, bound: u64, is_first: c_int, vt: *v_tab, ib: *i_buf, cc: *c_int, recursive: ?[*]const c_int) c_int {
    const curr = r.body.?[@intCast(bi)] orelse return -1;
    var sc: [8]c_int = [_]c_int{0} ** 8;
    var k: c_int = 0;
    var j: c_int = 0;

    if (is_first == 0) {
        j = 0;
        while (j < curr.nargs) : (j += 1) {
            if (curr.args.?[@intCast(j)].?.kind != parser.TOK_VAR) continue;
            const vi = v_find(vt, cs(curr.args.?[@intCast(j)].?.text));
            if (vi >= 0 and vi < 64 and (bound & (@as(u64, 1) << @intCast(vi))) != 0) sc[@intCast(j)] = 1;
        }
        while (k < curr.nargs and sc[@intCast(k)] != 0) k += 1;
    }

    const ip = i_emit(ib) orelse return -1;

    if (pat_idx[@intCast(bi)] != 0xFF) {
        var ts: [8]u8 = undefined;
        ip.op = OP_WALK;
        ip.imm = pat_idx[@intCast(bi)];
        ip.c = pat_col[@intCast(bi)];
        ip.a = @intCast(bri[@intCast(bi)]);
        ip.b = @intCast(curr.nargs);
        ip.body_idx = @intCast(bi);
        j = 0;
        while (j < curr.nargs) : (j += 1) {
            if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                const vi = v_find(vt, cs(curr.args.?[@intCast(j)].?.text));
                ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
            } else {
                var cname: [16]u8 = undefined;
                const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, cc, 'k'));
                ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
            }
        }
        j = 0;
        while (j < curr.nargs) : (j += 1) ip.slots[@intCast(j)] = ts[@intCast(j)];
        j = 0;
        while (j < curr.nargs) : (j += 1) {
            if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) continue;
            var cv: u32 = 0;
            if (token_const(db, curr.args.?[@intCast(j)], &cv) != 0) return -1;
            const eq = i_emit(ib) orelse return -1;
            eq.op = OP_EQ_CONST;
            eq.a = ts[@intCast(j)];
            eq.imm = cv;
        }
        return 0;
    }

    if (is_first != 0 or k == 0) {
        var any_shared: c_int = 0;
        if (is_first == 0) {
            j = 0;
            while (j < curr.nargs) : (j += 1) {
                if (sc[@intCast(j)] != 0) {
                    any_shared = 1;
                    break;
                }
            }
        }
        if (is_first == 0 and any_shared != 0) {
            if (emit_nonleading_join(db, r, curr, bri[@intCast(bi)], &sc, recursive, vt, ib, cc, ip, bi) != 0) return -1;
        } else {
            var ts: [8]u8 = undefined;
            ip.op = OP_SCAN;
            ip.a = @intCast(bri[@intCast(bi)]);
            ip.b = @intCast(curr.nargs);
            ip.body_idx = @intCast(bi);
            j = 0;
            while (j < curr.nargs) : (j += 1) {
                if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                    const vi = v_find(vt, cs(curr.args.?[@intCast(j)].?.text));
                    ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                } else {
                    var cname: [16]u8 = undefined;
                    const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, cc, 'k'));
                    ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                }
            }
            j = 0;
            while (j < curr.nargs) : (j += 1) ip.slots[@intCast(j)] = ts[@intCast(j)];
            if (emit_const_or_pattern(db, curr, &ts, vt, ib, cc, cs(r.head.?.pred)) != 0) return -1;
        }
    } else {
        var slot_map: [8]u8 = undefined;
        var si: c_int = 0;
        ip.op = OP_LOOKUP;
        ip.a = @intCast(bri[@intCast(bi)]);
        ip.b = @intCast(k);
        ip.c = @intCast(curr.nargs);
        ip.body_idx = @intCast(bi);
        j = 0;
        while (j < k) : (j += 1) {
            const vi = v_find(vt, cs(curr.args.?[@intCast(j)].?.text));
            ip.slots[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
            slot_map[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
        }
        si = k;
        j = k;
        while (j < curr.nargs) : (j += 1) {
            if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                const vi = v_find(vt, cs(curr.args.?[@intCast(j)].?.text));
                ip.slots[@intCast(si)] = vt.e.?[@intCast(vi)].slot;
                slot_map[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
            } else {
                var cname: [16]u8 = undefined;
                const vi = v_add(vt, v_fresh_name(vt, &cname, cname.len, cc, 'k'));
                ip.slots[@intCast(si)] = vt.e.?[@intCast(vi)].slot;
                slot_map[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
            }
            si += 1;
        }
        if (emit_const_or_pattern(db, curr, &slot_map, vt, ib, cc, cs(r.head.?.pred)) != 0) return -1;
    }
    return 0;
}

fn emit_iface_project(ec: *emit_ctx, slots: [*]const u8, ar: c_int) c_int {
    const pr = i_emit(ec.ib) orelse return -1;
    var j: c_int = 0;
    pr.op = OP_PROJECT;
    pr.a = 0xFF;
    pr.b = @intCast(ar);
    while (j < ar) : (j += 1) pr.slots[@intCast(j)] = slots[@intCast(j)];
    return 0;
}

fn emit_join(ec: *emit_ctx, atoms: [*]const c_int, n: c_int, iface_slots: ?[*]const u8, iface_ar: c_int) c_int {
    var all: u64 = 0;
    var i: c_int = 0;
    while (i < n) : (i += 1) all |= ec.mask[@intCast(atoms[@intCast(i)])];
    if (ec.do_bushy != 0 and n >= 4 and n <= BUSHY_MAX_ATOMS and popcount64(all) <= MAX_ARITY) {
        var bl: u32 = 0;
        var br: u32 = 0;
        var w: c_int = 0;
        if (min_cut_split(atoms, n, ec.mask, &bl, &br, &w) != 0 and w <= BUSHY_MAX_CUT)
            return emit_bushy_tree(ec, atoms, n, iface_slots, iface_ar);
    }
    return emit_leftdeep_seq(ec, atoms, n, iface_slots, iface_ar);
}

fn emit_leftdeep_seq(ec: *emit_ctx, atoms: [*]const c_int, n: c_int, iface_slots: ?[*]const u8, iface_ar: c_int) c_int {
    var bound: u64 = 0;
    var i: c_int = 0;
    while (i < n) : (i += 1) {
        if (emit_pos_atom(ec.db, ec.r, atoms[@intCast(i)], ec.bri, ec.pat_idx, ec.pat_col, bound, if (i == 0) 1 else 0, ec.vt, ec.ib, ec.cc, ec.recursive) != 0) return -1;
        bound |= ec.mask[@intCast(atoms[@intCast(i)])];
    }
    if (iface_slots) |ifs| return emit_iface_project(ec, ifs, iface_ar);
    return 0;
}

fn emit_bushy_tree(ec: *emit_ctx, atoms: [*]const c_int, n: c_int, iface_slots: ?[*]const u8, iface_ar: c_int) c_int {
    var bl: u32 = 0;
    var br: u32 = 0;
    var w: c_int = 0;
    if (min_cut_split(atoms, n, ec.mask, &bl, &br, &w) == 0) return -1;

    var L: [64]c_int = undefined;
    var R: [64]c_int = undefined;
    var nl: c_int = 0;
    var nr: c_int = 0;
    var mL: u64 = 0;
    var mR: u64 = 0;
    var i: c_int = 0;
    while (i < n) : (i += 1) {
        if ((bl & (@as(u32, 1) << @intCast(i))) != 0) {
            L[@intCast(nl)] = atoms[@intCast(i)];
            nl += 1;
            mL |= ec.mask[@intCast(atoms[@intCast(i)])];
        } else {
            R[@intCast(nr)] = atoms[@intCast(i)];
            nr += 1;
            mR |= ec.mask[@intCast(atoms[@intCast(i)])];
        }
    }
    var sh_slots: [8]u8 = undefined;
    var lp_slots: [8]u8 = undefined;
    var rp_slots: [8]u8 = undefined;
    const nsh = mask_to_slots(mL & mR, &sh_slots);
    const nlp = mask_to_slots(mL & ~mR, &lp_slots);
    const nrp = mask_to_slots(mR & ~mL, &rp_slots);

    var liface: [8]u8 = undefined;
    var riface: [8]u8 = undefined;
    var out_slots: [8]u8 = undefined;
    var la: c_int = 0;
    var ra: c_int = 0;
    var oa: c_int = 0;
    var j: c_int = 0;
    while (j < nsh) : (j += 1) {
        liface[@intCast(la)] = sh_slots[@intCast(j)];
        la += 1;
    }
    j = 0;
    while (j < nlp) : (j += 1) {
        liface[@intCast(la)] = lp_slots[@intCast(j)];
        la += 1;
    }
    j = 0;
    while (j < nsh) : (j += 1) {
        riface[@intCast(ra)] = sh_slots[@intCast(j)];
        ra += 1;
    }
    j = 0;
    while (j < nrp) : (j += 1) {
        riface[@intCast(ra)] = rp_slots[@intCast(j)];
        ra += 1;
    }
    j = 0;
    while (j < nsh) : (j += 1) {
        out_slots[@intCast(oa)] = sh_slots[@intCast(j)];
        oa += 1;
    }
    j = 0;
    while (j < nlp) : (j += 1) {
        out_slots[@intCast(oa)] = lp_slots[@intCast(j)];
        oa += 1;
    }
    j = 0;
    while (j < nrp) : (j += 1) {
        out_slots[@intCast(oa)] = rp_slots[@intCast(j)];
        oa += 1;
    }

    const bL = ec.next_buf;
    ec.next_buf += 1;
    const bR = ec.next_buf;
    ec.next_buf += 1;

    var mb = i_emit(ec.ib) orelse return -1;
    mb.op = OP_MAT_BEGIN;
    mb.a = @intCast(bL);
    mb.b = @intCast(la);
    mb.imm = 0;
    j = 0;
    while (j < la) : (j += 1) mb.slots[@intCast(j)] = liface[@intCast(j)];
    const left_begin = ec.ib.n - 1;
    if (emit_join(ec, &L, nl, &liface, la) != 0) return -1;
    ec.ib.b.?[@intCast(left_begin)].imm = @intCast(ec.ib.n);

    mb = i_emit(ec.ib) orelse return -1;
    mb.op = OP_MAT_BEGIN;
    mb.a = @intCast(bR);
    mb.b = @intCast(ra);
    mb.imm = 0;
    j = 0;
    while (j < ra) : (j += 1) mb.slots[@intCast(j)] = riface[@intCast(j)];
    const right_begin = ec.ib.n - 1;
    if (emit_join(ec, &R, nr, &riface, ra) != 0) return -1;
    ec.ib.b.?[@intCast(right_begin)].imm = @intCast(ec.ib.n);

    const mj = i_emit(ec.ib) orelse return -1;
    mj.op = OP_MAT_JOIN;
    mj.a = @intCast(bL);
    mj.b = @intCast(bR);
    mj.c = @intCast(nsh);
    j = 0;
    while (j < oa) : (j += 1) mj.slots[@intCast(j)] = out_slots[@intCast(j)];

    if (iface_slots) |ifs| return emit_iface_project(ec, ifs, iface_ar);
    return 0;
}

// ─── Compile one rule ──────────────────────────────────────────────────────

fn compile_one(db: *dx.dl_db, r: *parser.rule, rel_strata: ?[*]const c_int, recursive: ?[*]const c_int) ?*compiled_rule {
    g_rule_off = r.off;

    var vt: v_tab = std.mem.zeroes(v_tab);
    var ib: i_buf = std.mem.zeroes(i_buf);
    defer v_free(&vt);
    defer i_free(&ib);

    var pat_dfa: ?[*]?*regexwalk.regex_dfa = null;
    var n_pat: c_int = 0;
    var pat_cap: c_int = 0;
    defer {
        if (pat_dfa) |arr| {
            var pi: c_int = 0;
            while (pi < n_pat) : (pi += 1) regexwalk.regex_dfa_free(arr[@intCast(pi)]);
            c.free(@ptrCast(arr));
        }
    }
    var pat_idx: ?[*]u8 = null;
    var pat_col: ?[*]u8 = null;
    defer if (pat_idx) |p| c.free(@ptrCast(p));
    defer if (pat_col) |p| c.free(@ptrCast(p));
    var bri: ?[*]c_int = null;
    defer if (bri) |p| c.free(@ptrCast(p));

    defer {
        if (vt.err != 0) {
            cerr(r.off, "compile error: rule '{s}' exceeds the maximum of {d} distinct variables / temps / constants in a single rule\n", .{ cs(r.head.?.pred), MAX_VARS });
        }
    }

    var cc: c_int = 0;
    var tc: c_int = 0;
    var agg_body_idx: c_int = -1;
    var i: c_int = 0;
    var j: c_int = 0;
    var bi: c_int = 0;

    // ── M5: compile patterns from body atoms ──
    pat_idx = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(r.nbody)) * @sizeOf(u8)) orelse return null));
    pat_col = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(r.nbody)) * @sizeOf(u8)) orelse return null));
    i = 0;
    while (i < r.nbody) : (i += 1) {
        pat_idx.?[@intCast(i)] = 0xFF;
        pat_col.?[@intCast(i)] = 0;
    }
    bi = 0;
    while (bi < r.nbody) : (bi += 1) {
        const ba = r.body.?[@intCast(bi)] orelse return null;
        if (ba.pattern == null) continue;
        if (ba.negated != 0) {
            cerr(r.off, "compile error: negated pattern atom not supported (rule '{s}')\n", .{cs(r.head.?.pred)});
            return null;
        }
        if (ba.pattern_col < 0 or ba.pattern_col >= ba.nargs) {
            cerr(r.off, "compile error: pattern column out of range for atom", .{});
            return null;
        }
        pat_col.?[@intCast(bi)] = @intCast(ba.pattern_col);
        const dfa = regexwalk.regex_compile(cs(ba.pattern));
        if (dfa.?.errmsg != null) {
            cerr(r.off, "compile error: bad regex pattern '{s}': {s} (rule '{s}')\n", .{ cs(ba.pattern), cs(dfa.?.errmsg), cs(r.head.?.pred) });
            regexwalk.regex_dfa_free(dfa);
            return null;
        }
        if (n_pat >= pat_cap) {
            const nc: c_int = if (pat_cap != 0) pat_cap * 2 else 4;
            const np = c.realloc(@ptrCast(if (pat_dfa) |pd| pd else null), @as(usize, @intCast(nc)) * @sizeOf(?*regexwalk.regex_dfa));
            if (np == null) {
                regexwalk.regex_dfa_free(dfa);
                return null;
            }
            pat_dfa = @ptrCast(@alignCast(np));
            pat_cap = nc;
        }
        pat_idx.?[@intCast(bi)] = @intCast(n_pat);
        pat_dfa.?[@intCast(n_pat)] = dfa;
        n_pat += 1;
    }

    // ── 1. detect M3 aggregate body atom ──
    bi = 0;
    while (bi < r.nbody) : (bi += 1) {
        if (r.body.?[@intCast(bi)].?.aggregate != 0) {
            if (agg_body_idx >= 0) {
                cerr(r.off, "compile error: multiple aggregates not supported (rule '{s}')\n", .{cs(r.head.?.pred)});
                return null;
            }
            agg_body_idx = bi;
        }
    }
    const agg: ?*parser.atom = if (agg_body_idx >= 0) r.body.?[@intCast(agg_body_idx)] else null;
    if (agg != null and agg.?.negated != 0) {
        cerr(r.off, "compile error: aggregate inside negation not supported (rule '{s}')\n", .{cs(r.head.?.pred)});
        return null;
    }
    var agg_op_code: c_int = -1;
    var agg_src_var: ?[*c]const u8 = null;
    if (agg) |aggp| {
        const opname: [*c]const u8 = if (aggp.agg_op) |op| cs(op.text) else "";
        if (strEq(opname, "count")) {
            agg_op_code = 0;
        } else if (strEq(opname, "sum")) {
            agg_op_code = 1;
        } else if (strEq(opname, "min")) {
            agg_op_code = 2;
        } else if (strEq(opname, "max")) {
            agg_op_code = 3;
        } else {
            cerr(r.off, "compile error: unknown aggregate '{s}' (rule '{s}')\n", .{ opname, cs(r.head.?.pred) });
            return null;
        }
        if (agg_op_code == 0) {
            if (aggp.nargs != 0) {
                cerr(r.off, "compile error: 'count' takes no arguments (rule '{s}')\n", .{cs(r.head.?.pred)});
                return null;
            }
        } else {
            if (aggp.nargs != 1 or aggp.args.?[0].?.kind != parser.TOK_VAR) {
                cerr(r.off, "compile error: 'sum/min/max' require a source variable (rule '{s}')\n", .{cs(r.head.?.pred)});
                return null;
            }
            agg_src_var = cs(aggp.args.?[0].?.text);
        }
    }

    // ── 1b. reject list PATTERNS in the head/fact ──
    i = 0;
    while (i < r.head.?.nargs) : (i += 1) {
        if (list_is_pattern(r.head.?.args.?[@intCast(i)]) != 0) {
            cerr(r.off, "compile error: list pattern in head/fact of '{s}' — use cons(...) to construct lists\n", .{cs(r.head.?.pred)});
            return null;
        }
    }

    // ── 1c. reserved BUILTIN name cannot be a rule HEAD ──
    if (is_reserved_builtin_name(cs(r.head.?.pred))) {
        cerr(r.off, "compile error: '{s}' is a reserved builtin predicate name and cannot be used as a rule head (rule '{s}')\n", .{ cs(r.head.?.pred), cs(r.head.?.pred) });
        return null;
    }

    // ── 2. resolve head ──
    var head_ri = db_find_rel(db, cs(r.head.?.pred));
    if (head_ri < 0) {
        if (r.head.?.nargs < 1 or r.head.?.nargs > MAX_ARITY) {
            cerr(r.off, "compile error: head arity {d} for '{s}'\n", .{ r.head.?.nargs, cs(r.head.?.pred) });
            return null;
        }
        if (dx.dl_declare_relation(db, cs(r.head.?.pred), @intCast(r.head.?.nargs)) != 0) {
            cerr(r.off, "compile error: cannot declare '{s}/{d}'\n", .{ cs(r.head.?.pred), r.head.?.nargs });
            return null;
        }
        head_ri = db_find_rel(db, cs(r.head.?.pred));
        if (head_ri < 0) return null;
    } else if (db_rel_is_variadic(db, head_ri) != 0) {
        if (r.head.?.nargs < 1 or r.head.?.nargs > MAX_ARITY) {
            cerr(r.off, "compile error: head arity {d} for variadic '{s}'\n", .{ r.head.?.nargs, cs(r.head.?.pred) });
            return null;
        }
        if (dx.dl_ensure_variant(db, head_ri, @intCast(r.head.?.nargs)) == null) {
            cerr(r.off, "compile error: cannot materialize variant {d} of variadic '{s}'\n", .{ r.head.?.nargs, cs(r.head.?.pred) });
            return null;
        }
    } else {
        if (db_rel_arity(db, head_ri) != @as(u8, @intCast(r.head.?.nargs))) {
            cerr(r.off, "compile error: arity mismatch for '{s}': {d} vs {d}\n", .{ cs(r.head.?.pred), db_rel_arity(db, head_ri), r.head.?.nargs });
            return null;
        }
    }

    // ── 3. resolve body ──
    if (r.nbody == 0) {
        cerr(r.off, "compile error: rule '{s}' has no body\n", .{cs(r.head.?.pred)});
        return null;
    }
    if (agg != null) {
        i = 0;
        while (i < r.head.?.nargs) : (i += 1) {
            if (r.head.?.args.?[@intCast(i)].?.kind != parser.TOK_VAR) {
                cerr(r.off, "compile error: constants in head of aggregate rule not supported (rule '{s}')\n", .{cs(r.head.?.pred)});
                return null;
            }
        }
    }
    bri = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(r.nbody)) * @sizeOf(c_int)) orelse return null));
    bi = 0;
    while (bi < r.nbody) : (bi += 1) {
        const ba = r.body.?[@intCast(bi)] orelse return null;
        if (ba.aggregate != 0) {
            bri.?[@intCast(bi)] = -1;
            continue;
        }
        if (is_builtin_pred(ba)) {
            if (ba.negated != 0) {
                cerr(r.off, "compile error: negated builtin not supported (rule '{s}')\n", .{cs(r.head.?.pred)});
                return null;
            }
            if (is_str_builtin(ba) and str_builtin_valid(ba) == 0) {
                cerr(r.off, "compile error: malformed string builtin '{s}' (bad arity or non-symbol operand) (rule '{s}')\n", .{ cs(ba.pred), cs(r.head.?.pred) });
                return null;
            }
            if (is_list_builtin(ba) and list_builtin_valid(ba) == 0) {
                cerr(r.off, "compile error: malformed list builtin '{s}' (bad arity or non-variable result / non-constant operand) (rule '{s}')\n", .{ cs(ba.pred), cs(r.head.?.pred) });
                return null;
            }
            if (is_range_builtin(ba) and range_builtin_valid(db, ba) == 0) {
                cerr(r.off, "compile error: malformed range builtin '{s}' (expected range(X, Rel, Lo, Hi): X a variable, Rel a known non-variadic arity>=1 relation, Lo/Hi variable-or-int bounds) (rule '{s}')\n", .{ cs(ba.pred), cs(r.head.?.pred) });
                return null;
            }
            if (is_range_builtin(ba)) {
                const rr = db_find_rel(db, cs(ba.args.?[1].?.text));
                if (rr >= 0 and recursive != null and recursive.?[@intCast(rr)] != 0) {
                    cerr(r.off, "compile error: range over recursive relation '{s}' is not supported (OP_RANGE reads the idb directly, which the fixpoint never updates) (rule '{s}')\n", .{ cs(ba.args.?[1].?.text), cs(r.head.?.pred) });
                    return null;
                }
            }
            bri.?[@intCast(bi)] = -1;
            continue;
        }
        if (ba.negated != 0) {
            j = 0;
            while (j < ba.nargs) : (j += 1) {
                if (list_is_pattern(ba.args.?[@intCast(j)]) != 0) {
                    cerr(r.off, "compile error: list pattern in negated atom '{s}' (patterns cannot bind vars under negation) (rule '{s}')\n", .{ cs(ba.pred), cs(r.head.?.pred) });
                    return null;
                }
            }
        }
        const ri = db_find_rel(db, cs(ba.pred));
        if (ri < 0) {
            cerr(r.off, "compile error: unknown predicate '{s}'\n", .{cs(ba.pred)});
            return null;
        }
        if (db_rel_is_variadic(db, ri) != 0) {
            if (ba.nargs < 1 or ba.nargs > MAX_ARITY) {
                cerr(r.off, "compile error: body arity {d} for variadic '{s}'\n", .{ ba.nargs, cs(ba.pred) });
                return null;
            }
        } else if (db_rel_arity(db, ri) != @as(u8, @intCast(ba.nargs))) {
            cerr(r.off, "compile error: arity mismatch for '{s}'\n", .{cs(ba.pred)});
            return null;
        }
        bri.?[@intCast(bi)] = ri;
    }

    // ── 3b. reject aggregate rules that touch a variadic relation ──
    if (agg != null) {
        if (db_rel_is_variadic(db, head_ri) != 0) {
            cerr(r.off, "compile error: aggregate over a variadic relation is not supported (head '{s}', rule '{s}')\n", .{ cs(r.head.?.pred), cs(r.head.?.pred) });
            return null;
        }
        bi = 0;
        while (bi < r.nbody) : (bi += 1) {
            if (bri.?[@intCast(bi)] < 0) continue;
            if (db_rel_is_variadic(db, bri.?[@intCast(bi)]) != 0) {
                cerr(r.off, "compile error: aggregate over a variadic relation is not supported (body '{s}', rule '{s}')\n", .{ cs(r.body.?[@intCast(bi)].?.pred), cs(r.head.?.pred) });
                return null;
            }
        }
    }

    // ── 4. collect vars ──
    i = 0;
    while (i < r.head.?.nargs) : (i += 1) {
        if (r.head.?.args.?[@intCast(i)].?.kind == parser.TOK_VAR and v_add(&vt, cs(r.head.?.args.?[@intCast(i)].?.text)) < 0) return null;
    }
    i = 0;
    while (i < r.nbody) : (i += 1) {
        j = 0;
        while (j < r.body.?[@intCast(i)].?.nargs) : (j += 1) {
            if (r.body.?[@intCast(i)].?.args.?[@intCast(j)].?.kind == parser.TOK_VAR and v_add(&vt, cs(r.body.?[@intCast(i)].?.args.?[@intCast(j)].?.text)) < 0) return null;
        }
    }
    i = 0;
    while (i < r.nbody) : (i += 1) {
        j = 0;
        while (j < r.body.?[@intCast(i)].?.nargs) : (j += 1) {
            if (collect_token_vars(r.body.?[@intCast(i)].?.args.?[@intCast(j)], &vt) < 0) return null;
        }
    }
    i = 0;
    while (i < r.nbody) : (i += 1) {
        if (is_arith(r.body.?[@intCast(i)]))
            collect_expr_vars(r.body.?[@intCast(i)].?.arith, &vt);
    }
    if (agg) |aggp| {
        if (v_add(&vt, cs(aggp.pred)) < 0) return null;
        if (agg_src_var) |sv| {
            if (v_add(&vt, sv) < 0) return null;
        }
    }
    if (vt.err != 0) return null;

    // ── 5. negation safety check ──
    {
        var bound_vars = [_]c_int{0} ** 256;
        bi = 0;
        while (bi < r.nbody) : (bi += 1) {
            const ba = r.body.?[@intCast(bi)] orelse return null;
            if (ba.aggregate != 0) continue;
            if (is_equality(ba)) {
                continue; // equality runs after the relational phase (and NEG_CHECK)
            }
            if (is_arith(ba) or is_comparison(ba) or is_str_builtin(ba) or is_list_builtin(ba) or is_range_builtin(ba)) {
                continue; // builtins execute after the relational phase
            }
            if (ba.negated != 0) {
                j = 0;
                while (j < ba.nargs) : (j += 1) {
                    if (ba.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                        const vi = v_find(&vt, cs(ba.args.?[@intCast(j)].?.text));
                        if (vi < 0 or bound_vars[@intCast(vi)] == 0) {
                            cerr(r.off, "compile error: unsafe negation — variable '{s}' in negated atom '{s}' is not bound by a positive body atom (rule '{s}')\n", .{ cs(ba.args.?[@intCast(j)].?.text), cs(ba.pred), cs(r.head.?.pred) });
                            return null;
                        }
                    }
                }
            } else {
                j = 0;
                while (j < ba.nargs) : (j += 1) mark_token_vars_bound(ba.args.?[@intCast(j)], &vt, &bound_vars);
            }
        }
    }

    // ── 5b. M9 builtin-safety pass ──
    {
        var bound_vars = [_]c_int{0} ** 256;
        bi = 0;
        while (bi < r.nbody) : (bi += 1) {
            const ba = r.body.?[@intCast(bi)] orelse return null;
            if (ba.negated != 0 or ba.aggregate != 0 or is_builtin_pred(ba)) continue;
            j = 0;
            while (j < ba.nargs) : (j += 1) mark_token_vars_bound(ba.args.?[@intCast(j)], &vt, &bound_vars);
        }
        bi = 0;
        while (bi < r.nbody) : (bi += 1) {
            const ba = r.body.?[@intCast(bi)] orelse return null;
            if (!is_equality(ba)) continue;
            if (is_list_assign(ba)) {
                const vi1: c_int = if (ba.nargs >= 2 and ba.args.?[1].?.kind == parser.TOK_VAR) v_find(&vt, cs(ba.args.?[1].?.text)) else -1;
                const b1 = (vi1 >= 0) and bound_vars[@intCast(vi1)] != 0;
                if (ba.args.?[1].?.kind == parser.TOK_VAR) {
                    if (!b1) {
                        cerr(r.off, "compile error: ungrounded list assignment — list variable '{s}' in '[X|Xs] = {s}' is not bound by a positive body atom (rule '{s}')\n", .{ cs(ba.args.?[1].?.text), cs(ba.args.?[1].?.text), cs(r.head.?.pred) });
                        return null;
                    }
                    bound_vars[@intCast(vi1)] = 1;
                }
                mark_token_vars_bound(ba.args.?[0], &vt, &bound_vars);
                continue;
            }
            const vi0: c_int = if (ba.nargs >= 1 and ba.args.?[0].?.kind == parser.TOK_VAR) v_find(&vt, cs(ba.args.?[0].?.text)) else -1;
            const vi1: c_int = if (ba.nargs >= 2 and ba.args.?[1].?.kind == parser.TOK_VAR) v_find(&vt, cs(ba.args.?[1].?.text)) else -1;
            const b0 = (vi0 >= 0) and bound_vars[@intCast(vi0)] != 0;
            const b1 = (vi1 >= 0) and bound_vars[@intCast(vi1)] != 0;
            if (b0 or b1) {
                if (vi0 >= 0) bound_vars[@intCast(vi0)] = 1;
                if (vi1 >= 0) bound_vars[@intCast(vi1)] = 1;
            }
        }
        bi = 0;
        while (bi < r.nbody) : (bi += 1) {
            const ba = r.body.?[@intCast(bi)] orelse return null;
            if (is_arith(ba)) {
                if (expr_vars_bound(ba.arith, &vt, &bound_vars, cs(r.head.?.pred)) == 0) return null;
                if (ba.nargs >= 1 and ba.args.?[0].?.kind == parser.TOK_VAR) {
                    const vi = v_find(&vt, cs(ba.args.?[0].?.text));
                    if (vi >= 0) bound_vars[@intCast(vi)] = 1;
                }
            } else if (is_str_producing(ba)) {
                if (str_producing_operands_bound(ba, &vt, &bound_vars, cs(r.head.?.pred)) == 0) return null;
                const vi = v_find(&vt, cs(ba.args.?[0].?.text));
                if (vi >= 0) bound_vars[@intCast(vi)] = 1;
            } else if (is_list_producing(ba)) {
                if (list_producing_operands_bound(ba, &vt, &bound_vars, cs(r.head.?.pred)) == 0) return null;
                const vi = v_find(&vt, cs(ba.args.?[0].?.text));
                if (vi >= 0) bound_vars[@intCast(vi)] = 1;
            } else if (is_list_filter(ba)) {
                if (member_operand_bound(ba, &vt, &bound_vars, cs(r.head.?.pred)) == 0) return null;
                const vi = v_find(&vt, cs(ba.args.?[0].?.text));
                if (vi >= 0) bound_vars[@intCast(vi)] = 1;
            } else if (is_range_builtin(ba)) {
                if (range_operands_bound(ba, &vt, &bound_vars, cs(r.head.?.pred)) == 0) return null;
                const vi = v_find(&vt, cs(ba.args.?[0].?.text));
                if (vi >= 0) bound_vars[@intCast(vi)] = 1;
            }
        }
        bi = 0;
        while (bi < r.nbody) : (bi += 1) {
            const ba = r.body.?[@intCast(bi)] orelse return null;
            if (is_comparison(ba)) {
                j = 0;
                while (j < ba.nargs) : (j += 1) {
                    const t = ba.args.?[@intCast(j)] orelse return null;
                    if (t.kind != parser.TOK_VAR) continue;
                    const vi = v_find(&vt, cs(t.text));
                    if (vi < 0 or bound_vars[@intCast(vi)] == 0) {
                        cerr(r.off, "compile error: ungrounded comparison — variable '{s}' in comparison '{s}' is not bound by a positive body atom (rule '{s}')\n", .{ cs(t.text), cs(ba.pred), cs(r.head.?.pred) });
                        return null;
                    }
                }
            } else if (is_str_filter(ba)) {
                if (str_filter_operands_bound(ba, &vt, &bound_vars, cs(r.head.?.pred)) == 0) return null;
            }
        }
    }

    // ── 5c. reject division/modulo by literal 0 ──
    bi = 0;
    while (bi < r.nbody) : (bi += 1) {
        const ba = r.body.?[@intCast(bi)] orelse return null;
        if (is_arith(ba) and expr_has_div0(ba.arith) != 0) {
            cerr(r.off, "compile error: division/modulo by literal 0 (rule '{s}')\n", .{cs(r.head.?.pred)});
            return null;
        }
    }

    // ── 6. grounding ──
    i = 0;
    while (i < r.head.?.nargs) : (i += 1) {
        const a = r.head.?.args.?[@intCast(i)] orelse return null;
        if (a.kind != parser.TOK_VAR) continue;
        if (agg != null and strEq(cs(a.text), cs(agg.?.pred))) continue;
        {
            var ares: c_int = 0;
            bi = 0;
            while (bi < r.nbody) : (bi += 1) {
                const ba = r.body.?[@intCast(bi)] orelse return null;
                if (ba.nargs >= 1 and ba.args.?[0].?.kind == parser.TOK_VAR and
                    strEq(cs(ba.args.?[0].?.text), cs(a.text)) and
                    (is_arith(ba) or is_str_producing(ba) or is_list_producing(ba))) {
                    ares = 1;
                    break;
                }
            }
            if (ares != 0) continue;
        }
        var ok: c_int = 0;
        bi = 0;
        while (bi < r.nbody and ok == 0) : (bi += 1) {
            j = 0;
            while (j < r.body.?[@intCast(bi)].?.nargs) : (j += 1) {
                if (token_contains_var(r.body.?[@intCast(bi)].?.args.?[@intCast(j)], cs(a.text)) != 0) {
                    ok = 1;
                    break;
                }
            }
        }
        if (ok == 0) {
            cerr(r.off, "compile error: ungrounded variable '{s}' in head of '{s}'\n", .{ cs(a.text), cs(r.head.?.pred) });
            return null;
        }
    }

    // ── 8. emit bytecode ──

    if (r.has_negation == 0) {
        const pos: [*]c_int = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(if (r.nbody > 0) r.nbody else 1)) * @sizeOf(c_int)) orelse return null));
        defer c.free(@ptrCast(pos));
        const mask: [*]u64 = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(if (r.nbody > 0) r.nbody else 1)), @sizeOf(u64)) orelse return null));
        defer c.free(@ptrCast(mask));
        var n_pos: c_int = 0;
        var any_pattern: c_int = 0;

        bi = 0;
        while (bi < r.nbody) : (bi += 1) {
            const ba = r.body.?[@intCast(bi)] orelse return null;
            if (ba.negated != 0 or ba.aggregate != 0) continue;
            if (is_builtin_pred(ba)) continue;
            pos[@intCast(n_pos)] = bi;
            n_pos += 1;
            mask[@intCast(bi)] = atom_var_mask(r, bi, &vt);
            j = 0;
            while (j < ba.nargs) : (j += 1) {
                if (list_is_pattern(ba.args.?[@intCast(j)]) != 0) {
                    any_pattern = 1;
                    break;
                }
            }
            if (pat_idx.?[@intCast(bi)] != 0xFF) any_pattern = 1;
        }
        if (n_pos == 0) {
            var list_driver: c_int = 0;
            bi = 0;
            while (bi < r.nbody) : (bi += 1) {
                const ba = r.body.?[@intCast(bi)] orelse return null;
                if (is_list_filter(ba) or is_range_builtin(ba)) {
                    list_driver = 1;
                    break;
                }
                if (is_list_assign(ba) and ba.args.?[1].?.kind != parser.TOK_VAR) {
                    list_driver = 1;
                    break;
                }
            }
            if (list_driver == 0) {
                cerr(r.off, "compile error: rule '{s}' has no positive body atom\n", .{cs(r.head.?.pred)});
                return null;
            }
        } else {
            var ec: emit_ctx = undefined;
            @memset(std.mem.asBytes(&ec), 0);
            if (gReorderRef().* != 0) reorder_pos_atoms(db, bri.?, mask, pos, n_pos);
            ec.db = db;
            ec.r = r;
            ec.bri = bri.?;
            ec.pat_idx = pat_idx.?;
            ec.pat_col = pat_col.?;
            ec.vt = &vt;
            ec.ib = &ib;
            ec.cc = &cc;
            ec.mask = mask;
            ec.do_bushy = if (gBushyRef().* != 0 and agg == null and any_pattern == 0) 1 else 0;
            ec.recursive = recursive;
            ec.next_buf = 0;
            if (emit_join(&ec, pos, n_pos, null, 0) != 0) return null;
        }
    } else {
        var first_pos: c_int = -1;
        bi = 0;
        while (bi < r.nbody) : (bi += 1) {
            const ba = r.body.?[@intCast(bi)] orelse return null;
            if (ba.negated != 0 or ba.aggregate != 0) continue;
            if (is_builtin_pred(ba)) continue;
            first_pos = bi;
            break;
        }
        if (first_pos < 0) {
            var list_driver: c_int = 0;
            bi = 0;
            while (bi < r.nbody) : (bi += 1) {
                const ba = r.body.?[@intCast(bi)] orelse return null;
                if (is_list_filter(ba) or is_range_builtin(ba)) {
                    list_driver = 1;
                    break;
                }
                if (is_list_assign(ba) and ba.args.?[1].?.kind != parser.TOK_VAR) {
                    list_driver = 1;
                    break;
                }
            }
            if (list_driver == 0) {
                cerr(r.off, "compile error: rule '{s}' has no positive body atom\n", .{cs(r.head.?.pred)});
                return null;
            }
        }

        bi = 0;
        while (bi < first_pos) : (bi += 1) {
            const na = r.body.?[@intCast(bi)] orelse return null;
            var cslot: [8]u8 = undefined;
            j = 0;
            while (j < na.nargs) : (j += 1) {
                if (na.args.?[@intCast(j)].?.kind != parser.TOK_VAR) {
                    var cname: [16]u8 = undefined;
                    const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &cc, 'k'));
                    if (vi < 0) return null;
                    cslot[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                    var cv: u32 = 0;
                    if (token_const(db, na.args.?[@intCast(j)], &cv) != 0) return null;
                    const eq = i_emit(&ib) orelse return null;
                    eq.op = OP_EQ_CONST;
                    eq.a = cslot[@intCast(j)];
                    eq.imm = cv;
                }
            }
            const neg = i_emit(&ib) orelse return null;
            neg.op = OP_NEG_CHECK;
            neg.a = @intCast(bri.?[@intCast(bi)]);
            neg.b = @intCast(na.nargs);
            neg.body_idx = @intCast(bi);
            j = 0;
            while (j < na.nargs) : (j += 1) {
                if (na.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                    const vi = v_find(&vt, cs(na.args.?[@intCast(j)].?.text));
                    if (vi < 0) return null;
                    neg.slots[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                } else {
                    neg.slots[@intCast(j)] = cslot[@intCast(j)];
                }
            }
        }

        if (first_pos >= 0) {
            const a0 = r.body.?[@intCast(first_pos)] orelse return null;
            const ip = i_emit(&ib) orelse return null;
            if (pat_idx.?[@intCast(first_pos)] != 0xFF) {
                ip.op = OP_WALK;
                ip.imm = pat_idx.?[@intCast(first_pos)];
                ip.c = pat_col.?[@intCast(first_pos)];
            } else {
                ip.op = OP_SCAN;
            }
            ip.a = @intCast(bri.?[@intCast(first_pos)]);
            ip.b = @intCast(a0.nargs);
            ip.body_idx = @intCast(first_pos);
            var ts: [8]u8 = undefined;
            j = 0;
            while (j < a0.nargs) : (j += 1) {
                if (a0.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                    const vi = v_find(&vt, cs(a0.args.?[@intCast(j)].?.text));
                    ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                } else {
                    var cname: [16]u8 = undefined;
                    const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &cc, 'k'));
                    ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                }
            }
            j = 0;
            while (j < a0.nargs) : (j += 1) ip.slots[@intCast(j)] = ts[@intCast(j)];
            if (emit_const_or_pattern(db, a0, &ts, &vt, &ib, &cc, cs(r.head.?.pred)) != 0) return null;
        }

        bi = first_pos + 1;
        while (bi < r.nbody) : (bi += 1) {
            const curr = r.body.?[@intCast(bi)] orelse return null;
            if (curr.aggregate != 0) continue;
            if (is_builtin_pred(curr)) continue;

            if (curr.negated != 0) {
                var cslot: [8]u8 = undefined;
                j = 0;
                while (j < curr.nargs) : (j += 1) {
                    if (curr.args.?[@intCast(j)].?.kind != parser.TOK_VAR) {
                        var cname: [16]u8 = undefined;
                        const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &cc, 'k'));
                        if (vi < 0) return null;
                        cslot[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                        var cv: u32 = 0;
                        if (token_const(db, curr.args.?[@intCast(j)], &cv) != 0) return null;
                        const eq = i_emit(&ib) orelse return null;
                        eq.op = OP_EQ_CONST;
                        eq.a = cslot[@intCast(j)];
                        eq.imm = cv;
                    }
                }
                const neg = i_emit(&ib) orelse return null;
                neg.op = OP_NEG_CHECK;
                neg.a = @intCast(bri.?[@intCast(bi)]);
                neg.b = @intCast(curr.nargs);
                neg.body_idx = @intCast(bi);
                j = 0;
                while (j < curr.nargs) : (j += 1) {
                    if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                        const vi = v_find(&vt, cs(curr.args.?[@intCast(j)].?.text));
                        if (vi < 0) return null;
                        neg.slots[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                    } else {
                        neg.slots[@intCast(j)] = cslot[@intCast(j)];
                    }
                }
                continue;
            }

            var sc: [8]c_int = [_]c_int{0} ** 8;
            var k: c_int = 0;
            j = 0;
            while (j < curr.nargs) : (j += 1) {
                if (curr.args.?[@intCast(j)].?.kind != parser.TOK_VAR) continue;
                var bp: c_int = 0;
                while (bp < bi) : (bp += 1) {
                    const p = r.body.?[@intCast(bp)] orelse return null;
                    if (p.negated != 0 or p.aggregate != 0) continue;
                    if (is_builtin_pred(p)) continue;
                    var q: c_int = 0;
                    while (q < p.nargs) : (q += 1) {
                        if (p.args.?[@intCast(q)].?.kind == parser.TOK_VAR and
                            strEq(cs(p.args.?[@intCast(q)].?.text), cs(curr.args.?[@intCast(j)].?.text))) {
                            sc[@intCast(j)] = 1;
                            break;
                        }
                    }
                    if (sc[@intCast(j)] != 0) break;
                }
            }
            while (k < curr.nargs and sc[@intCast(k)] != 0) k += 1;

            const ip = i_emit(&ib) orelse return null;
            if (pat_idx.?[@intCast(bi)] != 0xFF) {
                ip.op = OP_WALK;
                ip.imm = pat_idx.?[@intCast(bi)];
                ip.c = pat_col.?[@intCast(bi)];
                ip.a = @intCast(bri.?[@intCast(bi)]);
                ip.b = @intCast(curr.nargs);
                ip.body_idx = @intCast(bi);
                var ts: [8]u8 = undefined;
                j = 0;
                while (j < curr.nargs) : (j += 1) {
                    if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                        const vi = v_find(&vt, cs(curr.args.?[@intCast(j)].?.text));
                        ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                    } else {
                        var cname: [16]u8 = undefined;
                        const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &cc, 'k'));
                        ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                    }
                }
                j = 0;
                while (j < curr.nargs) : (j += 1) ip.slots[@intCast(j)] = ts[@intCast(j)];
                j = 0;
                while (j < curr.nargs) : (j += 1) {
                    if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) continue;
                    var cv: u32 = 0;
                    if (token_const(db, curr.args.?[@intCast(j)], &cv) != 0) return null;
                    const eq = i_emit(&ib) orelse return null;
                    eq.op = OP_EQ_CONST;
                    eq.a = ts[@intCast(j)];
                    eq.imm = cv;
                }
            } else if (k > 0) {
                ip.op = OP_LOOKUP;
                ip.a = @intCast(bri.?[@intCast(bi)]);
                ip.b = @intCast(k);
                ip.c = @intCast(curr.nargs);
                ip.body_idx = @intCast(bi);
                var slot_map: [8]u8 = undefined;
                var si: c_int = 0;
                j = 0;
                while (j < k) : (j += 1) {
                    const vi = v_find(&vt, cs(curr.args.?[@intCast(j)].?.text));
                    ip.slots[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                    slot_map[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                }
                si = k;
                j = k;
                while (j < curr.nargs) : (j += 1) {
                    if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                        const vi = v_find(&vt, cs(curr.args.?[@intCast(j)].?.text));
                        ip.slots[@intCast(si)] = vt.e.?[@intCast(vi)].slot;
                        slot_map[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                    } else {
                        var cname: [16]u8 = undefined;
                        const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &cc, 'k'));
                        ip.slots[@intCast(si)] = vt.e.?[@intCast(vi)].slot;
                        slot_map[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                    }
                    si += 1;
                }
                if (emit_const_or_pattern(db, curr, &slot_map, &vt, &ib, &cc, cs(r.head.?.pred)) != 0) return null;
            } else {
                var any_shared: c_int = 0;
                j = 0;
                while (j < curr.nargs) : (j += 1) {
                    if (sc[@intCast(j)] != 0) {
                        any_shared = 1;
                        break;
                    }
                }
                if (any_shared != 0) {
                    if (emit_nonleading_join(db, r, curr, bri.?[@intCast(bi)], &sc, recursive, &vt, &ib, &cc, ip, bi) != 0) return null;
                } else {
                    ip.op = OP_SCAN;
                    ip.a = @intCast(bri.?[@intCast(bi)]);
                    ip.b = @intCast(curr.nargs);
                    ip.body_idx = @intCast(bi);
                    var ts: [8]u8 = undefined;
                    j = 0;
                    while (j < curr.nargs) : (j += 1) {
                        if (curr.args.?[@intCast(j)].?.kind == parser.TOK_VAR) {
                            const vi = v_find(&vt, cs(curr.args.?[@intCast(j)].?.text));
                            ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                        } else {
                            var cname: [16]u8 = undefined;
                            const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &cc, 'k'));
                            ts[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                        }
                    }
                    j = 0;
                    while (j < curr.nargs) : (j += 1) ip.slots[@intCast(j)] = ts[@intCast(j)];
                    if (emit_const_or_pattern(db, curr, &ts, &vt, &ib, &cc, cs(r.head.?.pred)) != 0) return null;
                }
            }
        }
    }

    // ── Equality body atoms → OP_EQ ──
    bi = 0;
    while (bi < r.nbody) : (bi += 1) {
        const ba = r.body.?[@intCast(bi)] orelse return null;
        if (ba.aggregate != 0) continue;
        if (!is_equality(ba)) continue;
        if (ba.negated != 0) continue;
        if (is_list_assign(ba)) {
            const rs = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 1);
            if (rs < 0) return null;
            if (emit_pattern(db, ba.args.?[0].?, @intCast(rs), &vt, &ib, &cc, cs(r.head.?.pred)) != 0) return null;
            continue;
        }
        const vi_l = v_find(&vt, cs(ba.args.?[0].?.text));
        const vi_r = v_find(&vt, cs(ba.args.?[1].?.text));
        if (vi_l < 0 or vi_r < 0) return null;
        const eq = i_emit(&ib) orelse return null;
        eq.op = OP_EQ;
        eq.a = vt.e.?[@intCast(vi_l)].slot;
        eq.b = vt.e.?[@intCast(vi_r)].slot;
        eq.body_idx = @intCast(bi);
    }

    // ── PRODUCERS in body order → arithmetic + string/list/range ──
    bi = 0;
    while (bi < r.nbody) : (bi += 1) {
        const ba = r.body.?[@intCast(bi)] orelse return null;
        if (is_arith(ba)) {
            if (ba.negated != 0) continue;
            const rs = lower_expr(db, ba.arith, &vt, &ib, &cc, &tc, bi);
            if (rs < 0) return null;
            const rvi = v_find(&vt, cs(ba.args.?[0].?.text));
            if (rvi < 0) return null;
            const eq = i_emit(&ib) orelse return null;
            eq.op = OP_EQ;
            eq.a = vt.e.?[@intCast(rvi)].slot;
            eq.b = @intCast(rs);
            eq.body_idx = @intCast(bi);
        } else if (is_str_producing(ba)) {
            if (ba.negated != 0) continue;
            var cname: [16]u8 = undefined;
            const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &tc, 't'));
            if (vi < 0) return null;
            var ls: c_int = 0;
            var rs: c_int = 0;
            if (str_bind_imm(cs(ba.pred)) >= 0) {
                ls = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 0);
                if (ls < 0) return null;
                if (strEq(cs(ba.pred), "concat")) {
                    rs = cmp_operand_slot(db, ba.args.?[2], &vt, &ib, &cc, 0);
                    if (rs < 0) return null;
                } else {
                    rs = ls;
                }
                const op = i_emit(&ib) orelse return null;
                op.op = OP_STR_BIND;
                op.a = @intCast(ls);
                op.b = @intCast(rs);
                op.c = vt.e.?[@intCast(vi)].slot;
                op.imm = @intCast(str_bind_imm(cs(ba.pred)));
                op.body_idx = @intCast(bi);
            } else {
                ls = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 1);
                if (ls < 0) return null;
                const op = i_emit(&ib) orelse return null;
                op.op = OP_STR_LEN;
                op.a = @intCast(ls);
                op.c = vt.e.?[@intCast(vi)].slot;
                op.imm = 0;
                op.body_idx = @intCast(bi);
            }
            const rvi = v_find(&vt, cs(ba.args.?[0].?.text));
            if (rvi < 0) return null;
            const eq = i_emit(&ib) orelse return null;
            eq.op = OP_EQ;
            eq.a = vt.e.?[@intCast(rvi)].slot;
            eq.b = vt.e.?[@intCast(vi)].slot;
            eq.body_idx = @intCast(bi);
        } else if (is_list_producing(ba)) {
            if (ba.negated != 0) continue;
            var cname: [16]u8 = undefined;
            const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &tc, 't'));
            if (vi < 0) return null;
            var ls: c_int = -1;
            var rs: c_int = -1;
            var opcode: u8 = 0;
            if (strEq(cs(ba.pred), "cons")) {
                ls = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 1);
                rs = cmp_operand_slot(db, ba.args.?[2], &vt, &ib, &cc, 1);
                if (ls < 0 or rs < 0) return null;
                opcode = OP_LIST_CONS;
            } else if (strEq(cs(ba.pred), "car")) {
                ls = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 1);
                if (ls < 0) return null;
                opcode = OP_LIST_CAR;
            } else if (strEq(cs(ba.pred), "cdr")) {
                ls = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 1);
                if (ls < 0) return null;
                opcode = OP_LIST_CDR;
            } else {
                ls = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 1);
                rs = cmp_operand_slot(db, ba.args.?[2], &vt, &ib, &cc, 1);
                if (ls < 0 or rs < 0) return null;
                opcode = OP_LIST_APPEND;
            }
            const op = i_emit(&ib) orelse return null;
            op.op = opcode;
            op.body_idx = @intCast(bi);
            if (opcode == OP_LIST_CONS) {
                op.a = vt.e.?[@intCast(vi)].slot;
                op.b = @intCast(ls);
                op.c = @intCast(rs);
            } else if (opcode == OP_LIST_CAR or opcode == OP_LIST_CDR) {
                op.a = @intCast(ls);
                op.c = vt.e.?[@intCast(vi)].slot;
            } else {
                op.a = @intCast(ls);
                op.b = @intCast(rs);
                op.c = vt.e.?[@intCast(vi)].slot;
            }
            const rvi = v_find(&vt, cs(ba.args.?[0].?.text));
            if (rvi < 0) return null;
            const eq = i_emit(&ib) orelse return null;
            eq.op = OP_EQ;
            eq.a = vt.e.?[@intCast(rvi)].slot;
            eq.b = vt.e.?[@intCast(vi)].slot;
            eq.body_idx = @intCast(bi);
        } else if (is_list_filter(ba)) {
            if (ba.negated != 0) continue;
            const ls = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 1);
            if (ls < 0) return null;
            const xvi = v_find(&vt, cs(ba.args.?[0].?.text));
            if (xvi < 0) return null;
            const m = i_emit(&ib) orelse return null;
            m.op = OP_LIST_MEMBER;
            m.a = @intCast(ls);
            m.b = vt.e.?[@intCast(xvi)].slot;
            m.slots[0] = vt.e.?[@intCast(xvi)].slot;
            m.body_idx = @intCast(bi);
        } else if (is_range_builtin(ba)) {
            if (ba.negated != 0) continue;
            const los = cmp_operand_slot(db, ba.args.?[2], &vt, &ib, &cc, 0);
            const his = cmp_operand_slot(db, ba.args.?[3], &vt, &ib, &cc, 0);
            if (los < 0 or his < 0) return null;
            const xvi = v_find(&vt, cs(ba.args.?[0].?.text));
            const rri = db_find_rel(db, cs(ba.args.?[1].?.text));
            if (xvi < 0 or rri < 0) return null;
            const m = i_emit(&ib) orelse return null;
            m.op = OP_RANGE;
            m.a = @intCast(los);
            m.b = @intCast(his);
            m.c = vt.e.?[@intCast(xvi)].slot;
            m.imm = @intCast(rri);
            m.slots[0] = vt.e.?[@intCast(xvi)].slot;
            m.body_idx = @intCast(bi);
        }
    }

    // ── FILTERS in body order → comparisons + string filters ──
    bi = 0;
    while (bi < r.nbody) : (bi += 1) {
        const ba = r.body.?[@intCast(bi)] orelse return null;
        if (is_comparison(ba)) {
            if (ba.negated != 0) continue;
            const ls = cmp_operand_slot(db, ba.args.?[0], &vt, &ib, &cc, 0);
            const rs = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 0);
            if (ls < 0 or rs < 0) return null;
            const cmp = i_emit(&ib) orelse return null;
            cmp.op = OP_CMP;
            cmp.a = @intCast(ls);
            cmp.b = @intCast(rs);
            cmp.imm = @intCast(cmp_op_code(cs(ba.pred)));
            cmp.body_idx = @intCast(bi);
        } else if (is_str_filter(ba)) {
            if (ba.negated != 0) continue;
            const ls = cmp_operand_slot(db, ba.args.?[0], &vt, &ib, &cc, 0);
            const rs = cmp_operand_slot(db, ba.args.?[1], &vt, &ib, &cc, 0);
            if (ls < 0 or rs < 0) return null;
            const f = i_emit(&ib) orelse return null;
            f.op = OP_STR_FILTER;
            f.a = @intCast(ls);
            f.b = @intCast(rs);
            f.imm = @intCast(str_filter_code(cs(ba.pred)));
            f.body_idx = @intCast(bi);
        }
    }

    // ── Aggregate: AGG_ACC ──
    if (agg) |aggp| {
        var group_vars: [8]c_int = undefined;
        var n_group: c_int = 0;
        i = 0;
        while (i < r.head.?.nargs) : (i += 1) {
            const a = r.head.?.args.?[@intCast(i)] orelse return null;
            if (a.kind != parser.TOK_VAR) continue;
            if (strEq(cs(a.text), cs(aggp.pred))) continue;
            const vi = v_find(&vt, cs(a.text));
            if (vi < 0) return null;
            var k2: c_int = 0;
            var dup: c_int = 0;
            while (k2 < n_group) : (k2 += 1) {
                if (group_vars[@intCast(k2)] == vt.e.?[@intCast(vi)].slot) {
                    dup = 1;
                    break;
                }
            }
            if (dup == 0) {
                group_vars[@intCast(n_group)] = vt.e.?[@intCast(vi)].slot;
                n_group += 1;
            }
        }
        if (n_group > 7) {
            cerr(r.off, "compile error: too many group-by columns (rule '{s}')\n", .{cs(r.head.?.pred)});
            return null;
        }
        const rvi = v_find(&vt, cs(aggp.pred));
        const aip = i_emit(&ib) orelse return null;
        aip.op = OP_AGG_ACC;
        aip.a = @intCast(n_group);
        aip.b = @intCast(agg_op_code);
        aip.c = vt.e.?[@intCast(rvi)].slot;
        j = 0;
        while (j < n_group) : (j += 1) aip.slots[@intCast(j)] = @intCast(group_vars[@intCast(j)]);
        if (agg_src_var) |sv| {
            const svi = v_find(&vt, sv);
            aip.slots[@intCast(n_group)] = vt.e.?[@intCast(svi)].slot;
        } else {
            aip.slots[@intCast(n_group)] = 0xFF;
        }
        aip.body_idx = @intCast(agg_body_idx);
    }

    // ── PROJECT (or AGG_EMIT) ──
    {
        var head_slots: [8]u8 = undefined;
        j = 0;
        while (j < r.head.?.nargs) : (j += 1) {
            const a = r.head.?.args.?[@intCast(j)] orelse return null;
            if (a.kind == parser.TOK_VAR) {
                const vi = v_find(&vt, cs(a.text));
                head_slots[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
            } else {
                var cv: u32 = 0;
                if (token_const(db, a, &cv) != 0) return null;
                var cname: [16]u8 = undefined;
                const vi = v_add(&vt, v_fresh_name(&vt, &cname, cname.len, &cc, 'k'));
                if (vi < 0) return null;
                head_slots[@intCast(j)] = vt.e.?[@intCast(vi)].slot;
                const eq = i_emit(&ib) orelse return null;
                eq.op = OP_EQ_CONST;
                eq.a = head_slots[@intCast(j)];
                eq.imm = cv;
            }
        }
        if (agg) |aggp| {
            const rvi = v_find(&vt, cs(aggp.pred));
            const aip = i_emit(&ib) orelse return null;
            aip.op = OP_AGG_EMIT;
            aip.a = @intCast(head_ri);
            aip.b = @intCast(r.head.?.nargs);
            aip.c = vt.e.?[@intCast(rvi)].slot;
            j = 0;
            while (j < r.head.?.nargs) : (j += 1) aip.slots[@intCast(j)] = head_slots[@intCast(j)];
        } else {
            const ip = i_emit(&ib) orelse return null;
            ip.op = OP_PROJECT;
            ip.a = @intCast(head_ri);
            ip.b = @intCast(r.head.?.nargs);
            j = 0;
            while (j < r.head.?.nargs) : (j += 1) ip.slots[@intCast(j)] = head_slots[@intCast(j)];
        }
    }

    {
        const ip = i_emit(&ib) orelse return null;
        ip.op = OP_HALT;
    }

    if (ib.err != 0) return null;

    // ── 9. build result ──
    const cr: *compiled_rule = @ptrCast(@alignCast(c.calloc(1, @sizeOf(compiled_rule)) orelse return null));
    cr.head_pred = strdup(cs(r.head.?.pred));
    cr.head_rel_id = @intCast(head_ri);
    cr.n_vars = @intCast(vt.n);
    cr.n_instrs = ib.n;
    cr.instrs = ib.b;
    ib.b = null;
    ib.n = 0;
    cr.stratum = 0;
    cr.is_recursive = 0;
    cr.has_aggregate = if (agg_body_idx >= 0) 1 else 0;
    cr.n_patterns = n_pat;
    cr.patterns = pat_dfa;
    pat_dfa = null;
    n_pat = 0;

    if (rel_strata != null and head_ri >= 0) cr.stratum = rel_strata.?[@intCast(head_ri)];

    if (cr.n_vars > 0) {
        cr.vars = @ptrCast(@alignCast(c.malloc(@as(usize, cr.n_vars) * @sizeOf(var_info)) orelse {
            compiled_rule_free(cr);
            return null;
        }));
        i = 0;
        while (i < vt.n) : (i += 1) {
            cr.vars.?[@intCast(i)].name = vt.e.?[@intCast(i)].name;
            vt.e.?[@intCast(i)].name = null;
            cr.vars.?[@intCast(i)].slot = vt.e.?[@intCast(i)].slot;
        }
    }

    return cr;
}

// ─── Public API ────────────────────────────────────────────────────────────

/// int compile_rules(dl_db*, rule**, int, compiled_rule***, int*)
pub export fn compile_rules(db: ?*dx.dl_db, rules: ?[*]?*parser.rule, n_rules: c_int, out_rules: ?*?[*]?*compiled_rule, out_n: ?*c_int) c_int {
    compile_has_err = 0;
    compile_err_off = 0;
    compile_err_msg[0] = 0;

    if (db == null or rules == null or n_rules <= 0 or out_rules == null or out_n == null) return -1;
    const d = db.?;

    var i: c_int = 0;
    while (i < n_rules) : (i += 1) {
        const r = rules.?[@intCast(i)] orelse return -1;
        const ri = db_find_rel(d, cs(r.head.?.pred));
        if (ri < 0) {
            if (r.head.?.nargs < 1 or r.head.?.nargs > MAX_ARITY) {
                cerr(r.off, "compile error: head arity {d} for '{s}'\n", .{ r.head.?.nargs, cs(r.head.?.pred) });
                return -1;
            }
            if (dx.dl_declare_relation(d, cs(r.head.?.pred), @intCast(r.head.?.nargs)) != 0) {
                cerr(r.off, "compile error: cannot declare '{s}/{d}'\n", .{ cs(r.head.?.pred), r.head.?.nargs });
                return -1;
            }
        }
    }

    const nrels = db_rel_count(d);
    const rel_strata: [*]c_int = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse return -1));
    defer c.free(@ptrCast(rel_strata));

    const recursive: [*]c_int = @ptrCast(@alignCast(c.calloc(nrels, @sizeOf(c_int)) orelse return -1));
    defer c.free(@ptrCast(recursive));

    if (compute_strata(d, rules.?, n_rules, rel_strata, recursive, @intCast(nrels)) != 0) return -1;

    const arr: [*]?*compiled_rule = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(n_rules)), @sizeOf(?*compiled_rule)) orelse return -1));

    out_n.?.* = 0;
    i = 0;
    while (i < n_rules) : (i += 1) {
        const r = rules.?[@intCast(i)] orelse return -1;
        arr[@intCast(i)] = compile_one(d, r, rel_strata, recursive);
        if (arr[@intCast(i)] == null) {
            var j2: c_int = 0;
            while (j2 < i) : (j2 += 1) compiled_rule_free(arr[@intCast(j2)]);
            c.free(@ptrCast(arr));
            return -1;
        }
        arr[@intCast(i)].?.is_recursive = if (arr[@intCast(i)].?.head_rel_id < @as(u8, @truncate(nrels)) and recursive[@intCast(arr[@intCast(i)].?.head_rel_id)] != 0) 1 else 0;

        if (arr[@intCast(i)].?.is_recursive != 0 and db_rel_is_variadic(d, arr[@intCast(i)].?.head_rel_id) != 0) {
            cerr(r.off, "compile error: recursive rule over a variadic head is not supported (rule '{s}')\n", .{cs(arr[@intCast(i)].?.head_pred)});
            var j2: c_int = 0;
            while (j2 <= i) : (j2 += 1) compiled_rule_free(arr[@intCast(j2)]);
            c.free(@ptrCast(arr));
            return -1;
        }

        if (arr[@intCast(i)].?.has_aggregate != 0 and arr[@intCast(i)].?.is_recursive != 0) {
            cerr(r.off, "compile error: aggregate in recursive rule not supported (rule '{s}')\n", .{cs(arr[@intCast(i)].?.head_pred)});
            var j2: c_int = 0;
            while (j2 <= i) : (j2 += 1) compiled_rule_free(arr[@intCast(j2)]);
            c.free(@ptrCast(arr));
            return -1;
        }
        out_n.?.* += 1;
    }

    out_rules.?.* = arr;
    return 0;
}

/// void compiled_rule_free(compiled_rule*)
pub export fn compiled_rule_free(cr: ?*compiled_rule) void {
    const r = cr orelse return;
    c.free(@ptrCast(r.head_pred));
    if (r.vars) |vars| {
        var i: c_int = 0;
        while (i < r.n_vars) : (i += 1) c.free(@ptrCast(vars[@intCast(i)].name));
        c.free(@ptrCast(vars));
    }
    c.free(@ptrCast(r.instrs));
    if (r.patterns) |pats| {
        var i: c_int = 0;
        while (i < r.n_patterns) : (i += 1) regexwalk.regex_dfa_free(pats[@intCast(i)]);
        c.free(@ptrCast(pats));
    }
    c.free(@ptrCast(r));
}

// ─── Tests (U8) ────────────────────────────────────────────────────────────
//
// These exercise the compiler END TO END through its C ABI surface:
// compile_rules / compiled_rule_free / compile_last_error / the g_* toggles,
// with the rule ASTs produced by the (ported) parser's parse_create/
// parse_rules — the same entry points dl.c drives — and a REAL dl_db from
// dl_open (facts via dl_add_fact, relations via dl_declare_relation).
// Everything below is test-only; the implementation above is untouched.

const testing = std.testing;

// parser.zig exports (C ABI) — the same symbols dl.c calls for dl_load_rules.
extern "c" fn parse_create(source: ?[*:0]const u8) ?*anyopaque;
extern "c" fn parse_rules(p: ?*anyopaque, n_rules: ?*c_int) ?[*]?*parser.rule;
extern "c" fn parse_free(p: ?*anyopaque) void;
extern "c" fn rule_free(r: ?*parser.rule) void;
// glibc system(3): the C suites (tests/test_m2.c) clear their db dirs the
// same way; avoids dragging std.Io into the ported modules.
extern "c" fn system(cmd: [*c]const u8) c_int;

const parsed_rules = struct {
    p: ?*anyopaque, // parser handle — owns the source the AST points into
    rules: ?[*]?*parser.rule,
    n: c_int,
};

/// Parse a ruleset.  The AST stays valid until testParseFree (parse_create
/// copies the source; tokens/atoms point into the parser-owned copy, exactly
/// like dl_load_rules which also frees the parser only AFTER compile_rules).
fn testParse(src: [*:0]const u8) ?parsed_rules {
    const p = parse_create(src) orelse return null;
    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse {
        parse_free(p);
        return null;
    };
    return .{ .p = p, .rules = rules, .n = n };
}

fn testParseFree(pd: parsed_rules) void {
    var i: c_int = 0;
    while (i < pd.n) : (i += 1) rule_free(pd.rules.?[@intCast(i)]);
    c.free(@ptrCast(pd.rules));
    parse_free(pd.p);
}

/// rm -rf `path` (bounded: test paths only).
fn testRmRf(path: [*:0]const u8) void {
    var buf: [256]u8 = undefined;
    const cmd = std.fmt.bufPrintZ(&buf, "rm -rf {s}", .{std.mem.span(path)}) catch return;
    _ = system(cmd.ptr);
}

/// Fresh writable db at `path` (removed first so card counts are exact).
fn testOpenDb(path: [*:0]const u8) *dx.dl_db {
    testRmRf(path);
    return dx.dl_open(path) orelse @panic("dl_open failed");
}

fn testCloseDb(db: *dx.dl_db, path: [*:0]const u8) void {
    dx.dl_close(db);
    testRmRf(path);
}

/// Add `n` facts row-major `cols` (arity `ar` each) to `rel`.
fn testAddFacts(db: *dx.dl_db, rel: [*:0]const u8, cols: []const u32, ar: u8, n: usize) void {
    var i: usize = 0;
    while (i < n) : (i += 1) {
        const rc = dx.dl_add_fact(db, rel, cols.ptr + i * ar, ar);
        if (rc != 1) @panic("dl_add_fact failed");
    }
}

/// Default toggle values (compiler.h): tests flip them and MUST restore.
fn testResetToggles() void {
    gBushyRef().* = 1;
    gReorderRef().* = 1;
    gPermSelectRef().* = 1;
    gPermCardThresholdRef().* = 4;
}

/// Compile `src` against `db` and return the compiled_rule** array (n out).
fn testCompile(db: *dx.dl_db, src: [*:0]const u8, n_out: *c_int) ?[*]?*compiled_rule {
    const pd = testParse(src) orelse return null;
    defer testParseFree(pd);
    var arr: ?[*]?*compiled_rule = null;
    const rc = compile_rules(db, pd.rules, pd.n, &arr, n_out);
    if (rc != 0) return null;
    return arr;
}

fn testFreeCompiled(arr: ?[*]?*compiled_rule, n: c_int) void {
    var i: c_int = 0;
    while (i < n) : (i += 1) compiled_rule_free(arr.?[@intCast(i)]);
    c.free(@ptrCast(arr));
}

fn testOpSeq(cr: *const compiled_rule, buf: []u8) []const u8 {
    const n: usize = @intCast(cr.n_instrs);
    for (cr.instrs.?[0..n], 0..) |ins, i| buf[i] = ins.op;
    return buf[0..n];
}

test "compile_rules: fact + simple rule bytecode shape (SCAN/PROJECT/HALT)" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_basic";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);
    var ops_buf: [16]u8 = undefined;


    // The "fact" half: two ground facts in the EDB relation the rule reads.
    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 2));
    testAddFacts(db, "edge", &.{ 1, 2, 2, 3 }, 2, 2);

    var n: c_int = 0;
    const arr = testCompile(db, "p(X,Y):-edge(X,Y).\n", &n) orelse return error.TestUnexpectedResult;
    defer testFreeCompiled(arr, n);
    try testing.expectEqual(@as(c_int, 1), n);

    const cr: *compiled_rule = arr[0].?;
    // edge declared first (rel 0); p auto-declared by compile_rules (rel 1).
    try testing.expectEqualStrings("p", std.mem.span(cr.head_pred.?));
    try testing.expectEqual(@as(u8, 1), cr.head_rel_id);
    try testing.expectEqual(@as(c_int, 3), cr.n_instrs);
    try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_PROJECT, OP_HALT }, testOpSeq(cr, &ops_buf));

    // SCAN edge, arity 2, body atom 0, slots = [X, Y] (head vars first:
    // X->0, Y->1).
    const scan = cr.instrs.?[0];
    try testing.expectEqual(OP_SCAN, scan.op);
    try testing.expectEqual(@as(u8, 0), scan.a); // rel edge
    try testing.expectEqual(@as(u8, 2), scan.b); // arity
    try testing.expectEqual(@as(u8, 0), scan.c);
    try testing.expectEqual(@as(u32, 0), scan.imm);
    try testing.expectEqual(@as(u8, 0), scan.body_idx);
    try testing.expectEqualSlices(u8, &.{ 0, 1 }, scan.slots[0..2]);

    // PROJECT p, arity 2, head slots [X, Y].
    const proj = cr.instrs.?[1];
    try testing.expectEqual(OP_PROJECT, proj.op);
    try testing.expectEqual(@as(u8, 1), proj.a); // rel p
    try testing.expectEqual(@as(u8, 2), proj.b);
    try testing.expectEqualSlices(u8, &.{ 0, 1 }, proj.slots[0..2]);

    // var table mirrors the slot assignment, names strdup'd.
    try testing.expectEqual(@as(u8, 2), cr.n_vars);
    try testing.expectEqualStrings("X", std.mem.span(cr.vars.?[0].name.?));
    try testing.expectEqual(@as(u8, 0), cr.vars.?[0].slot);
    try testing.expectEqualStrings("Y", std.mem.span(cr.vars.?[1].name.?));
    try testing.expectEqual(@as(u8, 1), cr.vars.?[1].slot);

    try testing.expectEqual(@as(c_int, 0), cr.stratum);
    try testing.expectEqual(@as(c_int, 0), cr.is_recursive);
    try testing.expectEqual(@as(c_int, 0), cr.has_aggregate);
    try testing.expectEqual(@as(c_int, 0), cr.n_patterns);
}

test "reorder toggle: 0 restores v1 body order; 1 reorders small-rel-first" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_reorder";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);
    var ops_buf: [16]u8 = undefined;


    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "big", 2));
    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "small", 2));
    testAddFacts(db, "big", &.{ 1, 1, 2, 2, 3, 3, 4, 4, 5, 5 }, 2, 5);
    // small stays EMPTY: the reorder hint db_rel_card() = rel_count() is the
    // DAFSA final-state count, and same-arity keys all share the terminator
    // suffix — so every non-empty fixed-arity relation costs 1, only an empty
    // one 0.  empty-vs-nonempty is the deterministic reorder trigger.

    const src = "p(X,Z):-big(X,Y),small(Y,Z).\n";

    // v1 body order (reorder=0): SCAN big, then LOOKUP small with the
    // bound leading column Y (k=1).  Threshold raised so the non-leading
    // consideration below stays out of the way of the order assertion.
    gReorderRef().* = 0;
    gPermCardThresholdRef().* = 1000;
    var n: c_int = 0;
    {
        const arr = testCompile(db, src, &n) orelse return error.TestUnexpectedResult;
        defer testFreeCompiled(arr, n);
        const cr: *compiled_rule = arr[0].?;
        try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_LOOKUP, OP_PROJECT, OP_HALT }, testOpSeq(cr, &ops_buf));
        try testing.expectEqual(@as(u8, 0), cr.instrs.?[0].a); // big
        try testing.expectEqual(@as(u8, 0), cr.instrs.?[0].body_idx);
        try testing.expectEqualSlices(u8, &.{ 0, 2 }, cr.instrs.?[0].slots[0..2]); // X, Y
        try testing.expectEqual(OP_LOOKUP, cr.instrs.?[1].op);
        try testing.expectEqual(@as(u8, 1), cr.instrs.?[1].a); // small
        try testing.expectEqual(@as(u8, 1), cr.instrs.?[1].b); // k = 1 bound leading col
        try testing.expectEqual(@as(u8, 1), cr.instrs.?[1].body_idx);
        try testing.expectEqualSlices(u8, &.{ 2, 1 }, cr.instrs.?[1].slots[0..2]); // Y bound, Z out
    }

    // Greedy reorder (default reorder=1): the EMPTY small (card 0) is
    // scanned before big (card 1) — and big's bound var Y is now NON-leading,
    // so with the perm index disabled by threshold it becomes the
    // OP_HASH_JOIN fallback.
    gReorderRef().* = 1;
    gPermCardThresholdRef().* = 1000;
    {
        const arr = testCompile(db, src, &n) orelse return error.TestUnexpectedResult;
        defer testFreeCompiled(arr, n);
        const cr: *compiled_rule = arr[0].?;
        try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_HASH_JOIN, OP_PROJECT, OP_HALT }, testOpSeq(cr, &ops_buf));
        try testing.expectEqual(@as(u8, 1), cr.instrs.?[0].a); // small first
        try testing.expectEqual(@as(u8, 1), cr.instrs.?[0].body_idx);
        try testing.expectEqualSlices(u8, &.{ 2, 1 }, cr.instrs.?[0].slots[0..2]); // Y, Z
        try testing.expectEqual(OP_HASH_JOIN, cr.instrs.?[1].op);
        try testing.expectEqual(@as(u8, 0), cr.instrs.?[1].a); // big second
        try testing.expectEqual(@as(u8, 0), cr.instrs.?[1].body_idx);
        try testing.expectEqual(@as(u8, 1), cr.instrs.?[1].b); // n_join (Y, non-leading)
        try testing.expectEqual(@as(u32, 1), cr.instrs.?[1].imm); // pack_perm([1,0],2)
        try testing.expectEqualSlices(u8, &.{ 0, 2 }, cr.instrs.?[1].slots[0..2]); // X, Y
    }
}

test "perm_select toggle: OP_LOOKUP_PERM vs OP_HASH_JOIN fallback" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_permsel";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);
    var ops_buf: [16]u8 = undefined;


    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "r0", 2));
    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "s", 2));
    testAddFacts(db, "r0", &.{ 1, 2 }, 2, 1);
    // 5 facts >= perm_card_threshold (4): perm index deemed worth it.
    testAddFacts(db, "s", &.{ 1, 1, 2, 2, 3, 3, 4, 4, 5, 5 }, 2, 5);

    // Source order pinned via reorder=0; in s(T,Z) the bound var Z sits in
    // a NON-leading column (col1), so the join goes through
    // emit_nonleading_join.  Slots: X->0, Z->1, T->2.
    const src = "q(X,Z):-r0(X,Z),s(T,Z).\n";

    // Default perm_select=1: non-recursive EDB join with card 5 >= 4
    // declares a perm index -> OP_LOOKUP_PERM (imm = perm id).
    var n: c_int = 0;
    {
        const arr = testCompile(db, src, &n) orelse return error.TestUnexpectedResult;
        defer testFreeCompiled(arr, n);
        const cr: *compiled_rule = arr[0].?;
        try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_LOOKUP_PERM, OP_PROJECT, OP_HALT }, testOpSeq(cr, &ops_buf));
        const j = cr.instrs.?[1];
        try testing.expectEqual(@as(u8, 1), j.a); // rel s
        try testing.expectEqual(@as(u8, 1), j.b); // n_join
        try testing.expectEqual(@as(u8, 2), j.c); // arity
        try testing.expectEqualSlices(u8, &.{ 2, 1 }, j.slots[0..2]); // T, Z
    }

    // perm_select=0: the oracle-equivalence backstop — never builds a perm
    // index, forces OP_HASH_JOIN with the perm packed into imm:
    // perm = [join_col 1, other_col 0] -> imm = 1 | (0 << 3) = 1.
    gPermSelectRef().* = 0;
    {
        const arr = testCompile(db, src, &n) orelse return error.TestUnexpectedResult;
        defer testFreeCompiled(arr, n);
        const cr: *compiled_rule = arr[0].?;
        try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_HASH_JOIN, OP_PROJECT, OP_HALT }, testOpSeq(cr, &ops_buf));
        const j = cr.instrs.?[1];
        try testing.expectEqual(@as(u8, 1), j.b); // n_join unchanged
        try testing.expectEqual(@as(u32, 1), j.imm); // pack_perm([1,0], 2)
        try testing.expectEqualSlices(u8, &.{ 2, 1 }, j.slots[0..2]);
    }
}

test "bushy toggle: left-deep when 0, MAT_BEGIN/MAT_JOIN plan when 1" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_bushy";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);
    var ops_buf: [16]u8 = undefined;


    inline for (.{ "a", "b", "c", "d" }) |rel| {
        try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, rel, 2));
        testAddFacts(db, rel, &.{ 1, 1 }, 2, 1);
    }

    // 4-atom cycle: diamond split L={a,b} R={c,d} with cut {X,Z} (w=2) is
    // bushy-eligible.  Slots: W->0 (head), X->1, Y->2, Z->3.
    const src = "p(W):-a(X,Y),b(Y,Z),c(Z,W),d(W,X).\n";

    // bushy=0: forced left-deep — only SCAN/LOOKUP (+PROJECT/HALT), and
    // because a,b,c,d all have card 1 the v1 order is preserved.
    gBushyRef().* = 0;
    var n: c_int = 0;
    {
        const arr = testCompile(db, src, &n) orelse return error.TestUnexpectedResult;
        defer testFreeCompiled(arr, n);
        const cr: *compiled_rule = arr[0].?;
        const ops = testOpSeq(cr, &ops_buf);
        try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_LOOKUP, OP_LOOKUP, OP_LOOKUP, OP_PROJECT, OP_HALT }, ops);
        for (ops) |op| {
            try testing.expect(op != OP_MAT_BEGIN and op != OP_MAT_JOIN);
        }
    }

    // Default bushy=1: L materialized (MAT_BEGIN buf0 + its 2 atoms + the
    // interface projection), then R the same, then MAT_JOIN buf0×buf1 on 2
    // shared cols, head PROJECT, HALT.
    gBushyRef().* = 1;
    {
        const arr = testCompile(db, src, &n) orelse return error.TestUnexpectedResult;
        defer testFreeCompiled(arr, n);
        const cr: *compiled_rule = arr[0].?;
        const ops = testOpSeq(cr, &ops_buf);
        try testing.expect(ops.len == 11);
        try testing.expectEqual(OP_MAT_BEGIN, ops[0]);
        try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_LOOKUP, OP_PROJECT }, ops[1..4]);
        try testing.expectEqual(OP_MAT_BEGIN, ops[4]);
        try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_LOOKUP, OP_PROJECT }, ops[5..8]);
        try testing.expectEqual(OP_MAT_JOIN, ops[8]);
        try testing.expectEqual(OP_PROJECT, ops[9]);
        try testing.expectEqual(OP_HALT, ops[10]);
        // First MAT_BEGIN: buf 0, interface arity 3 = [sh X,Z ++ private Y].
        try testing.expectEqual(@as(u8, 0), cr.instrs.?[0].a);
        try testing.expectEqual(@as(u8, 3), cr.instrs.?[0].b);
        try testing.expectEqualSlices(u8, &.{ 1, 3, 2 }, cr.instrs.?[0].slots[0..3]);
        // The subtree's interface projection copies the 3 iface slots.
        try testing.expectEqual(@as(u8, 0xFF), cr.instrs.?[3].a);
        try testing.expectEqual(@as(u8, 3), cr.instrs.?[3].b);
        try testing.expectEqualSlices(u8, &.{ 1, 3, 2 }, cr.instrs.?[3].slots[0..3]);
        // MAT_JOIN: L=buf0, R=buf1, 2 shared columns, out [X,Z,Y,W].
        const mj = cr.instrs.?[8];
        try testing.expectEqual(@as(u8, 0), mj.a);
        try testing.expectEqual(@as(u8, 1), mj.b);
        try testing.expectEqual(@as(u8, 2), mj.c);
        try testing.expectEqualSlices(u8, &.{ 1, 3, 2, 0 }, mj.slots[0..4]);
    }
}

test "SCC stratification: recursive tc gets stratum 0 + is_recursive" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_scc";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);
    var ops_buf: [16]u8 = undefined;


    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 2));
    testAddFacts(db, "edge", &.{ 1, 2, 2, 3 }, 2, 2);

    var n: c_int = 0;
    const arr = testCompile(
        db,
        "tc(X,Y):-edge(X,Y).\n" ++
            "tc(X,Y):-edge(X,Z),tc(Z,Y).\n",
        &n,
    ) orelse return error.TestUnexpectedResult;
    defer testFreeCompiled(arr, n);
    try testing.expectEqual(@as(c_int, 2), n);

    // Both rules head tc: auto-declared rel 1; positive self-loop keeps
    // stratum 0 but flags the SCC recursive.
    for ([_]usize{ 0, 1 }) |i| {
        const cr: *compiled_rule = arr[i].?;
        try testing.expectEqualStrings("tc", std.mem.span(cr.head_pred.?));
        try testing.expectEqual(@as(u8, 1), cr.head_rel_id);
        try testing.expectEqual(@as(c_int, 0), cr.stratum);
        try testing.expectEqual(@as(c_int, 1), cr.is_recursive);
        try testing.expectEqual(@as(c_int, 0), cr.has_aggregate);
    }

    // Rule 1: plain SCAN edge + PROJECT + HALT.
    try testing.expectEqual(@as(c_int, 3), arr[0].?.n_instrs);
    try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_PROJECT, OP_HALT }, testOpSeq(arr[0].?, &ops_buf));

    // Rule 2 (slots X->0, Y->1, Z->2): greedy reorder scans the EMPTY idb tc
    // (card 0) first, which leaves edge's bound var Z in a NON-leading
    // column — card(edge) 1 < threshold 4, so the OP_HASH_JOIN fallback.
    try testing.expectEqual(@as(c_int, 4), arr[1].?.n_instrs);
    try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_HASH_JOIN, OP_PROJECT, OP_HALT }, testOpSeq(arr[1].?, &ops_buf));
    try testing.expectEqual(@as(u8, 1), arr[1].?.instrs.?[0].a); // rel tc
    try testing.expectEqual(@as(u8, 1), arr[1].?.instrs.?[0].body_idx);
    try testing.expectEqual(@as(u8, 0), arr[1].?.instrs.?[1].a); // rel edge
    try testing.expectEqual(@as(u8, 0), arr[1].?.instrs.?[1].body_idx);
    try testing.expectEqual(@as(u8, 1), arr[1].?.instrs.?[1].b); // n_join (Z)
    try testing.expectEqual(@as(u32, 1), arr[1].?.instrs.?[1].imm); // pack_perm([1,0],2)
    try testing.expectEqualSlices(u8, &.{ 0, 2 }, arr[1].?.instrs.?[1].slots[0..2]); // X, Z
}

test "stratified negation: NEG_CHECK filter inside the scan frame + stratum 1" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_negation";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);
    var ops_buf: [16]u8 = undefined;


    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 2));
    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "blocked", 1));
    testAddFacts(db, "edge", &.{ 1, 2 }, 2, 1);
    testAddFacts(db, "blocked", &.{2}, 1, 1);

    var n: c_int = 0;
    const arr = testCompile(db, "p(X):-edge(X,Y),!blocked(Y).\n", &n) orelse return error.TestUnexpectedResult;
    defer testFreeCompiled(arr, n);
    try testing.expectEqual(@as(c_int, 1), n);

    const cr: *compiled_rule = arr[0].?;
    // edge=0, blocked=1, head p auto-declared=2.  The negated edge
    // blocked->p lifts p to stratum 1; p is not in any cycle.
    try testing.expectEqual(@as(u8, 2), cr.head_rel_id);
    try testing.expectEqual(@as(c_int, 1), cr.stratum);
    try testing.expectEqual(@as(c_int, 0), cr.is_recursive);
    try testing.expectEqual(@as(c_int, 4), cr.n_instrs);
    try testing.expectEqualSlices(u8, &.{ OP_SCAN, OP_NEG_CHECK, OP_PROJECT, OP_HALT }, testOpSeq(cr, &ops_buf));
    const neg = cr.instrs.?[1];
    try testing.expectEqual(OP_NEG_CHECK, neg.op);
    try testing.expectEqual(@as(u8, 1), neg.a); // rel blocked
    try testing.expectEqual(@as(u8, 1), neg.b); // arity
    try testing.expectEqual(@as(u8, 1), neg.body_idx);
    try testing.expectEqualSlices(u8, &.{1}, neg.slots[0..1]); // Y bound by edge
}

test "unsafe negation rejected: message + rule-head offset" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_unsafeneg";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 1));
    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "q", 1));

    const src = "p(X):-edge(X),!q(Y).\n";
    const pd = testParse(src) orelse return error.TestUnexpectedResult;
    defer testParseFree(pd);
    var arr: ?[*]?*compiled_rule = null;
    var n: c_int = 0;
    try testing.expectEqual(@as(c_int, -1), compile_rules(db, pd.rules, pd.n, &arr, &n));

    var off: c_uint = 0xFFFF;
    const msg = compile_last_error(&off) orelse return error.TestUnexpectedResult;
    try testing.expect(std.mem.indexOf(u8, std.mem.span(msg), "unsafe negation") != null);
    try testing.expect(std.mem.indexOf(u8, std.mem.span(msg), "'Y'") != null);
    try testing.expectEqual(@as(c_uint, 0), off); // head 'p' starts at byte 0
}

test "compile_last_error offset fidelity: error in the THIRD rule of a program" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_offset";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 1));

    // "r(X):-edge(X).\n" = 15 bytes, "s(X):-edge(X).\n" = 15 -> the bad
    // rule's head 't' sits at byte offset 30.
    const src = "r(X):-edge(X).\ns(X):-edge(X).\nt(X):-missing(X).\n";
    const pd = testParse(src) orelse return error.TestUnexpectedResult;
    defer testParseFree(pd);
    var arr: ?[*]?*compiled_rule = null;
    var n: c_int = 0;
    try testing.expectEqual(@as(c_int, -1), compile_rules(db, pd.rules, pd.n, &arr, &n));

    var off: c_uint = 0xFFFF;
    const msg = compile_last_error(&off) orelse return error.TestUnexpectedResult;
    try testing.expect(std.mem.indexOf(u8, std.mem.span(msg), "unknown predicate 'missing'") != null);
    try testing.expectEqual(@as(c_uint, 30), off);
}

test "unstratifiable program rejected: negation through a recursion cycle" {
    defer testResetToggles();
    testResetToggles();

    const path = "/tmp/datalog_zig_u8_compiler_unstrat";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "move", 2));
    testAddFacts(db, "move", &.{ 1, 2 }, 2, 1);

    const src = "win(X):-move(X,Y),!win(Y).\n";
    const pd = testParse(src) orelse return error.TestUnexpectedResult;
    defer testParseFree(pd);
    var arr: ?[*]?*compiled_rule = null;
    var n: c_int = 0;
    try testing.expectEqual(@as(c_int, -1), compile_rules(db, pd.rules, pd.n, &arr, &n));

    var off: c_uint = 7;
    const msg = compile_last_error(&off) orelse return error.TestUnexpectedResult;
    try testing.expect(std.mem.indexOf(u8, std.mem.span(msg), "unstratifiable") != null);
    try testing.expectEqual(@as(c_uint, 0), off); // strata rejection reports 0
}

test "extern struct layouts match src/compiler.h (LP64, pinned offsets)" {
    // vm_instr: 4×u8 + u32 imm + u8[8] slots + u8 body_idx, align 4.
    try testing.expectEqual(@as(usize, 20), @sizeOf(vm_instr));
    try testing.expectEqual(@as(usize, 4), @offsetOf(vm_instr, "imm"));
    try testing.expectEqual(@as(usize, 8), @offsetOf(vm_instr, "slots"));
    try testing.expectEqual(@as(usize, 16), @offsetOf(vm_instr, "body_idx"));
    try testing.expectEqual(@sizeOf(dx.vm_instr), @sizeOf(vm_instr));
    try testing.expectEqual(@offsetOf(dx.vm_instr, "imm"), @offsetOf(vm_instr, "imm"));
    try testing.expectEqual(@offsetOf(dx.vm_instr, "slots"), @offsetOf(vm_instr, "slots"));
    try testing.expectEqual(@offsetOf(dx.vm_instr, "body_idx"), @offsetOf(vm_instr, "body_idx"));

    // var_info: char* name + u8 slot, align 8.
    try testing.expectEqual(@as(usize, 16), @sizeOf(var_info));
    try testing.expectEqual(@as(usize, 8), @offsetOf(var_info, "slot"));
    try testing.expectEqual(@sizeOf(dx.var_info), @sizeOf(var_info));
    try testing.expectEqual(@offsetOf(dx.var_info, "slot"), @offsetOf(var_info, "slot"));

    // compiled_rule: 5 pointers + 4 ints interleaved with two u8s.
    try testing.expectEqual(@as(usize, 0), @offsetOf(compiled_rule, "head_pred"));
    try testing.expectEqual(@as(usize, 8), @offsetOf(compiled_rule, "head_rel_id"));
    try testing.expectEqual(@as(usize, 9), @offsetOf(compiled_rule, "n_vars"));
    try testing.expectEqual(@as(usize, 16), @offsetOf(compiled_rule, "vars"));
    try testing.expectEqual(@as(usize, 24), @offsetOf(compiled_rule, "n_instrs"));
    try testing.expectEqual(@as(usize, 32), @offsetOf(compiled_rule, "instrs"));
    try testing.expectEqual(@as(usize, 40), @offsetOf(compiled_rule, "stratum"));
    try testing.expectEqual(@as(usize, 44), @offsetOf(compiled_rule, "is_recursive"));
    try testing.expectEqual(@as(usize, 48), @offsetOf(compiled_rule, "has_aggregate"));
    try testing.expectEqual(@as(usize, 52), @offsetOf(compiled_rule, "n_patterns"));
    try testing.expectEqual(@as(usize, 56), @offsetOf(compiled_rule, "patterns"));
    try testing.expectEqual(@as(usize, 64), @sizeOf(compiled_rule));
    try testing.expectEqual(@sizeOf(dx.compiled_rule), @sizeOf(compiled_rule));
    try testing.expectEqual(@offsetOf(dx.compiled_rule, "stratum"), @offsetOf(compiled_rule, "stratum"));
    try testing.expectEqual(@offsetOf(dx.compiled_rule, "patterns"), @offsetOf(compiled_rule, "patterns"));
}
