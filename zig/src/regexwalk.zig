//! regexwalk.zig — port of src/regexwalk.c (Regex → DFA compiler +
//! automaton-intersection walkers, M5).
//!
//! Architecture (1:1 with the C oracle):
//!   1. Lexer (rxLex) + recursive-descent parser building a Thompson NFA
//!      (literals incl. \\, \xHH, \0, ., [abc]/[a-z]/[^abc], *, +, ?, |, ()).
//!   2. Subset construction: ε-closure → DFA via bitset state sets, with an
//!      open-addressing state map (dsm_*) for dedup.  trans[s*256+byte]
//!      (UINT32_MAX = DFA_DEAD) + accept[].
//!   3. Product DFS walkers over a DAFSA × regex DFA, with a FNV-1a visited
//!      set for cycle detection: regex_dfa_walk (in-memory, trans_arr_c) and
//!      regex_dfa_walk_view (mmap view, view_edge_next).  symbols_dfa_walk /
//!      symbols_dfa_walk_view match the string portion of symbol keys
//!      (utf8 \0 u32BE) and emit sym_ids; the symbol walkers are ITERATIVE
//!      (explicit heap stack, MAX_WORD_LEN frames) so deep symbol DAFSAs can
//!      never overflow the C stack.
//!
//! Strangler-hybrid ABI: `regex_dfa` and `sym_set` are CONCRETE C structs
//! (src/regexwalk.h) dereferenced by still-C code (relation.c→relation.zig's
//! rel_pattern/rel_filter_col, snapshot.c's view_pattern, vm.c's symbol
//! walks, tests/test_m5*) — so they are `extern struct`s with the exact C
//! field order/types.  The dafsa engine stays C in the hybrid .so
//! (vendor/dafsa/*.c) and is addressed via @cImport("dafsa_internal.h")
//! (struct dafsa/State/Edge/TransHeap, dafsa_view, view_edge_next), exactly
//! like the C oracle does; it is NOT reimplemented here.
//!
//! LATENT C BUG (fixed here, deliberately NOT reproduced): in the C oracle,
//! regex_compile's `fail:` label calls dsm_free(&dsm), but dsm_init(&dsm) only
//! runs partway down — and fail is reachable from parse/lexer/NFA errors
//! BEFORE that init.  C therefore frees UNINITIALIZED stack memory (the gcc
//! build only survives via -ftrivial-auto-var-init=zero).  Here the state map
//! is declared zero-initialized, so dsm.entries is NULL until dsm_init runs
//! and dsm_free is an explicit no-op on every pre-init fail path — the same
//! semantics the C flag papers over, but written down.
//!
//! Oracle: src/regexwalk.c + src/regexwalk.h (never modified).

const std = @import("std");
const c = std.c;

// dafsa_internal.h: struct dafsa/State/Edge/TransHeap, dafsa_view,
// view_edge_next/trans_find, strdup, crc32 etc.  Include path set by build.zig.
const dc = @cImport({
    @cInclude("dafsa_internal.h");
});

// ─── Constants (mirror regexwalk.h / dafsa_internal.h) ─────────────────────

const DFA_DEAD: u32 = 0xFFFFFFFF;
const REGEX_DFA_MAX_STATES = 50000; // comptime_int; coerces to usize/c_int as needed
const REGEX_DFA_ABORT_EARLY = 8192;
const NFA_MAX_TRANS = 4;
const VISITED_INIT_CAP = 1024;
const SYMSET_INIT_CAP = 64;
const MAX_WORD_LEN: usize = 65536; // dafsa_internal.h MAX_WORD_LEN
const FNV_OFFSET: u64 = 14695981039346656037;
const FNV_PRIME: u64 = 1099511628211;

// ─── Public C-ABI types (src/regexwalk.h) ─────────────────────────────────

/// struct regex_dfa — CONCRETE, dereferenced by C callers.  Field order/types
/// byte-identical to src/regexwalk.h:
///   uint32_t n_states; uint32_t *trans; uint8_t *accept; char *errmsg;
pub const regex_dfa = extern struct {
    n_states: u32, // number of states (0 = error / empty)
    trans: ?[*]u32, // trans[s * 256 + byte]; UINT32_MAX = dead
    accept: ?[*]u8, // accept[s] = 1 if accepting state
    errmsg: ?[*:0]u8, // NULL on success, error string on failure
};

/// struct sym_set — CONCRETE, dereferenced by C callers (rel_filter_col's
/// filter_cb).  Byte-identical to src/regexwalk.h:
///   uint32_t *keys; int cap; int used;
pub const sym_set = extern struct {
    keys: ?[*]u32,
    cap: c_int,
    used: c_int,
};

/// typedef int (*regex_walk_cb)(const unsigned char *key_bytes, size_t key_len, void *user)
pub const RegexWalkCb = ?*const fn (key_bytes: [*c]const u8, key_len: usize, user: ?*anyopaque) callconv(.c) c_int;

/// typedef int (*sym_walk_cb)(uint32_t sym_id, void *user)
pub const SymWalkCb = ?*const fn (sym_id: u32, user: ?*anyopaque) callconv(.c) c_int;

// ─── NFA symbolic transition types ─────────────────────────────────────────

const SymKind = enum(u8) {
    epsilon = 0,
    literal = 1, // matches exactly one byte value
    any = 2, // matches any byte 0x00-0xFF
    class = 3, // matches if bitmap[byte>>3] & (1<<(byte&7))
};

const SymEdge = struct {
    kind: SymKind,
    // C union — extern union (no active-field tracking) so field access and
    // zeroing behave exactly like the C oracle's raw byte union.
    u: extern union {
        literal: u8,
        bitmap: [32]u8, // 256 bits
    },
};

/// NFA state: a flat array of up to NFA_MAX_TRANS transitions.
const NfaState = struct {
    edges: [NFA_MAX_TRANS]SymEdge,
    targets: [NFA_MAX_TRANS]u32,
    nedges: c_int,
    accept: c_int, // non-zero if accepting
};

const Nfa = struct {
    nstates: c_int, // allocated count
    start: c_int, // start state index
    states: ?[*]NfaState,
    cap: c_int,
    errmsg: ?[*:0]const u8, // set on error (string literals only)
};

// ─── NFA construction helpers ──────────────────────────────────────────────

fn nfaAddState(n: *Nfa, accept: c_int) c_int {
    if (n.nstates >= n.cap) {
        const newcap: c_int = if (n.cap != 0) n.cap * 2 else 32;
        const old: ?*anyopaque = if (n.states) |st| @ptrCast(st) else null;
        const mem = c.realloc(old, @as(usize, @intCast(newcap)) * @sizeOf(NfaState)) orelse {
            n.errmsg = "OOM in NFA";
            return -1;
        };
        n.states = @ptrCast(@alignCast(mem));
        n.cap = newcap;
    }
    const id = n.nstates;
    n.nstates += 1;
    const st = &n.states.?[@intCast(id)];
    // C: memset(&n->states[id], 0, sizeof(nfa_state)).  The SymEdge union is
    // not std.mem.zeroes-able, so zero the raw bytes exactly like C.
    @memset(std.mem.asBytes(st), 0);
    st.accept = accept;
    return id;
}

fn nfaAddEdge(n: *Nfa, from: c_int, to: c_int, sym: SymEdge) c_int {
    const s = &n.states.?[@intCast(from)];
    if (s.nedges >= NFA_MAX_TRANS) {
        n.errmsg = "NFA edge overflow";
        return -1;
    }
    const idx: usize = @intCast(s.nedges);
    s.edges[idx] = sym;
    s.targets[idx] = @intCast(to);
    s.nedges += 1;
    return 0;
}

fn symEpsilon() SymEdge {
    var e: SymEdge = undefined;
    e.kind = .epsilon;
    return e;
}

fn symLiteral(ch: u8) SymEdge {
    var e: SymEdge = undefined;
    e.kind = .literal;
    e.u.literal = ch;
    return e;
}

fn symAny() SymEdge {
    var e: SymEdge = undefined;
    e.kind = .any;
    return e;
}

fn symClass(bitmap: *const [32]u8) SymEdge {
    var e: SymEdge = undefined;
    e.kind = .class;
    e.u.bitmap = bitmap.*;
    return e;
}

// ─── Regex lexer / parser ──────────────────────────────────────────────────

const RxTokenKind = enum(u8) {
    eof = 0,
    char = 1, // literal byte or escaped character
    dot = 2, // .
    star = 3, // *
    plus = 4, // +
    ques = 5, // ?
    pipe = 6, // |
    lparen = 7, // (
    rparen = 8, // )
    lbracket = 9, // [
    rbracket = 10, // ]
};

const RxToken = struct {
    kind: RxTokenKind,
    ch: u8, // for CHAR
};

const RxParser = struct {
    pos: [*c]const u8,
    src: [*c]const u8, // original pattern (kept for parity; unused)
    tok: RxToken,
    has_tok: c_int,
    errmsg: ?[*:0]u8, // owned (strdup), set on error
};

fn rxError(p: *RxParser, msg: [*c]const u8) void {
    if (p.errmsg == null) p.errmsg = dc.strdup(msg);
}

fn rxIsOctalDigit(ch: u8) bool {
    return ch >= '0' and ch <= '7';
}

fn rxIsHexDigit(ch: u8) bool {
    return (ch >= '0' and ch <= '9') or
        (ch >= 'a' and ch <= 'f') or
        (ch >= 'A' and ch <= 'F');
}

fn rxHexVal(ch: u8) u8 {
    if (ch >= '0' and ch <= '9') return ch - '0';
    if (ch >= 'a' and ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' and ch <= 'F') return ch - 'A' + 10;
    return 0;
}

/// Read one token into p.tok.
fn rxLex(p: *RxParser) void {
    var s: [*c]const u8 = p.pos;

    if (p.has_tok != 0) {
        p.has_tok = 0;
        return;
    }

    // Once an error has been recorded, always report EOF so the parser's
    // loops terminate instead of re-reading a stale token forever.
    if (p.errmsg != null) {
        p.tok.kind = .eof;
        return;
    }

    if (s[0] == 0) {
        p.tok.kind = .eof;
        return;
    }

    // Escapes
    if (s[0] == '\\') {
        s += 1;
        if (s[0] == 0) {
            rxError(p, "trailing backslash");
            return;
        }
        if (s[0] == '\\') {
            p.tok.kind = .char;
            p.tok.ch = '\\';
        } else if (s[0] == '0') {
            // could be \0 or \0NN (octal) — we only support \0
            if (s[1] != 0 and rxIsOctalDigit(s[1])) {
                // \0NN — read up to 2 more octal digits
                var val: u32 = 0;
                var i: usize = 0;
                while (i < 2 and s[i + 1] != 0 and rxIsOctalDigit(s[i + 1])) : (i += 1) {
                    val = val * 8 + @as(u32, s[i + 1] - '0');
                }
                p.tok.kind = .char;
                p.tok.ch = @truncate(val);
                s += i;
            } else {
                p.tok.kind = .char;
                p.tok.ch = 0x00;
            }
        } else if (s[0] == 'x' or s[0] == 'X') {
            s += 1;
            if (s[0] == 0 or s[1] == 0 or !rxIsHexDigit(s[0]) or !rxIsHexDigit(s[1])) {
                rxError(p, "invalid \\xHH escape");
                return;
            }
            p.tok.kind = .char;
            p.tok.ch = @truncate((@as(u32, rxHexVal(s[0])) << 4) | @as(u32, rxHexVal(s[1])));
            s += 1;
        } else if (s[0] == 'n') {
            p.tok.kind = .char;
            p.tok.ch = '\n';
        } else if (s[0] == 'r') {
            p.tok.kind = .char;
            p.tok.ch = '\r';
        } else if (s[0] == 't') {
            p.tok.kind = .char;
            p.tok.ch = '\t';
        } else {
            // unrecognized escape — pass through literal
            p.tok.kind = .char;
            p.tok.ch = s[0];
        }
        s += 1;
        p.pos = s;
        return;
    }

    // Metacharacters
    switch (s[0]) {
        '.' => {
            p.tok.kind = .dot;
            s += 1;
        },
        '*' => {
            p.tok.kind = .star;
            s += 1;
        },
        '+' => {
            p.tok.kind = .plus;
            s += 1;
        },
        '?' => {
            p.tok.kind = .ques;
            s += 1;
        },
        '|' => {
            p.tok.kind = .pipe;
            s += 1;
        },
        '(' => {
            p.tok.kind = .lparen;
            s += 1;
        },
        ')' => {
            p.tok.kind = .rparen;
            s += 1;
        },
        '[' => {
            p.tok.kind = .lbracket;
            s += 1;
        },
        ']' => {
            p.tok.kind = .rbracket;
            s += 1;
        },
        else => {
            // NUL in pattern (unescaped) is an error
            if (s[0] == 0) {
                rxError(p, "unescaped NUL in pattern");
                return;
            }
            p.tok.kind = .char;
            p.tok.ch = s[0];
            s += 1;
        },
    }
    p.pos = s;
}

fn rxPeek(p: *RxParser) RxToken {
    if (p.has_tok == 0) {
        rxLex(p);
        p.has_tok = 1;
    }
    return p.tok;
}

fn rxNext(p: *RxParser) RxToken {
    if (p.has_tok == 0) rxLex(p);
    p.has_tok = 0;
    return p.tok;
}

fn rxMatch(p: *RxParser, k: RxTokenKind) bool {
    if (rxPeek(p).kind == k) {
        _ = rxNext(p);
        return true;
    }
    return false;
}

// ─── Character class parsing ───────────────────────────────────────────────

/// Parse [...] and write a SYM_CLASS into *out.  Caller has consumed '['.
fn parseCharClass(p: *RxParser, out: *SymEdge) c_int {
    var bitmap: [32]u8 = [_]u8{0} ** 32;
    var negate: c_int = 0;
    var prev: c_int = -1; // previous char for range, -1 = none
    var have_prev: c_int = 0;
    var any: c_int = 0;

    // Leading '^' negates (only if it's the first char after '[')
    if (rxPeek(p).kind == .char and rxPeek(p).ch == '^') {
        _ = rxNext(p);
        negate = 1;
    }

    // ']' as first char (or after ^) is literal
    if (rxMatch(p, .rbracket)) {
        bitmap[']' >> 3] |= @as(u8, 1) << @intCast(']' & 7);
        any = 1;
    }

    while (rxPeek(p).kind != .rbracket and rxPeek(p).kind != .eof) {
        var ch: u8 = undefined;

        // Inside a character class every metacharacter is a literal byte.
        switch (rxPeek(p).kind) {
            .dot => {
                ch = '.';
                _ = rxNext(p);
            },
            .star => {
                ch = '*';
                _ = rxNext(p);
            },
            .plus => {
                ch = '+';
                _ = rxNext(p);
            },
            .ques => {
                ch = '?';
                _ = rxNext(p);
            },
            .pipe => {
                ch = '|';
                _ = rxNext(p);
            },
            .lparen => {
                ch = '(';
                _ = rxNext(p);
            },
            .rparen => {
                ch = ')';
                _ = rxNext(p);
            },
            .char => {
                ch = rxNext(p).ch;
            },
            else => {
                rxError(p, "unexpected token in character class");
                return -1;
            },
        }

        if (have_prev != 0 and prev >= 0 and @as(c_int, ch) > prev) {
            // Range: prev-ch
            var i: c_int = prev;
            while (i <= @as(c_int, ch)) : (i += 1) {
                bitmap[@intCast(i >> 3)] |= @as(u8, 1) << @intCast(i & 7);
            }
            have_prev = 0;
            prev = -1;
            any = 1;
        } else if (rxPeek(p).kind == .char and rxPeek(p).ch == '-') {
            // Possible range: check if next is a real char
            _ = rxNext(p); // consume '-'
            if (rxPeek(p).kind == .rbracket) {
                // '-' at end of class: literal
                bitmap['-' >> 3] |= @as(u8, 1) << @intCast('-' & 7);
                any = 1;
                if (have_prev != 0) {
                    bitmap[@intCast(prev >> 3)] |= @as(u8, 1) << @intCast(prev & 7);
                    have_prev = 0;
                }
            } else {
                // It's a range start
                prev = @as(c_int, ch);
                have_prev = 1;
            }
        } else {
            bitmap[ch >> 3] |= @as(u8, 1) << @intCast(ch & 7);
            any = 1;
            if (have_prev != 0) {
                // A pending range start that never became an ascending range
                // (e.g. [z-a]): emit the previous char AND the '-' as literals
                // so the dash is not silently dropped.
                bitmap[@intCast(prev >> 3)] |= @as(u8, 1) << @intCast(prev & 7);
                bitmap['-' >> 3] |= @as(u8, 1) << @intCast('-' & 7);
                have_prev = 0;
            }
            prev = -1;
            have_prev = 0;
        }
    }

    if (have_prev != 0) {
        bitmap[@intCast(prev >> 3)] |= @as(u8, 1) << @intCast(prev & 7);
        any = 1;
    }

    if (!rxMatch(p, .rbracket)) {
        rxError(p, "unclosed character class");
        return -1;
    }

    if (any == 0 and negate == 0) {
        // empty class [] — matches nothing
        @memset(&bitmap, 0);
    }

    if (negate != 0) {
        var i: usize = 0;
        while (i < 32) : (i += 1) bitmap[i] = ~bitmap[i];
    }

    out.* = symClass(&bitmap);
    return 0;
}

// ─── Thompson NFA construction (recursive descent) ─────────────────────────

const NfaFrag = struct {
    start: c_int,
    accept: c_int,
};

fn rxAlt(n: *Nfa, p: *RxParser, out: *NfaFrag) c_int {
    var left: NfaFrag = undefined;
    if (rxSeq(n, p, &left) != 0) return -1;

    while (rxMatch(p, .pipe)) {
        var right: NfaFrag = undefined;
        if (rxSeq(n, p, &right) != 0) return -1;

        const s = nfaAddState(n, 0);
        const a = nfaAddState(n, 1);
        if (s < 0 or a < 0) return -1;

        if (nfaAddEdge(n, s, left.start, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, s, right.start, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, left.accept, a, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, right.accept, a, symEpsilon()) != 0) return -1;

        n.states.?[@intCast(left.accept)].accept = 0;
        n.states.?[@intCast(right.accept)].accept = 0;

        left.start = s;
        left.accept = a;
    }

    out.* = left;
    return 0;
}

fn rxSeq(n: *Nfa, p: *RxParser, out: *NfaFrag) c_int {
    var left: NfaFrag = undefined;
    if (rxPostfix(n, p, &left) != 0) return -1;

    while (rxPeek(p).kind != .eof and rxPeek(p).kind != .pipe and rxPeek(p).kind != .rparen) {
        var right: NfaFrag = undefined;
        if (rxPostfix(n, p, &right) != 0) return -1;

        n.states.?[@intCast(left.accept)].accept = 0;
        if (nfaAddEdge(n, left.accept, right.start, symEpsilon()) != 0) return -1;

        left.accept = right.accept;
    }

    out.* = left;
    return 0;
}

fn rxPostfix(n: *Nfa, p: *RxParser, out: *NfaFrag) c_int {
    var e: NfaFrag = undefined;
    if (rxAtom(n, p, &e) != 0) return -1;

    if (rxMatch(p, .star)) {
        // e*
        const s = nfaAddState(n, 0);
        const a = nfaAddState(n, 1);
        if (s < 0 or a < 0) return -1;

        n.states.?[@intCast(e.accept)].accept = 0;
        if (nfaAddEdge(n, s, e.start, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, s, a, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, e.accept, e.start, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, e.accept, a, symEpsilon()) != 0) return -1;

        out.start = s;
        out.accept = a;
        return 0;
    }

    if (rxMatch(p, .plus)) {
        // e+
        const s = nfaAddState(n, 0);
        const a = nfaAddState(n, 1);
        if (s < 0 or a < 0) return -1;

        n.states.?[@intCast(e.accept)].accept = 0;
        if (nfaAddEdge(n, s, e.start, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, e.accept, e.start, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, e.accept, a, symEpsilon()) != 0) return -1;

        out.start = s;
        out.accept = a;
        return 0;
    }

    if (rxMatch(p, .ques)) {
        // e?
        const s = nfaAddState(n, 0);
        const a = nfaAddState(n, 1);
        if (s < 0 or a < 0) return -1;

        n.states.?[@intCast(e.accept)].accept = 0;
        if (nfaAddEdge(n, s, e.start, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, s, a, symEpsilon()) != 0) return -1;
        if (nfaAddEdge(n, e.accept, a, symEpsilon()) != 0) return -1;

        out.start = s;
        out.accept = a;
        return 0;
    }

    out.* = e;
    return 0;
}

fn rxAtom(n: *Nfa, p: *RxParser, out: *NfaFrag) c_int {
    if (rxMatch(p, .lparen)) {
        if (rxAlt(n, p, out) != 0) return -1;
        if (!rxMatch(p, .rparen)) {
            rxError(p, "unclosed parenthesis");
            return -1;
        }
        return 0;
    }

    if (rxMatch(p, .lbracket)) {
        var cls: SymEdge = undefined;
        if (parseCharClass(p, &cls) != 0) return -1;
        const s = nfaAddState(n, 0);
        const a = nfaAddState(n, 1);
        if (s < 0 or a < 0) return -1;
        if (nfaAddEdge(n, s, a, cls) != 0) return -1;
        out.start = s;
        out.accept = a;
        return 0;
    }

    if (rxPeek(p).kind == .dot) {
        _ = rxNext(p);
        const s = nfaAddState(n, 0);
        const a = nfaAddState(n, 1);
        if (s < 0 or a < 0) return -1;
        if (nfaAddEdge(n, s, a, symAny()) != 0) return -1;
        out.start = s;
        out.accept = a;
        return 0;
    }

    if (rxPeek(p).kind == .char) {
        const ch = rxNext(p).ch;
        const s = nfaAddState(n, 0);
        const a = nfaAddState(n, 1);
        if (s < 0 or a < 0) return -1;
        if (nfaAddEdge(n, s, a, symLiteral(ch)) != 0) return -1;
        out.start = s;
        out.accept = a;
        return 0;
    }

    rxError(p, "expected atom");
    return -1;
}

/// top-level: alt EOF (implicit ^...$ — just match the full string)
fn rxRegex(n: *Nfa, p: *RxParser, out: *NfaFrag) c_int {
    if (rxAlt(n, p, out) != 0) return -1;
    if (rxPeek(p).kind != .eof) {
        rxError(p, "unexpected trailing characters");
        return -1;
    }
    return 0;
}

// ─── Bitset helpers (for subset construction) ──────────────────────────────

const Bitset = struct {
    words: ?[*]u64,
    nwords: c_int,
};

fn bsNew(nwords: c_int) ?*Bitset {
    const bmem = c.calloc(1, @sizeOf(Bitset)) orelse return null;
    const b: *Bitset = @ptrCast(@alignCast(bmem));
    const w = c.calloc(@as(usize, @intCast(nwords)), @sizeOf(u64));
    if (w == null) {
        c.free(bmem);
        return null;
    }
    b.words = @ptrCast(@alignCast(w));
    b.nwords = nwords;
    return b;
}

fn bsFree(b: ?*Bitset) void {
    const bb = b orelse return;
    if (bb.words) |w| c.free(@ptrCast(w));
    c.free(@ptrCast(bb));
}

fn bsSet(b: *Bitset, idx: c_int) void {
    const w = b.words.?;
    w[@intCast(@divTrunc(idx, 64))] |= @as(u64, 1) << @intCast(idx & 63);
}

fn bsGet(b: *const Bitset, idx: c_int) c_int {
    const w = b.words.?;
    const bit: u64 = (w[@intCast(@divTrunc(idx, 64))] >> @intCast(idx & 63)) & 1;
    return @intCast(bit);
}

fn bsClear(b: *Bitset) void {
    const w = b.words.?;
    @memset(w[0..@intCast(b.nwords)], 0);
}

fn bsCopy(dst: *Bitset, src: *const Bitset) void {
    const d = dst.words.?;
    const s = src.words.?;
    @memcpy(d[0..@intCast(src.nwords)], s[0..@intCast(src.nwords)]);
}

fn bsEqual(a: *const Bitset, b: *const Bitset) bool {
    const aw = a.words.?;
    const bw = b.words.?;
    return std.mem.eql(u64, aw[0..@intCast(a.nwords)], bw[0..@intCast(a.nwords)]);
}

/// FNV-1a over the words (byte at a time).
fn bsHash(b: *const Bitset) u64 {
    var h: u64 = FNV_OFFSET;
    const words = b.words.?;
    var i: c_int = 0;
    while (i < b.nwords) : (i += 1) {
        var w = words[@intCast(i)];
        var j: usize = 0;
        while (j < 8) : (j += 1) {
            h ^= @as(u8, @truncate(w & 0xFF));
            h *%= FNV_PRIME;
            w >>= 8;
        }
    }
    return h;
}

/// Does the bitset contain any accepting NFA state?
fn bsHasAccept(b: *const Bitset, n: *const Nfa) c_int {
    var i: c_int = 0;
    while (i < n.nstates) : (i += 1) {
        if (bsGet(b, i) != 0 and n.states.?[@intCast(i)].accept != 0) return 1;
    }
    return 0;
}

// ─── ε-closure / move ──────────────────────────────────────────────────────

fn epsClosure(n: *const Nfa, set: *Bitset) void {
    var changed: c_int = 0;
    while (true) {
        changed = 0;
        var i: c_int = 0;
        while (i < n.nstates) : (i += 1) {
            if (bsGet(set, i) == 0) continue;
            const s = &n.states.?[@intCast(i)];
            var j: c_int = 0;
            while (j < s.nedges) : (j += 1) {
                if (s.edges[@intCast(j)].kind == .epsilon) {
                    const tgt: c_int = @intCast(s.targets[@intCast(j)]);
                    if (bsGet(set, tgt) == 0) {
                        bsSet(set, tgt);
                        changed = 1;
                    }
                }
            }
        }
        if (changed == 0) break;
    }
}

fn symMatches(e: *const SymEdge, byte: u8) c_int {
    return switch (e.kind) {
        .literal => @intFromBool(e.u.literal == byte),
        .any => 1,
        .class => @intCast((e.u.bitmap[@intCast(byte >> 3)] >> @intCast(byte & 7)) & 1),
        .epsilon => 0,
    };
}

fn move(n: *const Nfa, from: *const Bitset, byte: u8, to: *Bitset) void {
    bsClear(to);
    var i: c_int = 0;
    while (i < n.nstates) : (i += 1) {
        if (bsGet(from, i) == 0) continue;
        const s = &n.states.?[@intCast(i)];
        var j: c_int = 0;
        while (j < s.nedges) : (j += 1) {
            if (symMatches(&s.edges[@intCast(j)], byte) != 0)
                bsSet(to, @intCast(s.targets[@intCast(j)]));
        }
    }
    epsClosure(n, to);
}

// ─── Subset construction: NFA → DFA ────────────────────────────────────────

/// DFA state set hash table entry: key = hash of bitset, value = bitset + id.
const DfaStateEntry = struct {
    hash: u64,
    nfa_set: ?*Bitset,
    dfa_id: c_int,
    used: c_int,
};

const DfaStateMap = struct {
    entries: ?[*]DfaStateEntry,
    cap: c_int,
    used: c_int,
};

fn dsmInit(m: *DfaStateMap) c_int {
    m.* = std.mem.zeroes(DfaStateMap);
    m.cap = 256;
    m.entries = @ptrCast(@alignCast(c.calloc(256, @sizeOf(DfaStateEntry))));
    return if (m.entries == null) -1 else 0;
}

fn dsmFree(m: *DfaStateMap) void {
    // THE LATENT-C-BUG FIX: `entries` is NULL until dsmInit runs, so this is
    // an explicit no-op on any fail path reached before init (the C oracle
    // depended on -ftrivial-auto-var-init=zero for exactly this).
    const entries = m.entries orelse return;
    var i: c_int = 0;
    while (i < m.cap) : (i += 1) {
        const e = &entries[@intCast(i)];
        if (e.used != 0) bsFree(e.nfa_set);
    }
    c.free(@ptrCast(entries));
    m.* = std.mem.zeroes(DfaStateMap);
}

fn dsmFindOrAdd(m: *DfaStateMap, bs: *const Bitset, hash: u64, dfa_id: c_int, found: *c_int) c_int {
    var idx: usize = @intCast(hash & @as(u64, @intCast(m.cap - 1)));

    while (true) {
        const entries = m.entries.?;
        const e = &entries[idx];
        if (e.used == 0) {
            // Grow if > 75% full
            if (m.used * 4 >= m.cap * 3) {
                const old_cap = m.cap;
                const old = entries;
                const new_cap = old_cap * 2;
                const mem = c.calloc(@as(usize, @intCast(new_cap)), @sizeOf(DfaStateEntry)) orelse return -1;
                const ne: [*]DfaStateEntry = @ptrCast(@alignCast(mem));

                // Rehash
                var i: c_int = 0;
                while (i < old_cap) : (i += 1) {
                    if (old[@intCast(i)].used != 0) {
                        var ni: usize = @intCast(old[@intCast(i)].hash & @as(u64, @intCast(new_cap - 1)));
                        while (ne[ni].used != 0)
                            ni = (ni +% 1) & @as(usize, @intCast(new_cap - 1));
                        ne[ni] = old[@intCast(i)];
                    }
                }
                c.free(@ptrCast(old));
                m.entries = ne;
                m.cap = new_cap;
                idx = @intCast(hash & @as(u64, @intCast(new_cap - 1)));
                continue;
            }

            // Insert new
            e.hash = hash;
            e.nfa_set = bsNew(bs.nwords) orelse return -1;
            bsCopy(e.nfa_set.?, bs);
            e.dfa_id = dfa_id;
            e.used = 1;
            m.used += 1;
            found.* = 0;
            return dfa_id;
        }

        if (e.hash == hash and bsEqual(e.nfa_set.?, bs)) {
            found.* = 1;
            return e.dfa_id;
        }

        idx = (idx +% 1) & @as(usize, @intCast(m.cap - 1));
    }
}

// ─── regex_compile ─────────────────────────────────────────────────────────

/// Allocate a zeroed regex_dfa with `errmsg = strdup(msg)` (NULL errmsg on OOM).
fn makeErrDfa(msg: [*c]const u8) ?*regex_dfa {
    const mem = c.calloc(1, @sizeOf(regex_dfa)) orelse return null;
    const dfa: *regex_dfa = @ptrCast(@alignCast(mem));
    dfa.errmsg = dc.strdup(msg);
    return dfa;
}

/// The C `fail:` tail: return `d` if already set (specific message), else the
/// generic n.errmsg ?: "OOM" dfa.
fn failDfa(d: ?*regex_dfa, n: *const Nfa) ?*regex_dfa {
    if (d != null) return d;
    return makeErrDfa(if (n.errmsg != null) n.errmsg.? else "OOM");
}

/// regex_dfa *regex_compile(const char *pattern)
pub export fn regex_compile(pattern: [*c]const u8) ?*regex_dfa {
    // ── 0. Validate ───────────────────────────────────────────────────
    if (pattern == null or pattern[0] == 0) return makeErrDfa("empty pattern");

    var dfa: ?*regex_dfa = null;
    var n = std.mem.zeroes(Nfa);
    var p = std.mem.zeroes(RxParser);
    var frag: NfaFrag = undefined;
    var cur: ?*Bitset = null;
    var next: ?*Bitset = null;
    // THE FIX: zero-initialized so dsm.entries == NULL until dsmInit below;
    // dsmFree is therefore a no-op on any fail path reached before init.
    var dsm = std.mem.zeroes(DfaStateMap);
    var dfa_queue: ?[*]c_int = null;
    var queue_head: c_int = 0;
    var queue_tail: c_int = 0;
    var queue_cap: c_int = 0;
    var trans: ?[*]u32 = null;
    var accept: ?[*]u8 = null;

    // fail-path cleanup, mirroring the C `fail:` label's four ops
    // (dfa_queue / trans / accept / n.states / p.errmsg) plus dsm/cur/next —
    // and, on the success path, trans/accept are nulled out first (transferred
    // to dfa) so this defer skips them, exactly like the C success tail.
    defer {
        if (dfa_queue) |q| c.free(@ptrCast(q));
        dsmFree(&dsm);
        bsFree(cur);
        bsFree(next);
        if (trans) |t| c.free(@ptrCast(t));
        if (accept) |a| c.free(@ptrCast(a));
        if (n.states) |st| c.free(@ptrCast(st));
        if (p.errmsg) |e| c.free(@ptrCast(e));
    }

    // ── 1. Parse and build NFA ────────────────────────────────────────
    n.start = nfaAddState(&n, 0);
    if (n.start < 0) return failDfa(null, &n);

    p.src = pattern;
    p.pos = pattern;

    if (rxRegex(&n, &p, &frag) != 0) {
        // Parse error: n.errmsg > p.errmsg > generic (C order).
        var msg: [*c]const u8 = "regex parse error";
        if (p.errmsg != null) msg = p.errmsg.?;
        if (n.errmsg != null) msg = n.errmsg.?;
        return failDfa(makeErrDfa(msg), &n);
    }

    // A lexer error (e.g. trailing backslash) records errmsg even though the
    // parser loop exits normally on the EOF token — surface it, don't drop it.
    if (p.errmsg != null) return failDfa(makeErrDfa(p.errmsg.?), &n);

    if (n.errmsg != null) return failDfa(makeErrDfa(n.errmsg.?), &n);

    // Connect start to fragment, make fragment's accept the final state
    n.states.?[@intCast(frag.accept)].accept = 1;
    n.states.?[@intCast(n.start)].accept = 0;
    if (nfaAddEdge(&n, n.start, frag.start, symEpsilon()) != 0) {
        return failDfa(makeErrDfa(if (n.errmsg != null) n.errmsg.? else "NFA error"), &n);
    }

    // ── 2. Subset construction ────────────────────────────────────────
    const nwords: c_int = @divTrunc(n.nstates + 63, 64);
    cur = bsNew(nwords);
    next = bsNew(nwords);
    if (cur == null or next == null) return failDfa(null, &n);

    if (dsmInit(&dsm) != 0) return failDfa(null, &n);

    // Start with ε-closure of NFA start state
    bsSet(cur.?, n.start);
    epsClosure(&n, cur.?);

    // BFS queue for subset construction
    queue_cap = 64;
    const qmem = c.malloc(@as(usize, @intCast(queue_cap)) * @sizeOf(c_int));
    if (qmem == null) return failDfa(null, &n);
    dfa_queue = @ptrCast(@alignCast(qmem));

    // Initial DFA state
    {
        var found: c_int = 0;
        const h = bsHash(cur.?);
        if (dsmFindOrAdd(&dsm, cur.?, h, 0, &found) != 0) return failDfa(null, &n);
        // should not be found since dsm is empty
        dfa_queue.?[@intCast(queue_tail)] = 0;
        queue_tail += 1;
    }

    // Allocate initial DFA arrays, fill trans with DFA_DEAD
    {
        const sz: usize = REGEX_DFA_MAX_STATES;
        const tmem = c.malloc(sz * 256 * @sizeOf(u32));
        const amem = c.calloc(sz, @sizeOf(u8));
        trans = @ptrCast(@alignCast(tmem));
        accept = @ptrCast(@alignCast(amem));
        if (trans == null or accept == null) return failDfa(null, &n);
        var i: usize = 0;
        while (i < REGEX_DFA_MAX_STATES * 256) : (i += 1) trans.?[i] = DFA_DEAD;
    }

    // Set accept for initial state
    if (bsHasAccept(cur.?, &n) != 0) accept.?[0] = 1;

    // Main BFS loop
    while (queue_head < queue_tail) {
        // Early-abort: reject pathological patterns quickly.
        if (queue_tail >= REGEX_DFA_ABORT_EARLY) {
            return failDfa(makeErrDfa("regex state cap exceeded (8192)"), &n);
        }

        const dfa_s = dfa_queue.?[@intCast(queue_head)];
        queue_head += 1;

        // Restore the NFA set for this DFA state
        {
            var found_entry: c_int = 0;
            var i: c_int = 0;
            while (i < dsm.cap) : (i += 1) {
                const e = &dsm.entries.?[@intCast(i)];
                if (e.used != 0 and e.dfa_id == dfa_s) {
                    bsCopy(cur.?, e.nfa_set.?);
                    found_entry = 1;
                    break;
                }
            }
            if (found_entry == 0) continue; // shouldn't happen
        }

        var byte: c_int = 0;
        while (byte < 256) : (byte += 1) {
            move(&n, cur.?, @intCast(byte), next.?);

            // Check if next set is empty
            {
                var any: c_int = 0;
                var i: c_int = 0;
                while (i < nwords) : (i += 1) {
                    if (next.?.words.?[@intCast(i)] != 0) {
                        any = 1;
                        break;
                    }
                }
                if (any == 0) continue; // dead state
            }

            // Find or create DFA state for this NFA set
            {
                const h = bsHash(next.?);
                var found: c_int = 0;
                const tgt = dsmFindOrAdd(&dsm, next.?, h, queue_tail, &found);
                if (tgt < 0) return failDfa(null, &n);

                if (found == 0) {
                    // New DFA state
                    if (queue_tail >= REGEX_DFA_MAX_STATES) {
                        return failDfa(makeErrDfa("regex state cap exceeded (50000)"), &n);
                    }
                    if (queue_tail >= queue_cap) {
                        const nc = queue_cap * 2;
                        const nq = c.realloc(@ptrCast(dfa_queue), @as(usize, @intCast(nc)) * @sizeOf(c_int)) orelse return failDfa(null, &n);
                        dfa_queue = @ptrCast(@alignCast(nq));
                        queue_cap = nc;
                    }
                    dfa_queue.?[@intCast(queue_tail)] = queue_tail;
                    if (bsHasAccept(next.?, &n) != 0) accept.?[@intCast(queue_tail)] = 1;
                    queue_tail += 1;
                }

                trans.?[@as(usize, @intCast(dfa_s)) * 256 + @as(usize, @intCast(byte))] = @intCast(tgt);
            }
        }
    }

    const n_dfa = queue_tail;

    // ── 3. Build result ───────────────────────────────────────────────
    const mem = c.calloc(1, @sizeOf(regex_dfa)) orelse return failDfa(null, &n);
    const d: *regex_dfa = @ptrCast(@alignCast(mem));
    dfa = d;

    d.n_states = @intCast(n_dfa);
    d.trans = trans;
    trans = null;
    d.accept = accept;
    accept = null;

    return dfa;
}

/// void regex_dfa_free(regex_dfa *dfa)
pub export fn regex_dfa_free(dfa: ?*regex_dfa) void {
    const d = dfa orelse return;
    if (d.trans) |t| c.free(@ptrCast(t));
    if (d.accept) |a| c.free(@ptrCast(a));
    if (d.errmsg) |e| c.free(@ptrCast(e));
    c.free(@ptrCast(d));
}

// ─── Visited hash set for product DFS ──────────────────────────────────────

const VisitedSet = struct {
    keys: ?[*]u64, // (dafsa_state << 32) | regex_state
    cap: c_int,
    used: c_int,
};

fn vsInit(vs: *VisitedSet) c_int {
    vs.cap = VISITED_INIT_CAP;
    vs.used = 0;
    vs.keys = @ptrCast(@alignCast(c.calloc(VISITED_INIT_CAP, @sizeOf(u64))));
    return if (vs.keys == null) -1 else 0;
}

fn vsFree(vs: *VisitedSet) void {
    if (vs.keys) |k| c.free(@ptrCast(k));
}

fn vsHashKey(k0: u64) u64 {
    var h: u64 = FNV_OFFSET;
    var k = k0;
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        h ^= @as(u8, @truncate(k & 0xFF));
        h *%= FNV_PRIME;
        k >>= 8;
    }
    return h;
}

fn vsGrow(vs: *VisitedSet) c_int {
    const old_cap = vs.cap;
    const new_cap = old_cap * 2;
    const old_keys = vs.keys.?;
    const nkmem = c.calloc(@as(usize, @intCast(new_cap)), @sizeOf(u64)) orelse return -1;
    const nk: [*]u64 = @ptrCast(@alignCast(nkmem));

    var i: c_int = 0;
    while (i < old_cap) : (i += 1) {
        if (old_keys[@intCast(i)] != 0) {
            const h = vsHashKey(old_keys[@intCast(i)]);
            var idx: usize = @intCast(h & @as(u64, @intCast(new_cap - 1)));
            while (nk[idx] != 0)
                idx = (idx +% 1) & @as(usize, @intCast(new_cap - 1));
            nk[idx] = old_keys[@intCast(i)];
        }
    }
    c.free(@ptrCast(old_keys));
    vs.keys = nk;
    vs.cap = new_cap;
    return 0;
}

/// Returns 1 if key already present (cycle detected), 0 if newly inserted, -1 OOM.
fn vsTryInsert(vs: *VisitedSet, key: u64) c_int {
    if (vs.used * 4 >= vs.cap * 3) {
        if (vsGrow(vs) != 0) return -1;
    }
    const h = vsHashKey(key);
    var idx: usize = @intCast(h & @as(u64, @intCast(vs.cap - 1)));
    const keys = vs.keys.?;
    while (keys[idx] != 0) {
        if (keys[idx] == key) return 1; // cycle detected
        idx = (idx +% 1) & @as(usize, @intCast(vs.cap - 1));
    }
    keys[idx] = key;
    vs.used += 1;
    return 0; // newly inserted
}

/// Remove a key (mark as not-on-stack).  Returns 1 if found and removed.
fn vsRemove(vs: *VisitedSet, key: u64) c_int {
    const h = vsHashKey(key);
    var idx: usize = @intCast(h & @as(u64, @intCast(vs.cap - 1)));
    const keys = vs.keys.?;
    while (keys[idx] != 0) {
        if (keys[idx] == key) {
            keys[idx] = 0;
            vs.used -= 1;
            return 1;
        }
        idx = (idx +% 1) & @as(usize, @intCast(vs.cap - 1));
    }
    return 0;
}

// ─── trans_arr_c (translate-c workaround, same as relation.zig) ────────────

/// The translate-c output for the static-inline accessor fails to compile
/// under Zig 0.16 (@ptrCast discarding const); byte-identical logic:
///   s->trans_heap ? s->trans_heap->edges : s->trans
fn transArrC(s: [*c]const dc.State) [*c]const dc.Edge {
    const heap = s.*.trans_heap;
    if (heap != null) {
        return @ptrCast(@alignCast(&heap.*._edges));
    }
    return @ptrCast(&s.*.trans);
}

// ─── Product DFS: in-memory DAFSA × regex DFA ──────────────────────────────

const ProdCtx = struct {
    d: [*c]const dc.dafsa,
    dfa: [*c]const regex_dfa,
    cb: RegexWalkCb,
    user: ?*anyopaque,
    visited: VisitedSet,
    buf: [4096]u8,
    count: c_long,
};

fn prodDfs(ctx: *ProdCtx, dstate: c_uint, rstate: u32, depth: usize) c_int {
    if (depth >= ctx.buf.len) return 0;

    // Check if we're at an accepting pair
    const s = &ctx.d.*.states[dstate];
    if (s.*.is_final != 0 and ctx.dfa.*.accept.?[@intCast(rstate)] != 0) {
        ctx.count +%= 1;
        if (ctx.cb.?(ctx.buf[0..depth].ptr, depth, ctx.user) != 0) return 1;
    }

    // Cycle detection: if already on recursion stack, stop
    const vkey: u64 = (@as(u64, dstate) << 32) | @as(u64, rstate);
    const on_stack = vsTryInsert(&ctx.visited, vkey);
    if (on_stack < 0) return -1;
    if (on_stack == 1) return 0; // cycle — already on stack

    // Iterate over DAFSA edges from dstate
    var rc: c_int = 0;
    var j: c_uint = 0;
    while (j < s.*.ntrans) : (j += 1) {
        const e = transArrC(s) + j;
        const sym: u8 = e.*.sym;
        const next_rs = ctx.dfa.*.trans.?[@as(usize, rstate) * 256 + @as(usize, sym)];
        if (next_rs == DFA_DEAD) continue;

        ctx.buf[depth] = sym;
        rc = prodDfs(ctx, e.*.target, next_rs, depth + 1);
        if (rc != 0) break;
    }

    // Remove from recursion stack
    _ = vsRemove(&ctx.visited, vkey);
    return rc;
}

/// long regex_dfa_walk(const dafsa *d, const regex_dfa *dfa, regex_walk_cb cb, void *user)
pub export fn regex_dfa_walk(d: [*c]const dc.dafsa, dfa: [*c]const regex_dfa, cb: RegexWalkCb, user: ?*anyopaque) c_long {
    if (d == null or dfa == null or cb == null) return -1;
    if (dfa.*.n_states == 0) return 0;

    var ctx = ProdCtx{
        .d = d,
        .dfa = dfa,
        .cb = cb,
        .user = user,
        .visited = std.mem.zeroes(VisitedSet),
        .buf = [_]u8{0} ** 4096,
        .count = 0,
    };

    if (vsInit(&ctx.visited) != 0) return -1;
    defer vsFree(&ctx.visited);

    _ = prodDfs(&ctx, d.*.initial, 0, 0);
    return ctx.count;
}

// ─── Product DFS: mmap view DAFSA × regex DFA ──────────────────────────────

const ProdViewCtx = struct {
    v: [*c]const dc.dafsa_view,
    dfa: [*c]const regex_dfa,
    cb: RegexWalkCb,
    user: ?*anyopaque,
    visited: VisitedSet,
    buf: [4096]u8,
    count: c_long,
};

fn prodViewDfs(ctx: *ProdViewCtx, dstate: u32, rstate: u32, depth: usize) c_int {
    if (depth >= ctx.buf.len) return 0;

    // Accept check
    if ((ctx.v.*.final_bits[@intCast(dstate / 8)] & (@as(u8, 1) << @intCast(dstate % 8))) != 0) {
        if (ctx.dfa.*.accept.?[@intCast(rstate)] != 0) {
            ctx.count +%= 1;
            if (ctx.cb.?(ctx.buf[0..depth].ptr, depth, ctx.user) != 0) return 1;
        }
    }

    // Cycle detection: if already on recursion stack, stop
    const vkey: u64 = (@as(u64, dstate) << 32) | @as(u64, rstate);
    const on_stack = vsTryInsert(&ctx.visited, vkey);
    if (on_stack < 0) return -1;
    if (on_stack == 1) return 0; // cycle

    // Iterate edges via view_edge_next
    var rc: c_int = 0;
    var cur: [*c]const u8 = ctx.v.*.csr + @as(usize, ctx.v.*.state_off[@intCast(dstate)]);
    var sym: u8 = 0;
    var tgt: u32 = 0;

    while (dc.view_edge_next(ctx.v, dstate, &cur, &sym, &tgt) == 0) {
        const next_rs = ctx.dfa.*.trans.?[@as(usize, rstate) * 256 + @as(usize, sym)];
        if (next_rs == DFA_DEAD) continue;

        ctx.buf[depth] = sym;
        rc = prodViewDfs(ctx, tgt, next_rs, depth + 1);
        if (rc != 0) break;
    }

    // Remove from recursion stack
    _ = vsRemove(&ctx.visited, vkey);
    return rc;
}

/// long regex_dfa_walk_view(const dafsa_view *v, const regex_dfa *dfa, regex_walk_cb cb, void *user)
pub export fn regex_dfa_walk_view(v: [*c]const dc.dafsa_view, dfa: [*c]const regex_dfa, cb: RegexWalkCb, user: ?*anyopaque) c_long {
    if (v == null or dfa == null or cb == null) return -1;
    if (dfa.*.n_states == 0) return 0;

    var ctx = ProdViewCtx{
        .v = v,
        .dfa = dfa,
        .cb = cb,
        .user = user,
        .visited = std.mem.zeroes(VisitedSet),
        .buf = [_]u8{0} ** 4096,
        .count = 0,
    };

    if (vsInit(&ctx.visited) != 0) return -1;
    defer vsFree(&ctx.visited);

    _ = prodViewDfs(&ctx, v.*.initial, 0, 0);
    return ctx.count;
}

// ─── sym_set (open-addressing u32 hash set) ────────────────────────────────

fn symsetHash(k: u32) u32 {
    var h: u32 = 2166136261;
    h ^= (k >> 24) & 0xFF;
    h *%= 16777619;
    h ^= (k >> 16) & 0xFF;
    h *%= 16777619;
    h ^= (k >> 8) & 0xFF;
    h *%= 16777619;
    h ^= k & 0xFF;
    h *%= 16777619;
    return h;
}

fn symsetGrow(s: *sym_set) c_int {
    const old_cap = s.cap;
    const new_cap = old_cap * 2;
    const old_keys = s.keys.?;
    const nkmem = c.calloc(@as(usize, @intCast(new_cap)), @sizeOf(u32)) orelse return -1;
    const nk: [*]u32 = @ptrCast(@alignCast(nkmem));

    var i: c_int = 0;
    while (i < old_cap) : (i += 1) {
        if (old_keys[@intCast(i)] != 0) {
            const h = symsetHash(old_keys[@intCast(i)]);
            var idx: usize = @intCast(h & @as(u32, @intCast(new_cap - 1)));
            while (nk[idx] != 0)
                idx = (idx +% 1) & @as(usize, @intCast(new_cap - 1));
            nk[idx] = old_keys[@intCast(i)];
        }
    }
    c.free(@ptrCast(old_keys));
    s.keys = nk;
    s.cap = new_cap;
    return 0;
}

/// int symset_init(sym_set *s)
pub export fn symset_init(s: ?*sym_set) c_int {
    const ss = s orelse return -1;
    ss.cap = SYMSET_INIT_CAP;
    ss.used = 0;
    ss.keys = @ptrCast(@alignCast(c.calloc(SYMSET_INIT_CAP, @sizeOf(u32))));
    return if (ss.keys == null) -1 else 0;
}

/// void symset_free(sym_set *s) — NULL-safe
pub export fn symset_free(s: ?*sym_set) void {
    const ss = s orelse return;
    if (ss.keys) |k| c.free(@ptrCast(k));
    ss.keys = null;
    ss.cap = 0;
    ss.used = 0;
}

/// int symset_add(sym_set *s, uint32_t sym_id)
pub export fn symset_add(s: ?*sym_set, sym_id: u32) c_int {
    const ss = s orelse return -1;
    if (sym_id == 0) return 0;
    if (ss.used * 4 >= ss.cap * 3) {
        if (symsetGrow(ss) != 0) return -1;
    }
    const h = symsetHash(sym_id);
    var idx: usize = @intCast(h & @as(u32, @intCast(ss.cap - 1)));
    const keys = ss.keys.?;
    while (keys[idx] != 0) {
        if (keys[idx] == sym_id) return 0;
        idx = (idx +% 1) & @as(usize, @intCast(ss.cap - 1));
    }
    keys[idx] = sym_id;
    ss.used += 1;
    return 0;
}

/// int symset_contains(const sym_set *s, uint32_t sym_id)
pub export fn symset_contains(s: ?*const sym_set, sym_id: u32) c_int {
    const ss = s orelse return 0;
    if (sym_id == 0) return 0;
    const keys = ss.keys orelse return 0;
    const h = symsetHash(sym_id);
    var idx: usize = @intCast(h & @as(u32, @intCast(ss.cap - 1)));
    while (keys[idx] != 0) {
        if (keys[idx] == sym_id) return 1;
        idx = (idx +% 1) & @as(usize, @intCast(ss.cap - 1));
    }
    return 0;
}

// ─── Symbol DAFSA walkers ──────────────────────────────────────────────────

/// Read sym_id from 4-byte u32BE payload following NUL.  Each of the 4 payload
/// nodes must have exactly one outgoing edge (the next payload byte).
fn readSymId(d: [*c]const dc.dafsa, s: c_uint, bad: *c_int) u32 {
    var id: u32 = 0;
    var cur: c_uint = s;
    var i: usize = 0;
    while (i < 4) : (i += 1) {
        const st = &d.*.states[cur];
        if (st.*.ntrans != 1) {
            bad.* = 1;
            return 0xFFFFFFFF;
        }
        const e = transArrC(st);
        id = (id << 8) | @as(u32, e.*.sym);
        cur = e.*.target;
    }
    return id;
}

const SymProdCtx = struct {
    d: [*c]const dc.dafsa,
    dfa: [*c]const regex_dfa,
    cb: SymWalkCb,
    user: ?*anyopaque,
    visited: VisitedSet,
    count: c_long,
};

/// Explicit-stack frame for the iterative product DFS.  `j` is the next
/// transition index to consider; `entered` records whether the NUL/sym_id
/// check and visited-set insert have run.  The stack is heap-allocated because
/// it can grow to ~MAX_WORD_LEN frames (recursion would overflow the C stack).
const SpFrame = struct {
    dstate: c_uint,
    rstate: u32,
    depth: usize,
    j: c_uint,
    entered: c_int,
};

fn symProdDfs(ctx: *SymProdCtx, dstate: c_uint, rstate: u32, depth: usize) c_int {
    var cap: usize = 64;
    var top: usize = 0;

    if (depth >= MAX_WORD_LEN) return 0;

    const smem = c.malloc(cap * @sizeOf(SpFrame)) orelse return -1;
    var stack: [*]SpFrame = @ptrCast(@alignCast(smem));

    stack[top].dstate = dstate;
    stack[top].rstate = rstate;
    stack[top].depth = depth;
    stack[top].j = 0;
    stack[top].entered = 0;
    top += 1;

    while (top > 0) {
        const fi = top - 1;
        const f = &stack[fi];

        if (f.entered == 0) {
            f.entered = 1;
            const s = &ctx.d.*.states[f.dstate];

            // Check for NUL terminator + accepting regex state (before the
            // visited-set insert, exactly as the recursive form did).
            var j: c_uint = 0;
            while (j < s.*.ntrans) : (j += 1) {
                const e = transArrC(s) + j;
                if (e.*.sym == 0x00 and ctx.dfa.*.accept.?[@intCast(f.rstate)] != 0) {
                    var bad: c_int = 0;
                    const sym_id = readSymId(ctx.d, e.*.target, &bad);
                    if (bad == 0) {
                        ctx.count +%= 1;
                        if (ctx.cb.?(sym_id, ctx.user) != 0) {
                            c.free(@ptrCast(stack));
                            return 1;
                        }
                    }
                }
            }

            const vkey: u64 = (@as(u64, f.dstate) << 32) | @as(u64, f.rstate);
            const on_stack = vsTryInsert(&ctx.visited, vkey);
            if (on_stack < 0) {
                c.free(@ptrCast(stack));
                return -1;
            }
            if (on_stack == 1) {
                // Cycle: pair already on an ancestor's path.  Do not remove it
                // here — the ancestor still owns it.
                top -= 1;
                continue;
            }
        }

        // Descend into one child at a time; each child's subtree is fully
        // explored before this frame resumes at the next transition.
        {
            const s = &ctx.d.*.states[f.dstate];
            var pushed: c_int = 0;

            while (f.j < s.*.ntrans) {
                const e = transArrC(s) + f.j;
                const sym: u8 = e.*.sym;
                f.j += 1;
                if (sym == 0x00) continue;
                const next_rs = ctx.dfa.*.trans.?[@as(usize, f.rstate) * 256 + @as(usize, sym)];
                if (next_rs == DFA_DEAD) continue;
                if (f.depth + 1 >= MAX_WORD_LEN) continue;
                // Capture child fields before realloc may move `stack`
                // (and thus invalidate `f`).
                const child_d = e.*.target;
                const child_depth = f.depth + 1;
                if (top == cap) {
                    const ncap = cap * 2;
                    const ns = c.realloc(@ptrCast(stack), ncap * @sizeOf(SpFrame)) orelse {
                        c.free(@ptrCast(stack));
                        return -1;
                    };
                    stack = @ptrCast(@alignCast(ns));
                    cap = ncap;
                }
                stack[top].dstate = child_d;
                stack[top].rstate = next_rs;
                stack[top].depth = child_depth;
                stack[top].j = 0;
                stack[top].entered = 0;
                top += 1;
                pushed = 1;
                break;
            }
            if (pushed != 0) continue;
        }

        // Subtree fully explored: pop and drop this pair from the path set.
        _ = vsRemove(&ctx.visited, (@as(u64, f.dstate) << 32) | @as(u64, f.rstate));
        top -= 1;
    }

    c.free(@ptrCast(stack));
    return 0;
}

/// long symbols_dfa_walk(const dafsa *d, const regex_dfa *dfa, sym_walk_cb cb, void *user)
pub export fn symbols_dfa_walk(d: [*c]const dc.dafsa, dfa: [*c]const regex_dfa, cb: SymWalkCb, user: ?*anyopaque) c_long {
    if (d == null or dfa == null or cb == null) return -1;
    if (dfa.*.n_states == 0) return 0;

    var ctx = SymProdCtx{
        .d = d,
        .dfa = dfa,
        .cb = cb,
        .user = user,
        .visited = std.mem.zeroes(VisitedSet),
        .count = 0,
    };

    if (vsInit(&ctx.visited) != 0) return -1;
    defer vsFree(&ctx.visited);

    _ = symProdDfs(&ctx, d.*.initial, 0, 0);
    return ctx.count;
}

const SymProdViewCtx = struct {
    v: [*c]const dc.dafsa_view,
    dfa: [*c]const regex_dfa,
    cb: SymWalkCb,
    user: ?*anyopaque,
    visited: VisitedSet,
    count: c_long,
};

/// Explicit-stack frame for the iterative view product DFS.  `cur` is the
/// resume cursor into the mmap'd CSR for the next transition; `entered`
/// records whether the NUL/sym_id check and visited-set insert have run.
const SpViewFrame = struct {
    dstate: u32,
    rstate: u32,
    depth: usize,
    cur: [*c]const u8,
    entered: c_int,
};

fn symProdViewDfs(ctx: *SymProdViewCtx, dstate: u32, rstate: u32, depth: usize) c_int {
    var cap: usize = 64;
    var top: usize = 0;

    if (depth >= MAX_WORD_LEN) return 0;

    const smem = c.malloc(cap * @sizeOf(SpViewFrame)) orelse return -1;
    var stack: [*]SpViewFrame = @ptrCast(@alignCast(smem));

    stack[top].dstate = dstate;
    stack[top].rstate = rstate;
    stack[top].depth = depth;
    stack[top].cur = null;
    stack[top].entered = 0;
    top += 1;

    while (top > 0) {
        const fi = top - 1;
        const f = &stack[fi];

        if (f.entered == 0) {
            f.entered = 1;

            // Check for NUL terminator + accepting regex state (before the
            // visited-set insert, exactly as the recursive form did).
            {
                var cur: [*c]const u8 = ctx.v.*.csr + @as(usize, ctx.v.*.state_off[@intCast(f.dstate)]);
                var sym: u8 = 0;
                var tgt: u32 = 0;
                while (dc.view_edge_next(ctx.v, f.dstate, &cur, &sym, &tgt) == 0) {
                    if (sym == 0x00 and ctx.dfa.*.accept.?[@intCast(f.rstate)] != 0) {
                        // Reconstruct the 4-byte u32BE payload following the
                        // NUL.  Mirrors readSymId: each of the 4 payload nodes
                        // must have exactly one outgoing edge.
                        var bad: c_int = 0;
                        var sym_id: u32 = 0;
                        var cur2: u32 = tgt;
                        var i: usize = 0;
                        while (i < 4) : (i += 1) {
                            var cur_p: [*c]const u8 = ctx.v.*.csr + @as(usize, ctx.v.*.state_off[@intCast(cur2)]);
                            var sym_p: u8 = 0;
                            var tgt_p: u32 = 0;
                            // First edge must exist (the payload byte).
                            if (dc.view_edge_next(ctx.v, cur2, &cur_p, &sym_p, &tgt_p) != 0) {
                                bad = 1;
                                break;
                            }
                            // A second edge on the same node means malformed payload.
                            {
                                var cur_p2: [*c]const u8 = cur_p;
                                var sym_p2: u8 = 0;
                                var tgt_p2: u32 = 0;
                                if (dc.view_edge_next(ctx.v, cur2, &cur_p2, &sym_p2, &tgt_p2) == 0) {
                                    bad = 1;
                                    break;
                                }
                            }
                            sym_id = (sym_id << 8) | @as(u32, sym_p);
                            cur2 = tgt_p;
                        }
                        if (bad == 0) {
                            ctx.count +%= 1;
                            if (ctx.cb.?(sym_id, ctx.user) != 0) {
                                c.free(@ptrCast(stack));
                                return 1;
                            }
                        }
                    }
                }
            }

            const vkey: u64 = (@as(u64, f.dstate) << 32) | @as(u64, f.rstate);
            const on_stack = vsTryInsert(&ctx.visited, vkey);
            if (on_stack < 0) {
                c.free(@ptrCast(stack));
                return -1;
            }
            if (on_stack == 1) {
                // Cycle: pair already on an ancestor's path.
                top -= 1;
                continue;
            }

            // Initialize the resume cursor for the child-iteration loop.
            f.cur = ctx.v.*.csr + @as(usize, ctx.v.*.state_off[@intCast(f.dstate)]);
        }

        // Descend into one child at a time.
        {
            var sym: u8 = 0;
            var tgt: u32 = 0;
            var pushed: c_int = 0;

            while (dc.view_edge_next(ctx.v, f.dstate, &f.cur, &sym, &tgt) == 0) {
                if (sym == 0x00) continue;
                const next_rs = ctx.dfa.*.trans.?[@as(usize, f.rstate) * 256 + @as(usize, sym)];
                if (next_rs == DFA_DEAD) continue;
                if (f.depth + 1 >= MAX_WORD_LEN) continue;
                // Capture child fields before realloc may move `stack`.
                const child_d = tgt;
                const child_depth = f.depth + 1;
                if (top == cap) {
                    const ncap = cap * 2;
                    const ns = c.realloc(@ptrCast(stack), ncap * @sizeOf(SpViewFrame)) orelse {
                        c.free(@ptrCast(stack));
                        return -1;
                    };
                    stack = @ptrCast(@alignCast(ns));
                    cap = ncap;
                }
                stack[top].dstate = child_d;
                stack[top].rstate = next_rs;
                stack[top].depth = child_depth;
                stack[top].cur = null;
                stack[top].entered = 0;
                top += 1;
                pushed = 1;
                break;
            }
            if (pushed != 0) continue;
        }

        // Subtree fully explored: pop and drop this pair from the path set.
        _ = vsRemove(&ctx.visited, (@as(u64, f.dstate) << 32) | @as(u64, f.rstate));
        top -= 1;
    }

    c.free(@ptrCast(stack));
    return 0;
}

/// long symbols_dfa_walk_view(const dafsa_view *v, const regex_dfa *dfa, sym_walk_cb cb, void *user)
pub export fn symbols_dfa_walk_view(v: [*c]const dc.dafsa_view, dfa: [*c]const regex_dfa, cb: SymWalkCb, user: ?*anyopaque) c_long {
    if (v == null or dfa == null or cb == null) return -1;
    if (dfa.*.n_states == 0) return 0;

    var ctx = SymProdViewCtx{
        .v = v,
        .dfa = dfa,
        .cb = cb,
        .user = user,
        .visited = std.mem.zeroes(VisitedSet),
        .count = 0,
    };

    if (vsInit(&ctx.visited) != 0) return -1;
    defer vsFree(&ctx.visited);

    _ = symProdViewDfs(&ctx, v.*.initial, 0, 0);
    return ctx.count;
}

// ─── Tests ─────────────────────────────────────────────────────────────────

const WalkTestCtx = struct {
    count: c_long,
    keys: [8][64]u8,
    n: usize,
};

fn walkTestCb(key_bytes: [*c]const u8, key_len: usize, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *WalkTestCtx = @ptrCast(@alignCast(user orelse return -1));
    if (ctx.n < ctx.keys.len) {
        const k = &ctx.keys[ctx.n];
        @memcpy(k[0..key_len], key_bytes[0..key_len]);
        k[key_len] = 0;
    }
    ctx.n += 1;
    ctx.count += 1;
    return 0;
}

const SymTestCtx = struct {
    count: c_long,
    sum: u64,
};

fn symTestCb(sym_id: u32, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *SymTestCtx = @ptrCast(@alignCast(user orelse return -1));
    ctx.count += 1;
    ctx.sum += sym_id;
    return 0;
}

fn addSymKey(d: [*c]dc.dafsa, s: []const u8, id: u32) void {
    var key: [512]u8 = undefined;
    @memcpy(key[0..s.len], s);
    key[s.len] = 0;
    key[s.len + 1] = @truncate(id >> 24);
    key[s.len + 2] = @truncate(id >> 16);
    key[s.len + 3] = @truncate(id >> 8);
    key[s.len + 4] = @truncate(id);
    _ = dc.dafsa_add_n(d, key[0 .. s.len + 5].ptr, s.len + 5);
}

test "regex_compile: valid corpus has no error and >0 states" {
    const patterns = [_][]const u8{
        "hello",     ".",         "a.*",       ".*b.*",  "a|b",
        "a(bc?)?",   "[a-c].*",   "[^a-z]",    "(ab)*c", "a+",
        "a?",        "a\\\\b",    "\\x41\\x42", "a\\0b", "\\x00\\x00",
    };
    for (patterns) |pat| {
        const dfa = regex_compile(pat.ptr) orelse return error.CompileNull;
        defer regex_dfa_free(dfa);
        try std.testing.expect(dfa.*.errmsg == null);
        try std.testing.expect(dfa.*.n_states > 0);
    }
}

test "regex_compile: error cases report errmsg and 0 states" {
    const cases = [_][]const u8{
        "",      "[abc", "abc\\", "\\xG", "\\x", "\\x0", "(abc", "abc)",
        "\\",    "\\xA", "[",     "\\xGG",
    };
    for (cases) |pat| {
        const dfa = regex_compile(pat.ptr) orelse return error.CompileNull;
        defer regex_dfa_free(dfa);
        try std.testing.expect(dfa.*.errmsg != null);
        try std.testing.expectEqual(@as(u32, 0), dfa.*.n_states);
    }
    // NULL pattern also reports "empty pattern".
    const dnull = regex_compile(null) orelse return error.CompileNull;
    defer regex_dfa_free(dnull);
    try std.testing.expect(dnull.*.errmsg != null);
}

test "regex_compile: state cap exceeded (a|b)*a(a|b)^25" {
    var buf: [8192:0]u8 = undefined;
    var n: usize = 0;
    @memcpy(buf[n..][0..7], "(a|b)*a");
    n += 7;
    var i: usize = 0;
    while (i < 25) : (i += 1) {
        @memcpy(buf[n..][0..5], "(a|b)");
        n += 5;
    }
    buf[n] = 0;

    const dfa = regex_compile(&buf) orelse return error.CompileNull;
    defer regex_dfa_free(dfa);
    try std.testing.expect(dfa.*.errmsg != null);
    try std.testing.expectEqual(@as(u32, 0), dfa.*.n_states);
}

test "regex_dfa_walk over a small in-memory dafsa" {
    const d = dc.dafsa_create() orelse return error.OutOfMemory;
    defer dc.dafsa_free(d);

    const keys = [_][]const u8{ "hello", "hell", "world", "abc", "abcd" };
    for (keys) |k| {
        _ = dc.dafsa_add_n(d, k.ptr, k.len);
    }

    // "h.*" matches hello, hell (2); not world/abc/abcd.
    const dfa = regex_compile("h.*") orelse return error.CompileNull;
    defer regex_dfa_free(dfa);
    try std.testing.expect(dfa.*.errmsg == null);

    var ctx = WalkTestCtx{ .count = 0, .keys = undefined, .n = 0 };
    const n = regex_dfa_walk(d, dfa, walkTestCb, &ctx);
    try std.testing.expectEqual(@as(c_long, 2), n);
    try std.testing.expectEqual(@as(c_long, 2), ctx.count);

    // ".*b.*" matches abc, abcd (2).
    const dfa2 = regex_compile(".*b.*") orelse return error.CompileNull;
    defer regex_dfa_free(dfa2);
    var ctx2 = WalkTestCtx{ .count = 0, .keys = undefined, .n = 0 };
    const n2 = regex_dfa_walk(d, dfa2, walkTestCb, &ctx2);
    try std.testing.expectEqual(@as(c_long, 2), n2);

    // "world" matches exactly one; "a" matches none (full-key semantics).
    const dfa3 = regex_compile("world") orelse return error.CompileNull;
    defer regex_dfa_free(dfa3);
    var ctx3 = WalkTestCtx{ .count = 0, .keys = undefined, .n = 0 };
    const n3 = regex_dfa_walk(d, dfa3, walkTestCb, &ctx3);
    try std.testing.expectEqual(@as(c_long, 1), n3);
}

test "symbols_dfa_walk emits matched sym_ids" {
    const d = dc.dafsa_create() orelse return error.OutOfMemory;
    defer dc.dafsa_free(d);

    addSymKey(d, "apple", 1);
    addSymKey(d, "banana", 2);
    addSymKey(d, "avocado", 3);
    addSymKey(d, "pear", 4);

    // "a.*" matches apple(1) + avocado(3); not banana/pear.
    const dfa = regex_compile("a.*") orelse return error.CompileNull;
    defer regex_dfa_free(dfa);
    try std.testing.expect(dfa.*.errmsg == null);

    var ctx = SymTestCtx{ .count = 0, .sum = 0 };
    const n = symbols_dfa_walk(d, dfa, symTestCb, &ctx);
    try std.testing.expectEqual(@as(c_long, 2), n);
    try std.testing.expectEqual(@as(c_long, 2), ctx.count);
    try std.testing.expectEqual(@as(u64, 4), ctx.sum); // 1 + 3

    // "banana" matches exactly one sym_id.
    const dfa2 = regex_compile("banana") orelse return error.CompileNull;
    defer regex_dfa_free(dfa2);
    var ctx2 = SymTestCtx{ .count = 0, .sum = 0 };
    const n2 = symbols_dfa_walk(d, dfa2, symTestCb, &ctx2);
    try std.testing.expectEqual(@as(c_long, 1), n2);
    try std.testing.expectEqual(@as(u64, 2), ctx2.sum);
}

test "symset add/contains roundtrip" {
    var s: sym_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), symset_init(&s));
    defer symset_free(&s);

    try std.testing.expectEqual(@as(c_int, 0), symset_add(&s, 5));
    try std.testing.expectEqual(@as(c_int, 0), symset_add(&s, 5)); // dup
    try std.testing.expectEqual(@as(c_int, 0), symset_add(&s, 100000));
    try std.testing.expectEqual(@as(c_int, 0), symset_add(&s, 0)); // sentinel, ignored

    try std.testing.expectEqual(@as(c_int, 1), symset_contains(&s, 5));
    try std.testing.expectEqual(@as(c_int, 1), symset_contains(&s, 100000));
    try std.testing.expectEqual(@as(c_int, 0), symset_contains(&s, 6));
    try std.testing.expectEqual(@as(c_int, 0), symset_contains(&s, 0));
}
