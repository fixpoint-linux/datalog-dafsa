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
    TOK_TILDE,       /* ~ */
    TOK_STRING,      /* 'quoted string' — for regex patterns */
} token_kind;

typedef struct {
    token_kind kind;
    char      *text;       /* owned; NULL for punctuation tokens */
    uint32_t   ival;       /* integer value (TOK_INT only) */
} token;

/* An atom: predicate(args...). args are tokens (TOK_IDENT/TOK_VAR/TOK_INT). */
typedef struct {
    char   *pred;          /* predicate name */
    token **args;          /* array of argument tokens */
    int     nargs;         /* number of arguments */
    int     negated;       /* 1 if preceded by ! */
    int     aggregate;     /* 1 if aggregate (count/sum/min/max) */
    token  *agg_op;        /* aggregate operator token, NULL if not agg */
    char   *pattern;       /* M5: regex pattern string (from ~ '...'), or NULL */
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
