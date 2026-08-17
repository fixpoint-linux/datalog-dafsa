/*
 * playground-wasm.c — browser-callable entry point for the datalog-dafsa
 * language playground.
 *
 * Built to wasm via emscripten INSTEAD OF dl_cli.c (this file defines the
 * entry surface; dl_cli.c is NOT linked).  Export (EMSCRIPTEN_KEEPALIVE):
 *   const char *playground_run(const char *program, const char *goal)
 * Parse `program` (facts + rules + '#' comments), evaluate the `goal`
 * relation fully in memory, and return a newline-separated string of result
 * tuples, or a string beginning with "error:" on any failure.
 *
 * The returned pointer is a module-global buffer overwritten on the next
 * call — the JS bridge must copy it out (UTF8ToString) immediately.
 *
 * IN-MEMORY INGEST (the crux): the engine's public fact/declare APIs are ALL
 * disk-bound (dl_open requires a dir; dl_declare_relation rewrites rels.txt;
 * dl_add_fact WAL-appends + fsyncs; dl_load_facts reads a CSV path).  So this
 * entry builds a dir==NULL dl_db from scratch and drives the pure in-memory
 * primitives directly: intern_create()/term_create() -> fresh interner +
 * list store; rel_create(arity) -> fresh empty in-memory DAFSA;
 * rel_add_base(rel, cols) -> add an EDB fact (NO WAL, NO disk);
 * compile_rules() PUBLIC -> rule ASTs to bytecode; vm_execute()/rel_prefix()
 * PUBLIC -> run fixpoint + enumerate.  This mirrors exactly what the
 * magic/topdown path does via its static eval_db_clone() +
 * eval_db_declare_inmem() (dl.c:2292 / 2343).
 *
 * KEY SUBTLETY: the compiler only calls dl_declare_relation() for MISSING
 * head relations (db_find_rel < 0).  Because dir==NULL, that call would fail
 * loudly (dl.c:675).  So we pre-declare EVERY head (fact + rule) in-memory
 * before compile_rules, making the compiler skip its declare branch entirely.
 *
 * SUPPORTED SUBSET (in-memory): facts (incl. double-quoted strings, bare
 * lowercase symbols, integers, list literals), rules with recursion,
 * stratified negation, aggregates (count/sum/min/max), equality,
 * comparisons, arithmetic (+ - * / %), string builtins
 * (concat/length/lower/upper/prefix/suffix/contains), lists
 * (cons/car/cdr/append/member/[X|Xs] patterns), the range predicate, regex
 * pattern walks (OP_WALK).  NOT supported: publish/snapshot, time-travel
 * (as-of) queries, variadic relations (require the disk declare path),
 * WAL/incremental-maintenance API.
 */

#include "dl.h"
#include "dl_internal.h"
#include "intern.h"
#include "termstore.h"
#include "relation.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ─── Output buffer (module-global, overwritten per call) ─────────────── */

static char  *g_out = NULL;
static size_t g_out_len = 0;
static size_t g_out_cap = 0;

static void out_reset(void)
{
    g_out_len = 0;
    if (g_out) g_out[0] = '\0';
}

static void out_grow(size_t need)
{
    if (g_out_len + need + 1 > g_out_cap) {
        size_t nc = g_out_cap ? g_out_cap * 2 : 256;
        while (g_out_len + need + 1 > nc) nc *= 2;
        char *nb = realloc(g_out, nc);
        if (!nb) return;   /* keep old buffer; truncated output is acceptable */
        g_out = nb;
        g_out_cap = nc;
    }
}

static void out_append(const char *s, size_t n)
{
    out_grow(n);
    if (g_out_len + n + 1 > g_out_cap) return;   /* realloc failed */
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
    g_out[g_out_len] = '\0';
}

static void out_printf(const char *fmt, ...)
{
    char buf[32];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(buf)) {
        out_append(buf, (size_t)n);
    } else {
        /* long line — append via the growing path */
        char *big = malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        out_append(big, (size_t)n);
        free(big);
    }
}

/* ─── Value rendering (byte-for-byte the CLI's print_value heuristic) ──── */

static void emit_val(dl_db *db, uint32_t v, int depth);

static void emit_list(dl_db *db, uint32_t h, int depth)
{
    if (depth > 4096) { out_append("[...]", 5); return; }
    out_append("[", 1);
    while (h != TERM_NIL) {
        emit_val(db, term_car(db->terms, h), depth + 1);
        h = term_cdr(db->terms, h);
        if (h != TERM_NIL) out_append(", ", 2);
    }
    out_append("]", 1);
}

static void emit_val(dl_db *db, uint32_t v, int depth)
{
    if (term_is_list(db->terms, v)) {
        emit_list(db, v, depth);
        return;
    }
    const char *s = intern_str_of(db->ir, v);
    if (s && *s) {
        out_append(s, strlen(s));
    } else {
        out_printf("%u", v);
    }
}

/* dl_query callback: one tuple per line, columns space-separated. */
static int collect_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    dl_db *db = (dl_db *)user;
    uint8_t i;
    for (i = 0; i < arity; i++) {
        if (i > 0) out_append(" ", 1);
        emit_val(db, cols[i], 0);
    }
    out_append("\n", 1);
    return 0;
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
        for (i = 0; i < n_compiled; i++) compiled_rule_free(new_crules[i]);
        free(new_crules);
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

/* ─── Teardown: free everything (no leak across repeated Run clicks) ───── */

static void mem_db_free(dl_db *db)
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

EMSCRIPTEN_KEEPALIVE
const char *playground_run(const char *program, const char *goal)
{
    dl_db db;
    parser *p = NULL;
    rule **rules = NULL;
    int n_rules = 0;
    int i;
    int rc = -1;
    static const char *err = NULL;

    out_reset();

    memset(&db, 0, sizeof(db));
    db.dir = NULL;
    db.lock_fd = -1;
    db.ir = intern_create();
    db.terms = term_create();
    if (!db.ir || !db.terms) { err = "error: out of memory"; goto cleanup; }

    /* Parse the whole program (facts + rules). */
    p = parse_create(program);
    if (!p) { err = "error: parse failed"; goto cleanup; }
    rules = parse_rules(p, &n_rules);
    if (!rules) { err = "error: parse failed"; goto cleanup; }

    /* PASS 1: pre-declare EVERY head (facts AND rules) in-memory, so the
     * compiler's dl_declare_relation branch (which would hit the dir==NULL
     * guard) is never reached. */
    for (i = 0; i < n_rules; i++) {
        atom *head = rules[i]->head;
        if (!head || !head->pred || head->nargs < 1) {
            err = "error: malformed rule";
            goto cleanup;
        }
        if (mem_declare(&db, head->pred, (uint8_t)head->nargs) != 0) {
            err = "error: cannot declare relation";
            goto cleanup;
        }
    }

    /* PASS 2: split facts (nbody==0, ground head) from rules. */
    for (i = 0; i < n_rules; i++) {
        rule *r = rules[i];
        if (r->nbody == 0) {
            if (mem_add_fact(&db, r) != 0) {
                err = "error: non-ground fact";
                goto cleanup;
            }
        }
    }
    /* Compile all RULES (nbody > 0) — facts were handled above. */
    {
        rule **r_rules = NULL;
        int n_r_rules = 0;
        r_rules = malloc((size_t)(n_rules ? n_rules : 1) * sizeof(rule *));
        if (!r_rules) { err = "error: out of memory"; goto cleanup; }
        for (i = 0; i < n_rules; i++)
            if (rules[i]->nbody > 0) r_rules[n_r_rules++] = rules[i];
        if (load_rules_ast(&db, r_rules, n_r_rules) != 0) {
            free(r_rules);
            err = "error: rule compile failed";
            goto cleanup;
        }
        free(r_rules);
    }

    /* Evaluate (fixpoint) then stream the goal. */
    if (dl_compile(&db) != 0) { err = "error: evaluation failed"; goto cleanup; }

    if (goal && *goal) {
        long n = dl_query(&db, goal, collect_cb, &db);
        if (n < 0) {
            err = "error: query failed (unknown goal relation?)";
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    if (rules) {
        for (i = 0; i < n_rules; i++) rule_free(rules[i]);
        free(rules);
    }
    if (p) parse_free(p);
    mem_db_free(&db);
    if (rc != 0) return err ? err : "error: unknown";
    return g_out ? g_out : "";
}
