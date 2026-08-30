//! parser.zig — port of src/parser.c (hand-tokenized recursive-descent parser
//! for Datalog v1: facts, rules with conjunction, variables, constants,
//! negation, aggregates, M9 arithmetic `X = E`, `~ 'pattern'` atoms, v2 list
//! literals and [X|Xs] patterns).
//!
//! Strangler-hybrid ABI: the AST node types (token/expr/atom/rule) are
//! CONCRETE C structs — compiler.c/magic.c/dl.c/analyze.c/lsp.c dereference
//! them across the .so boundary — so they are `extern struct`s that MUST stay
//! byte-for-byte identical to src/parser.h (same field names/types/order;
//! C `int` -> c_int, `uint32_t` -> c_uint, self-referential `token**` ->
//! ?[*]?*token).  `struct parser` is only forward-declared in parser.h
//! (opaque), so it is a native Zig struct here.
//!
//! All AST allocation goes through raw libc (calloc/malloc/realloc/strdup/
//! free) exactly like the C oracle, so Zig-produced ASTs are rule_free()/
//! expr_free()-compatible with C callers and vice versa.
//!
//! Oracle: src/parser.c + src/parser.h (never modified).

const std = @import("std");
const c = std.c;

// glibc strdup — not re-exported by std.c; the C oracle uses it for every
// owned AST string.
extern "c" fn strdup(s: [*:0]const u8) ?[*:0]u8;

/// termstore.h: list handles live in [TERM_BASE, ...); raw int literals are
/// capped below it so they can never alias a handle (parser.c includes
/// termstore.h only for this constant).
const TERM_BASE: u64 = 0x80000000;

const MAX_TOKENS = 4096;

// ─── token_kind (C enum; plain c_int in the ABI) ────────────────────────────
pub const token_kind = c_int;
pub const TOK_EOF: token_kind = 0;
pub const TOK_IDENT: token_kind = 1; // lowercase or quoted: constant or predicate name
pub const TOK_VAR: token_kind = 2; // uppercase: variable
pub const TOK_INT: token_kind = 3; // integer literal
pub const TOK_COLONMINUS: token_kind = 4; // :-
pub const TOK_COMMA: token_kind = 5; // ,
pub const TOK_DOT: token_kind = 6; // .
pub const TOK_LPAREN: token_kind = 7; // (
pub const TOK_RPAREN: token_kind = 8; // )
pub const TOK_NOT: token_kind = 9; // !
pub const TOK_AGGREGATE: token_kind = 10; // count, sum, min, max
pub const TOK_EQ: token_kind = 11; // =
pub const TOK_LT: token_kind = 12; // <  (M9 ordering comparison)
pub const TOK_LE: token_kind = 13; // <=
pub const TOK_GT: token_kind = 14; // >
pub const TOK_GE: token_kind = 15; // >=
pub const TOK_NE: token_kind = 16; // !=
pub const TOK_PLUS: token_kind = 17; // +  (M9 arithmetic)
pub const TOK_MINUS: token_kind = 18; // -
pub const TOK_STAR: token_kind = 19; // *
pub const TOK_SLASH: token_kind = 20; // /
pub const TOK_PERCENT: token_kind = 21; // %
pub const TOK_TILDE: token_kind = 22; // ~
pub const TOK_STRING: token_kind = 23; // 'quoted string' — for regex patterns
pub const TOK_LBRACKET: token_kind = 24; // [  (v2 lists)
pub const TOK_RBRACKET: token_kind = 25; // ]  (v2 lists)
pub const TOK_PIPE: token_kind = 26; // |  (v2 lists — [X|Xs] head/tail pattern)
pub const TOK_LIST: token_kind = 27; // [e1,e2,...] literal — one argument token
// whose children are the element tokens

/// expr_kind (C enum).
pub const EX_INT: c_int = 0;
pub const EX_VAR: c_int = 1;
pub const EX_BINOP: c_int = 2;

/// typedef struct token — MUST stay byte-identical to src/parser.h.
pub const token = extern struct {
    kind: token_kind,
    off: c_uint, // byte offset of this token in the source (0-based)
    line: c_int, // 1-based line of off in the source (0 = unset)
    col: c_int, // 1-based column of off in the source (0 = unset)
    text: ?[*:0]u8, // owned; NULL for punctuation tokens
    ival: c_uint, // integer value (TOK_INT only)
    children: ?[*]?*token, // TOK_LIST: owned element tokens (NULL otherwise)
    nchildren: c_int, // TOK_LIST: element count (0 = NIL)
    tail: ?*token, // TOK_LIST: the token after '|' in a [X|Xs] pattern
};

/// typedef struct expr — MUST stay byte-identical to src/parser.h.
pub const expr = extern struct {
    kind: c_int,
    ival: c_uint, // EX_INT: literal value
    @"var": ?[*:0]u8, // EX_VAR: owned variable name
    op: u8, // EX_BINOP: '+', '-', '*', '/', '%'
    l: ?*expr, // EX_BINOP children
    r: ?*expr,
};

/// typedef struct { ... } atom — MUST stay byte-identical to src/parser.h.
pub const atom = extern struct {
    pred: ?[*:0]u8, // predicate name
    off: c_uint, // byte offset of this atom's predicate name (0-based)
    line: c_int, // 1-based line of this atom's predicate name (0 = unset)
    col: c_int, // 1-based column of this atom's predicate name (0 = unset)
    args: ?[*]?*token, // array of argument tokens
    nargs: c_int, // number of arguments
    negated: c_int, // 1 if preceded by !
    aggregate: c_int, // 1 if aggregate (count/sum/min/max)
    agg_op: ?*token, // aggregate operator token, NULL if not agg
    pattern: ?[*:0]u8, // M5: regex pattern string (from ~ '...'), or NULL
    pattern_col: c_int, // M5-symbols: column index for ~ pattern (0-based, default 0)
    arith: ?*expr, // M9: arithmetic expr tree for `X = E` atoms,
    //    NULL for every other atom kind
};

/// typedef struct { ... } rule — MUST stay byte-identical to src/parser.h.
pub const rule = extern struct {
    head: ?*atom,
    off: c_uint, // byte offset of this rule's head (0-based)
    body: ?[*]?*atom, // array of body atom pointers
    nbody: c_int,
    has_negation: c_int, // any body atom negated?
    has_aggregate: c_int, // any body atom an aggregate?
};

/// struct parser — opaque in parser.h (C only holds the pointer), so a native
/// Zig struct here.
const parser = struct {
    src: [*:0]const u8, // pointer into original source (not owned)
    pos: [*:0]const u8, // current position
    tokens: ?[*]?*token, // token buffer (array of token*)
    ntok: c_int, // number of tokens
    cur: c_int, // current token index
    src_owned: ?[*:0]u8, // owned copy of source (for parse_create)
    line_starts: ?[*]c_uint, // heap: byte offset of the start of each line;
    // line i (1-based) starts at line_starts[i-1]
    nlines: c_int, // number of entries in line_starts
    err_off: c_uint, // byte offset of the FIRST recorded error
    has_err: c_int, // 1 once an error has been recorded
    err_msg: [256]u8, // formatted text of the FIRST recorded error
};

fn ptrOff(p: *const parser, q: [*:0]const u8) c_uint {
    return @intCast(@intFromPtr(q) - @intFromPtr(p.src));
}

/// Build p.line_starts: an array of the byte offset of each line's start,
/// indexed by (line - 1).  line 1 starts at 0; every '\n' begins the next line
/// at offset i+1.  On allocation failure nlines stays 0 and position lookup
/// degrades to (0,0) — harmless, purely additive.
fn buildLineStarts(p: *parser) void {
    var cap: c_int = 16;
    var n: c_int = 0;

    p.line_starts = @ptrCast(@alignCast(c.calloc(@intCast(cap), @sizeOf(c_uint)) orelse return));

    p.line_starts.?[@intCast(n)] = 0; // line 1
    n += 1;
    var s = p.src;
    while (s[0] != 0) : (s += 1) {
        if (s[0] == '\n') {
            if (n >= cap) {
                const nc = cap * 2;
                const ns: ?[*]c_uint = @ptrCast(@alignCast(c.realloc(@ptrCast(p.line_starts), @as(usize, @intCast(nc)) * @sizeOf(c_uint))));
                if (ns == null) {
                    c.free(@ptrCast(p.line_starts));
                    p.line_starts = null;
                    n = 0;
                    break;
                }
                p.line_starts = ns;
                cap = nc;
            }
            p.line_starts.?[@intCast(n)] = @intCast(@intFromPtr(s) + 1 - @intFromPtr(p.src));
            n += 1;
        }
    }
    p.nlines = n;
}

/// Map a 0-based byte offset to 1-based line/col via p.line_starts (binary
/// search: greatest line start <= off).  Sets line/col; (0,0) when the table
/// is absent/empty.
fn offToPos(p: *const parser, off: c_uint, line: *c_int, col: *c_int) void {
    if (p.line_starts == null or p.nlines <= 0) {
        line.* = 0;
        col.* = 0;
        return;
    }
    const ls = p.line_starts.?;
    var lo: c_int = 0;
    var hi: c_int = p.nlines - 1;
    var best: c_int = 0;
    while (lo <= hi) {
        const mid = lo + @divTrunc(hi - lo, 2);
        if (ls[@intCast(mid)] <= off) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    line.* = best + 1;
    col.* = @as(c_int, @intCast(off - ls[@intCast(best)])) + 1;
}

/// Record a parse error: write the message to stderr BYTE-IDENTICALLY to the
/// C oracle's vfprintf (stderr is unbuffered => one raw fd-2 write), AND
/// capture the offset + formatted message so parse_last_error() can surface a
/// position.  Only the FIRST error is kept.  (vsnprintf truncation semantics
/// for err_msg[256] are mirrored by the bounded copy; every message used here
/// is far below both bounds.)
fn perr(p: ?*parser, off: c_uint, comptime fmt: []const u8, args: anytype) void {
    var buf: [1024]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch buf[0..0];

    _ = c.write(2, msg.ptr, msg.len);

    const pp = p orelse return;
    if (pp.has_err == 0) {
        pp.has_err = 1;
        pp.err_off = off;
        const n = @min(msg.len, pp.err_msg.len - 1);
        @memcpy(pp.err_msg[0..n], msg[0..n]);
        pp.err_msg[n] = 0;
    }
}

// ─── Character classification ───────────────────────────────────────────────

fn isVarStart(ch: u8) bool {
    return (ch >= 'A' and ch <= 'Z') or ch == '_';
}

fn isVarChar(ch: u8) bool {
    return (ch >= 'A' and ch <= 'Z') or (ch >= 'a' and ch <= 'z') or
        (ch >= '0' and ch <= '9') or ch == '_';
}

fn isPredStart(ch: u8) bool {
    return ch >= 'a' and ch <= 'z';
}

fn isPredChar(ch: u8) bool {
    return (ch >= 'a' and ch <= 'z') or (ch >= 'A' and ch <= 'Z') or
        (ch >= '0' and ch <= '9') or ch == '_';
}

/// strcmp(s, lit) == 0 for NUL-terminated byte strings.
fn cstrEq(s: [*:0]const u8, lit: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(s), lit);
}

// ─── Token creation ─────────────────────────────────────────────────────────

fn tokNew(kind: token_kind, start: ?[*]const u8, len: usize) ?*token {
    const t: *token = @ptrCast(@alignCast(c.calloc(1, @sizeOf(token)) orelse return null));
    t.kind = kind;
    if (start) |st| {
        const text: [*]u8 = @ptrCast(c.malloc(len + 1) orelse {
            c.free(t);
            return null;
        });
        @memcpy(text[0..len], st[0..len]);
        text[len] = 0;
        t.text = @ptrCast(text);
    }
    return t;
}

fn tokDup(t: *const token) ?*token {
    const n: *token = @ptrCast(@alignCast(c.calloc(1, @sizeOf(token)) orelse return null));
    n.kind = t.kind;
    n.off = t.off;
    n.line = t.line;
    n.col = t.col;
    n.ival = t.ival;
    if (t.text) |tx| {
        n.text = strdup(tx) orelse {
            c.free(n);
            return null;
        };
    }
    if (t.nchildren > 0) {
        n.children = @ptrCast(@alignCast(c.calloc(@intCast(t.nchildren), @sizeOf(?*token))));
        if (n.children == null) {
            c.free(@ptrCast(n.text));
            c.free(n);
            return null;
        }
        n.nchildren = t.nchildren;
        var i: c_int = 0;
        while (i < t.nchildren) : (i += 1) {
            n.children.?[@intCast(i)] = tokDup(t.children.?[@intCast(i)].?);
            if (n.children.?[@intCast(i)] == null) {
                tokFree(n);
                return null;
            }
        }
    }
    if (t.tail) |tl| {
        n.tail = tokDup(tl);
        if (n.tail == null) {
            tokFree(n);
            return null;
        }
    }
    return n;
}

fn tokFree(t: ?*token) void {
    const tp = t orelse return;
    if (tp.children) |ch| {
        var i: c_int = 0;
        while (i < tp.nchildren) : (i += 1) tokFree(ch[@intCast(i)]);
        c.free(@ptrCast(ch));
    }
    tokFree(tp.tail);
    if (tp.text) |tx| c.free(@ptrCast(tx));
    c.free(tp);
}

// ─── Lexer ──────────────────────────────────────────────────────────────────

/// Check if identifier is an aggregate keyword
fn isAggregate(s: [*]const u8, len: usize) bool {
    if (len == 3 and (std.mem.eql(u8, s[0..3], "sum") or
        std.mem.eql(u8, s[0..3], "min") or
        std.mem.eql(u8, s[0..3], "max"))) return true;
    if (len == 5 and std.mem.eql(u8, s[0..5], "count")) return true;
    return false;
}

fn lex(p: *parser) c_int {
    if (p.ntok >= MAX_TOKENS) return -1;

    var s = p.pos;

    // Skip whitespace and comments
    while (true) {
        while (s[0] == ' ' or s[0] == '\t' or s[0] == '\n' or s[0] == '\r')
            s += 1;
        // Line comment: # to end of line
        if (s[0] == '#') {
            while (s[0] != 0 and s[0] != '\n') s += 1;
            continue;
        }
        break;
    }

    // Token start offset (0-based byte offset in the source), captured BEFORE
    // the per-kind code below so every token kind — including EOF and
    // punctuation — carries its position.
    const tok_start = s;

    var t: ?*token = null;

    if (s[0] == 0) {
        t = tokNew(TOK_EOF, null, 0);
        if (t == null) return -1;
    } else if (s[0] == '(') {
        t = tokNew(TOK_LPAREN, null, 0);
        s += 1;
    } else if (s[0] == ')') {
        t = tokNew(TOK_RPAREN, null, 0);
        s += 1;
    } else if (s[0] == ',') {
        t = tokNew(TOK_COMMA, null, 0);
        s += 1;
    } else if (s[0] == '.') {
        t = tokNew(TOK_DOT, null, 0);
        s += 1;
    } else if (s[0] == '!') {
        if (s[1] == '=') {
            t = tokNew(TOK_NE, null, 0);
            s += 2;
        } else {
            t = tokNew(TOK_NOT, null, 0);
            s += 1;
        }
    } else if (s[0] == '=') {
        t = tokNew(TOK_EQ, null, 0);
        s += 1;
    } else if (s[0] == '<') {
        if (s[1] == '=') {
            t = tokNew(TOK_LE, null, 0);
            s += 2;
        } else {
            t = tokNew(TOK_LT, null, 0);
            s += 1;
        }
    } else if (s[0] == '>') {
        if (s[1] == '=') {
            t = tokNew(TOK_GE, null, 0);
            s += 2;
        } else {
            t = tokNew(TOK_GT, null, 0);
            s += 1;
        }
    } else if (s[0] == '+') {
        t = tokNew(TOK_PLUS, null, 0);
        s += 1;
    } else if (s[0] == '-') {
        t = tokNew(TOK_MINUS, null, 0);
        s += 1;
    } else if (s[0] == '*') {
        t = tokNew(TOK_STAR, null, 0);
        s += 1;
    } else if (s[0] == '/') {
        t = tokNew(TOK_SLASH, null, 0);
        s += 1;
    } else if (s[0] == '%') {
        t = tokNew(TOK_PERCENT, null, 0);
        s += 1;
    } else if (s[0] == '~') {
        t = tokNew(TOK_TILDE, null, 0);
        s += 1;
    } else if (s[0] == '[') {
        t = tokNew(TOK_LBRACKET, null, 0);
        s += 1;
    } else if (s[0] == ']') {
        t = tokNew(TOK_RBRACKET, null, 0);
        s += 1;
    } else if (s[0] == '|') {
        t = tokNew(TOK_PIPE, null, 0);
        s += 1;
    } else if (s[0] == ':' and s[1] == '-') {
        t = tokNew(TOK_COLONMINUS, null, 0);
        s += 2;
    } else if (s[0] == '\'') {
        // Single-quoted string: read until closing quote
        const start = s + 1;
        s += 1;
        while (s[0] != 0 and s[0] != '\'') s += 1;
        if (s[0] != '\'') {
            perr(p, ptrOff(p, s), "parser: unclosed single-quoted string at position {d}\n", .{@as(c_long, ptrOff(p, s))});
            return -1;
        }
        t = tokNew(TOK_STRING, start, @intFromPtr(s) - @intFromPtr(start));
        s += 1;
    } else if (s[0] == '"') {
        // Quoted string: read until closing quote
        const start = s + 1;
        s += 1; // skip opening quote
        while (s[0] != 0 and s[0] != '"') s += 1;
        if (s[0] != '"') {
            perr(p, ptrOff(p, s), "parser: unclosed string at position {d}\n", .{@as(c_long, ptrOff(p, s))});
            return -1;
        }
        t = tokNew(TOK_IDENT, start, @intFromPtr(s) - @intFromPtr(start));
        s += 1; // skip closing quote
    } else if (s[0] >= '0' and s[0] <= '9') {
        // Integer literal
        const start = s;
        var val: u64 = 0;
        while (s[0] >= '0' and s[0] <= '9') {
            val = val * 10 + (s[0] - '0');
            if (val > 0xFFFFFFFF) {
                perr(p, ptrOff(p, s), "parser: integer overflow at position {d}\n", .{@as(c_long, ptrOff(p, s))});
                return -1;
            }
            s += 1;
        }
        t = tokNew(TOK_INT, start, @intFromPtr(s) - @intFromPtr(start));
        if (t) |ti| ti.ival = @intCast(val);
        // v2-lists boundary: raw int literals are limited to 31 bits so they
        // cannot alias a list handle in [TERM_BASE, ...).
        if (val >= TERM_BASE) {
            perr(p, ptrOff(p, s), "parser: integer literal {d} is out of range (raw ints " ++
                "are limited to 31 bits, < {d}, to keep list handles " ++
                "distinct)\n", .{ val, @as(u32, @intCast(TERM_BASE)) });
            return -1;
        }
    } else if (isVarStart(s[0])) {
        // Variable (uppercase or _)
        const start = s;
        while (isVarChar(s[0])) s += 1;
        t = tokNew(TOK_VAR, start, @intFromPtr(s) - @intFromPtr(start));
    } else if (isPredStart(s[0])) {
        // Identifier (lowercase start)
        const start = s;
        while (isPredChar(s[0])) s += 1;
        if (isAggregate(start, @intFromPtr(s) - @intFromPtr(start))) {
            t = tokNew(TOK_AGGREGATE, start, @intFromPtr(s) - @intFromPtr(start));
        } else {
            t = tokNew(TOK_IDENT, start, @intFromPtr(s) - @intFromPtr(start));
        }
    } else {
        // '{s}' on a 1-byte array writes the raw byte, matching C's %c.
        perr(p, ptrOff(p, s), "parser: unexpected character '{s}' (0x{x:0>2}) at position {d}\n", .{ [_]u8{s[0]}, @as(c_uint, s[0]), @as(c_long, ptrOff(p, s)) });
        return -1;
    }

    const tok = t orelse return -1;
    tok.off = ptrOff(p, tok_start);
    offToPos(p, tok.off, &tok.line, &tok.col);
    p.tokens.?[@intCast(p.ntok)] = tok;
    p.ntok += 1;
    p.pos = s;
    return 0;
}

/// Tokenize the entire source. Returns 0 on success, -1 on error.
fn tokenize(p: *parser) c_int {
    while (true) {
        if (p.ntok > 0 and
            p.tokens.?[@intCast(p.ntok - 1)].?.kind == TOK_EOF)
            break;
        if (lex(p) != 0) return -1;
    }
    return 0;
}

// ─── Token helpers ──────────────────────────────────────────────────────────

fn peek(p: *parser) ?*token {
    if (p.cur < p.ntok)
        return p.tokens.?[@intCast(p.cur)];
    return null;
}

fn peekAt(p: *parser, offset: c_int) ?*token {
    const idx = p.cur + offset;
    if (idx >= 0 and idx < p.ntok)
        return p.tokens.?[@intCast(idx)];
    return null;
}

fn advance(p: *parser) ?*token {
    const t = peek(p) orelse return null;
    if (t.kind != TOK_EOF)
        p.cur += 1;
    return t;
}

fn expect(p: *parser, k: token_kind) ?*token {
    const t = peek(p);
    if (t == null or t.?.kind != k) {
        perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected token kind {d}, got {d}\n", .{
            k,
            if (t) |tp| tp.kind else @as(token_kind, -1),
        });
        return null;
    }
    return advance(p);
}

// ─── AST allocation ─────────────────────────────────────────────────────────

fn atomNew() ?*atom {
    return @ptrCast(@alignCast(c.calloc(1, @sizeOf(atom))));
}

fn atomFree(a: ?*atom) void {
    const ap = a orelse return;
    if (ap.pred) |pd| c.free(@ptrCast(pd));
    if (ap.pattern) |pt| c.free(@ptrCast(pt));
    if (ap.args) |ar| {
        var i: c_int = 0;
        while (i < ap.nargs) : (i += 1) tokFree(ar[@intCast(i)]);
        c.free(@ptrCast(ar));
    }
    // For aggregate atoms agg_op is OWNED (not borrowed from args).
    if (ap.aggregate != 0)
        tokFree(ap.agg_op);
    expr_free(ap.arith);
    c.free(ap);
}

export fn rule_free(r: ?*rule) void {
    const rp = r orelse return;
    atomFree(rp.head);
    if (rp.body) |bd| {
        var i: c_int = 0;
        while (i < rp.nbody) : (i += 1) atomFree(bd[@intCast(i)]);
        c.free(@ptrCast(bd));
    }
    c.free(rp);
}

// ─── Arithmetic expression tree (M9) ────────────────────────────────────────

fn exprNew(k: c_int) ?*expr {
    const e: *expr = @ptrCast(@alignCast(c.calloc(1, @sizeOf(expr)) orelse return null));
    e.kind = k;
    return e;
}

export fn expr_free(e: ?*expr) void {
    const ep = e orelse return;
    if (ep.@"var") |v| c.free(@ptrCast(v));
    expr_free(ep.l);
    expr_free(ep.r);
    c.free(ep);
}

export fn expr_clone(e: ?*const expr) ?*expr {
    const ep = e orelse return null;
    const n = exprNew(ep.kind) orelse return null;
    n.ival = ep.ival;
    n.op = ep.op;
    if (ep.@"var") |v| {
        n.@"var" = strdup(v) orelse {
            expr_free(n);
            return null;
        };
    }
    if (ep.l) |l| {
        n.l = expr_clone(l);
        if (n.l == null) {
            expr_free(n);
            return null;
        }
    }
    if (ep.r) |r| {
        n.r = expr_clone(r);
        if (n.r == null) {
            expr_free(n);
            return null;
        }
    }
    return n;
}

// ─── Recursive-descent parser ───────────────────────────────────────────────

// Grammar:
//   program    := rule*
//   rule       := head COLONMINUS body DOT   (rule)
//              |  atom DOT                   (fact, stored as head-only rule with empty body)
//   head       := atom
//   body       := body_atom (COMMA body_atom)*
//   body_atom  := [NOT] atom
//   atom       := IDENT LPAREN arg_list RPAREN
//   arg_list   := arg (COMMA arg)*
//   arg        := IDENT | VAR | INT | AGGREGATE

/// Parse an argument
fn parseArg(p: *parser) ?*token {
    const t = peek(p) orelse return null;
    if (t.kind == TOK_LBRACKET)
        return parseList(p); // a list literal is ONE argument
    if (t.kind == TOK_IDENT or t.kind == TOK_VAR or
        t.kind == TOK_INT or t.kind == TOK_AGGREGATE)
    {
        return tokDup(advance(p).?);
    }
    perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected argument (ident/var/int/list), got kind {d}\n", .{t.kind});
    return null;
}

/// Parse one list element.  A TOK_VAR element (or a '|' tail, handled in
/// parseList) marks this list as a Phase-2 PATTERN; constants + nested list
/// literals are the Phase-1 constant-list forms.
fn parseListElement(p: *parser) ?*token {
    const t = peek(p) orelse return null;
    if (t.kind == TOK_INT or t.kind == TOK_IDENT or t.kind == TOK_STRING or
        t.kind == TOK_VAR)
        return tokDup(advance(p).?);
    if (t.kind == TOK_LBRACKET)
        return parseList(p);
    if (t.kind == TOK_PIPE) {
        perr(p, if (peek(p)) |tp| tp.off else 0, "parser: unexpected '|' in list — it must follow at least one " ++
            "element ([X|Xs] head/tail pattern)\n", .{});
        return null;
    }
    perr(p, if (peek(p)) |tp| tp.off else 0, "parser: unexpected token kind {d} in list literal\n", .{t.kind});
    return null;
}

/// Parse a list literal [e1, e2, ...] into a single TOK_LIST token.  The
/// current token is '['.  '[]' = empty list (nchildren == 0 = NIL).
fn parseList(p: *parser) ?*token {
    if (expect(p, TOK_LBRACKET) == null) return null;

    const lst: *token = @ptrCast(@alignCast(c.calloc(1, @sizeOf(token)) orelse return null));
    lst.kind = TOK_LIST;

    var elems: ?[*]?*token = null;
    var n: c_int = 0;
    var cap: c_int = 0;
    var fail = false;

    blk: {
        if (peek(p) != null and peek(p).?.kind == TOK_RBRACKET) {
            _ = advance(p); // consume ']' — empty list
            break :blk;
        }

        while (true) {
            const e = parseListElement(p);
            if (e == null) {
                fail = true;
                break :blk;
            }
            if (n >= cap) {
                const nc: c_int = if (cap != 0) cap * 2 else 4;
                const ne: ?[*]?*token = @ptrCast(@alignCast(c.realloc(@ptrCast(elems), @as(usize, @intCast(nc)) * @sizeOf(?*token))));
                if (ne == null) {
                    tokFree(e);
                    fail = true;
                    break :blk;
                }
                elems = ne;
                cap = nc;
            }
            elems.?[@intCast(n)] = e;
            n += 1;

            const sep = peek(p) orelse {
                fail = true;
                break :blk;
            };
            if (sep.kind == TOK_COMMA) {
                _ = advance(p);
                continue;
            }
            if (sep.kind == TOK_RBRACKET) {
                _ = advance(p);
                break :blk;
            }
            if (sep.kind == TOK_PIPE) {
                // [X|Xs] head/tail pattern: exactly ONE tail token, which
                // MUST be a variable (no [a|[b]] sugar).
                _ = advance(p);
                const tl = peek(p);
                if (tl == null or tl.?.kind != TOK_VAR) {
                    perr(p, if (peek(p)) |tp| tp.off else 0, "parser: the tail after '|' in a list pattern " ++
                        "must be a variable (got kind {d})\n", .{if (tl) |tp| tp.kind else @as(token_kind, -1)});
                    fail = true;
                    break :blk;
                }
                _ = advance(p);
                lst.tail = tokDup(tl.?);
                if (lst.tail == null) {
                    fail = true;
                    break :blk;
                }
                if (expect(p, TOK_RBRACKET) == null) {
                    fail = true;
                    break :blk;
                }
                break :blk;
            }
            perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected ',' or ']' in list literal, " ++
                "got kind {d}\n", .{sep.kind});
            fail = true;
            break :blk;
        }
    }

    if (fail) {
        var k: c_int = 0;
        while (k < n) : (k += 1) tokFree(elems.?[@intCast(k)]);
        c.free(@ptrCast(elems));
        tokFree(lst);
        return null;
    }
    lst.children = elems;
    lst.nchildren = n;
    return lst;
}

/// Parse atom: pred ( args )
fn parseAtom(p: *parser) ?*atom {
    const pred = peek(p);
    if (pred == null or pred.?.kind != TOK_IDENT) {
        if (pred) |pd|
            perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected predicate name, got kind {d}\n", .{pd.kind})
        else
            perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected predicate name, got EOF\n", .{});
        return null;
    }
    _ = advance(p);

    const lp = peek(p);
    if (lp == null or lp.?.kind != TOK_LPAREN) {
        perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected '(' after predicate, got kind {d}\n", .{if (lp) |l| l.kind else @as(token_kind, -1)});
        return null;
    }
    _ = advance(p);

    const a = atomNew() orelse return null;
    a.pred = strdup(pred.?.text.?) orelse {
        atomFree(a);
        return null;
    };
    a.off = pred.?.off;
    a.line = pred.?.line;
    a.col = pred.?.col;

    // Parse arguments
    var args: ?[*]?*token = null;
    var nargs: c_int = 0;
    var cap: c_int = 0;

    {
        var rp = peek(p);
        if (rp != null and rp.?.kind == TOK_RPAREN) {
            _ = advance(p);
            // empty arg list
            a.args = null;
            a.nargs = 0;
            return a;
        }

        while (true) {
            const arg = parseArg(p);
            if (arg == null) {
                atomFree(a);
                return null;
            }

            if (nargs >= cap) {
                const newcap: c_int = if (cap != 0) cap * 2 else 4;
                const na: ?[*]?*token = @ptrCast(@alignCast(c.realloc(@ptrCast(args), @as(usize, @intCast(newcap)) * @sizeOf(?*token))));
                if (na == null) {
                    tokFree(arg);
                    atomFree(a);
                    return null;
                }
                args = na;
                cap = newcap;
            }
            args.?[@intCast(nargs)] = arg;
            nargs += 1;

            rp = peek(p);
            if (rp == null) {
                atomFree(a);
                return null;
            }

            if (rp.?.kind == TOK_COMMA) {
                _ = advance(p);
                continue;
            } else if (rp.?.kind == TOK_RPAREN) {
                _ = advance(p);
                break;
            } else {
                perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected ',' or ')', got kind {d}\n", .{rp.?.kind});
                atomFree(a);
                return null;
            }
        }

        a.args = args;
        a.nargs = nargs;
    }

    return a;
}

// ─── Arithmetic expression parser (M9) ──────────────────────────────────────
//
// Grammar (precedence climbing; * / % bind tighter than + -, all left-assoc):
//   E      := term (('+'|'-') term)*
//   term   := factor (('*'|'/'|'%') factor)*
//   factor := TOK_VAR | TOK_INT | '(' E ')'
//
// TOK_IDENT / TOK_STRING never reach here (rejected as a bad factor), which is
// the B6 arithmetic-on-symbol-constant reject (symbols have no numeric value).

fn parseExpr(p: *parser) ?*expr {
    var l = parseTerm(p) orelse return null;
    while (true) {
        const t = peek(p);
        if (t != null and (t.?.kind == TOK_PLUS or t.?.kind == TOK_MINUS)) {
            const op: u8 = if (t.?.kind == TOK_PLUS) '+' else '-';
            _ = advance(p);
            const r = parseTerm(p) orelse {
                expr_free(l);
                return null;
            };
            const n = exprNew(EX_BINOP) orelse {
                expr_free(l);
                expr_free(r);
                return null;
            };
            n.op = op;
            n.l = l;
            n.r = r;
            l = n;
        } else {
            break;
        }
    }
    return l;
}

fn parseTerm(p: *parser) ?*expr {
    var l = parseFactor(p) orelse return null;
    while (true) {
        const t = peek(p);
        if (t != null and (t.?.kind == TOK_STAR or t.?.kind == TOK_SLASH or
            t.?.kind == TOK_PERCENT))
        {
            const op: u8 = if (t.?.kind == TOK_STAR) '*' else if (t.?.kind == TOK_SLASH) '/' else '%';
            _ = advance(p);
            const r = parseFactor(p) orelse {
                expr_free(l);
                return null;
            };
            const n = exprNew(EX_BINOP) orelse {
                expr_free(l);
                expr_free(r);
                return null;
            };
            n.op = op;
            n.l = l;
            n.r = r;
            l = n;
        } else {
            break;
        }
    }
    return l;
}

fn parseFactor(p: *parser) ?*expr {
    const t = peek(p) orelse {
        perr(p, 0, "parser: unexpected end of input in expression\n", .{});
        return null;
    };

    if (t.kind == TOK_VAR or t.kind == TOK_INT) {
        _ = advance(p);
        const e = exprNew(if (t.kind == TOK_VAR) EX_VAR else EX_INT) orelse return null;
        if (t.kind == TOK_VAR) {
            e.@"var" = strdup(t.text.?) orelse {
                expr_free(e);
                return null;
            };
        } else {
            e.ival = t.ival;
        }
        return e;
    }
    if (t.kind == TOK_LPAREN) {
        _ = advance(p);
        const e = parseExpr(p) orelse return null;
        if (expect(p, TOK_RPAREN) == null) {
            expr_free(e);
            return null;
        }
        return e;
    }
    perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected variable, integer, or '(' in arithmetic " ++
        "expression, got kind {d}\n", .{t.kind});
    return null;
}

/// M9-strings: producing builtin names, recognized only in the `VAR = name(...)`
/// form.  Filter builtins (prefix/suffix/contains) need NO parser change — they
/// lex as ordinary lowercase function-call atoms and the compiler classifies
/// them.
fn isStrProducingName(s: [*:0]const u8) bool {
    return cstrEq(s, "concat") or cstrEq(s, "length") or
        cstrEq(s, "lower") or cstrEq(s, "upper");
}

/// M9/v2-lists: LIST-producing builtin names, recognized only in the
/// `VAR = name(...)` form.  cons(H,T)/car(L)/cdr(L)/append(A,B).  (length(L)
/// is a STRING-producing name but its operand parser additionally accepts a
/// list literal — see is_len below.)
fn isListProducingName(s: [*:0]const u8) bool {
    return cstrEq(s, "cons") or cstrEq(s, "car") or
        cstrEq(s, "cdr") or cstrEq(s, "append");
}

/// Parse a body atom: [ ! ] atom, where the atom may also be an equality
/// (VAR = VAR), a comparison (VAR <op> VAR/INT), an aggregate
/// (VAR = count()/sum(X)/min(X)/max(X)), or an arithmetic assignment
/// (VAR = E).
fn parseBodyAtom(p: *parser) ?*atom {
    var negated: c_int = 0;
    var t = peek(p);
    var after_not: c_int = undefined;

    if (t != null and t.?.kind == TOK_NOT) {
        negated = 1;
        _ = advance(p);
    }
    after_not = p.cur;

    // Special forms:
    //   VAR = VAR         equality
    //   VAR = agg(args)   aggregate
    //   VAR = E           arithmetic assignment (E is an expression)
    //   VAR <op> operand  comparison (< <= > >=: VAR/INT; !=: also IDENT)
    //   [pat] = RHS       list assignment (sugar over X=car(L), Xs=cdr(L))
    t = peek(p);
    if (t != null and t.?.kind == TOK_LBRACKET) {
        // list assignment [X|Xs] = L: parse the list pattern, then require
        // '=' and a variable/constant RHS (the list VALUE being destructured).
        // The compiler lowers it to emit_pattern (car/cdr/equality).
        const lst = parseList(p) orelse return null;
        {
            const eq = peek(p);
            if (eq != null and eq.?.kind == TOK_EQ) {
                _ = advance(p); // =
                const rhs = peek(p);
                if (rhs == null or (rhs.?.kind != TOK_VAR and
                    rhs.?.kind != TOK_INT and
                    rhs.?.kind != TOK_IDENT and
                    rhs.?.kind != TOK_STRING and
                    rhs.?.kind != TOK_LBRACKET))
                {
                    perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected variable or constant after '=' in " ++
                        "list assignment\n", .{});
                    tokFree(lst);
                    return null;
                }
                var rv: ?*token = undefined;
                if (rhs.?.kind == TOK_LBRACKET)
                    rv = parseList(p) // [X|_] = [1,2]
                else
                    rv = tokDup(advance(p).?);
                if (rv == null) {
                    tokFree(lst);
                    return null;
                }
                const a = atomNew() orelse {
                    tokFree(lst);
                    tokFree(rv);
                    return null;
                };
                a.pred = strdup("=");
                a.args = @ptrCast(@alignCast(c.malloc(2 * @sizeOf(?*token))));
                if (a.pred == null or a.args == null) {
                    atomFree(a);
                    tokFree(lst);
                    tokFree(rv);
                    return null;
                }
                a.args.?[0] = lst; // the pattern (may contain vars/tail)
                a.args.?[1] = rv;
                a.nargs = 2;
                a.negated = negated;
                return a;
            }
        }
        // a bare list literal is not a valid body atom on its own
        perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected '=' after list pattern (list " ++
            "assignment [X|Xs] = L)\n", .{});
        tokFree(lst);
        return null;
    }
    if (t != null and t.?.kind == TOK_VAR) {
        const op = peekAt(p, 1);
        if (op != null and op.?.kind == TOK_EQ) {
            const rhs = peekAt(p, 2);
            // `X = Y` is equality only if Y is NOT followed by an arithmetic
            // operator — otherwise it is `X = <expr starting with Y>`.
            var is_simple_eq = false;
            if (rhs != null and rhs.?.kind == TOK_VAR) {
                const after = peekAt(p, 3);
                if (!(after != null and (after.?.kind == TOK_PLUS or
                    after.?.kind == TOK_MINUS or
                    after.?.kind == TOK_STAR or
                    after.?.kind == TOK_SLASH or
                    after.?.kind == TOK_PERCENT)))
                    is_simple_eq = true;
            }
            if (is_simple_eq) {
                // equality: VAR = VAR
                const lv = advance(p).?;
                _ = advance(p); // =
                const rv = advance(p).?;
                const a = atomNew() orelse return null;
                a.pred = strdup("=");
                a.args = @ptrCast(@alignCast(c.malloc(2 * @sizeOf(?*token))));
                if (a.pred == null or a.args == null) {
                    atomFree(a);
                    return null;
                }
                a.args.?[0] = tokDup(lv);
                a.args.?[1] = tokDup(rv);
                a.nargs = 2;
                a.negated = negated;
                return a;
            } else if (rhs != null and rhs.?.kind == TOK_AGGREGATE) {
                // aggregate: VAR = agg ( [VAR] )
                const res = advance(p).?; // result var
                _ = advance(p); // =
                const agop = advance(p).?; // aggregate op token
                if (expect(p, TOK_LPAREN) == null) return null;

                const a = atomNew() orelse return null;
                a.aggregate = 1;
                a.pred = strdup(res.text.?); // result var name
                a.agg_op = tokDup(agop);
                a.negated = negated;
                if (a.pred == null or a.agg_op == null) {
                    atomFree(a);
                    return null;
                }

                if (cstrEq(agop.text.?, "count")) {
                    // count() requires empty parens
                    const rp = peek(p);
                    if (rp == null or rp.?.kind != TOK_RPAREN) {
                        perr(p, if (peek(p)) |tp| tp.off else 0, "parser: 'count' requires no arguments near '{s}'\n", .{std.mem.span(agop.text.?)});
                        atomFree(a);
                        return null;
                    }
                    _ = advance(p);
                    a.nargs = 0;
                    a.args = null;
                } else {
                    // sum/min/max require exactly one variable argument
                    const src = peek(p);
                    if (src == null or src.?.kind != TOK_VAR) {
                        perr(p, if (peek(p)) |tp| tp.off else 0, "parser: aggregate '{s}' requires one variable argument near '{s}'\n", .{ std.mem.span(agop.text.?), std.mem.span(agop.text.?) });
                        atomFree(a);
                        return null;
                    }
                    _ = advance(p);
                    if (expect(p, TOK_RPAREN) == null) {
                        atomFree(a);
                        return null;
                    }
                    a.args = @ptrCast(@alignCast(c.malloc(@sizeOf(?*token))));
                    if (a.args == null) {
                        atomFree(a);
                        return null;
                    }
                    a.args.?[0] = tokDup(src.?);
                    a.nargs = 1;
                }
                return a;
            } else if (rhs != null and rhs.?.kind == TOK_IDENT and
                (isStrProducingName(rhs.?.text.?) or
                    isListProducingName(rhs.?.text.?)) and
                peekAt(p, 3) != null and peekAt(p, 3).?.kind == TOK_LPAREN)
            {
                // Producing builtin: string (concat/length/lower/upper) or
                // list (cons/car/cdr/append).  See the operand checks below.
                const res = advance(p).?; // result var
                _ = advance(p); // =
                const nm = advance(p).?; // builtin name
                const is_list = isListProducingName(nm.text.?);
                const is_len = cstrEq(nm.text.?, "length");
                if (expect(p, TOK_LPAREN) == null) return null;

                const a = atomNew() orelse return null;
                a.pred = strdup(nm.text.?);
                a.off = nm.off;
                a.line = nm.line;
                a.col = nm.col;
                a.negated = negated;
                if (a.pred == null) {
                    atomFree(a);
                    return null;
                }

                var args: ?[*]?*token = @ptrCast(@alignCast(c.realloc(null, 4 * @sizeOf(?*token))));
                var nargs: c_int = 0;
                var cap: c_int = 4;
                var fail = false;
                if (args == null) {
                    atomFree(a);
                    return null;
                }
                args.?[0] = tokDup(res);
                nargs += 1;
                if (args.?[0] == null) fail = true;

                while (!fail) {
                    const opn = peek(p) orelse {
                        fail = true;
                        break;
                    };
                    if (opn.kind == TOK_RPAREN) {
                        _ = advance(p);
                        break;
                    }
                    if ((is_list or is_len) and opn.kind == TOK_LBRACKET) {
                        // nested list-literal operand (list builtins + length)
                        if (nargs >= cap) {
                            const nc = cap * 2;
                            const na: ?[*]?*token = @ptrCast(@alignCast(c.realloc(@ptrCast(args), @as(usize, @intCast(nc)) * @sizeOf(?*token))));
                            if (na == null) {
                                fail = true;
                                break;
                            }
                            args = na;
                            cap = nc;
                        }
                        args.?[@intCast(nargs)] = parseList(p);
                        nargs += 1;
                        if (args.?[@intCast(nargs - 1)] == null) {
                            fail = true;
                            break;
                        }
                    } else {
                        const ok = if (is_list)
                            (opn.kind == TOK_VAR or opn.kind == TOK_INT or
                                opn.kind == TOK_IDENT or opn.kind == TOK_STRING)
                        else
                            (opn.kind == TOK_VAR or opn.kind == TOK_IDENT);
                        if (!ok) {
                            if (is_list)
                                perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected variable or constant " ++
                                    "(int/string/list) operand in list builtin " ++
                                    "'{s}', got kind {d}\n", .{ std.mem.span(nm.text.?), opn.kind })
                            else
                                perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected variable or string " ++
                                    "constant operand in string builtin '{s}', " ++
                                    "got kind {d}\n", .{ std.mem.span(nm.text.?), opn.kind });
                            fail = true;
                            break;
                        }
                        if (nargs >= cap) {
                            const nc = cap * 2;
                            const na: ?[*]?*token = @ptrCast(@alignCast(c.realloc(@ptrCast(args), @as(usize, @intCast(nc)) * @sizeOf(?*token))));
                            if (na == null) {
                                fail = true;
                                break;
                            }
                            args = na;
                            cap = nc;
                        }
                        args.?[@intCast(nargs)] = tokDup(advance(p).?);
                        nargs += 1;
                        if (args.?[@intCast(nargs - 1)] == null) {
                            fail = true;
                            break;
                        }
                    }

                    const sep = peek(p) orelse {
                        fail = true;
                        break;
                    };
                    if (sep.kind == TOK_COMMA) {
                        _ = advance(p);
                        continue;
                    }
                    if (sep.kind == TOK_RPAREN) {
                        _ = advance(p);
                        break;
                    }
                    perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected ',' or ')' in string " ++
                        "builtin '{s}', got kind {d}\n", .{ std.mem.span(nm.text.?), sep.kind });
                    fail = true;
                    break;
                }

                if (fail) {
                    var k: c_int = 0;
                    while (k < nargs) : (k += 1) tokFree(args.?[@intCast(k)]);
                    c.free(@ptrCast(args));
                    atomFree(a);
                    return null;
                }
                a.args = args;
                a.nargs = nargs;
                return a;
            } else if (rhs != null) {
                // arithmetic assignment: VAR = E
                const res = advance(p).?; // result var
                _ = advance(p); // =
                const e = parseExpr(p) orelse return null;
                const a = atomNew() orelse {
                    expr_free(e);
                    return null;
                };
                a.pred = strdup("=");
                a.args = @ptrCast(@alignCast(c.malloc(@sizeOf(?*token))));
                if (a.pred == null or a.args == null) {
                    expr_free(e);
                    atomFree(a);
                    return null;
                }
                a.args.?[0] = tokDup(res);
                if (a.args.?[0] == null) {
                    expr_free(e);
                    atomFree(a);
                    return null;
                }
                a.nargs = 1;
                a.arith = e;
                a.negated = negated;
                return a;
            }
            // rhs == NULL (EOF after '=') -> fall through to error path
        } else if (op != null and (op.?.kind == TOK_LT or op.?.kind == TOK_LE or
            op.?.kind == TOK_GT or op.?.kind == TOK_GE or
            op.?.kind == TOK_NE))
        {
            // comparison: VAR <op> operand
            const rhs = peekAt(p, 2);
            if (rhs != null) {
                const optext: [*:0]const u8 = switch (op.?.kind) {
                    TOK_LT => "<",
                    TOK_LE => "<=",
                    TOK_GT => ">",
                    TOK_GE => ">=",
                    else => "!=",
                };
                if (rhs.?.kind != TOK_VAR and rhs.?.kind != TOK_INT and
                    !(op.?.kind == TOK_NE and rhs.?.kind == TOK_IDENT))
                {
                    if (rhs.?.kind == TOK_IDENT)
                        perr(p, if (peek(p)) |tp| tp.off else 0, "parser: symbol constant not allowed in ordering " ++
                            "comparison '{s}' (only in =/!=)\n", .{std.mem.span(optext)})
                    else
                        perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected variable or integer operand for " ++
                            "comparison '{s}', got kind {d}\n", .{ std.mem.span(optext), rhs.?.kind });
                    return null;
                }
                const lv = advance(p).?;
                _ = advance(p); // operator
                const rv = advance(p).?;
                const a = atomNew() orelse return null;
                a.pred = strdup(optext);
                a.args = @ptrCast(@alignCast(c.malloc(2 * @sizeOf(?*token))));
                if (a.pred == null or a.args == null) {
                    atomFree(a);
                    return null;
                }
                a.args.?[0] = tokDup(lv);
                a.args.?[1] = tokDup(rv);
                a.nargs = 2;
                a.negated = negated;
                return a;
            }
            // rhs == NULL -> fall through to error path
        }
    }

    // fallback: normal atom (possibly negated)
    p.cur = after_not;
    {
        const a = parseAtom(p) orelse return null;
        a.negated = negated;

        // M5: check for ~ [k] 'pattern' suffix (k is optional 0-based col)
        {
            var tt = peek(p);
            if (tt != null and tt.?.kind == TOK_TILDE) {
                _ = advance(p);
                // Parse optional column index: ~ [k] or ~ k
                tt = peek(p);
                if (tt != null and tt.?.kind == TOK_INT) {
                    a.pattern_col = @intCast(tt.?.ival);
                    _ = advance(p);
                    tt = peek(p);
                } else {
                    a.pattern_col = 0; // default to column 0
                }
                if (tt == null or tt.?.kind != TOK_STRING) {
                    perr(p, if (peek(p)) |tp| tp.off else 0, "parser: expected pattern string " ++
                        "after '~'\n", .{});
                    atomFree(a);
                    return null;
                }
                a.pattern = strdup(tt.?.text.?) orelse {
                    atomFree(a);
                    return null;
                };
                _ = advance(p);
            }
        }

        return a;
    }
}

/// Parse body: body_atom (, body_atom)*
fn parseBody(p: *parser, r: *rule) c_int {
    var body: ?[*]?*atom = null;
    var nbody: c_int = 0;
    var cap: c_int = 0;

    while (true) {
        const ba = parseBodyAtom(p) orelse return -1;

        if (ba.negated != 0) r.has_negation = 1;
        if (ba.aggregate != 0) r.has_aggregate = 1;

        if (nbody >= cap) {
            const newcap: c_int = if (cap != 0) cap * 2 else 4;
            const na: ?[*]?*atom = @ptrCast(@alignCast(c.realloc(@ptrCast(body), @as(usize, @intCast(newcap)) * @sizeOf(?*atom))));
            if (na == null) {
                atomFree(ba);
                return -1;
            }
            body = na;
            cap = newcap;
        }
        body.?[@intCast(nbody)] = ba;
        nbody += 1;

        const t = peek(p);
        if (t != null and t.?.kind == TOK_COMMA) {
            _ = advance(p);
            continue;
        }
        break;
    }

    r.body = body;
    r.nbody = nbody;
    return 0;
}

/// Parse one rule or fact
fn parseOneRule(p: *parser) ?*rule {
    // Check for EOF
    const t0 = peek(p);
    if (t0 == null or t0.?.kind == TOK_EOF)
        return null;

    const r: *rule = @ptrCast(@alignCast(c.calloc(1, @sizeOf(rule)) orelse return null));

    // Atom first
    r.head = parseAtom(p);
    if (r.head == null) {
        rule_free(r);
        return null;
    }
    r.off = r.head.?.off;

    const t = peek(p) orelse {
        rule_free(r);
        return null;
    };

    if (t.kind == TOK_COLONMINUS) {
        // Rule: head :- body
        _ = advance(p);
        if (parseBody(p, r) != 0) {
            rule_free(r);
            return null;
        }
    }

    // Expect '.'
    if (expect(p, TOK_DOT) == null) {
        rule_free(r);
        return null;
    }

    return r;
}

// ─── Public API ─────────────────────────────────────────────────────────────

export fn parse_create(source: ?[*:0]const u8) ?*parser {
    const src = source orelse return null;

    const p: *parser = @ptrCast(@alignCast(c.calloc(1, @sizeOf(parser)) orelse return null));

    p.src_owned = strdup(src) orelse {
        c.free(p);
        return null;
    };
    p.src = p.src_owned.?;
    p.pos = p.src;
    buildLineStarts(p);

    p.tokens = @ptrCast(@alignCast(c.calloc(MAX_TOKENS, @sizeOf(?*token))));
    if (p.tokens == null) {
        c.free(@ptrCast(p.src_owned));
        c.free(p);
        return null;
    }

    if (tokenize(p) != 0) {
        parse_free(p);
        return null;
    }

    return p;
}

/// LSP-only variant: identical to parse_create() EXCEPT that a lexer error does
/// NOT free the parser — it is returned so the caller can read the error
/// position via parse_last_error() before freeing it.  The CLI/playground keep
/// using parse_create(), whose observable behaviour is unchanged.
export fn parse_create_reporting(source: ?[*:0]const u8) ?*parser {
    const src = source orelse return null;

    const p: *parser = @ptrCast(@alignCast(c.calloc(1, @sizeOf(parser)) orelse return null));

    p.src_owned = strdup(src) orelse {
        c.free(p);
        return null;
    };
    p.src = p.src_owned.?;
    p.pos = p.src;
    buildLineStarts(p);

    p.tokens = @ptrCast(@alignCast(c.calloc(MAX_TOKENS, @sizeOf(?*token))));
    if (p.tokens == null) {
        c.free(@ptrCast(p.src_owned));
        c.free(p);
        return null;
    }

    if (tokenize(p) != 0)
        return p; // keep the parser: the caller reads parse_last_error()

    return p;
}

export fn parse_last_error(p: ?*const parser, off: ?*c_uint) ?[*:0]const u8 {
    if (off) |o| o.* = 0;
    const pp = p orelse return null;
    if (pp.has_err == 0) return null;
    if (off) |o| o.* = pp.err_off;
    return @ptrCast(&pp.err_msg);
}

export fn parse_rules(p: ?*parser, n_rules: ?*c_int) ?[*]?*rule {
    var rules: ?[*]?*rule = null;
    var nr: c_int = 0;
    var cap: c_int = 0;

    const pp = p orelse return null;
    const nrp = n_rules orelse return null;
    nrp.* = 0;

    while (true) {
        const r = parseOneRule(pp) orelse break;

        if (nr >= cap) {
            const newcap: c_int = if (cap != 0) cap * 2 else 4;
            const na: ?[*]?*rule = @ptrCast(@alignCast(c.realloc(@ptrCast(rules), @as(usize, @intCast(newcap)) * @sizeOf(?*rule))));
            if (na == null) {
                rule_free(r);
                // error: free everything parsed so far
                var i: c_int = 0;
                while (i < nr) : (i += 1) rule_free(rules.?[@intCast(i)]);
                c.free(@ptrCast(rules));
                return null;
            }
            rules = na;
            cap = newcap;
        }
        rules.?[@intCast(nr)] = r;
        nr += 1;
    }

    // Check for parse errors (non-EOF stop)
    {
        const t = peek(pp);
        if (t != null and t.?.kind != TOK_EOF) {
            perr(pp, if (peek(pp)) |tp| tp.off else 0, "parse: trailing tokens\n", .{});
            var i: c_int = 0;
            while (i < nr) : (i += 1) rule_free(rules.?[@intCast(i)]);
            c.free(@ptrCast(rules));
            return null;
        }
    }

    if (nr == 0) {
        perr(pp, if (peek(pp)) |tp| tp.off else 0, "parse: no rules found\n", .{});
        c.free(@ptrCast(rules));
        return null;
    }

    nrp.* = nr;
    return rules;
}

export fn parse_free(p: ?*parser) void {
    const pp = p orelse return;
    var i: c_int = 0;
    while (i < pp.ntok) : (i += 1) tokFree(pp.tokens.?[@intCast(i)]);
    c.free(@ptrCast(pp.tokens));
    c.free(@ptrCast(pp.line_starts));
    if (pp.src_owned) |so| c.free(@ptrCast(so));
    c.free(pp);
}

// ─── Tests ──────────────────────────────────────────────────────────────────

test "AST struct layouts match src/parser.h (LP64)" {
    // Cross-checked against gcc -Isrc offsetof/sizeof on the C oracle header
    // (see the differential driver's layout dump).
    try std.testing.expectEqual(@as(usize, 56), @sizeOf(token));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(token, "kind"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(token, "off"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(token, "line"));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(token, "col"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(token, "text"));
    try std.testing.expectEqual(@as(usize, 24), @offsetOf(token, "ival"));
    try std.testing.expectEqual(@as(usize, 32), @offsetOf(token, "children"));
    try std.testing.expectEqual(@as(usize, 40), @offsetOf(token, "nchildren"));
    try std.testing.expectEqual(@as(usize, 48), @offsetOf(token, "tail"));

    try std.testing.expectEqual(@as(usize, 40), @sizeOf(expr));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(expr, "kind"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(expr, "ival"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(expr, "var"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(expr, "op"));
    try std.testing.expectEqual(@as(usize, 24), @offsetOf(expr, "l"));
    try std.testing.expectEqual(@as(usize, 32), @offsetOf(expr, "r"));

    try std.testing.expectEqual(@as(usize, 80), @sizeOf(atom));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(atom, "pred"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(atom, "off"));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(atom, "line"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(atom, "col"));
    try std.testing.expectEqual(@as(usize, 24), @offsetOf(atom, "args"));
    try std.testing.expectEqual(@as(usize, 32), @offsetOf(atom, "nargs"));
    try std.testing.expectEqual(@as(usize, 36), @offsetOf(atom, "negated"));
    try std.testing.expectEqual(@as(usize, 40), @offsetOf(atom, "aggregate"));
    try std.testing.expectEqual(@as(usize, 48), @offsetOf(atom, "agg_op"));
    try std.testing.expectEqual(@as(usize, 56), @offsetOf(atom, "pattern"));
    try std.testing.expectEqual(@as(usize, 64), @offsetOf(atom, "pattern_col"));
    try std.testing.expectEqual(@as(usize, 72), @offsetOf(atom, "arith"));

    try std.testing.expectEqual(@as(usize, 40), @sizeOf(rule));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(rule, "head"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(rule, "off"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(rule, "body"));
    try std.testing.expectEqual(@as(usize, 24), @offsetOf(rule, "nbody"));
    try std.testing.expectEqual(@as(usize, 28), @offsetOf(rule, "has_negation"));
    try std.testing.expectEqual(@as(usize, 32), @offsetOf(rule, "has_aggregate"));
}

test "token stream: kinds, offsets, line/col" {
    const p = parse_create("edge(a, 1).\nq(X) :- r(X).\n") orelse return error.NoParser;
    defer parse_free(p);

    // edge ( a , 1 ) . q ( X ) :- r ( X ) . EOF
    try std.testing.expectEqual(@as(c_int, 18), p.ntok);
    const tk = p.tokens.?;

    try std.testing.expectEqual(TOK_IDENT, tk[0].?.kind);
    try std.testing.expectEqualStrings("edge", std.mem.span(tk[0].?.text.?));
    try std.testing.expectEqual(@as(c_uint, 0), tk[0].?.off);
    try std.testing.expectEqual(@as(c_int, 1), tk[0].?.line);
    try std.testing.expectEqual(@as(c_int, 1), tk[0].?.col);

    try std.testing.expectEqual(TOK_LPAREN, tk[1].?.kind);
    try std.testing.expectEqual(@as(c_uint, 4), tk[1].?.off);
    try std.testing.expectEqual(@as(c_int, 5), tk[1].?.col);

    try std.testing.expectEqual(TOK_IDENT, tk[2].?.kind);
    try std.testing.expectEqual(@as(c_uint, 5), tk[2].?.off);
    try std.testing.expectEqual(@as(c_int, 6), tk[2].?.col);

    try std.testing.expectEqual(TOK_COMMA, tk[3].?.kind);
    try std.testing.expectEqual(@as(c_uint, 6), tk[3].?.off);

    try std.testing.expectEqual(TOK_INT, tk[4].?.kind);
    try std.testing.expectEqual(@as(c_uint, 8), tk[4].?.off);
    try std.testing.expectEqual(@as(c_int, 9), tk[4].?.col);
    try std.testing.expectEqual(@as(c_uint, 1), tk[4].?.ival);

    try std.testing.expectEqual(TOK_DOT, tk[6].?.kind);
    try std.testing.expectEqual(@as(c_uint, 10), tk[6].?.off);

    // line 2 (starts at off 12): q 12/col1, ":-" 17/col6, r 20/col9,
    // X (in r(X)) 22/col11
    try std.testing.expectEqual(TOK_IDENT, tk[7].?.kind);
    try std.testing.expectEqual(@as(c_uint, 12), tk[7].?.off);
    try std.testing.expectEqual(@as(c_int, 2), tk[7].?.line);
    try std.testing.expectEqual(@as(c_int, 1), tk[7].?.col);
    try std.testing.expectEqual(TOK_COLONMINUS, tk[11].?.kind);
    try std.testing.expectEqual(@as(c_uint, 17), tk[11].?.off);
    try std.testing.expectEqual(@as(c_int, 6), tk[11].?.col);
    try std.testing.expectEqual(TOK_VAR, tk[9].?.kind);
    try std.testing.expectEqualStrings("X", std.mem.span(tk[9].?.text.?));
    try std.testing.expectEqual(@as(c_int, 11), tk[14].?.col);

    // EOF: off = src len (26), line 3, col 1
    try std.testing.expectEqual(TOK_EOF, tk[17].?.kind);
    try std.testing.expectEqual(@as(c_uint, 26), tk[17].?.off);
    try std.testing.expectEqual(@as(c_int, 3), tk[17].?.line);
    try std.testing.expectEqual(@as(c_int, 1), tk[17].?.col);
}

test "line/col after comments and continuation lines" {
    const p = parse_create("# comment\nedge(a,\n     1).\n") orelse return error.NoParser;
    defer parse_free(p);

    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse return error.NoRules;
    defer {
        var i: c_int = 0;
        while (i < n) : (i += 1) rule_free(rules[@intCast(i)]);
        c.free(@ptrCast(rules));
    }

    try std.testing.expectEqual(@as(c_int, 1), n);
    const head = rules[0].?.head.?;
    try std.testing.expectEqualStrings("edge", std.mem.span(head.pred.?));
    try std.testing.expectEqual(@as(c_uint, 10), head.off);
    try std.testing.expectEqual(@as(c_int, 2), head.line);
    try std.testing.expectEqual(@as(c_int, 1), head.col);
    try std.testing.expectEqual(@as(c_int, 2), head.nargs);
    // the int literal '1' sits on line 3, col 6 (0-based off 23)
    const arg1 = head.args.?[1].?;
    try std.testing.expectEqual(TOK_INT, arg1.kind);
    try std.testing.expectEqual(@as(c_uint, 23), arg1.off);
    try std.testing.expectEqual(@as(c_int, 3), arg1.line);
    try std.testing.expectEqual(@as(c_int, 6), arg1.col);
}

test "fact AST shape" {
    const p = parse_create("edge(a, 1).") orelse return error.NoParser;
    defer parse_free(p);

    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse return error.NoRules;
    defer {
        var i: c_int = 0;
        while (i < n) : (i += 1) rule_free(rules[@intCast(i)]);
        c.free(@ptrCast(rules));
    }

    try std.testing.expectEqual(@as(c_int, 1), n);
    const r = rules[0].?;
    try std.testing.expectEqualStrings("edge", std.mem.span(r.head.?.pred.?));
    try std.testing.expectEqual(@as(c_int, 2), r.head.?.nargs);
    try std.testing.expectEqual(@as(c_int, 0), r.nbody);
    try std.testing.expectEqual(@as(c_int, 0), r.has_negation);
    try std.testing.expectEqual(@as(c_int, 0), r.has_aggregate);
    const args = r.head.?.args.?;
    try std.testing.expectEqual(TOK_IDENT, args[0].?.kind);
    try std.testing.expectEqualStrings("a", std.mem.span(args[0].?.text.?));
    try std.testing.expectEqual(TOK_INT, args[1].?.kind);
    try std.testing.expectEqual(@as(c_uint, 1), args[1].?.ival);
}

test "rule with negation, aggregate and arithmetic expr tree" {
    const src = "s(X, S) :- p(X), !q(X), S = sum(Y), y(Y), T = X + 1 * 2.\n";
    const p = parse_create(src) orelse return error.NoParser;
    defer parse_free(p);

    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse return error.NoRules;
    defer {
        var i: c_int = 0;
        while (i < n) : (i += 1) rule_free(rules[@intCast(i)]);
        c.free(@ptrCast(rules));
    }

    try std.testing.expectEqual(@as(c_int, 1), n);
    const r = rules[0].?;
    try std.testing.expectEqual(@as(c_int, 1), r.has_negation);
    try std.testing.expectEqual(@as(c_int, 1), r.has_aggregate);
    try std.testing.expectEqual(@as(c_int, 5), r.nbody);

    const body = r.body.?;
    // !q(X)
    try std.testing.expectEqualStrings("q", std.mem.span(body[1].?.pred.?));
    try std.testing.expectEqual(@as(c_int, 1), body[1].?.negated);
    // S = sum(Y)
    const agg = body[2].?;
    try std.testing.expectEqual(@as(c_int, 1), agg.aggregate);
    try std.testing.expectEqualStrings("S", std.mem.span(agg.pred.?));
    try std.testing.expectEqualStrings("sum", std.mem.span(agg.agg_op.?.text.?));
    try std.testing.expectEqual(TOK_AGGREGATE, agg.agg_op.?.kind);
    try std.testing.expectEqual(@as(c_int, 1), agg.nargs);
    try std.testing.expectEqualStrings("Y", std.mem.span(agg.args.?[0].?.text.?));
    // T = X + 1 * 2  ->  + (X, * (1, 2))
    const ar = body[4].?;
    try std.testing.expectEqualStrings("=", std.mem.span(ar.pred.?));
    try std.testing.expectEqual(@as(c_int, 1), ar.nargs);
    try std.testing.expectEqualStrings("T", std.mem.span(ar.args.?[0].?.text.?));
    const e = ar.arith.?;
    try std.testing.expectEqual(EX_BINOP, e.kind);
    try std.testing.expectEqual(@as(u8, '+'), e.op);
    try std.testing.expectEqual(EX_VAR, e.l.?.kind);
    try std.testing.expectEqualStrings("X", std.mem.span(e.l.?.@"var".?));
    try std.testing.expectEqual(EX_BINOP, e.r.?.kind);
    try std.testing.expectEqual(@as(u8, '*'), e.r.?.op);
    try std.testing.expectEqual(EX_INT, e.r.?.l.?.kind);
    try std.testing.expectEqual(@as(c_uint, 1), e.r.?.l.?.ival);
    try std.testing.expectEqual(EX_INT, e.r.?.r.?.kind);
    try std.testing.expectEqual(@as(c_uint, 2), e.r.?.r.?.ival);

    // expr_clone deep-copies the tree; expr_free releases the copy
    const cl = expr_clone(e) orelse return error.NoClone;
    defer expr_free(cl);
    try std.testing.expect(cl != e);
    try std.testing.expectEqual(@as(u8, '+'), cl.op);
    try std.testing.expectEqualStrings("X", std.mem.span(cl.l.?.@"var".?));
    try std.testing.expectEqual(@as(c_uint, 2), cl.r.?.r.?.ival);
    try std.testing.expect(expr_clone(null) == null);
}

test "list literal and [X|Xs] pattern" {
    const p = parse_create("m(X) :- k(L), member(X, [1,2]).\nh(X, Xs) :- k(L), [X|Xs] = L.\n") orelse return error.NoParser;
    defer parse_free(p);

    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse return error.NoRules;
    defer {
        var i: c_int = 0;
        while (i < n) : (i += 1) rule_free(rules[@intCast(i)]);
        c.free(@ptrCast(rules));
    }

    try std.testing.expectEqual(@as(c_int, 2), n);

    // member(X, [1,2]) — args[1] is one TOK_LIST token with 2 INT children
    const mem = rules[0].?.body.?[1].?;
    try std.testing.expectEqualStrings("member", std.mem.span(mem.pred.?));
    try std.testing.expectEqual(@as(c_int, 2), mem.nargs);
    const lit = mem.args.?[1].?;
    try std.testing.expectEqual(TOK_LIST, lit.kind);
    try std.testing.expectEqual(@as(c_int, 2), lit.nchildren);
    try std.testing.expectEqual(TOK_INT, lit.children.?[0].?.kind);
    try std.testing.expectEqual(@as(c_uint, 1), lit.children.?[0].?.ival);
    try std.testing.expectEqual(TOK_INT, lit.children.?[1].?.kind);
    try std.testing.expectEqual(@as(c_uint, 2), lit.children.?[1].?.ival);
    try std.testing.expect(lit.tail == null);

    // [X|Xs] = L — '=' atom, args[0] = pattern list (1 child + var tail)
    const lst_as = rules[1].?.body.?[1].?;
    try std.testing.expectEqualStrings("=", std.mem.span(lst_as.pred.?));
    try std.testing.expectEqual(@as(c_int, 2), lst_as.nargs);
    const pat = lst_as.args.?[0].?;
    try std.testing.expectEqual(TOK_LIST, pat.kind);
    try std.testing.expectEqual(@as(c_int, 1), pat.nchildren);
    try std.testing.expectEqual(TOK_VAR, pat.children.?[0].?.kind);
    try std.testing.expectEqualStrings("X", std.mem.span(pat.children.?[0].?.text.?));
    try std.testing.expect(pat.tail != null);
    try std.testing.expectEqual(TOK_VAR, pat.tail.?.kind);
    try std.testing.expectEqualStrings("Xs", std.mem.span(pat.tail.?.text.?));
    const rhs = lst_as.args.?[1].?;
    try std.testing.expectEqual(TOK_VAR, rhs.kind);
    try std.testing.expectEqualStrings("L", std.mem.span(rhs.text.?));
}

test "regex pattern atom: ~ [k] 'pattern'" {
    const p = parse_create("g(X) :- h(X), h(X) ~ 1 'a[b]c'.") orelse return error.NoParser;
    defer parse_free(p);

    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse return error.NoRules;
    defer {
        var i: c_int = 0;
        while (i < n) : (i += 1) rule_free(rules[@intCast(i)]);
        c.free(@ptrCast(rules));
    }

    const pat_atom = rules[0].?.body.?[1].?;
    try std.testing.expectEqualStrings("h", std.mem.span(pat_atom.pred.?));
    try std.testing.expectEqual(@as(c_int, 1), pat_atom.nargs);
    try std.testing.expectEqualStrings("a[b]c", std.mem.span(pat_atom.pattern.?));
    try std.testing.expectEqual(@as(c_int, 1), pat_atom.pattern_col);
    try std.testing.expectEqual(@as(c_int, 0), pat_atom.negated);
}

test "parse_last_error: reporting parser survives lexer errors, plain does not" {
    // plain parse_create frees itself on lexer error -> NULL
    try std.testing.expect(parse_create("a(X) :- 'foo.") == null);

    // reporting variant keeps it, with the first error + offset
    const p = parse_create_reporting("a(X) :- 'foo.") orelse return error.NoParser;
    defer parse_free(p);
    var off: c_uint = 99;
    const msg = parse_last_error(p, &off) orelse return error.NoError;
    try std.testing.expectEqualStrings("parser: unclosed single-quoted string at position 13\n", std.mem.span(msg));
    try std.testing.expectEqual(@as(c_uint, 13), off);
    // only the FIRST error is kept
    try std.testing.expect(parse_last_error(null, &off) == null);

    // int literal >= TERM_BASE (0x80000000) is rejected with its position
    const q = parse_create_reporting("x(2147483648).") orelse return error.NoParser;
    defer parse_free(q);
    var off2: c_uint = 0;
    const msg2 = parse_last_error(q, &off2) orelse return error.NoError;
    try std.testing.expect(std.mem.indexOf(u8, std.mem.span(msg2), "out of range") != null);
    try std.testing.expectEqual(@as(c_uint, 12), off2);

    // a VAR before '~' never reaches the pattern suffix (pred must be IDENT)
    const bad = parse_create_reporting("g(X) :- h(X), X ~ 1 'a[b]c'.") orelse return error.NoParser;
    defer parse_free(bad);
    var nb: c_int = 0;
    try std.testing.expect(parse_rules(bad, &nb) == null);
    var off3: c_uint = 0;
    const msg3 = parse_last_error(bad, &off3) orelse return error.NoError;
    try std.testing.expectEqualStrings("parser: expected predicate name, got kind 2\n", std.mem.span(msg3));
    try std.testing.expectEqual(@as(c_uint, 14), off3);

    // 2147483647 (= TERM_BASE - 1) still parses, ival intact
    const ok = parse_create("x(2147483647).") orelse return error.NoParser;
    defer parse_free(ok);
    var n: c_int = 0;
    const rules = parse_rules(ok, &n) orelse return error.NoRules;
    defer {
        var i: c_int = 0;
        while (i < n) : (i += 1) rule_free(rules[@intCast(i)]);
        c.free(@ptrCast(rules));
    }
    try std.testing.expectEqual(@as(c_uint, 2147483647), rules[0].?.head.?.args.?[0].?.ival);
}

test "parse_rules: no rules / trailing tokens / null args" {
    // empty source -> "no rules found"
    const p0 = parse_create("") orelse return error.NoParser;
    defer parse_free(p0);
    var n: c_int = -1;
    try std.testing.expect(parse_rules(p0, &n) == null);
    try std.testing.expectEqual(@as(c_int, 0), n);
    const m0 = parse_last_error(p0, null) orelse return error.NoError;
    try std.testing.expectEqualStrings("parse: no rules found\n", std.mem.span(m0));

    // comment-only source behaves the same
    const pc = parse_create("# nothing here\n") orelse return error.NoParser;
    defer parse_free(pc);
    n = -1;
    try std.testing.expect(parse_rules(pc, &n) == null);
    try std.testing.expectEqual(@as(c_int, 0), n);

    // NULL parser / NULL n_rules
    n = 0;
    try std.testing.expect(parse_rules(null, &n) == null);
    try std.testing.expect(parse_create(null) == null);
    try std.testing.expect(parse_create_reporting(null) == null);

    // several rules parsed in order
    const pm = parse_create("edge(a, b).\npath(X, Y) :- edge(X, Y).\n") orelse return error.NoParser;
    defer parse_free(pm);
    n = 0;
    const rules = parse_rules(pm, &n) orelse return error.NoRules;
    defer {
        var i: c_int = 0;
        while (i < n) : (i += 1) rule_free(rules[@intCast(i)]);
        c.free(@ptrCast(rules));
    }
    try std.testing.expectEqual(@as(c_int, 2), n);
    try std.testing.expectEqualStrings("edge", std.mem.span(rules[0].?.head.?.pred.?));
    try std.testing.expectEqual(@as(c_int, 0), rules[0].?.nbody);
    try std.testing.expectEqualStrings("path", std.mem.span(rules[1].?.head.?.pred.?));
    try std.testing.expectEqual(@as(c_int, 1), rules[1].?.nbody);
    try std.testing.expectEqual(@as(c_int, 0), rules[1].?.has_negation);
}

test "rule_free / parse_free round-trip (no leaks under valgrind in CI)" {
    // Free a rule directly (the compiler does this after dl_load_rules).
    const p = parse_create("q(X) :- p(X), !r(X), X != s, X < 3.\n") orelse return error.NoParser;
    defer parse_free(p);
    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse return error.NoRules;
    try std.testing.expectEqual(@as(c_int, 1), n);
    try std.testing.expectEqual(@as(c_int, 1), rules[0].?.has_negation);
    rule_free(rules[0]);
    c.free(@ptrCast(rules));
}
