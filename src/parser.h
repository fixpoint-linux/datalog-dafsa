/*
 * parser.h — Datalog v1 parser
 *
 * Parses: facts, rules with conjunction, variables (uppercase), constants
 * (lowercase / quoted strings / ints), `:-` head/body separator, `,` body
 * separator, `(a,b,...)` atom args.
 *
 * Parses negation (`!atom`) and aggregates syntactically but they are
 * rejected at compile time (M2/M3).
 */
#ifndef PARSER_H
#define PARSER_H

#include <stdint.h>

/* ─── AST node types ──────────────────────────────────────────────────── */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,       /* lowercase or quoted: constant or predicate name */
    TOK_VAR,         /* uppercase: variable */
    TOK_INT,         /* integer literal */
    TOK_COLONMINUS,  /* :- */
    TOK_COMMA,       /* , */
    TOK_DOT,         /* . */
    TOK_LPAREN,      /* ( */
    TOK_RPAREN,      /* ) */
    TOK_NOT,         /* ! */
    TOK_AGGREGATE,   /* count, sum, min, max */
    TOK_EQ,          /* = */
    TOK_LT,          /* <  (M9 ordering comparison) */
    TOK_LE,          /* <= */
    TOK_GT,          /* >  */
    TOK_GE,          /* >= */
    TOK_NE,          /* != */
    TOK_PLUS,        /* +  (M9 arithmetic) */
    TOK_MINUS,       /* -  */
    TOK_STAR,        /* *  */
    TOK_SLASH,       /* /  */
    TOK_PERCENT,     /* %  */
    TOK_TILDE,       /* ~ */
    TOK_STRING,      /* 'quoted string' — for regex patterns */
    TOK_LBRACKET,    /* [  (v2 lists) */
    TOK_RBRACKET,    /* ]  (v2 lists) */
    TOK_PIPE,        /* |  (v2 lists — [X|Xs] deferred to Phase 2) */
    TOK_LIST,        /* [e1,e2,...] literal — a single argument token whose
                        children are the element tokens */
} token_kind;

typedef struct token {
    token_kind  kind;
    char       *text;       /* owned; NULL for punctuation tokens */
    uint32_t    ival;       /* integer value (TOK_INT only) */
    struct token **children;/* TOK_LIST: owned element tokens (NULL otherwise) */
    int          nchildren; /* TOK_LIST: element count (0 = NIL) */
} token;

/* ─── Arithmetic expression tree (M9) ─────────────────────────────────── */

typedef enum { EX_INT, EX_VAR, EX_BINOP } expr_kind;

typedef struct expr {
    expr_kind   kind;
    uint32_t    ival;       /* EX_INT: literal value */
    char       *var;        /* EX_VAR: owned variable name */
    char        op;         /* EX_BINOP: '+', '-', '*', '/', '%' */
    struct expr *l, *r;     /* EX_BINOP children */
} expr;

/* Deep-copy an expression tree (NULL in → NULL out). */
expr *expr_clone(const expr *e);

/* Free an expression tree (NULL-safe). */
void  expr_free(expr *e);

/* An atom: predicate(args...). args are tokens (TOK_IDENT/TOK_VAR/TOK_INT). */
typedef struct {
    char   *pred;          /* predicate name */
    token **args;          /* array of argument tokens */
    int     nargs;         /* number of arguments */
    int     negated;       /* 1 if preceded by ! */
    int     aggregate;     /* 1 if aggregate (count/sum/min/max) */
    token  *agg_op;        /* aggregate operator token, NULL if not agg */
    char   *pattern;       /* M5: regex pattern string (from ~ '...'), or NULL */
    expr   *arith;         /* M9: arithmetic expr tree for `X = E` atoms,
                              NULL for every other atom kind */
} atom;

/* A rule: head :- body1, body2, ..., bodyN. */
typedef struct {
    atom   *head;
    atom  **body;          /* array of body atom pointers */
    int     nbody;
    int     has_negation;  /* any body atom negated? */
    int     has_aggregate; /* any body atom an aggregate? */
} rule;

/* ─── Parser ──────────────────────────────────────────────────────────── */

typedef struct parser parser;

/* Create a parser for a Datalog source string (single rule or .dl file).
 * The source is copied; caller may free the original after this call. */
parser *parse_create(const char *source);

/* Parse all rules from the source.  Returns array of rules, sets *n_rules.
 * Returns NULL on parse error (messages written to stderr). */
rule  **parse_rules(parser *p, int *n_rules);

/* Free a parser and all associated tokens/AST. */
void    parse_free(parser *p);

/* Free a single rule (caller keeps the array). */
void    rule_free(rule *r);

#endif /* PARSER_H */
