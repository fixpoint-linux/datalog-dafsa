/* lsp.c — Language Server Protocol server for datalog-dafsa (MVP).
 * Speaks JSON-RPC 2.0 over stdio with Content-Length framing; a single
 * synchronous loop.  Reuses the real parser/compiler — via analyze_program()
 * (the shared playground ingest) — for diagnostics, and the parser AST for
 * hover + completion.
 *
 * Layering (shared by the native binary and the wasm build):
 *   - lsp_handle()      — process one incoming JSON-RPC message; append the
 *                         resulting response/notification frame(s) (still
 *                         Content-Length framed) to a module-global output
 *                         buffer (g_out).
 *   - lsp_out()/lsp_out_len() — expose that buffer (the wasm entry reads it;
 *                         the native main() flushes it to stdout).
 *   - main()            — stdio framing loop (read a frame -> lsp_handle ->
 *                         fwrite g_out -> repeat), compiled only for the native
 *                         target (the wasm build passes -DLSP_NO_MAIN).
 */
#include "analyze.h"
#include "json.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define LSP_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define LSP_EXPORT
#endif

/* ─── Builtin names (union of parser.c is_aggregate / is_str_producing_name /
 * is_list_producing_name and compiler.c is_reserved_builtin_name) ───────── */
static const char *const BUILTINS[] = {
    "count", "sum", "min", "max",           /* aggregates            */
    "concat", "length", "lower", "upper",   /* string-producing      */
    "prefix", "suffix", "contains",         /* string filters        */
    "cons", "car", "cdr", "append",         /* list-producing        */
    "member",                               /* list filter           */
    "range"                                 /* range predicate       */
};
#define N_BUILTINS (sizeof(BUILTINS) / sizeof(BUILTINS[0]))

static int is_builtin_name(const char *s)
{
    size_t i;
    if (!s) return 0;
    for (i = 0; i < N_BUILTINS; i++)
        if (!strcmp(BUILTINS[i], s)) return 1;
    return 0;
}

static const char *builtin_desc(const char *name)
{
    if (!strcmp(name, "count"))    return "aggregate — number of matching tuples in the group (count())";
    if (!strcmp(name, "sum"))      return "aggregate — sum of a variable over the group (sum(X))";
    if (!strcmp(name, "min"))      return "aggregate — minimum of a variable over the group (min(X))";
    if (!strcmp(name, "max"))      return "aggregate — maximum of a variable over the group (max(X))";
    if (!strcmp(name, "concat"))   return "string builtin — concat(A, B)";
    if (!strcmp(name, "length"))   return "string/list builtin — byte length of S (length(S))";
    if (!strcmp(name, "lower"))    return "string builtin — lowercase (lower(S))";
    if (!strcmp(name, "upper"))    return "string builtin — uppercase (upper(S))";
    if (!strcmp(name, "prefix"))   return "string filter — prefix(A, B)";
    if (!strcmp(name, "suffix"))   return "string filter — suffix(A, B)";
    if (!strcmp(name, "contains")) return "string filter — contains(A, B)";
    if (!strcmp(name, "cons"))     return "list builtin — cons(Head, Tail)";
    if (!strcmp(name, "car"))      return "list builtin — car(List)";
    if (!strcmp(name, "cdr"))      return "list builtin — cdr(List)";
    if (!strcmp(name, "append"))   return "list builtin — append(A, B)";
    if (!strcmp(name, "member"))   return "list builtin — member(X, List)";
    if (!strcmp(name, "range"))    return "range predicate — range(X, Rel, Lo, Hi)";
    return "builtin";
}

/* Comparison / equality operators never refer to a relation. */
static int is_operator_name(const char *s)
{
    return s && (!strcmp(s, "=") || !strcmp(s, "<") || !strcmp(s, "<=") ||
                 !strcmp(s, ">") || !strcmp(s, ">=") || !strcmp(s, "!="));
}

/* A body atom refers to a RELATION (not a builtin/operator/aggregate). */
static int is_relation_atom(const atom *a)
{
    if (!a || !a->pred || a->aggregate) return 0;
    if (is_operator_name(a->pred)) return 0;
    if (is_builtin_name(a->pred)) return 0;
    return 1;
}

/* ─── Output buffer (module-global, reset at the top of each message) ──── */

static char  *g_out = NULL;
static size_t g_out_len = 0;
static size_t g_out_cap = 0;

static void out_add(const char *s, size_t n) {
    if (g_out_len + n + 1 > g_out_cap) {
        size_t cap = g_out_cap ? g_out_cap : 256;
        while (g_out_len + n + 1 > cap) cap *= 2;
        g_out = realloc(g_out, cap);
        if (!g_out) { fputs("dl-lsp: out of memory\n", stderr); exit(3); }
        g_out_cap = cap;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
    g_out[g_out_len] = '\0';
}

/* Append one Content-Length-framed message (header + body) to g_out. */
static void write_frame(jbuf *b) {
    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", b->len);
    out_add(hdr, (size_t)hn);
    out_add(b->s, b->len);
}

LSP_EXPORT const char *lsp_out(void) { return g_out ? g_out : ""; }

LSP_EXPORT int lsp_out_len(void) { return (int)g_out_len; }

/* ─── Document store (heap, persists across messages) ──────────────────── */

typedef struct { char *uri; char *text; } Doc;
static Doc *g_docs; static int g_ndocs, g_cap;

static const char *docs_get(const char *uri) {
    for (int i = 0; i < g_ndocs; i++)
        if (!strcmp(g_docs[i].uri, uri)) return g_docs[i].text;
    return NULL;
}

static void docs_set(const char *uri, const char *text) {
    for (int i = 0; i < g_ndocs; i++) {
        if (!strcmp(g_docs[i].uri, uri)) {
            free(g_docs[i].text);
            g_docs[i].text = text ? strdup(text) : NULL;
            return;
        }
    }
    if (g_ndocs == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 8;
        g_docs = realloc(g_docs, (size_t)g_cap * sizeof *g_docs);
        if (!g_docs) { fputs("dl-lsp: out of memory\n", stderr); exit(3); }
    }
    g_docs[g_ndocs].uri = strdup(uri);
    g_docs[g_ndocs].text = text ? strdup(text) : NULL;
    g_ndocs++;
}

static void docs_remove(const char *uri) {
    for (int i = 0; i < g_ndocs; i++) {
        if (!strcmp(g_docs[i].uri, uri)) {
            free(g_docs[i].uri);
            free(g_docs[i].text);
            g_docs[i] = g_docs[--g_ndocs];
            return;
        }
    }
}

/* ─── Position conversion (0-based; datalog is ASCII so byte == UTF-16) ─── */

static void off_to_line_col(const char *src, uint32_t off, int *line, int *col)
{
    int l = 0, c = 0;
    const char *p = src;
    uint32_t i;
    for (i = 0; i < off && *p; i++) {
        if (*p == '\n') { l++; c = 0; } else { c++; }
        p++;
    }
    *line = l;
    *col = c;
}

static uint32_t line_col_to_off(const char *src, int line, int character)
{
    const char *p = src;
    int l = 0;
    while (*p && l < line) { if (*p == '\n') l++; p++; }
    /* p is now at the start of the target line */
    int c = 0;
    while (*p && *p != '\n' && c < character) { p++; c++; }
    return (uint32_t)(p - src);
}

/* ─── Diagnostics bridge (analyze_program per document) ────────────────── */

static void publish_diagnostics(const char *uri, const char *text,
                                const analyze_error *diag)
{
    jbuf b; jbuf_init(&b);
    json_write_raw(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    json_write_string(&b, uri);
    json_write_raw(&b, ",\"diagnostics\":[");
    if (diag) {
        int line = 0, col = 0;
        if (text) off_to_line_col(text, diag->off, &line, &col);
        json_write_raw(&b, "{\"range\":{\"start\":{\"line\":");
        json_write_int(&b, line);
        json_write_raw(&b, ",\"character\":");
        json_write_int(&b, col);
        json_write_raw(&b, "},\"end\":{\"line\":");
        json_write_int(&b, line);
        json_write_raw(&b, ",\"character\":");
        json_write_int(&b, col);
        json_write_raw(&b, "}},\"severity\":1,\"source\":\"dl\",\"message\":");
        json_write_string(&b, diag->msg);
        json_write_raw(&b, "}");
    }
    json_write_raw(&b, "]}}");
    write_frame(&b);
    jbuf_free(&b);
}

static void handle_doc_change(const char *uri, const char *text) {
    docs_set(uri, text);
    dl_db *db = NULL;
    analyze_error aerr;
    int ok = (analyze_program(text, &db, &aerr) == 0);
    analyze_db_free(db);
    publish_diagnostics(uri, text, ok ? NULL : &aerr);
}

/* ─── Response helpers ──────────────────────────────────────────────────── */

static void respond_result(const Json *id, const char *result_frag) {
    jbuf b; jbuf_init(&b);
    json_write_raw(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_emit(&b, id);
    json_write_raw(&b, ",\"result\":");
    json_write_raw(&b, result_frag);
    json_write_raw(&b, "}");
    write_frame(&b);
    jbuf_free(&b);
}

static void respond_error(const Json *id, int code, const char *msg) {
    jbuf b; jbuf_init(&b);
    json_write_raw(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_emit(&b, id);
    json_write_raw(&b, ",\"error\":{\"code\":");
    json_write_int(&b, code);
    json_write_raw(&b, ",\"message\":");
    json_write_string(&b, msg);
    json_write_raw(&b, "}}");
    write_frame(&b);
    jbuf_free(&b);
}

static void handle_initialize(const Json *id) {
    respond_result(id, "{\"capabilities\":{\"textDocumentSync\":1,"
                       "\"hoverProvider\":true,\"completionProvider\":true}}");
}

/* ─── Parse helper (returns rules or NULL; caller frees) ───────────────── */

static rule **parse_doc(const char *text, int *n_rules)
{
    parser *p;
    rule **rules;

    *n_rules = 0;
    if (!text) return NULL;
    p = parse_create_reporting(text);
    if (!p) return NULL;
    if (parse_last_error(p, NULL)) { parse_free(p); return NULL; }
    rules = parse_rules(p, n_rules);
    parse_free(p);
    return rules;
}

static void free_rules(rule **rules, int n_rules)
{
    int i;
    for (i = 0; i < n_rules; i++) rule_free(rules[i]);
    free(rules);
}

/* A predicate is IDB if it is the head of a rule (nbody > 0), else EDB. */
static int pred_is_idb(rule **rules, int n_rules, const char *pred)
{
    int i;
    for (i = 0; i < n_rules; i++)
        if (rules[i]->nbody > 0 && rules[i]->head && rules[i]->head->pred &&
            !strcmp(rules[i]->head->pred, pred))
            return 1;
    return 0;
}

/* ─── Hover ─────────────────────────────────────────────────────────────── */

static void handle_hover(const Json *id, const Json *params) {
    const Json *td = json_obj_get(params, "textDocument");
    const char *uri = td ? json_str(json_obj_get(td, "uri")) : NULL;
    const char *text = uri ? docs_get(uri) : NULL;
    if (!text) { respond_result(id, "null"); return; }

    const Json *pos = json_obj_get(params, "position");
    int line = (int)json_num(json_obj_get(pos, "line"));
    int ch   = (int)json_num(json_obj_get(pos, "character"));
    uint32_t off = line_col_to_off(text, line, ch);

    int n_rules = 0;
    rule **rules = parse_doc(text, &n_rules);
    if (!rules) { respond_result(id, "null"); return; }

    const char *found = NULL;      /* points into the AST (valid until free_rules) */
    char buf[256];

    /* (a) builtin keyword (aggregates live in agg_op->text; the other
     * builtins are atom->pred). */
    for (int i = 0; i < n_rules && !found; i++) {
        rule *r = rules[i];
        for (int j = 0; j < r->nbody; j++) {
            atom *a = r->body[j];
            if (a->aggregate && a->agg_op && a->agg_op->text &&
                is_builtin_name(a->agg_op->text)) {
                size_t len = strlen(a->agg_op->text);
                if (off >= a->agg_op->off && off < a->agg_op->off + len) {
                    snprintf(buf, sizeof buf, "builtin `%s` — %s",
                             a->agg_op->text, builtin_desc(a->agg_op->text));
                    found = buf;
                }
            }
            if (is_builtin_name(a->pred)) {
                size_t len = strlen(a->pred);
                if (off >= a->off && off < a->off + len) {
                    snprintf(buf, sizeof buf, "builtin `%s` — %s",
                             a->pred, builtin_desc(a->pred));
                    found = buf;
                }
            }
        }
    }

    /* (b) predicate name (relation reference in head or body). */
    for (int i = 0; i < n_rules && !found; i++) {
        rule *r = rules[i];
        atom *h = r->head;
        if (h && h->pred && !is_builtin_name(h->pred)) {
            size_t len = strlen(h->pred);
            if (off >= h->off && off < h->off + len) {
                int idb = pred_is_idb(rules, n_rules, h->pred);
                snprintf(buf, sizeof buf, "`%s/%d` — %s", h->pred, h->nargs,
                         idb ? "IDB" : "EDB");
                found = buf;
            }
        }
        for (int j = 0; j < r->nbody && !found; j++) {
            atom *a = r->body[j];
            if (is_relation_atom(a)) {
                size_t len = strlen(a->pred);
                if (off >= a->off && off < a->off + len) {
                    int idb = pred_is_idb(rules, n_rules, a->pred);
                    snprintf(buf, sizeof buf, "`%s/%d` — %s", a->pred, a->nargs,
                             idb ? "IDB" : "EDB");
                    found = buf;
                }
            }
        }
    }

    /* (c) variable (head args + body args, incl. nested list patterns). */
    for (int i = 0; i < n_rules && !found; i++) {
        rule *r = rules[i];
        int ai;
        for (ai = 0; ai < r->head->nargs && !found; ai++) {
            token *t = r->head->args[ai];
            if (t && t->kind == TOK_VAR && t->text) {
                size_t len = strlen(t->text);
                if (off >= t->off && off < t->off + len) {
                    snprintf(buf, sizeof buf, "variable `%s`", t->text);
                    found = buf;
                }
            }
        }
        for (int j = 0; j < r->nbody && !found; j++) {
            atom *a = r->body[j];
            for (ai = 0; ai < a->nargs && !found; ai++) {
                token *t = a->args[ai];
                if (t && t->kind == TOK_VAR && t->text) {
                    size_t len = strlen(t->text);
                    if (off >= t->off && off < t->off + len) {
                        snprintf(buf, sizeof buf, "variable `%s`", t->text);
                        found = buf;
                    }
                }
            }
        }
    }

    if (found) {
        jbuf b; jbuf_init(&b);
        json_write_raw(&b, "{\"contents\":{\"kind\":\"plaintext\",\"value\":");
        json_write_string(&b, found);
        json_write_raw(&b, "}}");
        respond_result(id, b.s);
        jbuf_free(&b);
    } else {
        respond_result(id, "null");
    }

    free_rules(rules, n_rules);
}

/* ─── Completion ────────────────────────────────────────────────────────── */

typedef struct { const char **labels; int n; int cap; } label_set;

static int label_has(const label_set *ls, const char *s) {
    for (int i = 0; i < ls->n; i++)
        if (!strcmp(ls->labels[i], s)) return 1;
    return 0;
}

static void label_add(label_set *ls, const char *s) {
    if (!s || label_has(ls, s)) return;
    if (ls->n == ls->cap) {
        ls->cap = ls->cap ? ls->cap * 2 : 32;
        ls->labels = realloc(ls->labels, (size_t)ls->cap * sizeof(const char *));
        if (!ls->labels) { fputs("dl-lsp: out of memory\n", stderr); exit(3); }
    }
    ls->labels[ls->n++] = s;
}

/* Collect every variable name reachable from a token (incl. list patterns). */
static void collect_token_vars(const token *t, label_set *ls) {
    if (!t) return;
    if (t->kind == TOK_VAR && t->text) label_add(ls, t->text);
    for (int i = 0; i < t->nchildren; i++)
        collect_token_vars(t->children[i], ls);
    collect_token_vars(t->tail, ls);
}

static void handle_completion(const Json *id, const Json *params) {
    const Json *td = json_obj_get(params, "textDocument");
    const char *uri = td ? json_str(json_obj_get(td, "uri")) : NULL;
    const char *text = uri ? docs_get(uri) : NULL;
    if (!text) { respond_result(id, "{\"isIncomplete\":false,\"items\":[]}"); return; }

    int n_rules = 0;
    rule **rules = parse_doc(text, &n_rules);

    label_set ls = { NULL, 0, 0 };

    /* relation names in scope: every declared head. */
    for (int i = 0; i < n_rules; i++) {
        if (rules[i]->head && rules[i]->head->pred)
            label_add(&ls, rules[i]->head->pred);
    }
    /* builtins. */
    for (size_t i = 0; i < N_BUILTINS; i++)
        label_add(&ls, BUILTINS[i]);
    /* variables bound by head/body atoms (and list patterns). */
    for (int i = 0; i < n_rules; i++) {
        rule *r = rules[i];
        for (int j = 0; j < r->head->nargs; j++)
            collect_token_vars(r->head->args[j], &ls);
        for (int j = 0; j < r->nbody; j++)
            for (int k = 0; k < r->body[j]->nargs; k++)
                collect_token_vars(r->body[j]->args[k], &ls);
    }

    jbuf b; jbuf_init(&b);
    json_write_raw(&b, "{\"isIncomplete\":false,\"items\":[");
    for (int i = 0; i < ls.n; i++) {
        if (i) json_write_raw(&b, ",");
        json_write_raw(&b, "{\"label\":");
        json_write_string(&b, ls.labels[i]);
        json_write_raw(&b, "}");
    }
    json_write_raw(&b, "]}");
    respond_result(id, b.s);
    jbuf_free(&b);

    free(ls.labels);
    if (rules) free_rules(rules, n_rules);
}

/* ─── Message core ──────────────────────────────────────────────────────── */

static bool g_shut_down = false;

LSP_EXPORT bool lsp_handle(const char *json, int len) {
    g_out_len = 0;
    if (g_out) g_out[0] = '\0';

    Json *root = json_parse(json, (size_t)len);
    if (!root) return false;

    const char *method = json_str(json_obj_get(root, "method"));
    Json *id = json_obj_get(root, "id");
    bool is_request = id && id->type != J_NULL;
    Json *params = json_obj_get(root, "params");

    bool done = false;
    if (method && !strcmp(method, "initialize")) {
        handle_initialize(id);
    } else if (method && !strcmp(method, "initialized")) {
        /* no reply */
    } else if (method && !strcmp(method, "shutdown")) {
        g_shut_down = true;
        respond_result(id, "null");
    } else if (method && !strcmp(method, "exit")) {
        done = true;
    } else if (method && !strcmp(method, "textDocument/didOpen")) {
        const Json *td = json_obj_get(params, "textDocument");
        const char *uri = td ? json_str(json_obj_get(td, "uri")) : NULL;
        const char *text = td ? json_str(json_obj_get(td, "text")) : NULL;
        if (uri && text) handle_doc_change(uri, text);
    } else if (method && !strcmp(method, "textDocument/didChange")) {
        const Json *td = json_obj_get(params, "textDocument");
        const char *uri = td ? json_str(json_obj_get(td, "uri")) : NULL;
        const Json *cc = json_obj_get(params, "contentChanges");
        const Json *last = (cc && cc->type == J_ARR && cc->as.arr.n > 0)
                           ? json_arr_get(cc, cc->as.arr.n - 1) : NULL;
        const char *text = last ? json_str(json_obj_get(last, "text")) : NULL;
        if (uri && text) handle_doc_change(uri, text);
    } else if (method && !strcmp(method, "textDocument/didClose")) {
        const Json *td = json_obj_get(params, "textDocument");
        const char *uri = td ? json_str(json_obj_get(td, "uri")) : NULL;
        if (uri) { docs_remove(uri); publish_diagnostics(uri, NULL, NULL); }
    } else if (method && !strcmp(method, "textDocument/hover")) {
        handle_hover(id, params);
    } else if (method && !strcmp(method, "textDocument/completion")) {
        handle_completion(id, params);
    } else if (is_request) {
        respond_error(id, -32601, "method not found");
    }

    json_free(root);
    return done;
}

/* ─── Native stdio framing (excluded from the wasm build) ───────────────── */

#ifndef LSP_NO_MAIN

static char *read_frame(void) {
    long content_length = -1;
    char line[256];
    for (;;) {
        size_t i = 0;
        int c;
        while (i + 1 < sizeof line && (c = fgetc(stdin)) != EOF && c != '\n')
            line[i++] = (char)c;
        if (c == EOF && i == 0) return NULL;
        line[i] = '\0';
        if (i > 0 && line[i - 1] == '\r') line[--i] = '\0';
        if (i == 0) break;
        if (strncmp(line, "Content-Length:", 15) == 0)
            content_length = strtol(line + 15, NULL, 10);
    }
    if (content_length < 0 || content_length > (1 << 24)) return NULL;
    char *buf = malloc((size_t)content_length + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < (size_t)content_length) {
        size_t n = fread(buf + got, 1, (size_t)content_length - got, stdin);
        if (n == 0) { free(buf); return NULL; }
        got += n;
    }
    buf[content_length] = '\0';
    return buf;
}

int main(void) {
    for (;;) {
        char *raw = read_frame();
        if (!raw) break;
        bool done = lsp_handle(raw, (int)strlen(raw));
        if (g_out_len > 0) fwrite(g_out, 1, g_out_len, stdout);
        fflush(stdout);
        free(raw);
        if (done) return g_shut_down ? 0 : 1;
    }
    return 0;
}

#endif /* !LSP_NO_MAIN */
