/*
 * analyze.c — shared in-memory program analysis (see analyze.h).
 *
 * This is the factor of playground-wasm.c's in-memory ingest: the pure
 * primitives that build a dir==NULL dl_db from a source document of facts +
 * rules.  Both the browser playground and the LSP call analyze_program(), so
 * the two agree byte-for-byte on what the engine accepts or rejects.
 */

#include "analyze.h"
#include "dl_internal.h"
#include "intern.h"
#include "termstore.h"
#include "relation.h"
#include "parser.h"
#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(analyze_error *err, int stage, uint32_t off, const char *msg)
{
    if (!err) return;
    err->stage = stage;
    err->off = off;
    if (msg) {
        strncpy(err->msg, msg, sizeof(err->msg) - 1);
        err->msg[sizeof(err->msg) - 1] = '\0';
    } else {
        err->msg[0] = '\0';
    }
}

/* ─── In-memory db helpers (mirror eval_db_declare_inmem, dl.c:2343) ──── */

static int mem_find_rel(dl_db *db, const char *name)
{
    size_t i;
    for (i = 0; i < db->nrels; i++)
        if (strcmp(db->rels[i].name, name) == 0)
            return (int)i;
    return -1;
}

/* Declare a FIXED relation in-memory (rel_create, no WAL/disk).  Idempotent
 * for the same name + arity (like dl_declare_relation_kind's no-op branch). */
static int mem_declare(dl_db *db, const char *name, uint8_t arity)
{
    int idx = mem_find_rel(db, name);
    relation *rel;
    if (arity == 0 || arity > 8) return -1;
    if (idx >= 0) {
        if (db->rels[idx].kind != RELK_FIXED) return -1;
        return (rel_arity(db->rels[idx].rel) == arity) ? 0 : -1;
    }
    if (db->nrels >= MAX_RELS) return -1;
    rel = rel_create(arity);
    if (!rel) return -1;
    db->rels[db->nrels].name = strdup(name);
    if (!db->rels[db->nrels].name) { rel_free(rel); return -1; }
    db->rels[db->nrels].kind  = RELK_FIXED;
    db->rels[db->nrels].arity = arity;
    db->rels[db->nrels].rel   = rel;
    db->rels[db->nrels].vrel  = NULL;
    db->nrels++;
    return 0;
}

/* ─── Ground fact constant → u32 (parser token) ────────────────────────── */

/* Convert a single ground element token to a u32 value.  Returns 1 on
 * success, 0 on error (a TOK_VAR / non-ground token). */
static int elem_of(dl_db *db, token *t, uint32_t *out)
{
    switch (t->kind) {
    case TOK_INT:
        *out = t->ival;
        return 1;
    case TOK_IDENT:
    case TOK_STRING:
        *out = intern_str(db->ir, t->text);
        return *out != 0;   /* 0 == OOM; a valid interned sym is never 0 */
    default:
        return 0;           /* TOK_VAR / TOK_LIST-as-elem (not supported here) */
    }
}

/* Build a list handle from a TOK_LIST token's element tokens.  Returns the
 * handle, or 0 on error (unbound element / OOM / non-list tail). */
static uint32_t list_of(dl_db *db, token *t)
{
    uint32_t tail = TERM_NIL;
    int i;
    if (t->kind != TOK_LIST) return 0;
    for (i = t->nchildren - 1; i >= 0; i--) {
        uint32_t el;
        if (!elem_of(db, t->children[i], &el)) return 0;
        tail = term_cons(db->terms, el, tail);
        if (tail == 0) return 0;   /* OOM or non-list tail */
    }
    return tail;
}

/* Convert a ground fact argument token to a u32 column value.
 * Returns 1 on success (cols_out set), 0 on error (non-ground fact). */
static int const_of(dl_db *db, token *t, uint32_t *cols_out)
{
    uint32_t h;
    switch (t->kind) {
    case TOK_INT:
        *cols_out = t->ival;
        return 1;
    case TOK_IDENT:
    case TOK_STRING:
        *cols_out = intern_str(db->ir, t->text);
        return *cols_out != 0;
    case TOK_LIST:
        h = list_of(db, t);
        if (h == 0) return 0;
        *cols_out = h;
        return 1;
    case TOK_VAR:
    default:
        return 0;   /* non-ground fact */
    }
}

/* Add one ground fact (head-only rule, nbody==0) to its relation's base. */
static int mem_add_fact(dl_db *db, rule *r)
{
    uint32_t cols[MAX_ARITY];
    int idx;
    int i;
    atom *head = r->head;
    if (!head || head->nargs > MAX_ARITY || head->nargs < 1) return -1;
    for (i = 0; i < head->nargs; i++) {
        if (!const_of(db, head->args[i], &cols[i])) return -1;
    }
    idx = mem_find_rel(db, head->pred);
    if (idx < 0) return -1;
    if (db->rels[idx].kind != RELK_FIXED) return -1;
    /* rel_add_base returns 1 (added), 0 (dup) or -1 (error).  Any
     * non-negative result is a successful insert — normalize to 0. */
    return rel_add_base(db->rels[idx].rel, cols) < 0 ? -1 : 0;
}

/* ─── Rule loading (compile_rules + append to db->crules) ──────────────── */

static int load_rules_ast(dl_db *db, rule **rules, int n_rules)
{
    compiled_rule **new_crules = NULL;
    int n_compiled = 0;
    int i;
    if (n_rules <= 0) return 0;
    if (compile_rules(db, rules, n_rules, &new_crules, &n_compiled) != 0) {
        if (new_crules) {
            for (i = 0; i < n_compiled; i++) compiled_rule_free(new_crules[i]);
            free(new_crules);
        }
        return -1;
    }
    {
        int new_total = db->n_crules + n_compiled;
        compiled_rule **merged = realloc(db->crules,
            (size_t)new_total * sizeof(compiled_rule *));
        if (!merged) {
            for (i = 0; i < n_compiled; i++) compiled_rule_free(new_crules[i]);
            free(new_crules);
            return -1;
        }
        memcpy(merged + db->n_crules, new_crules,
               (size_t)n_compiled * sizeof(compiled_rule *));
        free(new_crules);
        db->crules = merged;
        db->n_crules = new_total;
    }
    db->fixpoint_dirty = 1;
    return 0;
}

/* ─── Teardown ─────────────────────────────────────────────────────────── */

void analyze_db_free(dl_db *db)
{
    size_t i;
    if (!db) return;
    if (db->crules) {
        for (i = 0; i < (size_t)db->n_crules; i++)
            compiled_rule_free(db->crules[i]);
        free(db->crules);
        db->crules = NULL;
        db->n_crules = 0;
    }
    for (i = 0; i < db->nrels; i++) {
        if (db->rels[i].kind == RELK_VARIADIC)
            vrel_free(db->rels[i].vrel);
        else
            rel_free(db->rels[i].rel);
        free(db->rels[i].name);
    }
    db->nrels = 0;
    intern_free(db->ir);
    term_free(db->terms);
    memset(db, 0, sizeof(*db));
    db->lock_fd = -1;
}

/* ─── Entry point ─────────────────────────────────────────────────────── */

int analyze_program(const char *source, dl_db **out_db, analyze_error *err)
{
    dl_db *db = NULL;
    parser *p = NULL;
    rule **rules = NULL;
    int n_rules = 0;
    int i;
    int rc = -1;

    if (err) { err->stage = ANALYZE_PARSE; err->off = 0; err->msg[0] = '\0'; }
    if (out_db) *out_db = NULL;
    if (!source || !out_db) return -1;

    /* Parse (LSP-only reporting parse keeps the parser on a lexer error). */
    p = parse_create_reporting(source);
    if (!p) {
        set_err(err, ANALYZE_OOM, 0, "out of memory");
        return -1;
    }
    if (parse_last_error(p, NULL)) {
        /* tokenize failed — the parser is still alive so we can read the
         * position before freeing it. */
        uint32_t poff = 0;
        const char *pmsg = parse_last_error(p, &poff);
        set_err(err, ANALYZE_PARSE, poff, pmsg);
        parse_free(p);
        return -1;
    }

    rules = parse_rules(p, &n_rules);
    if (!rules) {
        uint32_t poff = 0;
        const char *pmsg = parse_last_error(p, &poff);
        set_err(err, ANALYZE_PARSE, poff, pmsg ? pmsg : "parse failed");
        parse_free(p);
        return -1;
    }
    parse_free(p);
    p = NULL;

    /* Build the dir==NULL in-memory db. */
    db = calloc(1, sizeof(*db));
    if (!db) {
        set_err(err, ANALYZE_OOM, 0, "out of memory");
        goto fail;
    }
    db->dir = NULL;
    db->lock_fd = -1;
    db->ir = intern_create();
    db->terms = term_create();
    if (!db->ir || !db->terms) {
        set_err(err, ANALYZE_OOM, 0, "out of memory");
        goto fail;
    }

    /* PASS 1: pre-declare EVERY head (facts AND rules) in-memory, so the
     * compiler's dl_declare_relation branch (which would hit the dir==NULL
     * guard) is never reached. */
    for (i = 0; i < n_rules; i++) {
        atom *head = rules[i]->head;
        if (!head || !head->pred || head->nargs < 1) {
            set_err(err, ANALYZE_MALFORMED, rules[i]->off, "malformed rule");
            goto fail;
        }
        if (mem_declare(db, head->pred, (uint8_t)head->nargs) != 0) {
            set_err(err, ANALYZE_DECLARE, rules[i]->off,
                    "cannot declare relation");
            goto fail;
        }
    }

    /* PASS 2: split facts (nbody==0, ground head) from rules. */
    for (i = 0; i < n_rules; i++) {
        rule *r = rules[i];
        if (r->nbody == 0) {
            if (mem_add_fact(db, r) != 0) {
                set_err(err, ANALYZE_FACT, r->off,
                        "compile error: non-ground fact");
                goto fail;
            }
        }
    }

    /* Compile all RULES (nbody > 0) — facts were handled above. */
    {
        rule **r_rules = NULL;
        int n_r_rules = 0;
        r_rules = malloc((size_t)(n_rules ? n_rules : 1) * sizeof(rule *));
        if (!r_rules) {
            set_err(err, ANALYZE_OOM, 0, "out of memory");
            goto fail;
        }
        for (i = 0; i < n_rules; i++)
            if (rules[i]->nbody > 0) r_rules[n_r_rules++] = rules[i];
        if (load_rules_ast(db, r_rules, n_r_rules) != 0) {
            uint32_t coff = 0;
            const char *cmsg = compile_last_error(&coff);
            free(r_rules);
            set_err(err, ANALYZE_COMPILE, coff,
                    cmsg ? cmsg : "rule compile failed");
            goto fail;
        }
        free(r_rules);
    }

    rc = 0;

fail:
    if (rules) {
        for (i = 0; i < n_rules; i++) rule_free(rules[i]);
        free(rules);
    }
    if (p) parse_free(p);
    if (rc != 0) {
        analyze_db_free(db);
        db = NULL;
    }
    *out_db = db;
    return rc;
}
