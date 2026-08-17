/*
 * parser.c — Datalog v1 parser implementation
 *
 * Hand-tokenized recursive-descent parser.  Supports:
 *   - Facts: predicate(const1, const2, ...). (no :-)
 *   - Rules: head :- body1, body2, ..., bodyN.
 *   - Variables (uppercase A-Z or _), constants (lowercase / quoted / int)
 *   - Negation (!atom) and aggregates (count/sum/min/max) parsed but flagged
 */

#include "parser.h"
#include "termstore.h"   /* TERM_BASE: reject int literals >= TERM_BASE */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ─── Token buffer ────────────────────────────────────────────────────── */

#define MAX_TOKENS 4096

struct parser {
    const char *src;        /* pointer into original source (not owned) */
    const char *pos;        /* current position */
    token     **tokens;     /* token buffer (array of token*) */
    int         ntok;       /* number of tokens */
    int         cur;        /* current token index */
    char       *src_owned;  /* owned copy of source (for parse_create) */
    uint32_t   *line_starts;/* heap: byte offset of the start of each line;
                               line i (1-based) starts at line_starts[i-1] */
    int         nlines;     /* number of entries in line_starts */
    uint32_t    err_off;    /* byte offset of the FIRST recorded error */
    int         has_err;    /* 1 once an error has been recorded */
    char        err_msg[256];/* formatted text of the FIRST recorded error */
};

/* Build p->line_starts: an array of the byte offset of each line's start,
 * indexed by (line - 1).  line 1 starts at 0; every '\n' begins the next line
 * at offset i+1.  On allocation failure nlines stays 0 and position lookup
 * degrades to (0,0) — harmless, purely additive. */
static void build_line_starts(parser *p)
{
    const char *s = p->src;
    int cap = 16, n = 0;

    p->line_starts = malloc((size_t)cap * sizeof(uint32_t));
    if (!p->line_starts) return;

    p->line_starts[n++] = 0;                 /* line 1 */
    while (*s) {
        if (*s == '\n') {
            if (n >= cap) {
                int nc = cap * 2;
                uint32_t *ns = realloc(p->line_starts, (size_t)nc * sizeof(uint32_t));
                if (!ns) { free(p->line_starts); p->line_starts = NULL; n = 0; break; }
                p->line_starts = ns;
                cap = nc;
            }
            p->line_starts[n++] = (uint32_t)((s + 1) - p->src);
        }
        s++;
    }
    p->nlines = n;
}

/* Map a 0-based byte offset to 1-based line/col via p->line_starts (binary
 * search: greatest line start <= off).  Sets *line and *col; (0,0) when the
 * table is absent/empty. */
static void off_to_pos(const parser *p, uint32_t off, int *line, int *col)
{
    int lo = 0, hi, best = 0;

    if (!p->line_starts || p->nlines <= 0) {
        *line = 0;
        *col = 0;
        return;
    }
    hi = p->nlines - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->line_starts[mid] <= off) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    *line = best + 1;
    *col = (int)(off - p->line_starts[best]) + 1;
}

/* Record a parse error: write the message to stderr BYTE-IDENTICALLY to the
 * pre-LSP parser (vfprintf), AND capture the offset + formatted message so
 * parse_last_error() can surface a position.  Only the FIRST error is kept. */
static void perr(parser *p, uint32_t off, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (p && !p->has_err) {
        p->has_err = 1;
        p->err_off = off;
        va_start(ap, fmt);
        vsnprintf(p->err_msg, sizeof(p->err_msg), fmt, ap);
        va_end(ap);
    }
}

/* ─── Character classification ────────────────────────────────────────── */

static int is_var_start(int c)
{
    return (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_var_char(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int is_pred_start(int c)
{
    return (c >= 'a' && c <= 'z');
}

static int is_pred_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* ─── Token creation ──────────────────────────────────────────────────── */

static token *tok_new(token_kind kind, const char *start, size_t len)
{
    token *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->kind = kind;
    if (start) {
        t->text = malloc(len + 1);
        if (!t->text) { free(t); return NULL; }
        memcpy(t->text, start, len);
        t->text[len] = '\0';
    }
    return t;
}

static void tok_free(token *t);

static token *tok_dup(const token *t)
{
    token *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->kind = t->kind;
    n->off = t->off;
    n->line = t->line;
    n->col = t->col;
    n->ival = t->ival;
    if (t->text) {
        n->text = strdup(t->text);
        if (!n->text) { free(n); return NULL; }
    }
    if (t->nchildren > 0) {
        int i;
        n->children = calloc((size_t)t->nchildren, sizeof(token *));
        if (!n->children) { free(n->text); free(n); return NULL; }
        n->nchildren = t->nchildren;
        for (i = 0; i < t->nchildren; i++) {
            n->children[i] = tok_dup(t->children[i]);
            if (!n->children[i]) { tok_free(n); return NULL; }
        }
    }
    if (t->tail) {
        n->tail = tok_dup(t->tail);
        if (!n->tail) { tok_free(n); return NULL; }
    }
    return n;
}

static void tok_free(token *t)
{
    int i;
    if (!t) return;
    if (t->children) {
        for (i = 0; i < t->nchildren; i++)
            tok_free(t->children[i]);
        free(t->children);
    }
    tok_free(t->tail);
    free(t->text);
    free(t);
}

/* ─── Lexer ───────────────────────────────────────────────────────────── */

/* Check if identifier is an aggregate keyword */
static int is_aggregate(const char *s, size_t len)
{
    if (len == 3 && strncmp(s, "sum", 3) == 0) return 1;
    if (len == 3 && strncmp(s, "min", 3) == 0) return 1;
    if (len == 3 && strncmp(s, "max", 3) == 0) return 1;
    if (len == 5 && strncmp(s, "count", 5) == 0) return 1;
    return 0;
}

static int lex(parser *p)
{
    const char *s;
    token *t;

    if (p->ntok >= MAX_TOKENS) return -1;

    s = p->pos;

    /* Skip whitespace and comments */
    while (1) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        /* Line comment: # to end of line */
        if (*s == '#') {
            while (*s && *s != '\n') s++;
            continue;
        }
        break;
    }

    /* Token start offset (0-based byte offset in the source), captured BEFORE
     * the per-kind code below so every token kind — including EOF and
     * punctuation — carries its position. */
    const char *tok_start = s;

    if (*s == '\0') {
        t = tok_new(TOK_EOF, NULL, 0);
        if (!t) return -1;
        t->off = (uint32_t)(tok_start - p->src);
        off_to_pos(p, t->off, &t->line, &t->col);
        p->tokens[p->ntok++] = t;
        p->pos = s;
        return 0;
    }

    /* Punctuation */
    if (*s == '(') {
        t = tok_new(TOK_LPAREN, NULL, 0);
        s++;
    } else if (*s == ')') {
        t = tok_new(TOK_RPAREN, NULL, 0);
        s++;
    } else if (*s == ',') {
        t = tok_new(TOK_COMMA, NULL, 0);
        s++;
    } else if (*s == '.') {
        t = tok_new(TOK_DOT, NULL, 0);
        s++;
    } else if (*s == '!') {
        if (*(s + 1) == '=') {
            t = tok_new(TOK_NE, NULL, 0);
            s += 2;
        } else {
            t = tok_new(TOK_NOT, NULL, 0);
            s++;
        }
    } else if (*s == '=') {
        t = tok_new(TOK_EQ, NULL, 0);
        s++;
    } else if (*s == '<') {
        if (*(s + 1) == '=') {
            t = tok_new(TOK_LE, NULL, 0);
            s += 2;
        } else {
            t = tok_new(TOK_LT, NULL, 0);
            s++;
        }
    } else if (*s == '>') {
        if (*(s + 1) == '=') {
            t = tok_new(TOK_GE, NULL, 0);
            s += 2;
        } else {
            t = tok_new(TOK_GT, NULL, 0);
            s++;
        }
    } else if (*s == '+') {
        t = tok_new(TOK_PLUS, NULL, 0);
        s++;
    } else if (*s == '-') {
        t = tok_new(TOK_MINUS, NULL, 0);
        s++;
    } else if (*s == '*') {
        t = tok_new(TOK_STAR, NULL, 0);
        s++;
    } else if (*s == '/') {
        t = tok_new(TOK_SLASH, NULL, 0);
        s++;
    } else if (*s == '%') {
        t = tok_new(TOK_PERCENT, NULL, 0);
        s++;
    } else if (*s == '~') {
        t = tok_new(TOK_TILDE, NULL, 0);
        s++;
    } else if (*s == '[') {
        t = tok_new(TOK_LBRACKET, NULL, 0);
        s++;
    } else if (*s == ']') {
        t = tok_new(TOK_RBRACKET, NULL, 0);
        s++;
    } else if (*s == '|') {
        t = tok_new(TOK_PIPE, NULL, 0);
        s++;
    } else if (*s == ':' && *(s + 1) == '-') {
        t = tok_new(TOK_COLONMINUS, NULL, 0);
        s += 2;
    } else if (*s == '\'') {
        /* Single-quoted string: read until closing quote */
        const char *start = s + 1;
        s++;
        while (*s && *s != '\'') s++;
        if (*s != '\'') {
            perr(p, (uint32_t)(s - p->src), "parser: unclosed single-quoted string at position %ld\n",
                    (long)(s - p->src));
            return -1;
        }
        t = tok_new(TOK_STRING, start, (size_t)(s - start));
        s++;
    } else if (*s == '"') {
        /* Quoted string: read until closing quote */
        const char *start = s + 1;
        s++; /* skip opening quote */
        while (*s && *s != '"') s++;
        if (*s != '"') {
            perr(p, (uint32_t)(s - p->src), "parser: unclosed string at position %ld\n",
                    (long)(s - p->src));
            return -1;
        }
        t = tok_new(TOK_IDENT, start, (size_t)(s - start));
        s++; /* skip closing quote */
    } else if (*s >= '0' && *s <= '9') {
        /* Integer literal */
        const char *start = s;
        unsigned long val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (unsigned long)(*s - '0');
            if (val > 0xFFFFFFFFUL) {
                perr(p, (uint32_t)(s - p->src), "parser: integer overflow at position %ld\n",
                        (long)(s - p->src));
                return -1;
            }
            s++;
        }
        t = tok_new(TOK_INT, start, (size_t)(s - start));
        if (t) t->ival = (uint32_t)val;
        /* v2-lists boundary: raw int literals are limited to 31 bits so they
         * cannot alias a list handle in [TERM_BASE, ...). */
        if (val >= TERM_BASE) {
            perr(p, (uint32_t)(s - p->src), 
                    "parser: integer literal %lu is out of range (raw ints "
                    "are limited to 31 bits, < %u, to keep list handles "
                    "distinct)\n",
                    val, (unsigned)TERM_BASE);
            return -1;
        }
    } else if (is_var_start(*s)) {
        /* Variable (uppercase or _) */
        const char *start = s;
        while (is_var_char(*s)) s++;
        t = tok_new(TOK_VAR, start, (size_t)(s - start));
    } else if (is_pred_start(*s)) {
        /* Identifier (lowercase start) */
        const char *start = s;
        while (is_pred_char(*s)) s++;
        if (is_aggregate(start, (size_t)(s - start))) {
            t = tok_new(TOK_AGGREGATE, start, (size_t)(s - start));
        } else {
            t = tok_new(TOK_IDENT, start, (size_t)(s - start));
        }
    } else {
        perr(p, (uint32_t)(s - p->src), "parser: unexpected character '%c' (0x%02x) at position %ld\n",
                *s, (unsigned char)*s, (long)(s - p->src));
        return -1;
    }

    if (!t) return -1;
    t->off = (uint32_t)(tok_start - p->src);
    off_to_pos(p, t->off, &t->line, &t->col);
    p->tokens[p->ntok++] = t;
    p->pos = s;
    return 0;
}

/* Tokenize the entire source. Returns 0 on success, -1 on error. */
static int tokenize(parser *p)
{
    while (1) {
        if (p->ntok > 0 &&
            p->tokens[p->ntok - 1]->kind == TOK_EOF)
            break;
        if (lex(p) != 0) return -1;
    }
    return 0;
}

/* ─── Token helpers ───────────────────────────────────────────────────── */

static token *peek(parser *p)
{
    if (p->cur < p->ntok)
        return p->tokens[p->cur];
    return NULL;
}

static token *peek_at(parser *p, int offset)
{
    int idx = p->cur + offset;
    if (idx >= 0 && idx < p->ntok)
        return p->tokens[idx];
    return NULL;
}

static token *advance(parser *p)
{
    token *t = peek(p);
    if (t && t->kind != TOK_EOF)
        p->cur++;
    return t;
}

static token *expect(parser *p, token_kind k)
{
    token *t = peek(p);
    if (!t || t->kind != k) {
        perr(p, peek(p) ? peek(p)->off : 0, "parser: expected token kind %d, got %d\n",
                (int)k, t ? (int)t->kind : -1);
        return NULL;
    }
    return advance(p);
}

/* ─── AST allocation ──────────────────────────────────────────────────── */

static atom *atom_new(void)
{
    return calloc(1, sizeof(atom));
}

static void atom_free(atom *a)
{
    int i;
    if (!a) return;
    free(a->pred);
    free(a->pattern);
    if (a->args) {
        for (i = 0; i < a->nargs; i++)
            tok_free(a->args[i]);
        free(a->args);
    }
    /* For aggregate atoms agg_op is OWNED (not borrowed from args). */
    if (a->aggregate)
        tok_free(a->agg_op);
    expr_free(a->arith);
    free(a);
}

void rule_free(rule *r)
{
    int i;
    if (!r) return;
    atom_free(r->head);
    if (r->body) {
        for (i = 0; i < r->nbody; i++)
            atom_free(r->body[i]);
        free(r->body);
    }
    free(r);
}

/* ─── Arithmetic expression tree (M9) ─────────────────────────────────── */

static expr *expr_new(expr_kind k)
{
    expr *e = calloc(1, sizeof(*e));
    if (e) e->kind = k;
    return e;
}

void expr_free(expr *e)
{
    if (!e) return;
    free(e->var);
    expr_free(e->l);
    expr_free(e->r);
    free(e);
}

expr *expr_clone(const expr *e)
{
    expr *n;
    if (!e) return NULL;
    n = expr_new(e->kind);
    if (!n) return NULL;
    n->ival = e->ival;
    n->op   = e->op;
    if (e->var) {
        n->var = strdup(e->var);
        if (!n->var) { expr_free(n); return NULL; }
    }
    if (e->l) {
        n->l = expr_clone(e->l);
        if (!n->l) { expr_free(n); return NULL; }
    }
    if (e->r) {
        n->r = expr_clone(e->r);
        if (!n->r) { expr_free(n); return NULL; }
    }
    return n;
}

/* ─── Recursive-descent parser ────────────────────────────────────────── */

/*
 * Grammar:
 *   program    := rule*
 *   rule       := head COLONMINUS body DOT   (rule)
 *              |  atom DOT                   (fact, stored as head-only rule with empty body)
 *   head       := atom
 *   body       := body_atom (COMMA body_atom)*
 *   body_atom  := [NOT] atom
 *   atom       := IDENT LPAREN arg_list RPAREN
 *   arg_list   := arg (COMMA arg)*
 *   arg        := IDENT | VAR | INT | AGGREGATE
 */

/* Forward declaration */
static atom *parse_atom(parser *p);
static token *parse_list(parser *p);

/* Parse an argument */
static token *parse_arg(parser *p)
{
    token *t = peek(p);
    if (!t) return NULL;
    if (t->kind == TOK_LBRACKET)
        return parse_list(p);   /* a list literal is ONE argument */
    if (t->kind == TOK_IDENT || t->kind == TOK_VAR ||
        t->kind == TOK_INT || t->kind == TOK_AGGREGATE) {
        return tok_dup(advance(p));
    }
    perr(p, peek(p) ? peek(p)->off : 0, "parser: expected argument (ident/var/int/list), got kind %d\n",
            t->kind);
    return NULL;
}

/* Parse one list element.  A TOK_VAR element (or a '|' tail, handled in
 * parse_list) marks this list as a Phase-2 PATTERN; constants + nested list
 * literals are the Phase-1 constant-list forms. */
static token *parse_list_element(parser *p)
{
    token *t = peek(p);
    if (!t) return NULL;
    if (t->kind == TOK_INT || t->kind == TOK_IDENT || t->kind == TOK_STRING ||
        t->kind == TOK_VAR)
        return tok_dup(advance(p));
    if (t->kind == TOK_LBRACKET)
        return parse_list(p);
    if (t->kind == TOK_PIPE) {
        perr(p, peek(p) ? peek(p)->off : 0, 
                "parser: unexpected '|' in list — it must follow at least one "
                "element ([X|Xs] head/tail pattern)\n");
        return NULL;
    }
    perr(p, peek(p) ? peek(p)->off : 0, "parser: unexpected token kind %d in list literal\n",
            (int)t->kind);
    return NULL;
}

/* Parse a list literal [e1, e2, ...] into a single TOK_LIST token.  The
 * current token is '['.  '[]' = empty list (nchildren == 0 = NIL). */
static token *parse_list(parser *p)
{
    token *lst = NULL;
    token **elems = NULL;
    int n = 0, cap = 0;

    if (!expect(p, TOK_LBRACKET)) return NULL;

    lst = calloc(1, sizeof(*lst));
    if (!lst) return NULL;
    lst->kind = TOK_LIST;

    if (peek(p) && peek(p)->kind == TOK_RBRACKET) {
        advance(p);              /* consume ']' */
        return lst;              /* empty list */
    }

    while (1) {
        token *e = parse_list_element(p);
        if (!e) goto fail;
        if (n >= cap) {
            int nc = cap ? cap * 2 : 4;
            token **ne = realloc(elems, (size_t)nc * sizeof(token *));
            if (!ne) { tok_free(e); goto fail; }
            elems = ne;
            cap = nc;
        }
        elems[n++] = e;

        {
            token *sep = peek(p);
            if (!sep) goto fail;
            if (sep->kind == TOK_COMMA) { advance(p); continue; }
            if (sep->kind == TOK_RBRACKET) { advance(p); break; }
            if (sep->kind == TOK_PIPE) {
                /* [X|Xs] head/tail pattern: exactly ONE tail token, which
                 * MUST be a variable (no [a|[b]] sugar). */
                token *tl;
                advance(p);
                tl = peek(p);
                if (!tl || tl->kind != TOK_VAR) {
                    perr(p, peek(p) ? peek(p)->off : 0, 
                            "parser: the tail after '|' in a list pattern "
                            "must be a variable (got kind %d)\n",
                            tl ? (int)tl->kind : -1);
                    goto fail;
                }
                advance(p);
                lst->tail = tok_dup(tl);
                if (!lst->tail) goto fail;
                if (!expect(p, TOK_RBRACKET)) goto fail;
                break;
            }
            perr(p, peek(p) ? peek(p)->off : 0, "parser: expected ',' or ']' in list literal, "
                    "got kind %d\n", (int)sep->kind);
            goto fail;
        }
    }

    lst->children = elems;
    lst->nchildren = n;
    return lst;

fail:
    {
        int k;
        for (k = 0; k < n; k++)
            tok_free(elems[k]);
        free(elems);
        tok_free(lst);
        return NULL;
    }
}

/* Parse atom: pred ( args ) */
static atom *parse_atom(parser *p)
{
    atom *a;
    token *pred;
    token *lp, *rp;

    pred = peek(p);
    if (!pred || pred->kind != TOK_IDENT) {
        if (pred)
            perr(p, peek(p) ? peek(p)->off : 0, "parser: expected predicate name, got kind %d\n",
                    pred->kind);
        else
            perr(p, peek(p) ? peek(p)->off : 0, "parser: expected predicate name, got EOF\n");
        return NULL;
    }
    advance(p);

    lp = peek(p);
    if (!lp || lp->kind != TOK_LPAREN) {
        perr(p, peek(p) ? peek(p)->off : 0, "parser: expected '(' after predicate, got kind %d\n",
                lp ? (int)lp->kind : -1);
        return NULL;
    }
    advance(p);

    a = atom_new();
    if (!a) return NULL;
    a->pred = strdup(pred->text);
    if (!a->pred) { atom_free(a); return NULL; }
    a->off = pred->off;
    a->line = pred->line;
    a->col = pred->col;

    /* Parse arguments */
    {
        token **args = NULL;
        int nargs = 0;
        int cap = 0;

        rp = peek(p);
        if (rp && rp->kind == TOK_RPAREN) {
            advance(p);
            /* empty arg list */
            a->args = NULL;
            a->nargs = 0;
            return a;
        }

        while (1) {
            token *arg = parse_arg(p);
            if (!arg) { atom_free(a); return NULL; }

            if (nargs >= cap) {
                int newcap = cap ? cap * 2 : 4;
                token **na = realloc(args, (size_t)newcap * sizeof(token *));
                if (!na) { tok_free(arg); atom_free(a); return NULL; }
                args = na;
                cap = newcap;
            }
            args[nargs++] = arg;

            rp = peek(p);
            if (!rp) { atom_free(a); return NULL; }

            if (rp->kind == TOK_COMMA) {
                advance(p);
                continue;
            } else if (rp->kind == TOK_RPAREN) {
                advance(p);
                break;
            } else {
                perr(p, peek(p) ? peek(p)->off : 0, "parser: expected ',' or ')', got kind %d\n",
                        rp->kind);
                atom_free(a);
                return NULL;
            }
        }

        a->args = args;
        a->nargs = nargs;
    }

    return a;
}

/* ─── Arithmetic expression parser (M9) ─────────────────────────────────
 *
 * Grammar (precedence climbing; * / % bind tighter than + -, all left-assoc):
 *   E      := term (('+'|'-') term)*
 *   term   := factor (('*'|'/'|'%') factor)*
 *   factor := TOK_VAR | TOK_INT | '(' E ')'
 *
 * TOK_IDENT / TOK_STRING never reach here (rejected as a bad factor), which is
 * the B6 arithmetic-on-symbol-constant reject (symbols have no numeric value).
 */

static expr *parse_factor(parser *p);
static expr *parse_term(parser *p);

static expr *parse_expr(parser *p)
{
    expr *l = parse_term(p);
    token *t;
    if (!l) return NULL;
    while (1) {
        t = peek(p);
        if (t && (t->kind == TOK_PLUS || t->kind == TOK_MINUS)) {
            char op = (t->kind == TOK_PLUS) ? '+' : '-';
            expr *r, *n;
            advance(p);
            r = parse_term(p);
            if (!r) { expr_free(l); return NULL; }
            n = expr_new(EX_BINOP);
            if (!n) { expr_free(l); expr_free(r); return NULL; }
            n->op = op; n->l = l; n->r = r;
            l = n;
        } else {
            break;
        }
    }
    return l;
}

static expr *parse_term(parser *p)
{
    expr *l = parse_factor(p);
    token *t;
    if (!l) return NULL;
    while (1) {
        t = peek(p);
        if (t && (t->kind == TOK_STAR || t->kind == TOK_SLASH ||
                  t->kind == TOK_PERCENT)) {
            char op = (t->kind == TOK_STAR) ? '*' :
                      (t->kind == TOK_SLASH) ? '/' : '%';
            expr *r, *n;
            advance(p);
            r = parse_factor(p);
            if (!r) { expr_free(l); return NULL; }
            n = expr_new(EX_BINOP);
            if (!n) { expr_free(l); expr_free(r); return NULL; }
            n->op = op; n->l = l; n->r = r;
            l = n;
        } else {
            break;
        }
    }
    return l;
}

static expr *parse_factor(parser *p)
{
    token *t = peek(p);
    expr *e;

    if (!t) {
        perr(p, peek(p) ? peek(p)->off : 0, "parser: unexpected end of input in expression\n");
        return NULL;
    }
    if (t->kind == TOK_VAR || t->kind == TOK_INT) {
        advance(p);
        e = expr_new((t->kind == TOK_VAR) ? EX_VAR : EX_INT);
        if (!e) return NULL;
        if (t->kind == TOK_VAR) {
            e->var = strdup(t->text);
            if (!e->var) { expr_free(e); return NULL; }
        } else {
            e->ival = t->ival;
        }
        return e;
    }
    if (t->kind == TOK_LPAREN) {
        advance(p);
        e = parse_expr(p);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN)) { expr_free(e); return NULL; }
        return e;
    }
    perr(p, peek(p) ? peek(p)->off : 0, 
            "parser: expected variable, integer, or '(' in arithmetic "
            "expression, got kind %d\n", (int)t->kind);
    return NULL;
}

/* M9-strings: producing builtin names, recognized only in the `VAR = name(...)`
 * form.  Filter builtins (prefix/suffix/contains) need NO parser change — they
 * lex as ordinary lowercase function-call atoms and the compiler classifies
 * them. */
static int is_str_producing_name(const char *s)
{
    return s && (strcmp(s, "concat") == 0 || strcmp(s, "length") == 0 ||
                 strcmp(s, "lower") == 0 || strcmp(s, "upper") == 0);
}

/* M9/v2-lists: LIST-producing builtin names, recognized only in the
 * `VAR = name(...)` form.  cons(H,T)/car(L)/cdr(L)/append(A,B).  (length(L)
 * is a STRING-producing name but its operand parser additionally accepts a
 * list literal — see is_len above.) */
static int is_list_producing_name(const char *s)
{
    return s && (strcmp(s, "cons") == 0 || strcmp(s, "car") == 0 ||
                 strcmp(s, "cdr") == 0 || strcmp(s, "append") == 0);
}

/* Parse a body atom: [ ! ] atom, where the atom may also be an equality
 * (VAR = VAR), a comparison (VAR <op> VAR/INT), an aggregate
 * (VAR = count()/sum(X)/min(X)/max(X)), or an arithmetic assignment
 * (VAR = E). */
static atom *parse_body_atom(parser *p)
{
    int negated = 0;
    token *t = peek(p);
    int after_not;

    if (t && t->kind == TOK_NOT) {
        negated = 1;
        advance(p);
    }
    after_not = p->cur;

    /* Special forms:
     *   VAR = VAR         equality
     *   VAR = agg(args)   aggregate
     *   VAR = E           arithmetic assignment (E is an expression)
     *   VAR <op> operand  comparison (< <= > >=: VAR/INT; !=: also IDENT)
     *   [pat] = RHS       list assignment (sugar over X=car(L), Xs=cdr(L)) */
    t = peek(p);
    if (t && t->kind == TOK_LBRACKET) {
        /* list assignment [X|Xs] = L: parse the list pattern, then require
         * '=' and a variable/constant RHS (the list VALUE being destructured).
         * The compiler lowers it to emit_pattern (car/cdr/equality). */
        token *lst = parse_list(p);
        if (!lst) return NULL;
        {
            token *eq = peek(p);
            if (eq && eq->kind == TOK_EQ) {
                advance(p);          /* = */
                token *rhs = peek(p);
                if (!rhs || (rhs->kind != TOK_VAR &&
                             rhs->kind != TOK_INT &&
                             rhs->kind != TOK_IDENT &&
                             rhs->kind != TOK_STRING &&
                             rhs->kind != TOK_LBRACKET)) {
                    perr(p, peek(p) ? peek(p)->off : 0, 
                        "parser: expected variable or constant after '=' in "
                        "list assignment\n");
                    tok_free(lst); return NULL;
                }
                token *rv;
                if (rhs->kind == TOK_LBRACKET)
                    rv = parse_list(p);         /* [X|_] = [1,2] */
                else
                    rv = tok_dup(advance(p));
                if (!rv) { tok_free(lst); return NULL; }
                atom *a = atom_new();
                if (!a) { tok_free(lst); tok_free(rv); return NULL; }
                a->pred = strdup("=");
                a->args = malloc(2 * sizeof(token *));
                if (!a->pred || !a->args) {
                    atom_free(a); tok_free(lst); tok_free(rv); return NULL;
                }
                a->args[0] = lst;    /* the pattern (may contain vars/tail) */
                a->args[1] = rv;
                a->nargs = 2;
                a->negated = negated;
                return a;
            }
        }
        /* a bare list literal is not a valid body atom on its own */
        perr(p, peek(p) ? peek(p)->off : 0, "parser: expected '=' after list pattern (list "
                "assignment [X|Xs] = L)\n");
        tok_free(lst);
        return NULL;
    }
    if (t && t->kind == TOK_VAR) {
        token *op = peek_at(p, 1);
        if (op && op->kind == TOK_EQ) {
            token *rhs = peek_at(p, 2);
            /* `X = Y` is equality only if Y is NOT followed by an arithmetic
             * operator — otherwise it is `X = <expr starting with Y>`. */
            int is_simple_eq = 0;
            if (rhs && rhs->kind == TOK_VAR) {
                token *after = peek_at(p, 3);
                if (!(after && (after->kind == TOK_PLUS ||
                                after->kind == TOK_MINUS ||
                                after->kind == TOK_STAR ||
                                after->kind == TOK_SLASH ||
                                after->kind == TOK_PERCENT)))
                    is_simple_eq = 1;
            }
            if (is_simple_eq) {
                /* equality: VAR = VAR */
                token *lv = advance(p);
                advance(p);          /* = */
                token *rv = advance(p);
                atom *a = atom_new();
                if (!a) return NULL;
                a->pred = strdup("=");
                a->args = malloc(2 * sizeof(token *));
                if (!a->pred || !a->args) { atom_free(a); return NULL; }
                a->args[0] = tok_dup(lv);
                a->args[1] = tok_dup(rv);
                a->nargs = 2;
                a->negated = negated;
                return a;
            } else if (rhs && rhs->kind == TOK_AGGREGATE) {
                /* aggregate: VAR = agg ( [VAR] ) */
                token *res = advance(p);   /* result var */
                advance(p);                /* = */
                token *agop = advance(p);  /* aggregate op token */
                if (!expect(p, TOK_LPAREN)) return NULL;

                atom *a = atom_new();
                if (!a) return NULL;
                a->aggregate = 1;
                a->pred = strdup(res->text);   /* result var name */
                a->agg_op = tok_dup(agop);
                a->negated = negated;
                if (!a->pred || !a->agg_op) { atom_free(a); return NULL; }

                if (!strcmp(agop->text, "count")) {
                    /* count() requires empty parens */
                    token *rp = peek(p);
                    if (!rp || rp->kind != TOK_RPAREN) {
                        perr(p, peek(p) ? peek(p)->off : 0, 
                            "parser: 'count' requires no arguments near '%s'\n",
                            agop->text);
                        atom_free(a); return NULL;
                    }
                    advance(p);
                    a->nargs = 0;
                    a->args  = NULL;
                } else {
                    /* sum/min/max require exactly one variable argument */
                    token *src = peek(p);
                    if (!src || src->kind != TOK_VAR) {
                        perr(p, peek(p) ? peek(p)->off : 0, 
                            "parser: aggregate '%s' requires one variable argument near '%s'\n",
                            agop->text, agop->text);
                        atom_free(a); return NULL;
                    }
                    advance(p);
                    if (!expect(p, TOK_RPAREN)) { atom_free(a); return NULL; }
                    a->args = malloc(sizeof(token *));
                    if (!a->args) { atom_free(a); return NULL; }
                    a->args[0] = tok_dup(src);
                    a->nargs = 1;
                }
                return a;
            } else if (rhs && rhs->kind == TOK_IDENT &&
                       (is_str_producing_name(rhs->text) ||
                        is_list_producing_name(rhs->text)) &&
                       peek_at(p, 3) && peek_at(p, 3)->kind == TOK_LPAREN) {
                /* Producing builtin: string (concat/length/lower/upper) or
                 * list (cons/car/cdr/append).  See the operand checks below. */
                token *res = advance(p);   /* result var */
                advance(p);                /* = */
                token *nm  = advance(p);   /* builtin name */
                int   is_list = is_list_producing_name(nm->text);
                int   is_len  = (nm->text && strcmp(nm->text, "length") == 0);
                if (!expect(p, TOK_LPAREN)) return NULL;

                atom *a = atom_new();
                if (!a) return NULL;
                a->pred = strdup(nm->text);
                a->off = nm->off;
                a->line = nm->line;
                a->col = nm->col;
                a->negated = negated;
                if (!a->pred) { atom_free(a); return NULL; }

                token **args = NULL;
                int nargs = 0, cap = 4, fail = 0;
                args = realloc(args, (size_t)cap * sizeof(token *));
                if (!args) { atom_free(a); return NULL; }
                args[nargs++] = tok_dup(res);
                if (!args[0]) { fail = 1; }

                while (!fail) {
                    token *op = peek(p);
                    if (!op) { fail = 1; break; }
                    if (op->kind == TOK_RPAREN) { advance(p); break; }
                    if ((is_list || is_len) && op->kind == TOK_LBRACKET) {
                        /* nested list-literal operand (list builtins + length) */
                        if (nargs >= cap) {
                            int nc = cap * 2;
                            token **na = realloc(args, (size_t)nc * sizeof(token *));
                            if (!na) { fail = 1; break; }
                            args = na; cap = nc;
                        }
                        args[nargs++] = parse_list(p);
                        if (!args[nargs - 1]) { fail = 1; break; }
                    } else {
                        int ok;
                        if (is_list)
                            ok = (op->kind == TOK_VAR || op->kind == TOK_INT ||
                                  op->kind == TOK_IDENT || op->kind == TOK_STRING);
                        else
                            ok = (op->kind == TOK_VAR || op->kind == TOK_IDENT);
                        if (!ok) {
                            if (is_list)
                                perr(p, peek(p) ? peek(p)->off : 0, 
                                    "parser: expected variable or constant "
                                    "(int/string/list) operand in list builtin "
                                    "'%s', got kind %d\n",
                                    nm->text, (int)op->kind);
                            else
                                perr(p, peek(p) ? peek(p)->off : 0, 
                                    "parser: expected variable or string "
                                    "constant operand in string builtin '%s', "
                                    "got kind %d\n",
                                    nm->text, (int)op->kind);
                            fail = 1; break;
                        }
                        if (nargs >= cap) {
                            int nc = cap * 2;
                            token **na = realloc(args, (size_t)nc * sizeof(token *));
                            if (!na) { fail = 1; break; }
                            args = na; cap = nc;
                        }
                        args[nargs++] = tok_dup(advance(p));
                        if (!args[nargs - 1]) { fail = 1; break; }
                    }

                    token *sep = peek(p);
                    if (!sep) { fail = 1; break; }
                    if (sep->kind == TOK_COMMA) { advance(p); continue; }
                    if (sep->kind == TOK_RPAREN) { advance(p); break; }
                    perr(p, peek(p) ? peek(p)->off : 0, "parser: expected ',' or ')' in string "
                            "builtin '%s', got kind %d\n",
                            nm->text, (int)sep->kind);
                    fail = 1; break;
                }

                if (fail) {
                    int k;
                    for (k = 0; k < nargs; k++) tok_free(args[k]);
                    free(args);
                    atom_free(a);
                    return NULL;
                }
                a->args = args;
                a->nargs = nargs;
                return a;
            } else if (rhs) {
                /* arithmetic assignment: VAR = E */
                token *res = advance(p);   /* result var */
                advance(p);                /* = */
                expr *e = parse_expr(p);
                if (!e) return NULL;
                atom *a = atom_new();
                if (!a) { expr_free(e); return NULL; }
                a->pred = strdup("=");
                a->args = malloc(sizeof(token *));
                if (!a->pred || !a->args) { expr_free(e); atom_free(a); return NULL; }
                a->args[0] = tok_dup(res);
                if (!a->args[0]) { expr_free(e); atom_free(a); return NULL; }
                a->nargs = 1;
                a->arith = e;
                a->negated = negated;
                return a;
            }
            /* rhs == NULL (EOF after '=') → fall through to error path */
        } else if (op && (op->kind == TOK_LT || op->kind == TOK_LE ||
                          op->kind == TOK_GT || op->kind == TOK_GE ||
                          op->kind == TOK_NE)) {
            /* comparison: VAR <op> operand */
            token *rhs = peek_at(p, 2);
            if (rhs) {
                const char *optext;
                switch (op->kind) {
                    case TOK_LT: optext = "<";  break;
                    case TOK_LE: optext = "<="; break;
                    case TOK_GT: optext = ">";  break;
                    case TOK_GE: optext = ">="; break;
                    default:     optext = "!="; break;
                }
                if (rhs->kind != TOK_VAR && rhs->kind != TOK_INT &&
                    !(op->kind == TOK_NE && rhs->kind == TOK_IDENT)) {
                    if (rhs->kind == TOK_IDENT)
                        perr(p, peek(p) ? peek(p)->off : 0, 
                            "parser: symbol constant not allowed in ordering "
                            "comparison '%s' (only in =/!=)\n", optext);
                    else
                        perr(p, peek(p) ? peek(p)->off : 0, 
                            "parser: expected variable or integer operand for "
                            "comparison '%s', got kind %d\n",
                            optext, (int)rhs->kind);
                    return NULL;
                }
                token *lv = advance(p);
                advance(p);          /* operator */
                token *rv = advance(p);
                atom *a = atom_new();
                if (!a) return NULL;
                a->pred = strdup(optext);
                a->args = malloc(2 * sizeof(token *));
                if (!a->pred || !a->args) { atom_free(a); return NULL; }
                a->args[0] = tok_dup(lv);
                a->args[1] = tok_dup(rv);
                a->nargs = 2;
                a->negated = negated;
                return a;
            }
            /* rhs == NULL → fall through to error path */
        }
    }

    /* fallback: normal atom (possibly negated) */
    p->cur = after_not;
    {
        atom *a = parse_atom(p);
        if (!a) return NULL;
        a->negated = negated;

        /* M5: check for ~ 'pattern' suffix */
        {
            token *t = peek(p);
            if (t && t->kind == TOK_TILDE) {
                advance(p);
                t = peek(p);
                if (!t || t->kind != TOK_STRING) {
                    perr(p, peek(p) ? peek(p)->off : 0, "parser: expected pattern string "
                            "after '~'\n");
                    atom_free(a);
                    return NULL;
                }
                a->pattern = strdup(t->text);
                if (!a->pattern) { atom_free(a); return NULL; }
                advance(p);
            }
        }

        return a;
    }
}

/* Parse body: body_atom (, body_atom)* */
static int parse_body(parser *p, rule *r)
{
    atom **body = NULL;
    int nbody = 0;
    int cap = 0;

    while (1) {
        atom *ba = parse_body_atom(p);
        if (!ba) return -1;

        if (ba->negated) r->has_negation = 1;
        if (ba->aggregate) r->has_aggregate = 1;

        if (nbody >= cap) {
            int newcap = cap ? cap * 2 : 4;
            atom **na = realloc(body, (size_t)newcap * sizeof(atom *));
            if (!na) { atom_free(ba); return -1; }
            body = na;
            cap = newcap;
        }
        body[nbody++] = ba;

        token *t = peek(p);
        if (t && t->kind == TOK_COMMA) {
            advance(p);
            continue;
        }
        break;
    }

    r->body = body;
    r->nbody = nbody;
    return 0;
}

/* Parse one rule or fact */
static rule *parse_one_rule(parser *p)
{
    rule *r = NULL;
    token *t;

    /* Check for EOF */
    t = peek(p);
    if (!t || t->kind == TOK_EOF)
        return NULL;

    r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    /* Atom first */
    r->head = parse_atom(p);
    if (!r->head) { rule_free(r); return NULL; }
    r->off = r->head->off;

    t = peek(p);
    if (!t) { rule_free(r); return NULL; }

    if (t->kind == TOK_COLONMINUS) {
        /* Rule: head :- body */
        advance(p);
        if (parse_body(p, r) != 0) { rule_free(r); return NULL; }
    }

    /* Expect '.' */
    if (!expect(p, TOK_DOT)) { rule_free(r); return NULL; }

    return r;
}

/* ─── Public API ──────────────────────────────────────────────────────── */

parser *parse_create(const char *source)
{
    parser *p;

    if (!source) return NULL;

    p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->src_owned = strdup(source);
    if (!p->src_owned) { free(p); return NULL; }
    p->src = p->src_owned;
    p->pos = p->src;
    build_line_starts(p);

    p->tokens = calloc(MAX_TOKENS, sizeof(token *));
    if (!p->tokens) { free(p->src_owned); free(p); return NULL; }

    if (tokenize(p) != 0) {
        parse_free(p);
        return NULL;
    }

    return p;
}

/* LSP-only variant: identical to parse_create() EXCEPT that a lexer error does
 * NOT free the parser — it is returned so the caller can read the error
 * position via parse_last_error() before freeing it.  The CLI/playground keep
 * using parse_create(), whose observable behaviour is unchanged. */
parser *parse_create_reporting(const char *source)
{
    parser *p;

    if (!source) return NULL;

    p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->src_owned = strdup(source);
    if (!p->src_owned) { free(p); return NULL; }
    p->src = p->src_owned;
    p->pos = p->src;
    build_line_starts(p);

    p->tokens = calloc(MAX_TOKENS, sizeof(token *));
    if (!p->tokens) { free(p->src_owned); free(p); return NULL; }

    if (tokenize(p) != 0)
        return p;   /* keep the parser: the caller reads parse_last_error() */

    return p;
}

const char *parse_last_error(const parser *p, uint32_t *off)
{
    if (off) *off = 0;
    if (!p || !p->has_err) return NULL;
    if (off) *off = p->err_off;
    return p->err_msg;
}

rule **parse_rules(parser *p, int *n_rules)
{
    rule **rules = NULL;
    int nr = 0;
    int cap = 0;

    if (!p || !n_rules) return NULL;
    *n_rules = 0;

    while (1) {
        rule *r = parse_one_rule(p);
        if (!r) break;

        if (nr >= cap) {
            int newcap = cap ? cap * 2 : 4;
            rule **na = realloc(rules, (size_t)newcap * sizeof(rule *));
            if (!na) { rule_free(r); goto error; }
            rules = na;
            cap = newcap;
        }
        rules[nr++] = r;
    }

    /* Check for parse errors (non-EOF stop) */
    {
        token *t = peek(p);
        if (t && t->kind != TOK_EOF) {
            perr(p, peek(p) ? peek(p)->off : 0, "parse: trailing tokens\n");
            goto error;
        }
    }

    if (nr == 0) {
        perr(p, peek(p) ? peek(p)->off : 0, "parse: no rules found\n");
        free(rules);
        return NULL;
    }

    *n_rules = nr;
    return rules;

error:
    {
        int i;
        for (i = 0; i < nr; i++) rule_free(rules[i]);
        free(rules);
        return NULL;
    }
}

void parse_free(parser *p)
{
    int i;
    if (!p) return;
    for (i = 0; i < p->ntok; i++)
        tok_free(p->tokens[i]);
    free(p->tokens);
    free(p->line_starts);
    free(p->src_owned);
    free(p);
}
