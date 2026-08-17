/*
 * analyze.h — shared in-memory program analysis (parse + fact/rule split +
 * compile) used by BOTH the browser playground (playground-wasm.c) and the
 * language server (lsp.c).
 *
 * This is the single source of truth for the PLAYGROUND language: one document
 * holding facts AND rules.  It mirrors playground_run's ingest exactly —
 * PASS1 pre-declares every head (fact + rule) in-memory, PASS2 adds the ground
 * facts, then compile_rules() runs on the nbody>0 rules only.  (dl_load_rules
 * is RULE-ONLY and rejects facts with "rule X has no body", so it must NOT be
 * used here.)  analyze_program() STOPS before dl_compile()/dl_query(): it
 * never runs the fixpoint.
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include "dl.h"

#include <stdint.h>

/* Failure stage of analyze_program().  The five-way split is REQUIRED so the
 * playground can map each stage back to its EXACT pre-existing coarse error
 * string ("error: parse failed" / "error: malformed rule" / "error: cannot
 * declare relation" / "error: non-ground fact" / "error: rule compile failed")
 * and stay byte-identical, while the LSP reads the precise message in `msg`
 * (the parser's or compiler's own text). */
typedef enum {
    ANALYZE_PARSE     = 0,  /* parse error;  msg = parser's message          */
    ANALYZE_MALFORMED = 1,  /* head missing / no pred / nargs < 1            */
    ANALYZE_DECLARE   = 2,  /* in-memory declare failed (arity/name clash)   */
    ANALYZE_FACT      = 3,  /* non-ground fact; msg = "compile error: ..."   */
    ANALYZE_COMPILE   = 4,  /* compile_rules error; msg = compiler's message */
    ANALYZE_OOM       = 5   /* internal out-of-memory                        */
} analyze_stage;

typedef struct {
    int      stage;         /* one of analyze_stage                         */
    uint32_t off;           /* 0-based byte offset into the source          */
    char     msg[256];      /* human-readable message (parser/compiler text)*/
} analyze_error;

/* Analyze `source` (facts + rules in one document) in memory.
 *
 * On success: returns 0 and sets *out_db to a dir==NULL dl_db with every head
 * declared, ground facts added, and nbody>0 rules compiled (but NOT
 * dl_compile'd — the caller runs dl_compile + dl_query).  The caller owns
 * *out_db and frees it with analyze_db_free().
 *
 * On failure: returns -1, sets *out_db = NULL (the partially-built db is
 * already freed), and fills *err (err may be NULL). */
int  analyze_program(const char *source, dl_db **out_db, analyze_error *err);

/* Free a db produced by analyze_program() (NULL-safe).  Mirrors the
 * playground's mem_db_free: compiled rules, relations, interner, term store. */
void analyze_db_free(dl_db *db);

#endif /* ANALYZE_H */
