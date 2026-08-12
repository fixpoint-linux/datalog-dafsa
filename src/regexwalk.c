/*
 * regexwalk.c — Regex → DFA compiler + automaton-intersection walkers (M5)
 *
 * Architecture:
 *   1. Parser: recursive-descent, builds an NFA-AST node tree
 *   2. Thompson NFA construction: builds an NFA with ε-transitions
 *      and symbolic edges (SYM_LITERAL, SYM_ANY, SYM_CLASS)
 *   3. Subset construction: ε-closure → DFA via bitset state sets
 *   4. Walkers: product DFS on (dafsa_state, regex_state) with
 *      visited hash set for cycle detection
 *
 * Symbolic transitions avoid 256-edge explosion for . and character classes.
 */

#include "regexwalk.h"
#include "dafsa_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

/* ─── DFA dead sentinel ────────────────────────────────────────────────── */

#define DFA_DEAD UINT32_MAX

/* ─── NFA symbolic transition types ────────────────────────────────────── */

typedef enum {
    SYM_EPSILON = 0,
    SYM_LITERAL,   /* matches exactly one byte value */
    SYM_ANY,       /* matches any byte 0x00-0xFF */
    SYM_CLASS,     /* matches if bitmap[byte>>3] & (1<<(byte&7)) */
} sym_kind;

typedef struct {
    sym_kind kind;
    union {
        unsigned char literal;
        uint8_t       bitmap[32];  /* 256 bits */
    } u;
} sym_edge;

/*
 * NFA state: an array of transitions.
 * We store NFA in a flat array of state structs for easy bit-set operations
 * during subset construction.
 */
#define NFA_MAX_TRANS 4   /* small fixed cap per state */

typedef struct {
    sym_edge edges[NFA_MAX_TRANS];
    uint32_t targets[NFA_MAX_TRANS];
    int      nedges;
    int      accept;  /* non-zero if accepting */
} nfa_state;

typedef struct {
    int        nstates;    /* allocated count */
    int        start;      /* start state index */
    nfa_state *states;
    int        cap;
    char      *errmsg;     /* set on error */
} nfa;

/* ─── NFA construction helpers ─────────────────────────────────────────── */

static int nfa_add_state(nfa *n, int accept)
{
    if (n->nstates >= n->cap) {
        int newcap = n->cap ? n->cap * 2 : 32;
        nfa_state *ns = realloc(n->states,
                                (size_t)newcap * sizeof(nfa_state));
        if (!ns) { n->errmsg = "OOM in NFA"; return -1; }
        n->states = ns;
        n->cap = newcap;
    }
    int id = n->nstates++;
    memset(&n->states[id], 0, sizeof(nfa_state));
    n->states[id].accept = accept;
    return id;
}

static int nfa_add_edge(nfa *n, int from, int to, sym_edge sym)
{
    nfa_state *s = &n->states[from];
    if (s->nedges >= NFA_MAX_TRANS) { n->errmsg = "NFA edge overflow"; return -1; }
    s->edges[s->nedges] = sym;
    s->targets[s->nedges] = (uint32_t)to;
    s->nedges++;
    return 0;
}

static sym_edge sym_epsilon(void)
{
    sym_edge e; e.kind = SYM_EPSILON;
    return e;
}

static sym_edge sym_literal(unsigned char c)
{
    sym_edge e; e.kind = SYM_LITERAL; e.u.literal = c;
    return e;
}

static sym_edge sym_any(void)
{
    sym_edge e; e.kind = SYM_ANY;
    return e;
}

static sym_edge sym_class(const uint8_t bitmap[32])
{
    sym_edge e;
    e.kind = SYM_CLASS;
    memcpy(e.u.bitmap, bitmap, 32);
    return e;
}

/* ─── Regex parser ─────────────────────────────────────────────────────── */

typedef enum {
    RX_TOKEN_EOF = 0,
    RX_TOKEN_CHAR,     /* literal byte or escaped character */
    RX_TOKEN_DOT,      /* . */
    RX_TOKEN_STAR,     /* * */
    RX_TOKEN_PLUS,     /* + */
    RX_TOKEN_QUES,     /* ? */
    RX_TOKEN_PIPE,     /* | */
    RX_TOKEN_LPAREN,   /* ( */
    RX_TOKEN_RPAREN,   /* ) */
    RX_TOKEN_LBRACKET, /* [ */
    RX_TOKEN_RBRACKET, /* ] */
} rx_token_kind;

typedef struct {
    rx_token_kind kind;
    unsigned char ch;  /* for CHAR */
    char          *err;
} rx_token;

typedef struct {
    const char *pos;
    const char *src;
    rx_token     tok;
    int          has_tok;
    char        *errmsg;
} rx_parser;

static void rx_error(rx_parser *p, const char *msg)
{
    if (!p->errmsg) p->errmsg = strdup(msg);
}

static int rx_is_octal_digit(int c) { return c >= '0' && c <= '7'; }
static int rx_is_hex_digit(int c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
static int rx_hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Read one token into p->tok */
static void rx_lex(rx_parser *p)
{
    const char *s = p->pos;

    if (p->has_tok) { p->has_tok = 0; return; }

    if (!*s) { p->tok.kind = RX_TOKEN_EOF; return; }

    /* Escapes */
    if (*s == '\\') {
        s++;
        if (!*s) { rx_error(p, "trailing backslash"); return; }
        if (*s == '\\') {
            p->tok.kind = RX_TOKEN_CHAR; p->tok.ch = '\\';
        } else if (*s == '0') {
            /* could be \0 or \0NN (octal) — we only support \0 */
            if (*(s + 1) && rx_is_octal_digit(*(s + 1))) {
                /* \0NN — read up to 2 more octal digits */
                unsigned val = 0;
                int i;
                for (i = 0; i < 2 && s[i + 1] && rx_is_octal_digit(s[i + 1]); i++)
                    val = val * 8 + (unsigned)(s[i + 1] - '0');
                p->tok.kind = RX_TOKEN_CHAR;
                p->tok.ch = (unsigned char)val;
                s += i;
            } else {
                p->tok.kind = RX_TOKEN_CHAR; p->tok.ch = 0x00;
            }
        } else if (*s == 'x' || *s == 'X') {
            s++;
            if (!s[0] || !s[1] || !rx_is_hex_digit(s[0]) || !rx_is_hex_digit(s[1])) {
                rx_error(p, "invalid \\xHH escape"); return;
            }
            p->tok.kind = RX_TOKEN_CHAR;
            p->tok.ch = (unsigned char)((rx_hex_val(s[0]) << 4) | rx_hex_val(s[1]));
            s += 1;
        } else if (*s == 'n') {
            p->tok.kind = RX_TOKEN_CHAR; p->tok.ch = '\n';
        } else if (*s == 'r') {
            p->tok.kind = RX_TOKEN_CHAR; p->tok.ch = '\r';
        } else if (*s == 't') {
            p->tok.kind = RX_TOKEN_CHAR; p->tok.ch = '\t';
        } else {
            /* unrecognized escape — pass through literal */
            p->tok.kind = RX_TOKEN_CHAR; p->tok.ch = (unsigned char)*s;
        }
        s++;
        p->pos = s;
        return;
    }

    /* Metacharacters */
    switch (*s) {
    case '.':  p->tok.kind = RX_TOKEN_DOT;      s++; break;
    case '*':  p->tok.kind = RX_TOKEN_STAR;     s++; break;
    case '+':  p->tok.kind = RX_TOKEN_PLUS;     s++; break;
    case '?':  p->tok.kind = RX_TOKEN_QUES;     s++; break;
    case '|':  p->tok.kind = RX_TOKEN_PIPE;     s++; break;
    case '(':  p->tok.kind = RX_TOKEN_LPAREN;   s++; break;
    case ')':  p->tok.kind = RX_TOKEN_RPAREN;   s++; break;
    case '[':  p->tok.kind = RX_TOKEN_LBRACKET; s++; break;
    case ']':  p->tok.kind = RX_TOKEN_RBRACKET; s++; break;
    default:
        /* NUL in pattern (unescaped) is an error */
        if (*s == '\0') {
            rx_error(p, "unescaped NUL in pattern");
            return;
        }
        p->tok.kind = RX_TOKEN_CHAR;
        p->tok.ch = (unsigned char)*s;
        s++;
        break;
    }
    p->pos = s;
}

static const rx_token *rx_peek(rx_parser *p)
{
    if (!p->has_tok) { rx_lex(p); p->has_tok = 1; }
    return &p->tok;
}

static const rx_token *rx_next(rx_parser *p)
{
    if (!p->has_tok) rx_lex(p);
    p->has_tok = 0;
    return &p->tok;
}

static int rx_match(rx_parser *p, rx_token_kind k)
{
    if (rx_peek(p)->kind == k) { rx_next(p); return 1; }
    return 0;
}

/* ─── Character class parsing ──────────────────────────────────────────── */

/* Parse [...] and return a SYM_CLASS.  Caller has consumed '['. */
static int parse_char_class(rx_parser *p, sym_edge *out)
{
    uint8_t bitmap[32] = {0};
    int negate = 0;
    int prev = -1;  /* previous char for range, -1 = none */
    int have_prev = 0;
    int any = 0;

    /* Leading '^' negates */
    if (rx_peek(p)->kind == RX_TOKEN_CHAR && rx_peek(p)->ch == '^') {
        /* but only if it's the first char after [ */
        rx_next(p);
        negate = 1;
    }

    /* ']' as first char (or after ^) is literal */
    if (rx_match(p, RX_TOKEN_RBRACKET)) {
        bitmap[']' >> 3] |= (uint8_t)(1u << (']' & 7));
        any = 1;
    }

    while (rx_peek(p)->kind != RX_TOKEN_RBRACKET &&
           rx_peek(p)->kind != RX_TOKEN_EOF) {
        unsigned char ch;

        if (rx_match(p, RX_TOKEN_DOT)) {
            ch = '.';
        } else if (rx_peek(p)->kind == RX_TOKEN_CHAR) {
            ch = rx_next(p)->ch;
        } else {
            rx_error(p, "unexpected token in character class");
            return -1;
        }

        if (have_prev && prev >= 0 && (int)ch > prev) {
            /* Range: prev-ch */
            int i;
            for (i = prev; i <= (int)ch; i++)
                bitmap[i >> 3] |= (uint8_t)(1u << (i & 7));
            have_prev = 0;
            prev = -1;
            any = 1;
        } else if (rx_peek(p)->kind == RX_TOKEN_CHAR &&
                   rx_peek(p)->ch == '-') {
            /* Possible range: check if next is a real char */
            rx_next(p); /* consume '-' */
            if (rx_peek(p)->kind == RX_TOKEN_RBRACKET) {
                /* '-' at end of class: literal */
                bitmap['-' >> 3] |= (uint8_t)(1u << ('-' & 7));
                any = 1;
                if (have_prev) {
                    bitmap[prev >> 3] |= (uint8_t)(1u << (prev & 7));
                    have_prev = 0;
                }
            } else {
                /* It's a range start */
                prev = (int)ch;
                have_prev = 1;
            }
        } else {
            bitmap[ch >> 3] |= (uint8_t)(1u << (ch & 7));
            any = 1;
            if (have_prev) {
                bitmap[prev >> 3] |= (uint8_t)(1u << (prev & 7));
                have_prev = 0;
            }
            prev = -1;
            have_prev = 0;
        }
    }

    if (have_prev) {
        bitmap[prev >> 3] |= (uint8_t)(1u << (prev & 7));
        any = 1;
    }

    if (!rx_match(p, RX_TOKEN_RBRACKET)) {
        rx_error(p, "unclosed character class");
        return -1;
    }

    if (!any && !negate) {
        /* empty class [] — matches nothing */
        memset(bitmap, 0, 32);
    }

    if (negate) {
        int i;
        for (i = 0; i < 32; i++)
            bitmap[i] = (uint8_t)(~bitmap[i]);
    }

    *out = sym_class(bitmap);
    return 0;
}

/* ─── Forward NFA fragment ─────────────────────────────────────────────── */

typedef struct {
    int start;
    int accept;
} nfa_frag;

/* ─── Thompson NFA construction (recursive descent) ────────────────────── */

/* Forward declarations */
static int rx_regex(nfa *n, rx_parser *p, nfa_frag *out);
static int rx_alt(nfa *n, rx_parser *p, nfa_frag *out);
static int rx_seq(nfa *n, rx_parser *p, nfa_frag *out);
static int rx_postfix(nfa *n, rx_parser *p, nfa_frag *out);
static int rx_atom(nfa *n, rx_parser *p, nfa_frag *out);

/* alternation: seq (| seq)* */
static int rx_alt(nfa *n, rx_parser *p, nfa_frag *out)
{
    nfa_frag left;
    if (rx_seq(n, p, &left) != 0) return -1;

    while (rx_match(p, RX_TOKEN_PIPE)) {
        nfa_frag right;
        if (rx_seq(n, p, &right) != 0) return -1;

        int s = nfa_add_state(n, 0);
        int a = nfa_add_state(n, 1);
        if (s < 0 || a < 0) return -1;

        if (nfa_add_edge(n, s, left.start,  sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, s, right.start, sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, left.accept,  a, sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, right.accept, a, sym_epsilon()) != 0) return -1;

        n->states[left.accept].accept = 0;
        n->states[right.accept].accept = 0;

        left.start = s;
        left.accept = a;
    }

    *out = left;
    return 0;
}

/* sequence: postfix* */
static int rx_seq(nfa *n, rx_parser *p, nfa_frag *out)
{
    nfa_frag left;
    if (rx_postfix(n, p, &left) != 0) return -1;

    while (rx_peek(p)->kind != RX_TOKEN_EOF &&
           rx_peek(p)->kind != RX_TOKEN_PIPE &&
           rx_peek(p)->kind != RX_TOKEN_RPAREN) {
        nfa_frag right;
        if (rx_postfix(n, p, &right) != 0) return -1;

        n->states[left.accept].accept = 0;
        if (nfa_add_edge(n, left.accept, right.start, sym_epsilon()) != 0)
            return -1;

        left.accept = right.accept;
    }

    *out = left;
    return 0;
}

/* postfix: atom ('*' | '+' | '?')? */
static int rx_postfix(nfa *n, rx_parser *p, nfa_frag *out)
{
    nfa_frag e;
    if (rx_atom(n, p, &e) != 0) return -1;

    if (rx_match(p, RX_TOKEN_STAR)) {
        /* e* */
        int s = nfa_add_state(n, 0);
        int a = nfa_add_state(n, 1);
        if (s < 0 || a < 0) return -1;

        n->states[e.accept].accept = 0;
        if (nfa_add_edge(n, s, e.start,   sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, s, a,          sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, e.accept, e.start, sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, e.accept, a,   sym_epsilon()) != 0) return -1;

        out->start = s;
        out->accept = a;
        return 0;
    }

    if (rx_match(p, RX_TOKEN_PLUS)) {
        /* e+ */
        int s = nfa_add_state(n, 0);
        int a = nfa_add_state(n, 1);
        if (s < 0 || a < 0) return -1;

        n->states[e.accept].accept = 0;
        if (nfa_add_edge(n, s, e.start,        sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, e.accept, e.start, sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, e.accept, a,        sym_epsilon()) != 0) return -1;

        out->start = s;
        out->accept = a;
        return 0;
    }

    if (rx_match(p, RX_TOKEN_QUES)) {
        /* e? */
        int s = nfa_add_state(n, 0);
        int a = nfa_add_state(n, 1);
        if (s < 0 || a < 0) return -1;

        n->states[e.accept].accept = 0;
        if (nfa_add_edge(n, s, e.start,   sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, s, a,          sym_epsilon()) != 0) return -1;
        if (nfa_add_edge(n, e.accept, a,   sym_epsilon()) != 0) return -1;

        out->start = s;
        out->accept = a;
        return 0;
    }

    *out = e;
    return 0;
}

/* atom: CHAR | '.' | '(' regex ')' | '[' class ']' */
static int rx_atom(nfa *n, rx_parser *p, nfa_frag *out)
{
    if (rx_match(p, RX_TOKEN_LPAREN)) {
        if (rx_alt(n, p, out) != 0) return -1;
        if (!rx_match(p, RX_TOKEN_RPAREN)) {
            rx_error(p, "unclosed parenthesis");
            return -1;
        }
        return 0;
    }

    if (rx_match(p, RX_TOKEN_LBRACKET)) {
        sym_edge cls;
        if (parse_char_class(p, &cls) != 0) return -1;
        int s = nfa_add_state(n, 0);
        int a = nfa_add_state(n, 1);
        if (s < 0 || a < 0) return -1;
        if (nfa_add_edge(n, s, a, cls) != 0) return -1;
        out->start = s;
        out->accept = a;
        return 0;
    }

    if (rx_peek(p)->kind == RX_TOKEN_DOT) {
        rx_next(p);
        int s = nfa_add_state(n, 0);
        int a = nfa_add_state(n, 1);
        if (s < 0 || a < 0) return -1;
        if (nfa_add_edge(n, s, a, sym_any()) != 0) return -1;
        out->start = s;
        out->accept = a;
        return 0;
    }

    if (rx_peek(p)->kind == RX_TOKEN_CHAR) {
        unsigned char ch = rx_next(p)->ch;
        int s = nfa_add_state(n, 0);
        int a = nfa_add_state(n, 1);
        if (s < 0 || a < 0) return -1;
        if (nfa_add_edge(n, s, a, sym_literal(ch)) != 0) return -1;
        out->start = s;
        out->accept = a;
        return 0;
    }

    rx_error(p, "expected atom");
    return -1;
}

/* top-level: alt EOF (with implicit ^...$ — just match full string) */
static int rx_regex(nfa *n, rx_parser *p, nfa_frag *out)
{
    if (rx_alt(n, p, out) != 0) return -1;
    if (rx_peek(p)->kind != RX_TOKEN_EOF) {
        rx_error(p, "unexpected trailing characters");
        return -1;
    }
    return 0;
}

/* ─── Bitset helpers (for subset construction) ─────────────────────────── */

#define BITSET_WORDS(n) (((n) + 63) / 64)

typedef struct {
    uint64_t *words;
    int       nwords;
} bitset;

static bitset *bs_new(int nwords)
{
    bitset *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->words = calloc((size_t)nwords, sizeof(uint64_t));
    if (!b->words) { free(b); return NULL; }
    b->nwords = nwords;
    return b;
}

static void bs_free(bitset *b)
{
    if (!b) return;
    free(b->words);
    free(b);
}

static void bs_set(bitset *b, int idx)
{
    b->words[idx / 64] |= ((uint64_t)1u << (idx & 63));
}

static int bs_get(const bitset *b, int idx)
{
    return (b->words[idx / 64] >> (idx & 63)) & 1;
}

static void bs_clear(bitset *b)
{
    memset(b->words, 0, (size_t)b->nwords * sizeof(uint64_t));
}

static void bs_copy(bitset *dst, const bitset *src)
{
    memcpy(dst->words, src->words, (size_t)src->nwords * sizeof(uint64_t));
}

static int bs_equal(const bitset *a, const bitset *b)
{
    return memcmp(a->words, b->words,
                  (size_t)a->nwords * sizeof(uint64_t)) == 0;
}

/* Hash a bitset for DFA state dedup */
static uint64_t bs_hash(const bitset *b)
{
    /* FNV-1a over the words */
    uint64_t h = 14695981039346656037ULL;
    int i;
    for (i = 0; i < b->nwords; i++) {
        uint64_t w = b->words[i];
        int j;
        for (j = 0; j < 8; j++) {
            h ^= (uint8_t)(w & 0xFF);
            h *= 1099511628211ULL;
            w >>= 8;
        }
    }
    return h;
}

/* Does the bitset contain any accepting NFA state? */
static int bs_has_accept(const bitset *b, const nfa *n)
{
    int i;
    for (i = 0; i < n->nstates; i++) {
        if (bs_get(b, i) && n->states[i].accept)
            return 1;
    }
    return 0;
}

/* ─── ε-closure ─────────────────────────────────────────────────────────── */

static void eps_closure(const nfa *n, bitset *set)
{
    /* Iterative fixpoint: add ε-reachable states */
    int changed;
    do {
        changed = 0;
        int i;
        for (i = 0; i < n->nstates; i++) {
            if (!bs_get(set, i)) continue;
            nfa_state *s = &n->states[i];
            int j;
            for (j = 0; j < s->nedges; j++) {
                if (s->edges[j].kind == SYM_EPSILON) {
                    int tgt = (int)s->targets[j];
                    if (!bs_get(set, tgt)) {
                        bs_set(set, tgt);
                        changed = 1;
                    }
                }
            }
        }
    } while (changed);
}

/* ─── Move: NFA states reachable from set via edge matching byte ───────── */

static int sym_matches(const sym_edge *e, unsigned char byte)
{
    switch (e->kind) {
    case SYM_LITERAL:
        return e->u.literal == byte;
    case SYM_ANY:
        return 1;
    case SYM_CLASS:
        return (e->u.bitmap[byte >> 3] >> (byte & 7)) & 1;
    default:
        return 0;
    }
}

static void move(const nfa *n, const bitset *from, unsigned char byte,
                 bitset *to)
{
    bs_clear(to);
    int i;
    for (i = 0; i < n->nstates; i++) {
        if (!bs_get(from, i)) continue;
        nfa_state *s = &n->states[i];
        int j;
        for (j = 0; j < s->nedges; j++) {
            if (sym_matches(&s->edges[j], byte)) {
                bs_set(to, (int)s->targets[j]);
            }
        }
    }
    eps_closure(n, to);
}

/* ─── Subset construction: NFA → DFA ───────────────────────────────────── */

/*
 * DFA state set hash table for dedup.
 * Simple open-addressing hash map: key = hash of bitset, value = bitset + dfa_state_id.
 */
typedef struct {
    uint64_t  hash;
    bitset   *nfa_set;
    int       dfa_id;
    int       used;
} dfa_state_entry;

typedef struct {
    dfa_state_entry *entries;
    int              cap;
    int              used;
} dfa_state_map;

static int dsm_init(dfa_state_map *m)
{
    memset(m, 0, sizeof(*m));
    m->cap = 256;
    m->entries = calloc((size_t)m->cap, sizeof(dfa_state_entry));
    return m->entries ? 0 : -1;
}

static void dsm_free(dfa_state_map *m)
{
    int i;
    if (!m->entries) return;
    for (i = 0; i < m->cap; i++) {
        if (m->entries[i].used)
            bs_free(m->entries[i].nfa_set);
    }
    free(m->entries);
    memset(m, 0, sizeof(*m));
}

static int dsm_find_or_add(dfa_state_map *m, const bitset *bs, uint64_t hash,
                           int dfa_id, int *found)
{
    size_t idx = (size_t)(hash & (uint64_t)((size_t)m->cap - 1));

    for (;;) {
        dfa_state_entry *e = &m->entries[idx];
        if (!e->used) {
            /* Grow if > 75% full */
            if (m->used * 4 >= m->cap * 3) {
                int old_cap = m->cap;
                dfa_state_entry *old = m->entries;
                int new_cap = old_cap * 2;
                dfa_state_entry *ne = calloc((size_t)new_cap,
                                             sizeof(dfa_state_entry));
                if (!ne) return -1;

                /* Rehash */
                int i;
                for (i = 0; i < old_cap; i++) {
                    if (old[i].used) {
                        size_t ni = (size_t)(old[i].hash &
                                            (uint64_t)((size_t)new_cap - 1));
                        while (ne[ni].used)
                            ni = (ni + 1) & (size_t)(new_cap - 1);
                        ne[ni] = old[i];
                    }
                }
                free(old);
                m->entries = ne;
                m->cap = new_cap;
                /* retry from beginning */
                idx = (size_t)(hash & (uint64_t)((size_t)new_cap - 1));
                continue;
            }

            /* Insert new */
            e->hash     = hash;
            e->nfa_set  = bs_new(bs->nwords);
            if (!e->nfa_set) return -1;
            bs_copy(e->nfa_set, bs);
            e->dfa_id   = dfa_id;
            e->used     = 1;
            m->used++;
            *found = 0;
            return dfa_id;
        }

        if (e->hash == hash && bs_equal(e->nfa_set, bs)) {
            *found = 1;
            return e->dfa_id;
        }

        idx = (idx + 1) & (size_t)(m->cap - 1);
    }
}

/* ─── regex_compile ──────────────────────────────────────────────────────── */

regex_dfa *regex_compile(const char *pattern)
{
    regex_dfa *dfa = NULL;
    nfa         n;
    rx_parser  p;
    nfa_frag    frag;
    bitset     *cur = NULL, *next = NULL;
    dfa_state_map dsm;
    int       *dfa_queue = NULL;  /* BFS queue of DFA state ids */
    int        queue_head = 0, queue_tail = 0;
    int        queue_cap = 0;
    int        nwords;
    int        i, byte;
    uint32_t  *trans = NULL;
    uint8_t   *accept = NULL;
    int        n_dfa;

    /* ── 0. Validate ─────────────────────────────────────────────────── */
    if (!pattern || !*pattern) {
        dfa = calloc(1, sizeof(*dfa));
        if (dfa) dfa->errmsg = strdup("empty pattern");
        return dfa;
    }

    /* ── 1. Parse and build NFA ──────────────────────────────────────── */
    memset(&n, 0, sizeof(n));
    n.start = nfa_add_state(&n, 0);
    if (n.start < 0) goto fail;

    memset(&p, 0, sizeof(p));
    p.src = pattern;
    p.pos = pattern;

    if (rx_regex(&n, &p, &frag) != 0) {
        /* Parse error */
        dfa = calloc(1, sizeof(*dfa));
        if (dfa) {
            const char *msg = p.errmsg ? p.errmsg : "regex parse error";
            if (n.errmsg) msg = n.errmsg;
            dfa->errmsg = strdup(msg);
        }
        goto fail;
    }

    if (n.errmsg) {
        dfa = calloc(1, sizeof(*dfa));
        if (dfa) dfa->errmsg = strdup(n.errmsg);
        goto fail;
    }

    /* Connect start to fragment, make fragment's accept the final state */
    n.states[frag.accept].accept = 1;
    n.states[n.start].accept = 0;
    if (nfa_add_edge(&n, n.start, frag.start, sym_epsilon()) != 0) {
        dfa = calloc(1, sizeof(*dfa));
        if (dfa) dfa->errmsg = strdup(n.errmsg ? n.errmsg : "NFA error");
        goto fail;
    }

    /* ── 2. Subset construction ──────────────────────────────────────── */
    nwords = BITSET_WORDS(n.nstates);
    cur = bs_new(nwords);
    next = bs_new(nwords);
    if (!cur || !next) goto fail;

    if (dsm_init(&dsm) != 0) goto fail;

    /* Start with ε-closure of NFA start state */
    bs_set(cur, n.start);
    eps_closure(&n, cur);

    /* BFS queue for subset construction */
    queue_cap = 64;
    dfa_queue = malloc((size_t)queue_cap * sizeof(int));
    if (!dfa_queue) goto fail;

    /* Initial DFA state */
    {
        int found;
        uint64_t h = bs_hash(cur);
        if (dsm_find_or_add(&dsm, cur, h, 0, &found) != 0) goto fail;
        /* should not be found since dsm is empty */
        dfa_queue[queue_tail++] = 0;
    }

    /* Allocate initial DFA arrays */
    {
        size_t sz = (size_t)REGEX_DFA_MAX_STATES;
        trans  = malloc(sz * 256 * sizeof(uint32_t));
        accept = calloc(sz, sizeof(uint8_t));
        if (!trans || !accept) goto fail;
        /* Fill trans with DFA_DEAD */
        for (i = 0; i < REGEX_DFA_MAX_STATES * 256; i++)
            trans[i] = DFA_DEAD;
    }

    /* Set accept for initial state */
    if (bs_has_accept(cur, &n))
        accept[0] = 1;

    /* Main BFS loop */
    while (queue_head < queue_tail) {
        int dfa_s;

        /* Early-abort: reject pathological patterns quickly.  A pattern
         * that needs more than REGEX_DFA_ABORT_EARLY DFA states is far
         * beyond any useful regex; bailing here costs a fraction of the
         * time that reaching REGEX_DFA_MAX_STATES would (each state does
         * 256 byte-values x NFA closure).  REGEX_DFA_MAX_STATES remains
         * the absolute ceiling for memory safety (trans array sizing). */
        if (queue_tail >= REGEX_DFA_ABORT_EARLY) {
            dfa = calloc(1, sizeof(*dfa));
            if (dfa) dfa->errmsg = strdup(
                "regex state cap exceeded (8192)");
            goto fail;
        }

        dfa_s = dfa_queue[queue_head++];

        /* Restore the NFA set for this DFA state */
        {
            int found_entry = 0;
            for (i = 0; i < dsm.cap; i++) {
                if (dsm.entries[i].used && dsm.entries[i].dfa_id == dfa_s) {
                    bs_copy(cur, dsm.entries[i].nfa_set);
                    found_entry = 1;
                    break;
                }
            }
            if (!found_entry) continue; /* shouldn't happen */
        }

        for (byte = 0; byte < 256; byte++) {
            move(&n, cur, (unsigned char)byte, next);

            /* Check if next set is empty */
            {
                int any = 0;
                for (i = 0; i < nwords; i++) {
                    if (next->words[i]) { any = 1; break; }
                }
                if (!any) continue;  /* dead state */
            }

            /* Find or create DFA state for this NFA set */
            {
                uint64_t h = bs_hash(next);
                int found;
                int tgt = dsm_find_or_add(&dsm, next, h,
                                           queue_tail, &found);
                if (tgt < 0) goto fail;

                if (!found) {
                    /* New DFA state */
                    if (queue_tail >= REGEX_DFA_MAX_STATES) {
                        dfa = calloc(1, sizeof(*dfa));
                        if (dfa) dfa->errmsg = strdup(
                            "regex state cap exceeded (50000)");
                        goto fail;
                    }
                    if (queue_tail >= queue_cap) {
                        int nc = queue_cap * 2;
                        int *nq = realloc(dfa_queue,
                                          (size_t)nc * sizeof(int));
                        if (!nq) goto fail;
                        dfa_queue = nq;
                        queue_cap = nc;
                    }
                    dfa_queue[queue_tail] = queue_tail;
                    if (bs_has_accept(next, &n))
                        accept[queue_tail] = 1;
                    queue_tail++;
                }

                trans[(size_t)dfa_s * 256 + byte] = (uint32_t)tgt;
            }
        }
    }

    n_dfa = queue_tail;

    /* ── 3. Build result ─────────────────────────────────────────────── */
    dfa = calloc(1, sizeof(*dfa));
    if (!dfa) goto fail;

    dfa->n_states = (uint32_t)n_dfa;
    dfa->trans    = trans;     trans = NULL;
    dfa->accept   = accept;    accept = NULL;

    /* Cleanup */
    free(dfa_queue);
    dsm_free(&dsm);
    bs_free(cur);
    bs_free(next);
    free(n.states);
    free(p.errmsg);
    return dfa;

fail:
    if (!dfa) {
        dfa = calloc(1, sizeof(*dfa));
        if (dfa)
            dfa->errmsg = strdup(n.errmsg ? n.errmsg : "OOM");
    }
    free(dfa_queue);
    dsm_free(&dsm);
    bs_free(cur);
    bs_free(next);
    free(trans);
    free(accept);
    free(n.states);
    free(p.errmsg);
    return dfa;
}

void regex_dfa_free(regex_dfa *dfa)
{
    if (!dfa) return;
    free(dfa->trans);
    free(dfa->accept);
    free(dfa->errmsg);
    free(dfa);
}

/* ─── Visited hash set for product DFS ──────────────────────────────────── */

#define VISITED_INIT_CAP 1024

typedef struct {
    uint64_t *keys;    /* (dafsa_state << 32) | regex_state */
    int       cap;
    int       used;
} visited_set;

static int vs_init(visited_set *vs)
{
    vs->cap  = VISITED_INIT_CAP;
    vs->used = 0;
    vs->keys = calloc((size_t)vs->cap, sizeof(uint64_t));
    return vs->keys ? 0 : -1;
}

static void vs_free(visited_set *vs)
{
    free(vs->keys);
}

static uint64_t vs_hash_key(uint64_t k)
{
    uint64_t h = 14695981039346656037ULL;
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (uint8_t)(k & 0xFF);
        h *= 1099511628211ULL;
        k >>= 8;
    }
    return h;
}

static int vs_grow(visited_set *vs)
{
    int old_cap = vs->cap;
    int new_cap = old_cap * 2;
    uint64_t *old_keys = vs->keys;
    uint64_t *new_keys = calloc((size_t)new_cap, sizeof(uint64_t));
    if (!new_keys) return -1;

    int i;
    for (i = 0; i < old_cap; i++) {
        if (old_keys[i] != 0) {
            uint64_t h = vs_hash_key(old_keys[i]);
            size_t idx = (size_t)(h & (uint64_t)((size_t)new_cap - 1));
            while (new_keys[idx] != 0)
                idx = (idx + 1) & (size_t)(new_cap - 1);
            new_keys[idx] = old_keys[i];
        }
    }
    free(old_keys);
    vs->keys = new_keys;
    vs->cap  = new_cap;
    return 0;
}

/* Returns 1 if key already present (cycle detected), 0 if newly inserted */
static int vs_try_insert(visited_set *vs, uint64_t key)
{
    if (vs->used * 4 >= vs->cap * 3) {
        if (vs_grow(vs) != 0) return -1;
    }
    uint64_t h = vs_hash_key(key);
    size_t idx = (size_t)(h & (uint64_t)((size_t)vs->cap - 1));
    while (vs->keys[idx] != 0) {
        if (vs->keys[idx] == key) return 1; /* cycle detected */
        idx = (idx + 1) & (size_t)((size_t)vs->cap - 1);
    }
    vs->keys[idx] = key;
    vs->used++;
    return 0; /* newly inserted */
}

/* Remove a key (mark as not-on-stack). Returns 1 if found and removed. */
static int vs_remove(visited_set *vs, uint64_t key)
{
    uint64_t h = vs_hash_key(key);
    size_t idx = (size_t)(h & (uint64_t)((size_t)vs->cap - 1));
    while (vs->keys[idx] != 0) {
        if (vs->keys[idx] == key) {
            vs->keys[idx] = 0;
            vs->used--;
            return 1;
        }
        idx = (idx + 1) & (size_t)((size_t)vs->cap - 1);
    }
    return 0;
}

/* ─── Product DFS: in-memory DAFSA × regex DFA ──────────────────────────── */

typedef struct {
    const dafsa *d;
    const regex_dfa *dfa;
    regex_walk_cb cb;
    void *user;
    visited_set  visited;
    unsigned char buf[4096];
    long count;
} prod_ctx;

static int prod_dfs(prod_ctx *ctx, unsigned int dstate, uint32_t rstate,
                    size_t depth)
{
    const State *s;
    uint32_t j;
    int on_stack;
    uint64_t vkey;
    int rc;

    if (depth >= sizeof(ctx->buf)) return 0;

    /* Check if we're at an accepting pair */
    s = &ctx->d->states[dstate];
    if (s->is_final && ctx->dfa->accept[rstate]) {
        ctx->count++;
        if (ctx->cb(ctx->buf, depth, ctx->user) != 0)
            return 1;
    }

    /* Cycle detection: if already on recursion stack, stop */
    vkey = ((uint64_t)dstate << 32) | (uint64_t)rstate;
    on_stack = vs_try_insert(&ctx->visited, vkey);
    if (on_stack < 0) return -1;
    if (on_stack == 1) return 0; /* cycle — already on stack */

    /* Iterate over DAFSA edges from dstate */
    rc = 0;
    for (j = 0; j < s->ntrans; j++) {
        const Edge *e = &trans_arr_c(s)[j];
        unsigned char sym = e->sym;
        uint32_t next_rs = ctx->dfa->trans[(size_t)rstate * 256 + sym];
        if (next_rs == DFA_DEAD) continue;

        ctx->buf[depth] = sym;
        rc = prod_dfs(ctx, e->target, next_rs, depth + 1);
        if (rc != 0) break;
    }

    /* Remove from recursion stack */
    vs_remove(&ctx->visited, vkey);
    return rc;
}

long regex_dfa_walk(const dafsa *d, const regex_dfa *dfa,
                    regex_walk_cb cb, void *user)
{
    prod_ctx ctx;

    if (!d || !dfa || !cb) return -1;
    if (dfa->n_states == 0) return 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.d   = d;
    ctx.dfa = dfa;
    ctx.cb  = cb;
    ctx.user = user;

    if (vs_init(&ctx.visited) != 0) return -1;

    prod_dfs(&ctx, d->initial, 0, 0);

    vs_free(&ctx.visited);
    return ctx.count;
}

/* ─── Product DFS: mmap view DAFSA × regex DFA ──────────────────────────── */

typedef struct {
    const dafsa_view *v;
    const regex_dfa  *dfa;
    regex_walk_cb     cb;
    void             *user;
    visited_set       visited;
    unsigned char     buf[4096];
    long              count;
} prod_view_ctx;

static int prod_view_dfs(prod_view_ctx *ctx, uint32_t dstate,
                         uint32_t rstate, size_t depth)
{
    int on_stack;
    uint64_t vkey;
    int rc;

    if (depth >= sizeof(ctx->buf)) return 0;

    /* Accept check */
    if (ctx->v->final_bits[dstate / 8] & (uint8_t)(1u << (dstate % 8))) {
        if (ctx->dfa->accept[rstate]) {
            ctx->count++;
            if (ctx->cb(ctx->buf, depth, ctx->user) != 0)
                return 1;
        }
    }

    /* Cycle detection: if already on recursion stack, stop */
    vkey = ((uint64_t)dstate << 32) | (uint64_t)rstate;
    on_stack = vs_try_insert(&ctx->visited, vkey);
    if (on_stack < 0) return -1;
    if (on_stack == 1) return 0; /* cycle */

    /* Iterate edges via view_edge_next */
    rc = 0;
    {
        const uint8_t *cur = ctx->v->csr + ctx->v->state_off[dstate];
        unsigned char sym;
        uint32_t tgt;

        while (view_edge_next(ctx->v, dstate, &cur, &sym, &tgt) == 0) {
            uint32_t next_rs = ctx->dfa->trans[(size_t)rstate * 256 + sym];
            if (next_rs == DFA_DEAD) continue;

            ctx->buf[depth] = sym;
            rc = prod_view_dfs(ctx, tgt, next_rs, depth + 1);
            if (rc != 0) break;
        }
    }

    /* Remove from recursion stack */
    vs_remove(&ctx->visited, vkey);
    return rc;
}

long regex_dfa_walk_view(const dafsa_view *v, const regex_dfa *dfa,
                         regex_walk_cb cb, void *user)
{
    prod_view_ctx ctx;

    if (!v || !dfa || !cb) return -1;
    if (dfa->n_states == 0) return 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.v    = v;
    ctx.dfa  = dfa;
    ctx.cb   = cb;
    ctx.user = user;

    if (vs_init(&ctx.visited) != 0) return -1;

    prod_view_dfs(&ctx, v->initial, 0, 0);

    vs_free(&ctx.visited);
    return ctx.count;
}
