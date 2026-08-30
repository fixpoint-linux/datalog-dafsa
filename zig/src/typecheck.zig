//! typecheck.zig — port of src/typecheck.c (S3 per-rule Datalog typechecker,
//! occurrence-consistency).
//!
//! Given a dl_schema and the parser's rule** from dl_load_rules, verify each
//! rule's variables are used with a CONSISTENT type across every occurrence
//! (head + body).  v1 does NO polymorphism/unification: each variable maps to a
//! single dl_colspec for the whole rule; the first conflicting occurrence is
//! reported with both sites (file:line:col via the S1 line/col fields).
//!
//! Strangler-hybrid ABI: the single export is dl_typecheck_rules (declared in
//! src/schema.h; `rules` stays `void*` there so that header need not include
//! parser.h).  dl_schema/dl_colspec/dl_coltype come from schema.zig (U2, the
//! byte-identical extern structs); the AST comes from parser.zig (U5).
//!
//! Atom dispatch mirrors compiler.c's name-based recognition (the builtin
//! predicate-name sets are COPIED here — the compiler's helpers are static, so
//! this module must not depend on compiler internals).  List builtins and the
//! `range` builtin are typed here too (v2).  Stratification is the compiler's
//! job, so a negated atom is typed exactly like its positive form.
//!
//! Because dl_typecheck_rules is called by dl_load_rules which knows no source
//! filename, the default "file" component of every diagnostic is the literal
//! `<input>` — the dlp tool overrides it with the real rules-file path via the
//! `srcname` parameter.
//!
//! Oracle: src/typecheck.c (never modified).

const std = @import("std");
const c = std.c;
const schema_mod = @import("schema.zig");
const parser_mod = @import("parser.zig");

// glibc strdup — not re-exported by std.c; the C oracle uses it for varent's
// owned variable-name copies.
extern "c" fn strdup(s: [*:0]const u8) ?[*:0]u8;

const dl_schema = schema_mod.dl_schema;
const dl_reldef = schema_mod.dl_reldef;
const dl_colspec = schema_mod.dl_colspec;
const dl_coltype = schema_mod.dl_coltype;
const DLT_NATURAL = schema_mod.DLT_NATURAL;
const DLT_TEXT = schema_mod.DLT_TEXT;
const DLT_BOOL = schema_mod.DLT_BOOL;
const DLT_CHAR = schema_mod.DLT_CHAR;
const DLT_DATE = schema_mod.DLT_DATE;
const DLT_TIMESTAMP = schema_mod.DLT_TIMESTAMP;
const DLT_SIGNED = schema_mod.DLT_SIGNED;
const DLT_LIST = schema_mod.DLT_LIST;
const DLT_OPTIONAL = schema_mod.DLT_OPTIONAL;
const DLT_ENUM = schema_mod.DLT_ENUM;

const token = parser_mod.token;
const atom = parser_mod.atom;
const rule = parser_mod.rule;
const expr = parser_mod.expr;

// schema.zig `export fn`s (C ABI); link against them directly — same pattern
// as relation.zig's ts_* externs (keeps schema.zig's decls un-pub).
extern "c" fn dl_schema_add(s: ?*dl_schema, name: ?[*:0]const u8, arity: u8, cols: ?[*]const dl_colspec, is_idb: c_int) c_int;
extern "c" fn dl_schema_find(s: ?*const dl_schema, name: ?[*:0]const u8) ?*const dl_reldef;
extern "c" fn dl_colspec_eq(a: dl_colspec, b: dl_colspec) c_int;

// parser.zig `export fn`s (C ABI), used by the tests below (the parser handle
// is opaque here, exactly as in C callers that only hold a `parser *`).
extern "c" fn parse_create(source: ?[*:0]const u8) ?*anyopaque;
extern "c" fn parse_rules(p: ?*anyopaque, n_rules: ?*c_int) ?[*]?*rule;
extern "c" fn parse_free(p: ?*anyopaque) void;
extern "c" fn rule_free(r: ?*rule) void;

/// strcmp == 0 for NUL-terminated byte strings.
fn cstrEql(a: [*:0]const u8, b: [*:0]const u8) bool {
    return std.mem.eql(u8, std.mem.span(a), std.mem.span(b));
}

/// Source name used in diagnostics.  dl_typecheck_rules sets it from its
/// `srcname` parameter (NULL => `<input>`).
var g_srcname: [*:0]const u8 = "<input>";

// ─── Copy of the builtin predicate-name classification (from compiler.c,
//    which keeps these static — do NOT depend on compiler internals) ─────────

fn isComparisonPred(p: [*:0]const u8) bool {
    return cstrEql(p, "<") or cstrEql(p, "<=") or
        cstrEql(p, ">") or cstrEql(p, ">=") or
        cstrEql(p, "!=");
}

fn isStrProducingPred(p: [*:0]const u8) bool {
    return cstrEql(p, "concat") or cstrEql(p, "length") or
        cstrEql(p, "lower") or cstrEql(p, "upper");
}

fn isStrFilterPred(p: [*:0]const u8) bool {
    return cstrEql(p, "prefix") or
        cstrEql(p, "suffix") or
        cstrEql(p, "contains");
}

fn isListBuiltinPred(p: [*:0]const u8) bool {
    return cstrEql(p, "cons") or cstrEql(p, "car") or
        cstrEql(p, "cdr") or cstrEql(p, "append") or
        cstrEql(p, "member");
}

fn isRangeBuiltinPred(p: [*:0]const u8) bool {
    return cstrEql(p, "range");
}

fn isReservedBuiltinName(name: [*:0]const u8) bool {
    return cstrEql(name, "member") or cstrEql(name, "car") or
        cstrEql(name, "cons") or cstrEql(name, "cdr") or
        cstrEql(name, "append") or cstrEql(name, "concat") or
        cstrEql(name, "length") or cstrEql(name, "lower") or
        cstrEql(name, "upper") or cstrEql(name, "prefix") or
        cstrEql(name, "suffix") or cstrEql(name, "contains") or
        cstrEql(name, "range");
}

// ─── Per-rule variable table ────────────────────────────────────────────────

const varent = struct {
    name: ?[*:0]u8 = null, // owned copy of the variable name
    type: dl_colspec = std.mem.zeroes(dl_colspec), // tag 0 = untyped so far
    line: c_int = 0, // first site that typed it (S1 line:col)
    col: c_int = 0,
    site: ?[*:0]const u8 = null, // human-readable label of that first site
};

const vtab = struct {
    v: ?[*]varent = null,
    n: c_int = 0,
    cap: c_int = 0,
};

fn vtabFree(t: *vtab) void {
    var i: c_int = 0;
    while (i < t.n) : (i += 1) {
        if (t.v.?[@intCast(i)].name) |nm| c.free(@ptrCast(nm));
    }
    c.free(@ptrCast(t.v));
    t.* = .{};
}

fn vtabFind(t: *vtab, name: [*:0]const u8) ?*varent {
    var i: c_int = 0;
    while (i < t.n) : (i += 1) {
        if (cstrEql(t.v.?[@intCast(i)].name.?, name)) return &t.v.?[@intCast(i)];
    }
    return null;
}

/// Get-or-create the entry for `name`; returns NULL on OOM.
fn vtabGet(t: *vtab, name: [*:0]const u8) ?*varent {
    if (vtabFind(t, name)) |e| return e;
    if (t.n == t.cap) {
        const nc: c_int = if (t.cap != 0) t.cap * 2 else 16;
        const nv: ?[*]varent = @ptrCast(@alignCast(c.realloc(@ptrCast(t.v), @as(usize, @intCast(nc)) * @sizeOf(varent))));
        if (nv == null) return null;
        t.v = nv;
        t.cap = nc;
    }
    const e = &t.v.?[@intCast(t.n)];
    e.* = .{};
    e.name = strdup(name) orelse return null;
    t.n += 1;
    return e;
}

// ─── Diagnostic writing ─────────────────────────────────────────────────────

/// vsnprintf(errbuf, errcap, ...) semantics: format, then truncate to
/// errcap-1 chars + NUL.  (Every typecheck.c diagnostic is far below the 1 KiB
/// staging buffer, so bufPrint cannot fail for this fixed message set.)
fn setErr(errbuf: ?[*]u8, errcap: usize, comptime fmt: []const u8, args: anytype) void {
    const buf = errbuf orelse return;
    if (errcap == 0) return;
    var tmp: [1024]u8 = undefined;
    const msg = std.fmt.bufPrint(&tmp, fmt, args) catch tmp[0..0];
    const n = @min(msg.len, errcap - 1);
    @memcpy(buf[0..n], msg[0..n]);
    buf[n] = 0;
}

/// snprintf(out, cap, "%s", s) semantics for type_name's fixed strings.
fn copyBounded(out: []u8, s: []const u8) void {
    if (out.len == 0) return;
    const n = @min(s.len, out.len - 1);
    @memcpy(out[0..n], s[0..n]);
    out[n] = 0;
}

/// A flat scalar colspec (tag only; elem/evalues left zero).
fn scalar(tag: dl_coltype) dl_colspec {
    var cs: dl_colspec = std.mem.zeroes(dl_colspec);
    cs.tag = tag;
    return cs;
}

/// A List colspec wrapping a flat element type (v1 elements are flat scalars).
fn listOf(elem: dl_coltype) dl_colspec {
    var cs = scalar(0);
    cs.tag = DLT_LIST;
    cs.elem = elem;
    return cs;
}

/// Full human-readable type name (flat scalars + List<elem>/Optional<elem>/Enum).
fn typeName(cc: ?*const dl_colspec, out: []u8) void {
    const cp = cc orelse {
        copyBounded(out, "?");
        return;
    };
    switch (cp.tag) {
        DLT_NATURAL => copyBounded(out, "Natural"),
        DLT_TEXT => copyBounded(out, "Text"),
        DLT_BOOL => copyBounded(out, "Bool"),
        DLT_CHAR => copyBounded(out, "Char"),
        DLT_DATE => copyBounded(out, "Date"),
        DLT_TIMESTAMP => copyBounded(out, "Timestamp"),
        DLT_SIGNED => copyBounded(out, "Signed"),
        DLT_LIST, DLT_OPTIONAL => {
            // elem is a flat scalar (v1) — its name is short, so a small buffer
            // is enough and the combined "<...>" always fits the caller's cap.
            const ec = scalar(cp.elem);
            var en: [16]u8 = undefined;
            typeName(&ec, &en);
            var tmp: [64]u8 = undefined;
            const s = std.fmt.bufPrint(&tmp, "{s}<{s}>", .{
                if (cp.tag == DLT_LIST) "List" else "Optional",
                std.mem.sliceTo(&en, 0),
            }) catch {
                copyBounded(out, "?");
                return;
            };
            copyBounded(out, s);
        },
        DLT_ENUM => copyBounded(out, "Enum"),
        else => copyBounded(out, "?"),
    }
}

/// Is `t` an ORDERABLE scalar?  Raw u32 order == semantic order for these;
/// Signed is EXCLUDED (zigzag breaks order) and Text/List/Optional/Enum are
/// not orderable.
fn isOrderable(t: dl_colspec) bool {
    return t.tag == DLT_NATURAL or t.tag == DLT_TIMESTAMP or
        t.tag == DLT_DATE or t.tag == DLT_BOOL or
        t.tag == DLT_CHAR;
}

// ─── Constraining ───────────────────────────────────────────────────────────

/// Record that variable `name` must be type `t` at (line,col) of site `site`.
/// Returns 0 on success, -1 on a type conflict (message written to errbuf).
fn constrainVar(t: *vtab, name: [*:0]const u8, want: dl_colspec, line: c_int, col: c_int, site: [*:0]const u8, errbuf: ?[*]u8, errcap: usize) c_int {
    const e = vtabGet(t, name) orelse {
        setErr(errbuf, errcap, "{s}: out of memory in typechecker\n", .{std.mem.span(g_srcname)});
        return -1;
    };
    if (e.type.tag == 0) {
        e.type = want;
        e.line = line;
        e.col = col;
        e.site = site;
        return 0;
    }
    if (dl_colspec_eq(e.type, want) == 0) {
        var wbuf: [64]u8 = undefined;
        var hbuf: [64]u8 = undefined;
        typeName(&want, &wbuf);
        typeName(&e.type, &hbuf);
        setErr(errbuf, errcap, "{s}:{d}:{d}: variable {s} is {s} here ({s}) but {s} at " ++
            "{s}:{d}:{d} ({s})\n", .{
            std.mem.span(g_srcname),
            line,
            col,
            std.mem.span(name),
            std.mem.sliceTo(&wbuf, 0),
            std.mem.span(site),
            std.mem.sliceTo(&hbuf, 0),
            std.mem.span(g_srcname),
            e.line,
            e.col,
            if (e.site) |st| std.mem.span(st) else "",
        });
        return -1;
    }
    return 0;
}

/// Constrain a relational/operand token to type `want` at site `site`:
///   TOK_VAR   -> constrain_var
///   TOK_INT   -> require Natural (an int constant in a Text column is a type
///                error: the value is a raw u32).
///   TOK_IDENT / TOK_STRING -> require Text (a symbol/string constant is not a
///                Natural number).
///   TOK_LIST  -> v1 reject: lists are not yet in the typed universe.
/// Returns 0 on success, -1 on conflict (message written to errbuf).
fn constrainArg(t: *vtab, a: ?*const token, want: dl_colspec, site: [*:0]const u8, errbuf: ?[*]u8, errcap: usize) c_int {
    const ap = a orelse return 0;
    switch (ap.kind) {
        parser_mod.TOK_VAR => return constrainVar(t, ap.text.?, want, ap.line, ap.col, site, errbuf, errcap),
        parser_mod.TOK_INT => {
            if (dl_colspec_eq(want, scalar(DLT_NATURAL)) == 0) {
                var wbuf: [64]u8 = undefined;
                typeName(&want, &wbuf);
                setErr(errbuf, errcap, "{s}:{d}:{d}: int constant {d} in a {s} column ({s})\n", .{
                    std.mem.span(g_srcname),
                    ap.line,
                    ap.col,
                    ap.ival,
                    std.mem.sliceTo(&wbuf, 0),
                    std.mem.span(site),
                });
                return -1;
            }
            return 0;
        },
        parser_mod.TOK_IDENT, parser_mod.TOK_STRING => {
            if (dl_colspec_eq(want, scalar(DLT_TEXT)) == 0) {
                var wbuf: [64]u8 = undefined;
                typeName(&want, &wbuf);
                setErr(errbuf, errcap, "{s}:{d}:{d}: constant '{s}' is Text but a {s} column " ++
                    "({s}) requires Natural\n", .{
                    std.mem.span(g_srcname),
                    ap.line,
                    ap.col,
                    std.mem.span(ap.text.?),
                    std.mem.sliceTo(&wbuf, 0),
                    std.mem.span(site),
                });
                return -1;
            }
            return 0;
        },
        parser_mod.TOK_LIST => {
            if (want.tag != DLT_LIST) {
                var wbuf: [64]u8 = undefined;
                typeName(&want, &wbuf);
                setErr(errbuf, errcap, "{s}:{d}:{d}: list literal in a {s} column ({s})\n", .{
                    std.mem.span(g_srcname),
                    ap.line,
                    ap.col,
                    std.mem.sliceTo(&wbuf, 0),
                    std.mem.span(site),
                });
                return -1;
            }
            return typeListLiteralKnown(t, ap, want.elem, site, errbuf, errcap);
        },
        else => return 0,
    }
}

// ─── Atom typing ────────────────────────────────────────────────────────────

/// Relational atom (a pred that is NOT a builtin): closed-world check against
/// the schema (declared + arity match), then per-arg column typing.  Also used
/// for the rule HEAD.  A reserved builtin name can never be a rule head.
fn typeRelational(t: *vtab, schm: ?*const dl_schema, a: *const atom, is_head: c_int, errbuf: ?[*]u8, errcap: usize) c_int {
    if (is_head != 0 and isReservedBuiltinName(a.pred.?)) {
        setErr(errbuf, errcap, "{s}:{d}:{d}: '{s}' is a reserved builtin predicate name and " ++
            "cannot be used as a rule head\n", .{
            std.mem.span(g_srcname),
            a.line,
            a.col,
            std.mem.span(a.pred.?),
        });
        return -1;
    }

    const rd: ?*const dl_reldef = dl_schema_find(schm, a.pred);
    if (rd == null) {
        setErr(errbuf, errcap, "{s}:{d}:{d}: relation '{s}' is not declared in schema.dhall\n", .{
            std.mem.span(g_srcname),
            a.line,
            a.col,
            std.mem.span(a.pred.?),
        });
        return -1;
    }
    if (a.nargs != @as(c_int, rd.?.arity)) {
        setErr(errbuf, errcap, "{s}:{d}:{d}: relation '{s}' has arity {d} but the rule uses " ++
            "{d} argument(s)\n", .{
            std.mem.span(g_srcname),
            a.line,
            a.col,
            std.mem.span(a.pred.?),
            @as(c_int, rd.?.arity),
            a.nargs,
        });
        return -1;
    }
    var j: c_int = 0;
    while (j < a.nargs) : (j += 1) {
        if (constrainArg(t, a.args.?[@intCast(j)], rd.?.cols[@intCast(j)], a.pred.?, errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}

/// `X = E` arithmetic: result var + every variable in E are Natural.
fn typeArith(t: *vtab, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    const res: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
    // result var
    if (res != null and res.?.kind == parser_mod.TOK_VAR) {
        if (constrainVar(t, res.?.text.?, scalar(DLT_NATURAL), res.?.line, res.?.col, a.pred.?, errbuf, errcap) != 0)
            return -1;
    }
    // every variable inside the expr tree is Natural
    return typeExpr(t, a.arith, a.line, a.col, a.pred.?, errbuf, errcap);
}

/// Inherent type of a constant token (tag 0 = not a constant / unknown).
fn tokenInherentType(a: ?*const token) dl_colspec {
    const ap = a orelse return scalar(0);
    return switch (ap.kind) {
        parser_mod.TOK_INT => scalar(DLT_NATURAL),
        parser_mod.TOK_IDENT, parser_mod.TOK_STRING => scalar(DLT_TEXT),
        else => scalar(0), // TOK_VAR / TOK_LIST / punctuation
    };
}

// ─── TOK_LIST literal typing (finish-dlp Item 1) ────────────────────────────

/// Inherent flat element type of a constant list element, or 0.
fn elemOfConst(a: ?*const token) dl_coltype {
    const ap = a orelse return 0;
    if (ap.kind == parser_mod.TOK_INT) return DLT_NATURAL;
    if (ap.kind == parser_mod.TOK_IDENT or ap.kind == parser_mod.TOK_STRING) return DLT_TEXT;
    return 0;
}

/// Check one list-literal element against flat scalar elem.  var binds elem;
/// constant must int->Natural / ident|string->Text; nested list rejected.
fn constrainListElem(t: *vtab, e: ?*const token, elem: dl_coltype, site: [*:0]const u8, errbuf: ?[*]u8, errcap: usize) c_int {
    const ep = e orelse return 0;
    var wbuf: [32]u8 = undefined;
    if (ep.kind == parser_mod.TOK_VAR)
        return constrainVar(t, ep.text.?, scalar(elem), ep.line, ep.col, site, errbuf, errcap);
    if (ep.kind == parser_mod.TOK_INT) {
        if (elem != DLT_NATURAL) {
            const wc = scalar(elem);
            typeName(&wc, &wbuf);
            setErr(errbuf, errcap, "{s}:{d}:{d}: list literal int element in a List<{s}> ({s})\n", .{
                std.mem.span(g_srcname),
                ep.line,
                ep.col,
                std.mem.sliceTo(&wbuf, 0),
                std.mem.span(site),
            });
            return -1;
        }
        return 0;
    }
    if (ep.kind == parser_mod.TOK_IDENT or ep.kind == parser_mod.TOK_STRING) {
        if (elem != DLT_TEXT) {
            const wc = scalar(elem);
            typeName(&wc, &wbuf);
            setErr(errbuf, errcap, "{s}:{d}:{d}: list literal '{s}' element in a List<{s}> ({s})\n", .{
                std.mem.span(g_srcname),
                ep.line,
                ep.col,
                std.mem.span(ep.text.?),
                std.mem.sliceTo(&wbuf, 0),
                std.mem.span(site),
            });
            return -1;
        }
        return 0;
    }
    if (ep.kind == parser_mod.TOK_LIST) {
        setErr(errbuf, errcap, "{s}:{d}:{d}: nested list literal element is not supported " ++
            "(flat element type only) ({s})\n", .{
            std.mem.span(g_srcname),
            ep.line,
            ep.col,
            std.mem.span(site),
        });
        return -1;
    }
    return 0;
}

/// Type a TOK_LIST against a KNOWN flat element type.
fn typeListLiteralKnown(t: *vtab, lit: ?*const token, elem: dl_coltype, site: [*:0]const u8, errbuf: ?[*]u8, errcap: usize) c_int {
    const lp = lit orelse return 0;
    if (lp.kind != parser_mod.TOK_LIST) return 0;
    var i: c_int = 0;
    while (i < lp.nchildren) : (i += 1) {
        if (constrainListElem(t, lp.children.?[@intCast(i)], elem, site, errbuf, errcap) != 0)
            return -1;
    }
    if (lp.tail != null and lp.tail.?.kind == parser_mod.TOK_VAR)
        if (constrainVar(t, lp.tail.?.text.?, listOf(elem), lp.tail.?.line, lp.tail.?.col, site, errbuf, errcap) != 0)
            return -1;
    return 0;
}

/// Infer element type of an all-constant list literal (list-builtin operand).
fn inferListLiteralElem(lit: ?*const token, elem: *dl_coltype, errbuf: ?[*]u8, errcap: usize) c_int {
    const lp = lit orelse return -1;
    var got: dl_coltype = 0;
    if (lp.kind != parser_mod.TOK_LIST) return -1;
    if (lp.tail != null) {
        setErr(errbuf, errcap, "{s}:{d}:{d}: list pattern is not allowed as a list-builtin operand\n", .{
            std.mem.span(g_srcname),
            lp.line,
            lp.col,
        });
        return -1;
    }
    var i: c_int = 0;
    while (i < lp.nchildren) : (i += 1) {
        const e = lp.children.?[@intCast(i)].?;
        if (e.kind == parser_mod.TOK_VAR) {
            setErr(errbuf, errcap, "{s}:{d}:{d}: list literal with a variable element is not " ++
                "allowed as a list-builtin operand (it is a pattern)\n", .{
                std.mem.span(g_srcname),
                e.line,
                e.col,
            });
            return -1;
        }
        if (e.kind == parser_mod.TOK_LIST) {
            setErr(errbuf, errcap, "{s}:{d}:{d}: nested list literal element is not supported\n", .{
                std.mem.span(g_srcname),
                e.line,
                e.col,
            });
            return -1;
        }
        const ec = elemOfConst(e);
        if (ec == 0) continue;
        if (got == 0) {
            got = ec;
        } else if (got != ec) {
            setErr(errbuf, errcap, "{s}:{d}:{d}: list literal has mixed element types " ++
                "({s} and {s})\n", .{
                std.mem.span(g_srcname),
                lp.line,
                lp.col,
                if (got == DLT_NATURAL) "Natural" else "Text",
                if (ec == DLT_NATURAL) "Natural" else "Text",
            });
            return -1;
        }
    }
    if (got == 0) {
        setErr(errbuf, errcap, "{s}:{d}:{d}: cannot infer list element type from an empty list " ++
            "literal [] (bind it via another operand or a column)\n", .{
            std.mem.span(g_srcname),
            lp.line,
            lp.col,
        });
        return -1;
    }
    elem.* = got;
    return 0;
}

/// List assignment `[X|Xs] = L` (the parser builds an '=' atom with nargs==2,
/// args[0] a TOK_LIST pattern, args[1] the RHS list value).  Resolve the RHS
/// to a List<elem> (reusing resolve_list_operand, defined below type_equality),
/// then bind the pattern per emit_pattern semantics (compiler.c): each head
/// element var (children[i]) := elem, the tail var (tail) := List<elem>.
/// Constant head elements (TOK_INT/TOK_IDENT) have no var to constrain — skip.
/// `[X] = L` (single head child, no tail) still binds X := elem (car pattern).
fn typeListAssignment(t: *vtab, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    const pat: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
    const rhs: ?*const token = if (a.nargs > 1) a.args.?[1] else null;
    var lt: dl_colspec = undefined;

    if (resolveListOperand(t, rhs, a.pred.?, &lt, errbuf, errcap) == 0) return -1;
    const ec = scalar(lt.elem);
    const lr = listOf(lt.elem);

    if (pat) |pt| {
        var i: c_int = 0;
        while (i < pt.nchildren) : (i += 1) {
            const el = pt.children.?[@intCast(i)];
            if (el != null and el.?.kind == parser_mod.TOK_VAR) {
                if (constrainVar(t, el.?.text.?, ec, el.?.line, el.?.col, a.pred.?, errbuf, errcap) != 0)
                    return -1;
            }
        }
        if (pt.tail != null and pt.tail.?.kind == parser_mod.TOK_VAR) {
            if (constrainVar(t, pt.tail.?.text.?, lr, pt.tail.?.line, pt.tail.?.col, a.pred.?, errbuf, errcap) != 0)
                return -1;
        }
    }
    return 0;
}

/// `X = Y` plain equality: both sides must have the same type.  Variables take
/// their already-typed value (or stay untyped); a constant contributes its
/// inherent type (int -> Natural, symbol/string -> Text).
fn typeEquality(t: *vtab, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    const l: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
    const r: ?*const token = if (a.nargs > 1) a.args.?[1] else null;
    var lt = scalar(0);
    var rt = scalar(0);

    // List assignment `[X|Xs] = L` (parser builds an equality atom whose
    // args[0] is a TOK_LIST pattern): type the RHS as a List and bind the
    // pattern vars (head elements := elem, tail := List<elem>).
    if (l != null and l.?.kind == parser_mod.TOK_LIST)
        return typeListAssignment(t, a, errbuf, errcap);

    if (l != null and l.?.kind == parser_mod.TOK_VAR) {
        const le = vtabFind(t, l.?.text.?);
        lt = if (le) |e| e.type else scalar(0);
    } else {
        lt = tokenInherentType(l);
    }
    if (r != null and r.?.kind == parser_mod.TOK_VAR) {
        const re = vtabFind(t, r.?.text.?);
        rt = if (re) |e| e.type else scalar(0);
    } else {
        rt = tokenInherentType(r);
    }

    // Both sides typed and differ: report against the second occurrence.
    if (lt.tag != 0 and rt.tag != 0 and dl_colspec_eq(lt, rt) == 0) {
        if (r != null and r.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, r.?.text.?, lt, r.?.line, r.?.col, a.pred.?, errbuf, errcap);
        if (l != null and l.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, l.?.text.?, rt, l.?.line, l.?.col, a.pred.?, errbuf, errcap);
        // both constants of different types
        setErr(errbuf, errcap, "{s}:{d}:{d}: mismatched types in equality\n", .{
            std.mem.span(g_srcname),
            a.line,
            a.col,
        });
        return -1;
    }
    // One side typed, the other an untyped variable: propagate the type.
    if (lt.tag != 0 and rt.tag == 0) {
        if (r != null and r.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, r.?.text.?, lt, r.?.line, r.?.col, a.pred.?, errbuf, errcap);
        return 0;
    }
    if (rt.tag != 0 and lt.tag == 0) {
        if (l != null and l.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, l.?.text.?, rt, l.?.line, l.?.col, a.pred.?, errbuf, errcap);
        return 0;
    }
    // Neither side has a type yet: register both (untyped) so the rule-level
    // untyped-variable check can report them if no other atom types them.
    if (lt.tag == 0 and rt.tag == 0) {
        if (l != null and l.?.kind == parser_mod.TOK_VAR) _ = vtabGet(t, l.?.text.?);
        if (r != null and r.?.kind == parser_mod.TOK_VAR) _ = vtabGet(t, r.?.text.?);
    }
    return 0;
}

/// Aggregate atom: a->pred is the result VAR name, a->agg_op->text is the op.
///   count      -> result Natural
///   sum(V)     -> V Natural + result Natural
///   min(V)/max(V) -> result = operand's ESTABLISHED type; operand must be
///                     Natural / Timestamp / Date (orderable, NOT Signed).
fn typeAggregate(t: *vtab, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    const op: [*:0]const u8 = if (a.agg_op) |ao| ao.text.? else "";

    // min/max: result takes the operand's established type.
    if (cstrEql(op, "min") or cstrEql(op, "max")) {
        if (a.nargs > 0 and a.args.?[0].?.kind == parser_mod.TOK_VAR) {
            const e = vtabFind(t, a.args.?[0].?.text.?);
            const ot: dl_colspec = if (e) |ee| ee.type else scalar(0);
            if (ot.tag == 0) {
                setErr(errbuf, errcap, "{s}:{d}:{d}: {s} operand is not yet typed " ++
                    "(min/max needs a Natural/Timestamp/Date column)\n", .{
                    std.mem.span(g_srcname),
                    a.args.?[0].?.line,
                    a.args.?[0].?.col,
                    std.mem.span(op),
                });
                return -1;
            }
            if (ot.tag != DLT_NATURAL and ot.tag != DLT_TIMESTAMP and
                ot.tag != DLT_DATE)
            {
                var obuf: [64]u8 = undefined;
                typeName(&ot, &obuf);
                setErr(errbuf, errcap, "{s}:{d}:{d}: {s} over a {s} column is not supported " ++
                    "(min/max needs Natural/Timestamp/Date)\n", .{
                    std.mem.span(g_srcname),
                    a.args.?[0].?.line,
                    a.args.?[0].?.col,
                    std.mem.span(op),
                    std.mem.sliceTo(&obuf, 0),
                });
                return -1;
            }
            return constrainVar(t, a.pred.?, ot, a.line, a.col, op, errbuf, errcap);
        }
        // non-var operand: keep Natural (v1)
        return constrainVar(t, a.pred.?, scalar(DLT_NATURAL), a.line, a.col, op, errbuf, errcap);
    }

    // count / sum: result Natural.
    if (constrainVar(t, a.pred.?, scalar(DLT_NATURAL), a.line, a.col, op, errbuf, errcap) != 0)
        return -1;
    if (cstrEql(op, "count")) {
        return 0;
    }
    // sum: the source var is Natural.
    if (a.nargs > 0 and a.args.?[0].?.kind == parser_mod.TOK_VAR) {
        return constrainVar(t, a.args.?[0].?.text.?, scalar(DLT_NATURAL), a.args.?[0].?.line, a.args.?[0].?.col, op, errbuf, errcap);
    }
    return 0;
}

/// A producing string builtin: concat(Res,A,B) all Text; length(Res,S) Res
/// Natural + S Text; lower/upper(Res,S) Text.  args[0] is the result var.
fn typeStrProducing(t: *vtab, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    const p = a.pred.?;

    if (cstrEql(p, "length")) {
        // result Natural, operand Text
        if (a.nargs > 0 and a.args.?[0].?.kind == parser_mod.TOK_VAR) {
            if (constrainVar(t, a.args.?[0].?.text.?, scalar(DLT_NATURAL), a.args.?[0].?.line, a.args.?[0].?.col, p, errbuf, errcap) != 0)
                return -1;
        }
        if (a.nargs > 1) {
            if (constrainArg(t, a.args.?[1], scalar(DLT_TEXT), p, errbuf, errcap) != 0)
                return -1;
        }
        return 0;
    }
    // concat / lower / upper: all args Text
    var j: c_int = 0;
    while (j < a.nargs) : (j += 1) {
        if (constrainArg(t, a.args.?[@intCast(j)], scalar(DLT_TEXT), p, errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}

/// A filter string builtin prefix/suffix/contains: both args Text.
fn typeStrFilter(t: *vtab, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    var j: c_int = 0;
    while (j < a.nargs) : (j += 1) {
        if (constrainArg(t, a.args.?[@intCast(j)], scalar(DLT_TEXT), a.pred.?, errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}

// ─── v2 list builtins (member/car/cdr/cons/append) ──────────────────────────

/// Resolve the LIST OPERAND of a list builtin to its List colspec.
///
///   - a TOK_LIST literal          -> type it (all-constant form);
///   - a variable typed as a List  -> write its List colspec into *lt, return 1;
///   - an UNTYPED variable         -> 'cannot infer list element type' (v1
///                                    left-to-right boundary — a two-pass /
///                                    unification typechecker removes this);
///   - a variable typed non-List   -> type-conflict diagnostic;
///   - any other token             -> 'requires a List operand'.
/// Returns 1 on success, 0 on failure (message written to errbuf).
fn resolveListOperand(t: *vtab, tok: ?*const token, pred: [*:0]const u8, lt: *dl_colspec, errbuf: ?[*]u8, errcap: usize) c_int {
    const tp = tok orelse {
        setErr(errbuf, errcap, "{s}: '{s}' is missing its list operand\n", .{
            std.mem.span(g_srcname),
            std.mem.span(pred),
        });
        return 0;
    };
    if (tp.kind == parser_mod.TOK_LIST) {
        var elem: dl_coltype = 0;
        if (inferListLiteralElem(tp, &elem, errbuf, errcap) != 0) return 0;
        lt.* = listOf(elem);
        return 1;
    }
    if (tp.kind == parser_mod.TOK_VAR) {
        const e = vtabFind(t, tp.text.?);
        if (e != null and e.?.type.tag == DLT_LIST) {
            lt.* = e.?.type;
            return 1;
        }
        if (e != null and e.?.type.tag == 0) {
            setErr(errbuf, errcap, "{s}:{d}:{d}: cannot infer list element type in '{s}' " ++
                "(list operand '{s}' is untyped)\n", .{
                std.mem.span(g_srcname),
                tp.line,
                tp.col,
                std.mem.span(pred),
                std.mem.span(tp.text.?),
            });
            return 0;
        }
        if (e != null) {
            var wbuf: [64]u8 = undefined;
            typeName(&e.?.type, &wbuf);
            setErr(errbuf, errcap, "{s}:{d}:{d}: '{s}' in '{s}' is {s}, not a List\n", .{
                std.mem.span(g_srcname),
                tp.line,
                tp.col,
                std.mem.span(tp.text.?),
                std.mem.span(pred),
                std.mem.sliceTo(&wbuf, 0),
            });
            return 0;
        }
        setErr(errbuf, errcap, "{s}:{d}:{d}: cannot infer list element type in '{s}' " ++
            "(list operand '{s}' is untyped)\n", .{
            std.mem.span(g_srcname),
            tp.line,
            tp.col,
            std.mem.span(pred),
            std.mem.span(tp.text.?),
        });
        return 0;
    }
    setErr(errbuf, errcap, "{s}:{d}:{d}: '{s}' requires a List operand, got a constant\n", .{
        std.mem.span(g_srcname),
        tp.line,
        tp.col,
        std.mem.span(pred),
    });
    return 0;
}

/// Type one v2 list builtin.  Arg order matches the compiler's
/// list_builtin_valid: car/cdr have 2 args (Result, List); cons/append have 3
/// (Result, Head/A, Tail/B); member is a filter (X, List).  args[0] is always
/// a result/member variable.
///
///   member(X,L):  L typed List<elem> -> X := elem.
///   car(R,L):     L List<elem>       -> R := elem.
///   cdr(R,L):     L List<elem>       -> R := List<elem>.
///   cons(R,H,T):  T List<elem>       -> H := elem, R := List<elem>.
///   append(R,A,B):A List<elem>       -> B := List<elem>, R := List<elem>.
fn typeListBuiltin(t: *vtab, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    const p = a.pred.?;
    var lt: dl_colspec = undefined;

    if (cstrEql(p, "member")) {
        const mx: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
        const ml: ?*const token = if (a.nargs > 1) a.args.?[1] else null;
        if (resolveListOperand(t, ml, p, &lt, errbuf, errcap) == 0) return -1;
        const ec = scalar(lt.elem);
        if (mx != null and mx.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, mx.?.text.?, ec, mx.?.line, mx.?.col, p, errbuf, errcap);
        return 0;
    }

    if (cstrEql(p, "car")) {
        const res: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
        const ll: ?*const token = if (a.nargs > 1) a.args.?[1] else null;
        if (resolveListOperand(t, ll, p, &lt, errbuf, errcap) == 0) return -1;
        const ec = scalar(lt.elem);
        if (res != null and res.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, res.?.text.?, ec, res.?.line, res.?.col, p, errbuf, errcap);
        return 0;
    }

    if (cstrEql(p, "cdr")) {
        const res: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
        const ll: ?*const token = if (a.nargs > 1) a.args.?[1] else null;
        if (resolveListOperand(t, ll, p, &lt, errbuf, errcap) == 0) return -1;
        const lr = listOf(lt.elem);
        if (res != null and res.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, res.?.text.?, lr, res.?.line, res.?.col, p, errbuf, errcap);
        return 0;
    }

    if (cstrEql(p, "cons")) {
        const res: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
        const hh: ?*const token = if (a.nargs > 1) a.args.?[1] else null;
        const tt: ?*const token = if (a.nargs > 2) a.args.?[2] else null;
        if (resolveListOperand(t, tt, p, &lt, errbuf, errcap) == 0) return -1;
        const ec = scalar(lt.elem);
        const lr = listOf(lt.elem);
        if (hh) |h| {
            if (h.kind == parser_mod.TOK_VAR) {
                if (constrainVar(t, h.text.?, ec, h.line, h.col, p, errbuf, errcap) != 0)
                    return -1;
            } else if (h.kind == parser_mod.TOK_LIST) {
                setErr(errbuf, errcap, "{s}:{d}:{d}: cons head must be a flat scalar, got a " ++
                    "list literal\n", .{
                    std.mem.span(g_srcname),
                    h.line,
                    h.col,
                });
                return -1;
            } else if (constrainListElem(t, h, lt.elem, p, errbuf, errcap) != 0) {
                return -1;
            }
        }
        if (res != null and res.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, res.?.text.?, lr, res.?.line, res.?.col, p, errbuf, errcap);
        return 0;
    }

    // append
    {
        const res: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
        const aa: ?*const token = if (a.nargs > 1) a.args.?[1] else null;
        const bb: ?*const token = if (a.nargs > 2) a.args.?[2] else null;
        if (resolveListOperand(t, aa, p, &lt, errbuf, errcap) == 0) return -1;
        const lr = listOf(lt.elem);
        if (bb) |b| {
            if (b.kind == parser_mod.TOK_VAR) {
                if (constrainVar(t, b.text.?, lr, b.line, b.col, p, errbuf, errcap) != 0)
                    return -1;
            } else if (b.kind == parser_mod.TOK_LIST) {
                if (typeListLiteralKnown(t, b, lt.elem, p, errbuf, errcap) != 0)
                    return -1;
            } else {
                setErr(errbuf, errcap, "{s}:{d}:{d}: append's second operand must be a List, " ++
                    "got a constant\n", .{
                    std.mem.span(g_srcname),
                    b.line,
                    b.col,
                });
                return -1;
            }
        }
        if (res != null and res.?.kind == parser_mod.TOK_VAR)
            return constrainVar(t, res.?.text.?, lr, res.?.line, res.?.col, p, errbuf, errcap);
        return 0;
    }
    return 0;
}

/// `range(X, Rel, Lo, Hi)` (compiler.c range_builtin_valid): args[0] is the
/// member variable X := Natural, args[1] is the relation NAME (TOK_IDENT — a
/// name, NOT a value: it is resolved by the compiler against declared
/// relations, so it carries no column type), args[2]/args[3] are the half-open
/// bounds (TOK_VAR|TOK_INT) := Natural.
fn typeRange(t: *vtab, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    const x: ?*const token = if (a.nargs > 0) a.args.?[0] else null;
    const rl: ?*const token = if (a.nargs > 1) a.args.?[1] else null;
    const lo: ?*const token = if (a.nargs > 2) a.args.?[2] else null;
    const hi: ?*const token = if (a.nargs > 3) a.args.?[3] else null;

    if (a.nargs != 4) {
        setErr(errbuf, errcap, "{s}:{d}:{d}: 'range' expects 4 arguments " ++
            "(range(X, Rel, Lo, Hi))\n", .{
            std.mem.span(g_srcname),
            a.line,
            a.col,
        });
        return -1;
    }
    // Rel is a relation NAME (TOK_IDENT), not a value operand.
    if (rl == null or rl.?.kind != parser_mod.TOK_IDENT) {
        setErr(errbuf, errcap, "{s}:{d}:{d}: range relation must be a name (got a value or " ++
            "variable)\n", .{
            std.mem.span(g_srcname),
            if (rl) |r| r.line else a.line,
            if (rl) |r| r.col else a.col,
        });
        return -1;
    }
    if (x != null and x.?.kind == parser_mod.TOK_VAR) {
        if (constrainVar(t, x.?.text.?, scalar(DLT_NATURAL), x.?.line, x.?.col, "range", errbuf, errcap) != 0)
            return -1;
    }
    if (constrainArg(t, lo, scalar(DLT_NATURAL), "range", errbuf, errcap) != 0)
        return -1;
    if (constrainArg(t, hi, scalar(DLT_NATURAL), "range", errbuf, errcap) != 0)
        return -1;
    return 0;
}

fn typeExpr(t: *vtab, e: ?*const expr, line: c_int, col: c_int, site: [*:0]const u8, errbuf: ?[*]u8, errcap: usize) c_int {
    const ep = e orelse return 0;
    if (ep.kind == parser_mod.EX_VAR) {
        return constrainVar(t, ep.@"var".?, scalar(DLT_NATURAL), line, col, site, errbuf, errcap);
    }
    if (ep.kind == parser_mod.EX_BINOP) {
        if (typeExpr(t, ep.l, line, col, site, errbuf, errcap) != 0)
            return -1;
        return typeExpr(t, ep.r, line, col, site, errbuf, errcap);
    }
    return 0;
}

/// Type a single body atom against the schema + var table.
fn typeBodyAtom(t: *vtab, schm: ?*const dl_schema, a: *const atom, errbuf: ?[*]u8, errcap: usize) c_int {
    if (a.aggregate != 0)
        return typeAggregate(t, a, errbuf, errcap);

    // regex `~ 'pat'`: constrain the atom's columns to Text
    if (a.pattern != null) {
        var j: c_int = 0;
        while (j < a.nargs) : (j += 1) {
            if (constrainArg(t, a.args.?[@intCast(j)], scalar(DLT_TEXT), "~", errbuf, errcap) != 0)
                return -1;
        }
        return 0;
    }

    // arithmetic `X = E`
    if (a.arith != null)
        return typeArith(t, a, errbuf, errcap);

    // `!=` is a raw u32 inequality (compiler OP_CMP on materialized u32
    // values): type-AGNOSTIC.  Forcing operands to Natural would falsely
    // reject valid `X != symbol` (test_m9_arith T8d) and `Text != Text`.
    // Register vars so the untyped-variable check can still flag a var
    // that nothing else types, but do NOT constrain them.
    if (cstrEql(a.pred.?, "!=")) {
        var j: c_int = 0;
        while (j < a.nargs) : (j += 1) {
            const arg = a.args.?[@intCast(j)];
            if (arg != null and arg.?.kind == parser_mod.TOK_VAR)
                _ = vtabGet(t, arg.?.text.?);
        }
        return 0;
    }

    // ordering comparison {<,<=,>=,>}: both args must be the SAME orderable
    // scalar (Natural/Timestamp/Date/Bool/Char — raw u32 order == semantic
    // order).  Signed is NOT orderable (zigzag breaks order); Text and the
    // parameterized types are not orderable.  If neither arg has an established
    // orderable type, default to Natural (v1).
    if (isComparisonPred(a.pred.?)) {
        var lc: dl_colspec = if (a.nargs > 0) tokenInherentType(a.args.?[0]) else scalar(DLT_NATURAL);
        var rc: dl_colspec = if (a.nargs > 1) tokenInherentType(a.args.?[1]) else lc;
        if (a.nargs > 0 and a.args.?[0].?.kind == parser_mod.TOK_VAR) {
            if (vtabFind(t, a.args.?[0].?.text.?)) |le| lc = le.type;
        }
        if (a.nargs > 1 and a.args.?[1].?.kind == parser_mod.TOK_VAR) {
            if (vtabFind(t, a.args.?[1].?.text.?)) |re| rc = re.type;
        }
        // pick the first established orderable operand type; else Natural
        const want: dl_colspec = if (isOrderable(lc)) lc else (if (isOrderable(rc)) rc else scalar(DLT_NATURAL));
        var j: c_int = 0;
        while (j < a.nargs) : (j += 1) {
            if (constrainArg(t, a.args.?[@intCast(j)], want, a.pred.?, errbuf, errcap) != 0)
                return -1;
        }
        return 0;
    }

    // `X = Y` plain equality
    if (cstrEql(a.pred.?, "=") and a.nargs == 2 and a.arith == null)
        return typeEquality(t, a, errbuf, errcap);

    // string builtins
    if (isStrProducingPred(a.pred.?))
        return typeStrProducing(t, a, errbuf, errcap);
    if (isStrFilterPred(a.pred.?))
        return typeStrFilter(t, a, errbuf, errcap);

    // v2 list builtins: real typing.
    if (isListBuiltinPred(a.pred.?))
        return typeListBuiltin(t, a, errbuf, errcap);
    // range(X, Rel, Lo, Hi): real typing (X/Lo/Hi Natural, Rel a name).
    if (isRangeBuiltinPred(a.pred.?))
        return typeRange(t, a, errbuf, errcap);

    // otherwise: relational atom (negation types as its positive form)
    return typeRelational(t, schm, a, 0, errbuf, errcap);
}

/// Type one rule: walk head + every body atom.  On the first conflict, write the
/// diagnostic and return -1.  After the walk, every variable must have a type
/// (occurrence-consistency is per-rule).
fn typeRule(schm: ?*const dl_schema, r: *const rule, errbuf: ?[*]u8, errcap: usize) c_int {
    var t: vtab = .{};

    if (r.head) |hd| {
        if (typeRelational(&t, schm, hd, 1, errbuf, errcap) != 0) {
            vtabFree(&t);
            return -1;
        }
    }
    var i: c_int = 0;
    while (i < r.nbody) : (i += 1) {
        if (typeBodyAtom(&t, schm, r.body.?[@intCast(i)].?, errbuf, errcap) != 0) {
            vtabFree(&t);
            return -1;
        }
    }

    // untyped-variable check: every var seen in this rule must have a type.
    // A var that never receives a relational/builtin constraint is an error.
    i = 0;
    while (i < t.n) : (i += 1) {
        if (t.v.?[@intCast(i)].type.tag == 0) {
            setErr(errbuf, errcap, "{s}: untyped variable {s} in rule '{s}' " ++
                "(no column constrains it)\n", .{
                std.mem.span(g_srcname),
                std.mem.span(t.v.?[@intCast(i)].name.?),
                if (r.head) |hd| std.mem.span(hd.pred.?) else "",
            });
            vtabFree(&t);
            return -1;
        }
    }

    vtabFree(&t);
    return 0;
}

export fn dl_typecheck_rules(schm: ?*const dl_schema, rules: ?*anyopaque, n_rules: c_int, srcname: ?[*:0]const u8, errbuf: ?[*]u8, errcap: usize) c_int {
    g_srcname = if (srcname) |sn| sn else "<input>";

    const rr: ?[*]?*rule = @ptrCast(@alignCast(rules));

    if (schm == null or rr == null or n_rules <= 0)
        return 0; // nothing to check

    var i: c_int = 0;
    while (i < n_rules) : (i += 1) {
        const r = rr.?[@intCast(i)] orelse continue;
        if (typeRule(schm, r, errbuf, errcap) != 0)
            return -1;
    }
    return 0;
}

// ─── Tests ──────────────────────────────────────────────────────────────────

/// test_typecheck.c's build_schema(): the worked-example schema.
fn buildTestSchema(s: *dl_schema) !void {
    const c1 = [_]dl_colspec{.{ .tag = DLT_NATURAL }};
    const c2n = [_]dl_colspec{ .{ .tag = DLT_NATURAL }, .{ .tag = DLT_NATURAL } };
    const c2nt = [_]dl_colspec{ .{ .tag = DLT_NATURAL }, .{ .tag = DLT_TEXT } };
    const c1t = [_]dl_colspec{.{ .tag = DLT_TEXT }};
    try std.testing.expectEqual(@as(c_int, 0), dl_schema_add(s, "node", 1, &c1, 0));
    try std.testing.expectEqual(@as(c_int, 0), dl_schema_add(s, "edge", 2, &c2n, 0));
    try std.testing.expectEqual(@as(c_int, 0), dl_schema_add(s, "tc", 2, &c2nt, 0));
    try std.testing.expectEqual(@as(c_int, 0), dl_schema_add(s, "label", 1, &c1t, 0));
}

/// Parse `src`, typecheck against `s`, free, return rc (errbuf is the
/// CALLER's buffer: a slice into this frame's local array would dangle).
fn checkProg(s: *const dl_schema, src: [*:0]const u8, srcname: ?[*:0]const u8, errbuf: *[512]u8) c_int {
    const p = parse_create(src) orelse return -2;
    defer parse_free(p);
    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse return -3;
    defer {
        var i: c_int = 0;
        while (i < n) : (i += 1) rule_free(rules[@intCast(i)]);
        c.free(@ptrCast(rules));
    }
    errbuf[0] = 0;
    return dl_typecheck_rules(s, @ptrCast(rules), n, srcname, errbuf, errbuf.len);
}

test "typecheck: valid worked-example program accepted" {
    var s: dl_schema = std.mem.zeroes(dl_schema);
    try buildTestSchema(&s);

    var errbuf: [512]u8 = undefined;

    // mixed Natural/Text columns, arith, aggregate, list builtins; heads are
    // the schema-declared relations (a closed-world check runs on the head).
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "tc(X, S) :- edge(X, Y), node(X).\n", "pos.datalog", &errbuf));
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "label(S) :- node(X), tc(Y, S).\n", "pos.datalog", &errbuf));
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "tc(A, S) :- edge(X, Y), A = sum(Y), label(S).\n", "pos.datalog", &errbuf));
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "node(A) :- edge(X, Y), A = X + 1.\n", "pos.datalog", &errbuf));
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "node(X) :- edge(X, Y), X < Y, X != 3.\n", "pos.datalog", &errbuf));
    // negation types as its positive form
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "node(X) :- node(X), !label(S), S != a.\n", "pos.datalog", &errbuf));
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "node(X) :- edge(X, Y), member(X, [1,2]).\n", "pos.datalog", &errbuf));
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "label(R) :- label(L), R = concat(L, x).\n", "pos.datalog", &errbuf));
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "node(N) :- edge(X, Y), range(N, edge, 1, 10).\n", "pos.datalog", &errbuf));

    // srcname NULL => `<input>` default (no diagnostic on success either way)
    try std.testing.expectEqual(@as(c_int, 0), checkProg(&s, "node(X) :- edge(X, Y).\n", null, &errbuf));
}

test "typecheck: type conflict reported with both sites" {
    var s: dl_schema = std.mem.zeroes(dl_schema);
    try buildTestSchema(&s);

    var errbuf: [512]u8 = undefined;

    // X is Natural via the head/node(X), then Text via tc's col 1
    var rc = checkProg(&s, "node(X) :- node(X), tc(Y, X).\n", "conf.datalog", &errbuf);
    try std.testing.expectEqual(@as(c_int, -1), rc);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "conf.datalog") != null);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "variable X is Text here (tc)") != null);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "but Natural at conf.datalog:") != null);

    // int constant into a Text column (tc col 1)
    rc = checkProg(&s, "label(X) :- tc(Y, 5).\n", "conf.datalog", &errbuf);
    try std.testing.expectEqual(@as(c_int, -1), rc);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "int constant 5 in a Text column") != null);

    // symbol constant into a Natural column
    rc = checkProg(&s, "node(X) :- node(a).\n", "conf.datalog", &errbuf);
    try std.testing.expectEqual(@as(c_int, -1), rc);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "constant 'a' is Text but a Natural column") != null);

    // equality propagates a conflict: X Natural vs A Text
    rc = checkProg(&s, "node(X) :- node(X), label(A), X = A.\n", "conf.datalog", &errbuf);
    try std.testing.expectEqual(@as(c_int, -1), rc);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "variable A is Natural here (=) but Text") != null);
}

test "typecheck: unbound variable / wrong arity / undeclared relation / reserved head" {
    var s: dl_schema = std.mem.zeroes(dl_schema);
    try buildTestSchema(&s);

    var errbuf: [512]u8 = undefined;

    // untyped variable Z: `!=` only REGISTERS its vars (type-agnostic), so Z
    // never receives a constraint and the rule-level check flags it
    var rc = checkProg(&s, "node(X) :- edge(X, Y), X != Z.\n", null, &errbuf);
    try std.testing.expectEqual(@as(c_int, -1), rc);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "untyped variable Z in rule 'node'") != null);

    // wrong arity
    rc = checkProg(&s, "node(X) :- edge(X).\n", null, &errbuf);
    try std.testing.expectEqual(@as(c_int, -1), rc);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "relation 'edge' has arity 2 but the rule uses 1 argument(s)") != null);

    // undeclared relation
    rc = checkProg(&s, "node(X) :- nope(X).\n", null, &errbuf);
    try std.testing.expectEqual(@as(c_int, -1), rc);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "relation 'nope' is not declared in schema.dhall") != null);

    // reserved builtin name as a rule head
    rc = checkProg(&s, "member(X, L) :- edge(X, L).\n", null, &errbuf);
    try std.testing.expectEqual(@as(c_int, -1), rc);
    try std.testing.expect(std.mem.indexOf(u8, std.mem.sliceTo(&errbuf, 0), "'member' is a reserved builtin predicate name") != null);
}

test "typecheck: degenerate args return 0 (nothing to check)" {
    var s: dl_schema = std.mem.zeroes(dl_schema);
    try buildTestSchema(&s);

    // NULL schema / NULL rules / n_rules <= 0 => 0
    try std.testing.expectEqual(@as(c_int, 0), dl_typecheck_rules(null, null, 0, null, null, 0));
    var errbuf: [64]u8 = undefined;
    try std.testing.expectEqual(@as(c_int, 0), dl_typecheck_rules(&s, null, 3, null, &errbuf, errbuf.len));
    try std.testing.expectEqual(@as(c_int, 0), dl_typecheck_rules(&s, null, -1, null, &errbuf, errbuf.len));
}
