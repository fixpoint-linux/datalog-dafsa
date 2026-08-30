//! analyze.zig — port of src/analyze.c (shared in-memory program analysis).
//!
//! This is the factor of playground-wasm.c's in-memory ingest: the pure
//! primitives that build a dir==NULL dl_db from a source document of facts +
//! rules.  Both the browser playground and the LSP call analyze_program(), so
//! the two agree byte-for-byte on what the engine accepts or rejects.
//!
//! PASS1 pre-declares every head (fact + rule) in-memory, PASS2 adds the
//! ground facts, then compile_rules() runs on the nbody>0 rules only.
//! analyze_program() STOPS before dl_compile()/dl_query(): it never runs the
//! fixpoint.
//!
//! Strangler-hybrid ABI: `analyze_error` is a CONCRETE extern struct still
//! filled by analyze_program and read by the retained C consumers (lsp.c /
//! playground), so it is defined here byte-identical to analyze.h and
//! comptime-gated against @cImport("analyze.h")'s translate-c layout.  The
//! in-memory dl_db is dl.zig's authoritative DlDb extern struct (comptime-
//! gated against dl_internal.h there); relations via relation.zig/vrelation.zig,
//! rule ASTs via parser.zig, compile_rules/compiled_rule_free/compile_last_error
//! via compiler.zig.  The interner/term-store are reached through the same
//! raw extern bindings dl.zig uses (module-private Zig types).
//!
//! Oracle: src/analyze.c (never modified).  All allocation through raw libc.

const std = @import("std");
const c = std.c;

const dl = @import("dl.zig");
const parser = @import("parser.zig");
const compiler = @import("compiler.zig");
const relation = @import("relation.zig");
const vrelation = @import("vrelation.zig");
const termstore = @import("termstore.zig");

// analyze.h: analyze_error + analyze_stage (reference for the comptime
// layout gate only — the implementation mirrors the C oracle).
const ax = @cImport({
    @cInclude("analyze.h");
});

// libc decls not in std.c (precedent: dl.zig).
extern "c" fn strdup(s: [*c]const u8) ?[*:0]u8;

// Ported-module exports whose Zig types are module-private (same bindings
// dl.zig uses): interner/term-store handles + the parser's opaque handle.
extern "c" fn intern_create() ?*anyopaque;
extern "c" fn intern_free(ir: ?*anyopaque) void;
extern "c" fn intern_str(ir: ?*anyopaque, str: [*c]const u8) u32;
extern "c" fn term_create() ?*anyopaque;
extern "c" fn term_free(t: ?*anyopaque) void;
extern "c" fn term_cons(t: ?*anyopaque, head: u32, tail: u32) u32;
extern "c" fn parse_create_reporting(source: ?[*:0]const u8) ?*anyopaque;
extern "c" fn parse_last_error(p: ?*anyopaque, off: ?*c_uint) ?[*:0]const u8;
extern "c" fn parse_rules(p: ?*anyopaque, n_rules: ?*c_int) ?[*]?*parser.rule;
extern "c" fn parse_free(p: ?*anyopaque) void;
extern "c" fn rule_free(r: ?*parser.rule) void;
// compile_rules takes compiler.zig's private @cImport'd dl_db (same layout as
// DlDb) — reached through the same raw extern binding as dl.zig.
extern "c" fn compile_rules(db: ?*dl.DlDb, rules: ?[*]?*parser.rule, n_rules: c_int, out_rules: ?*?[*]?*compiler.compiled_rule, out_n: ?*c_int) c_int;

// ─── Constants (mirror src/dl_internal.h / compiler.h) ────────────────────

const MAX_RELS: usize = 64;
const MAX_ARITY: u8 = 8;

const RELK_FIXED: u8 = 0;
const RELK_VARIADIC: u8 = 1;

// ─── Public C-ABI struct (src/analyze.h, byte-for-byte) ───────────────────

/// analyze_stage — the five-way split (plus OOM) is REQUIRED so the playground
/// can map each stage back to its EXACT pre-existing coarse error string.
pub const ANALYZE_PARSE: c_int = 0; // parse error;  msg = parser's message
pub const ANALYZE_MALFORMED: c_int = 1; // head missing / no pred / nargs < 1
pub const ANALYZE_DECLARE: c_int = 2; // in-memory declare failed (arity/name clash)
pub const ANALYZE_FACT: c_int = 3; // non-ground fact; msg = "compile error: ..."
pub const ANALYZE_COMPILE: c_int = 4; // compile_rules error; msg = compiler's message
pub const ANALYZE_OOM: c_int = 5; // internal out-of-memory

/// typedef struct { int stage; uint32_t off; char msg[256]; } analyze_error.
pub const AnalyzeError = extern struct {
    stage: c_int, // one of analyze_stage
    off: u32, // 0-based byte offset into the source
    msg: [256]u8, // human-readable message (parser/compiler text)
};

// Comptime gate: our extern layout must be byte-identical to the C header.
comptime {
    std.debug.assert(@sizeOf(AnalyzeError) == @sizeOf(ax.analyze_error));
    std.debug.assert(@offsetOf(AnalyzeError, "off") == @offsetOf(ax.analyze_error, "off"));
    std.debug.assert(@offsetOf(AnalyzeError, "msg") == @offsetOf(ax.analyze_error, "msg"));
    std.debug.assert(@sizeOf(@FieldType(AnalyzeError, "msg")) == @sizeOf(@FieldType(ax.analyze_error, "msg")));
}

// ─── C-string helper (mirror snapshot.zig) ────────────────────────────────

fn strEq(a: [*c]const u8, b: [*c]const u8) bool {
    var i: usize = 0;
    while (a[i] != 0 and a[i] == b[i]) i += 1;
    return a[i] == b[i];
}

fn setErr(err: ?*AnalyzeError, stage: c_int, off: u32, msg: ?[*:0]const u8) void {
    const e = err orelse return;
    e.stage = stage;
    e.off = off;
    if (msg) |m| {
        // strncpy(msg, 255) + explicit NUL: copy, then zero-fill the rest.
        const len = @min(std.mem.len(m), 255);
        @memcpy(e.msg[0..len], m[0..len]);
        @memset(e.msg[len..], 0);
    } else {
        e.msg[0] = 0;
    }
}

// ─── In-memory db helpers (mirror eval_db_declare_inmem, dl.c:2343) ────

fn memFindRel(db: *dl.DlDb, name: [*c]const u8) c_int {
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (db.rels[i].name != null and strEq(db.rels[i].name.?, name))
            return @intCast(i);
    }
    return -1;
}

/// Declare a FIXED relation in-memory (rel_create, no WAL/disk).  Idempotent
/// for the same name + arity (like dl_declare_relation_kind's no-op branch).
fn memDeclare(db: *dl.DlDb, name: [*c]const u8, arity: u8) c_int {
    const idx = memFindRel(db, name);
    if (arity == 0 or arity > MAX_ARITY) return -1;
    if (idx >= 0) {
        if (db.rels[@intCast(idx)].kind != RELK_FIXED) return -1;
        return if (relation.rel_arity(db.rels[@intCast(idx)].rel.?) == arity) 0 else -1;
    }
    if (db.nrels >= MAX_RELS) return -1;
    const rel = relation.rel_create(arity) orelse return -1;
    const name_copy = strdup(name) orelse {
        relation.rel_free(rel);
        return -1;
    };
    db.rels[db.nrels] = .{
        .name = name_copy,
        .kind = RELK_FIXED,
        .arity = arity,
        .rel = rel,
        .vrel = null,
    };
    db.nrels += 1;
    return 0;
}

// ─── Ground fact constant → u32 (parser token) ──────────────────────────

/// Convert a single ground element token to a u32 value.  Returns 1 on
/// success, 0 on error (a TOK_VAR / non-ground token).
fn elemOf(db: *dl.DlDb, t: *const parser.token, out: *u32) c_int {
    switch (t.kind) {
        parser.TOK_INT => {
            out.* = t.ival;
            return 1;
        },
        parser.TOK_IDENT, parser.TOK_STRING => {
            out.* = intern_str(db.ir, t.text);
            return if (out.* != 0) 1 else 0; // 0 == OOM; a valid sym is never 0
        },
        else => return 0, // TOK_VAR / TOK_LIST-as-elem (not supported here)
    }
}

/// Build a list handle from a TOK_LIST token's element tokens.  Returns the
/// handle, or 0 on error (unbound element / OOM / non-list tail).
fn listOf(db: *dl.DlDb, t: *const parser.token) u32 {
    var tail: u32 = termstore.TERM_NIL;
    if (t.kind != parser.TOK_LIST) return 0;
    var i: c_int = t.nchildren - 1;
    while (i >= 0) : (i -= 1) {
        var el: u32 = undefined;
        if (elemOf(db, t.children.?[@intCast(i)].?, &el) == 0) return 0;
        tail = term_cons(db.terms, el, tail);
        if (tail == 0) return 0; // OOM or non-list tail
    }
    return tail;
}

/// Convert a ground fact argument token to a u32 column value.
/// Returns 1 on success (cols_out set), 0 on error (non-ground fact).
fn constOf(db: *dl.DlDb, t: *const parser.token, cols_out: *u32) c_int {
    switch (t.kind) {
        parser.TOK_INT => {
            cols_out.* = t.ival;
            return 1;
        },
        parser.TOK_IDENT, parser.TOK_STRING => {
            cols_out.* = intern_str(db.ir, t.text);
            return if (cols_out.* != 0) 1 else 0;
        },
        parser.TOK_LIST => {
            const h = listOf(db, t);
            if (h == 0) return 0;
            cols_out.* = h;
            return 1;
        },
        else => return 0, // TOK_VAR / other: non-ground fact
    }
}

/// Add one ground fact (head-only rule, nbody==0) to its relation's base.
fn memAddFact(db: *dl.DlDb, r: *parser.rule) c_int {
    var cols: [MAX_ARITY]u32 = undefined;
    const head = r.head orelse return -1;
    if (head.nargs > MAX_ARITY or head.nargs < 1) return -1;
    var i: usize = 0;
    while (i < @as(usize, @intCast(head.nargs))) : (i += 1) {
        if (constOf(db, head.args.?[@intCast(i)].?, &cols[i]) == 0) return -1;
    }
    const idx = memFindRel(db, head.pred.?);
    if (idx < 0) return -1;
    if (db.rels[@intCast(idx)].kind != RELK_FIXED) return -1;
    // rel_add_base returns 1 (added), 0 (dup) or -1 (error).  Any
    // non-negative result is a successful insert — normalize to 0.
    return if (relation.rel_add_base(db.rels[@intCast(idx)].rel.?, &cols) < 0) -1 else 0;
}

// ─── Rule loading (compile_rules + append to db->crules) ────────────────

fn freeNewCrules(new_crules: ?[*]?*compiler.compiled_rule, n_compiled: c_int) void {
    if (new_crules) |arr| {
        var i: c_int = 0;
        while (i < n_compiled) : (i += 1) compiler.compiled_rule_free(arr[@intCast(i)]);
        c.free(@ptrCast(arr));
    }
}

fn loadRulesAst(db: *dl.DlDb, rules: ?[*]?*parser.rule, n_rules: c_int) c_int {
    var new_crules: ?[*]?*compiler.compiled_rule = null;
    var n_compiled: c_int = 0;
    if (n_rules <= 0) return 0;
    if (compile_rules(db, rules, n_rules, &new_crules, &n_compiled) != 0) {
        freeNewCrules(new_crules, n_compiled);
        return -1;
    }

    const new_total = db.n_crules + n_compiled;
    const merged_raw = c.realloc(@ptrCast(db.crules), @as(usize, @intCast(new_total)) * @sizeOf(?*compiler.compiled_rule)) orelse {
        freeNewCrules(new_crules, n_compiled);
        return -1;
    };
    const merged: [*]?*compiler.compiled_rule = @ptrCast(@alignCast(merged_raw));
    // memcpy(merged + db->n_crules, new_crules, n_compiled * sizeof(ptr))
    var i: usize = 0;
    while (i < @as(usize, @intCast(n_compiled))) : (i += 1)
        merged[@as(usize, @intCast(db.n_crules)) + i] = new_crules.?[i];
    c.free(@ptrCast(new_crules.?));
    db.crules = merged;
    db.n_crules = new_total;

    db.fixpoint_dirty = 1;
    return 0;
}

// ─── Teardown ───────────────────────────────────────────────────────────

/// Free a db produced by analyze_program() (NULL-safe).  Mirrors the
/// playground's mem_db_free: compiled rules, relations, interner, term store.
pub export fn analyze_db_free(db: ?*dl.DlDb) void {
    const d = db orelse return;
    if (d.crules) |arr| {
        var i: usize = 0;
        while (i < @as(usize, @intCast(d.n_crules))) : (i += 1)
            compiler.compiled_rule_free(arr[i]);
        c.free(@ptrCast(arr));
        d.crules = null;
        d.n_crules = 0;
    }
    var i: usize = 0;
    while (i < d.nrels) : (i += 1) {
        if (d.rels[i].kind == RELK_VARIADIC)
            vrelation.vrel_free(d.rels[i].vrel)
        else
            relation.rel_free(d.rels[i].rel);
        c.free(@ptrCast(d.rels[i].name));
    }
    d.nrels = 0;
    intern_free(d.ir);
    term_free(d.terms);
    d.* = std.mem.zeroes(dl.DlDb);
    d.lock_fd = -1;
}

// ─── Entry point ───────────────────────────────────────────────────────

/// Analyze `source` (facts + rules in one document) in memory.
/// On success: 0 + *out_db (caller frees via analyze_db_free).  On failure:
/// -1 + *out_db = NULL + *err filled with the 5-way stage split.
pub export fn analyze_program(source: ?[*:0]const u8, out_db: ?*?*dl.DlDb, err: ?*AnalyzeError) c_int {
    var db: ?*dl.DlDb = null;
    var p: ?*anyopaque = null;
    var rules: ?[*]?*parser.rule = null;
    var n_rules: c_int = 0;
    var rc: c_int = -1;

    if (err) |e| {
        e.stage = ANALYZE_PARSE;
        e.off = 0;
        e.msg[0] = 0;
    }
    if (out_db) |o| o.* = null;
    if (source == null or out_db == null) return -1;

    // Parse (LSP-only reporting parse keeps the parser on a lexer error).
    p = parse_create_reporting(source) orelse {
        setErr(err, ANALYZE_OOM, 0, "out of memory");
        return -1;
    };
    if (parse_last_error(p, null) != null) {
        // tokenize failed — the parser is still alive so we can read the
        // position before freeing it.
        var poff: c_uint = 0;
        const pmsg = parse_last_error(p, &poff);
        setErr(err, ANALYZE_PARSE, poff, pmsg);
        parse_free(p);
        return -1;
    }

    rules = parse_rules(p, &n_rules);
    if (rules == null) {
        var poff: c_uint = 0;
        const pmsg = parse_last_error(p, &poff);
        setErr(err, ANALYZE_PARSE, poff, if (pmsg != null) pmsg else "parse failed");
        parse_free(p);
        return -1;
    }
    parse_free(p);
    p = null;

    build: {
        // Build the dir==NULL in-memory db.
        db = @ptrCast(@alignCast(c.calloc(1, @sizeOf(dl.DlDb)) orelse {
            setErr(err, ANALYZE_OOM, 0, "out of memory");
            break :build;
        }));
        db.?.dir = null;
        db.?.lock_fd = -1;
        db.?.rev_rel_id = -1; // CAS: no rev relation on an eval clone
        db.?.ir = intern_create();
        db.?.terms = term_create();
        if (db.?.ir == null or db.?.terms == null) {
            setErr(err, ANALYZE_OOM, 0, "out of memory");
            break :build;
        }

        // PASS 1: pre-declare EVERY head (facts AND rules) in-memory, so the
        // compiler's dl_declare_relation branch (which would hit the dir==NULL
        // guard) is never reached.
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) {
            const head = rules.?[@intCast(i)].?.head;
            if (head == null or head.?.pred == null or head.?.nargs < 1) {
                setErr(err, ANALYZE_MALFORMED, rules.?[@intCast(i)].?.off, "malformed rule");
                break :build;
            }
            if (memDeclare(db.?, head.?.pred.?, @intCast(head.?.nargs)) != 0) {
                setErr(err, ANALYZE_DECLARE, rules.?[@intCast(i)].?.off, "cannot declare relation");
                break :build;
            }
        }

        // PASS 2: split facts (nbody==0, ground head) from rules.
        i = 0;
        while (i < n_rules) : (i += 1) {
            const r = rules.?[@intCast(i)].?;
            if (r.nbody == 0) {
                if (memAddFact(db.?, r) != 0) {
                    setErr(err, ANALYZE_FACT, r.off, "compile error: non-ground fact");
                    break :build;
                }
            }
        }

        // Compile all RULES (nbody > 0) — facts were handled above.
        {
            var n_r_rules: c_int = 0;
            const alloc_n: usize = if (n_rules != 0) @intCast(n_rules) else 1;
            const r_rules_raw = c.malloc(alloc_n * @sizeOf(?*parser.rule)) orelse {
                setErr(err, ANALYZE_OOM, 0, "out of memory");
                break :build;
            };
            const r_rules: [*]?*parser.rule = @ptrCast(@alignCast(r_rules_raw));
            var j: c_int = 0;
            while (j < n_rules) : (j += 1) {
                if (rules.?[@intCast(j)].?.nbody > 0) {
                    r_rules[@intCast(n_r_rules)] = rules.?[@intCast(j)];
                    n_r_rules += 1;
                }
            }
            if (loadRulesAst(db.?, r_rules, n_r_rules) != 0) {
                var coff: c_uint = 0;
                const cmsg = compiler.compile_last_error(&coff);
                c.free(r_rules_raw);
                setErr(err, ANALYZE_COMPILE, coff, if (cmsg != null) cmsg else "rule compile failed");
                break :build;
            }
            c.free(r_rules_raw);
        }

        rc = 0;
    }

    // fail:
    if (rules) |arr| {
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) rule_free(arr[@intCast(i)]);
        c.free(@ptrCast(arr));
    }
    if (p != null) parse_free(p);
    if (rc != 0) {
        analyze_db_free(db);
        db = null;
    }
    out_db.?.* = db;
    return rc;
}

// ─── Tests ────────────────────────────────────────────────────────────────

const testing = std.testing;

test "extern struct layout matches src/analyze.h (LP64, pinned offsets)" {
    try testing.expectEqual(@as(usize, 264), @sizeOf(AnalyzeError));
    try testing.expectEqual(@as(usize, 0), @offsetOf(AnalyzeError, "stage"));
    try testing.expectEqual(@as(usize, 4), @offsetOf(AnalyzeError, "off"));
    try testing.expectEqual(@as(usize, 8), @offsetOf(AnalyzeError, "msg"));
}

test "analyze_program: facts + rule compile, then analyze_db_free" {
    var db: ?*dl.DlDb = null;
    var err = std.mem.zeroes(AnalyzeError);

    const rc = analyze_program(
        \\edge(a, b).
        \\edge(b, c).
        \\path(X, Y) :- edge(X, Y).
    , &db, &err);
    try testing.expectEqual(@as(c_int, 0), rc);
    try testing.expect(db != null);
    defer analyze_db_free(db);

    // PASS1 declared both heads in-memory (edge = fact head, path = rule head).
    try testing.expect(memFindRel(db.?, "edge") >= 0);
    try testing.expect(memFindRel(db.?, "path") >= 0);

    // PASS2 added the two ground facts to edge's base.
    try testing.expectEqual(@as(u64, 2), dl.dl_count(db, "edge"));

    // The nbody>0 rule was compiled (and is owned by the db).
    try testing.expectEqual(@as(c_int, 1), db.?.n_crules);

    // STOPS before dl_compile(): the fixpoint never ran, so path stays empty.
    try testing.expectEqual(@as(u64, 0), dl.dl_count(db, "path"));
}

test "analyze_program: error stages (parse / fact / compile)" {
    var db: ?*dl.DlDb = null;
    var err = std.mem.zeroes(AnalyzeError);

    // Parse error (unbalanced paren) -> ANALYZE_PARSE with the parser offset.
    try testing.expectEqual(@as(c_int, -1), analyze_program("edge(a, b\n.", &db, &err));
    try testing.expect(db == null);
    try testing.expectEqual(ANALYZE_PARSE, err.stage);
    try testing.expect(err.off > 0);
    try testing.expect(err.msg[0] != 0);

    // Non-ground fact -> ANALYZE_FACT at the fact's offset.
    try testing.expectEqual(@as(c_int, -1), analyze_program("edge(a, b).\nedge(X, c).\n", &db, &err));
    try testing.expect(db == null);
    try testing.expectEqual(ANALYZE_FACT, err.stage);
    try testing.expectEqualStrings("compile error: non-ground fact", std.mem.span(@as([*:0]const u8, @ptrCast(&err.msg))));

    // Compile error (unsafe negation: Y unbound at !q(Y)) -> COMPILE.
    try testing.expectEqual(@as(c_int, -1), analyze_program("edge(a, b).\nq(c).\np(X) :- edge(X, Z), !q(Y).\n", &db, &err));
    try testing.expect(db == null);
    try testing.expectEqual(ANALYZE_COMPILE, err.stage);
    try testing.expect(err.msg[0] != 0);

    // NULL contract: NULL source or NULL out_db -> -1, no crash.
    try testing.expectEqual(@as(c_int, -1), analyze_program(null, &db, &err));
    try testing.expectEqual(@as(c_int, -1), analyze_program("edge(a,b).", null, &err));

    // err == NULL is accepted (C: "err may be NULL").
    try testing.expectEqual(@as(c_int, -1), analyze_program("edge(a, b", null, null));

    // Lists: ground list fact + a list-bearing rule compile fine (v2 lists).
    try testing.expectEqual(@as(c_int, 0), analyze_program("holds(a, [1, 2, 3]).\nsame(L) :- holds(a, L).\n", &db, &err));
    try testing.expect(db != null);
    analyze_db_free(db);
    db = null;
}

test "analyze_db_free: NULL-safe + resets the struct" {
    var db: ?*dl.DlDb = null;
    var err = std.mem.zeroes(AnalyzeError);
    try testing.expectEqual(@as(c_int, 0), analyze_program("edge(a, b).\n", &db, &err));
    const d = db.?;
    analyze_db_free(d);
    // memset + lock_fd = -1 (mirror of the C teardown).
    try testing.expectEqual(@as(c_int, -1), d.lock_fd);
    try testing.expectEqual(@as(usize, 0), d.nrels);
    try testing.expectEqual(@as(c_int, 0), d.n_crules);
    analyze_db_free(null); // NULL-safe
}
