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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Token buffer ────────────────────────────────────────────────────── */

#define MAX_TOKENS 4096

struct parser {
    const char *src;        /* pointer into original source (not owned) */
    const char *pos;        /* current position */
    token     **tokens;     /* token buffer (array of token*) */
    int         ntok;       /* number of tokens */
    int         cur;        /* current token index */
    char       *src_owned;  /* owned copy of source (for parse_create) */
};

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
    if (start && len > 0) {
        t->text = malloc(len + 1);
        if (!t->text) { free(t); return NULL; }
        memcpy(t->text, start, len);
        t->text[len] = '\0';
    }
    return t;
}

static token *tok_dup(const token *t)
{
    token *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->kind = t->kind;
    n->ival = t->ival;
    if (t->text) {
        n->text = strdup(t->text);
        if (!n->text) { free(n); return NULL; }
    }
    return n;
}

static void tok_free(token *t)
{
    if (!t) return;
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

    if (*s == '\0') {
        t = tok_new(TOK_EOF, NULL, 0);
        if (!t) return -1;
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
        t = tok_new(TOK_NOT, NULL, 0);
        s++;
    } else if (*s == '=') {
        t = tok_new(TOK_EQ, NULL, 0);
        s++;
    } else if (*s == '~') {
        t = tok_new(TOK_TILDE, NULL, 0);
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
            fprintf(stderr, "parser: unclosed single-quoted string at position %ld\n",
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
            fprintf(stderr, "parser: unclosed string at position %ld\n",
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
                fprintf(stderr, "parser: integer overflow at position %ld\n",
                        (long)(s - p->src));
                return -1;
            }
            s++;
        }
        t = tok_new(TOK_INT, start, (size_t)(s - start));
        if (t) t->ival = (uint32_t)val;
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
        fprintf(stderr, "parser: unexpected character '%c' (0x%02x) at position %ld\n",
                *s, (unsigned char)*s, (long)(s - p->src));
        return -1;
    }

    if (!t) return -1;
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
        fprintf(stderr, "parser: expected token kind %d, got %d\n",
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

/* Parse an argument */
static token *parse_arg(parser *p)
{
    token *t = peek(p);
    if (!t) return NULL;
    if (t->kind == TOK_IDENT || t->kind == TOK_VAR ||
        t->kind == TOK_INT || t->kind == TOK_AGGREGATE) {
        return tok_dup(advance(p));
    }
    fprintf(stderr, "parser: expected argument (ident/var/int), got kind %d\n",
            t->kind);
    return NULL;
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
            fprintf(stderr, "parser: expected predicate name, got kind %d\n",
                    pred->kind);
        else
            fprintf(stderr, "parser: expected predicate name, got EOF\n");
        return NULL;
    }
    advance(p);

    lp = peek(p);
    if (!lp || lp->kind != TOK_LPAREN) {
        fprintf(stderr, "parser: expected '(' after predicate, got kind %d\n",
                lp ? (int)lp->kind : -1);
        return NULL;
    }
    advance(p);

    a = atom_new();
    if (!a) return NULL;
    a->pred = strdup(pred->text);
    if (!a->pred) { atom_free(a); return NULL; }

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
                fprintf(stderr, "parser: expected ',' or ')', got kind %d\n",
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

/* Parse a body atom: [ ! ] atom, where the atom may also be an equality
 * (VAR = VAR) or an aggregate (VAR = count()/sum(X)/min(X)/max(X)). */
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

    /* Special forms: VAR = VAR (equality) or VAR = agg(args) (aggregate) */
    t = peek(p);
    if (t && t->kind == TOK_VAR) {
        token *eq = peek_at(p, 1);
        if (eq && eq->kind == TOK_EQ) {
            token *rhs = peek_at(p, 2);
            if (rhs && rhs->kind == TOK_VAR) {
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
                token *op  = advance(p);   /* aggregate op token */
                if (!expect(p, TOK_LPAREN)) return NULL;

                atom *a = atom_new();
                if (!a) return NULL;
                a->aggregate = 1;
                a->pred = strdup(res->text);   /* result var name */
                a->agg_op = tok_dup(op);
                a->negated = negated;
                if (!a->pred || !a->agg_op) { atom_free(a); return NULL; }

                if (!strcmp(op->text, "count")) {
                    /* count() requires empty parens */
                    token *rp = peek(p);
                    if (!rp || rp->kind != TOK_RPAREN) {
                        fprintf(stderr,
                            "parser: 'count' requires no arguments near '%s'\n",
                            op->text);
                        atom_free(a); return NULL;
                    }
                    advance(p);
                    a->nargs = 0;
                    a->args  = NULL;
                } else {
                    /* sum/min/max require exactly one variable argument */
                    token *src = peek(p);
                    if (!src || src->kind != TOK_VAR) {
                        fprintf(stderr,
                            "parser: aggregate '%s' requires one variable argument near '%s'\n",
                            op->text, op->text);
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
            }
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
                    fprintf(stderr, "parser: expected pattern string "
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

    p->tokens = calloc(MAX_TOKENS, sizeof(token *));
    if (!p->tokens) { free(p->src_owned); free(p); return NULL; }

    if (tokenize(p) != 0) {
        parse_free(p);
        return NULL;
    }

    return p;
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
            fprintf(stderr, "parse: trailing tokens\n");
            goto error;
        }
    }

    if (nr == 0) {
        fprintf(stderr, "parse: no rules found\n");
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
    free(p->src_owned);
    free(p);
}
