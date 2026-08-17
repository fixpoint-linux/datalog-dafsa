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
 * IN-MEMORY INGEST: the engine's public fact/declare APIs are ALL disk-bound
 * (dl_open requires a dir; dl_declare_relation rewrites rels.txt; dl_add_fact
 * WAL-appends + fsyncs; dl_load_facts reads a CSV path).  The pure in-memory
 * ingest (build a dir==NULL dl_db, pre-declare every head, add ground facts,
 * compile the rules) now lives in src/analyze.c::analyze_program() and is
 * SHARED with the language server — see analyze.h.  This file only renders the
 * result and maps analyze_program()'s failure stages back to the exact coarse
 * error strings the browser playground has always produced.
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
#include "analyze.h"

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

/* ─── Entry point ─────────────────────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE
const char *playground_run(const char *program, const char *goal)
{
    dl_db *db = NULL;
    analyze_error aerr;
    int rc = -1;
    static const char *err = NULL;

    out_reset();

    /* Parse + pre-declare + facts + compile (see analyze.c).  On failure the
     * db is already freed and aerr carries the stage + precise message. */
    if (analyze_program(program, &db, &aerr) != 0) {
        switch (aerr.stage) {
        case ANALYZE_PARSE:     err = "error: parse failed"; break;
        case ANALYZE_MALFORMED: err = "error: malformed rule"; break;
        case ANALYZE_DECLARE:   err = "error: cannot declare relation"; break;
        case ANALYZE_FACT:      err = "error: non-ground fact"; break;
        case ANALYZE_COMPILE:   err = "error: rule compile failed"; break;
        case ANALYZE_OOM:
        default:                err = "error: out of memory"; break;
        }
        goto cleanup;
    }

    /* Evaluate (fixpoint) then stream the goal. */
    if (dl_compile(db) != 0) { err = "error: evaluation failed"; goto cleanup; }

    if (goal && *goal) {
        long n = dl_query(db, goal, collect_cb, db);
        if (n < 0) {
            err = "error: query failed (unknown goal relation?)";
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    analyze_db_free(db);
    if (rc != 0) return err ? err : "error: unknown";
    return g_out ? g_out : "";
}
