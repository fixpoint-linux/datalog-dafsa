/* json.c — minimal JSON decoder (heap tree) + encoder (to a jbuf) for the LSP
   JSON-RPC 2.0 protocol.  Handles the full JSON grammar we need (objects,
   arrays, strings with \uXXXX escapes incl. surrogate pairs, numbers, true/
   false/null) and emits compact JSON (no insignificant whitespace).  Out-of-
   memory is fatal (exit 3), matching the rest of the codebase. */
#include "json.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* OOM is fatal, per the rest of the codebase. */
static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fputs("dl-lsp: out of memory\n", stderr); exit(3); }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fputs("dl-lsp: out of memory\n", stderr); exit(3); }
    return q;
}

/* ─── growable byte buffer ──────────────────────────────────────────── */

void jbuf_init(jbuf *b) { memset(b, 0, sizeof(*b)); }

void jbuf_free(jbuf *b) {
    if (!b) return;
    free(b->s);
    b->s = NULL; b->len = b->cap = 0;
}

static void jbuf_grow(jbuf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    {
        size_t cap = b->cap ? b->cap : 64;
        while (b->len + need + 1 > cap) cap *= 2;
        {
            char *ns = realloc(b->s, cap);
            if (!ns) { fputs("dl-lsp: out of memory\n", stderr); exit(3); }
            b->s = ns;
            b->cap = cap;
        }
    }
}

static void jbuf_add(jbuf *b, const char *s, size_t n) {
    jbuf_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void jbuf_addc(jbuf *b, char c) { jbuf_add(b, &c, 1); }

/* ─── decoder ───────────────────────────────────────────────────────── */

typedef struct { const char *p; const char *end; } JP;

static Json *jparse_value(JP *jp);

static Json *jnew(JsonType t) {
    Json *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

static void jskip_ws(JP *jp) {
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') jp->p++;
        else break;
    }
}

static void put_utf8(char *dst, unsigned cp, int *w) {
    if (cp < 0x80)          { dst[(*w)++] = (char)cp; }
    else if (cp < 0x800)    { dst[(*w)++] = (char)(0xC0 | (cp >> 6));
                              dst[(*w)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000)  { dst[(*w)++] = (char)(0xE0 | (cp >> 12));
                              dst[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                              dst[(*w)++] = (char)(0x80 | (cp & 0x3F)); }
    else                    { dst[(*w)++] = (char)(0xF0 | (cp >> 18));
                              dst[(*w)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                              dst[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                              dst[(*w)++] = (char)(0x80 | (cp & 0x3F)); }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(JP *jp) {
    if (jp->end - jp->p < 4) return -1;
    int u = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval((unsigned char)jp->p[i]);
        if (h < 0) return -1;
        u = (u << 4) | h;
    }
    jp->p += 4;
    return u;
}

static char *jparse_string(JP *jp) {
    if (jp->p >= jp->end || *jp->p != '"') return NULL;
    jp->p++;
    size_t cap = (size_t)(jp->end - jp->p) + 1;
    char *out = xmalloc(cap);
    int w = 0;
    while (jp->p < jp->end) {
        unsigned char c = (unsigned char)*jp->p;
        if (c == '"') { jp->p++; out[w] = '\0'; return out; }
        if (c == '\\') {
            jp->p++;
            if (jp->p >= jp->end) break;
            unsigned char e = (unsigned char)*jp->p++;
            switch (e) {
            case '"':  out[w++] = '"';  break;
            case '\\': out[w++] = '\\'; break;
            case '/':  out[w++] = '/';  break;
            case 'b':  out[w++] = '\b'; break;
            case 'f':  out[w++] = '\f'; break;
            case 'n':  out[w++] = '\n'; break;
            case 'r':  out[w++] = '\r'; break;
            case 't':  out[w++] = '\t'; break;
            case 'u': {
                int hi = read_hex4(jp);
                if (hi < 0) { free(out); return NULL; }
                unsigned cp = (unsigned)hi;
                if (hi >= 0xD800 && hi <= 0xDBFF) {
                    if (jp->end - jp->p < 2 || jp->p[0] != '\\' || jp->p[1] != 'u') { free(out); return NULL; }
                    jp->p += 2;
                    int lo = read_hex4(jp);
                    if (lo < 0xDC00 || lo > 0xDFFF) { free(out); return NULL; }
                    cp = 0x10000 + (((unsigned)hi - 0xD800) << 10) + ((unsigned)lo - 0xDC00);
                }
                put_utf8(out, cp, &w);
                break;
            }
            default: free(out); return NULL;
            }
        } else if (c < 0x20) {
            free(out); return NULL;
        } else {
            out[w++] = (char)c;
            jp->p++;
        }
    }
    free(out);
    return NULL;
}

static Json *jparse_string_node(JP *jp) {
    char *s = jparse_string(jp);
    if (!s) return NULL;
    Json *v = jnew(J_STR);
    v->as.str = s;
    return v;
}

static Json *jparse_number(JP *jp) {
    const char *start = jp->p;
    if (jp->p < jp->end && *jp->p == '-') jp->p++;
    while (jp->p < jp->end && isdigit((unsigned char)*jp->p)) jp->p++;
    if (jp->p < jp->end && *jp->p == '.') {
        jp->p++;
        while (jp->p < jp->end && isdigit((unsigned char)*jp->p)) jp->p++;
    }
    if (jp->p < jp->end && (*jp->p == 'e' || *jp->p == 'E')) {
        jp->p++;
        if (jp->p < jp->end && (*jp->p == '+' || *jp->p == '-')) jp->p++;
        while (jp->p < jp->end && isdigit((unsigned char)*jp->p)) jp->p++;
    }
    Json *v = jnew(J_NUM);
    v->as.num = strtod(start, NULL);
    return v;
}

static Json *jparse_array(JP *jp) {
    jp->p++;
    Json *v = jnew(J_ARR);
    Json **items = NULL;
    int n = 0, cap = 0;
    jskip_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') { jp->p++; return v; }
    for (;;) {
        jskip_ws(jp);
        Json *item = jparse_value(jp);
        if (!item) goto fail;
        if (n == cap) { cap = cap ? cap * 2 : 4; items = xrealloc(items, (size_t)cap * sizeof *items); }
        items[n++] = item;
        jskip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
        if (jp->p < jp->end && *jp->p == ']') { jp->p++; break; }
        goto fail;
    }
    v->as.arr.items = items;
    v->as.arr.n = n;
    return v;
fail:
    for (int i = 0; i < n; i++) json_free(items[i]);
    free(items);
    json_free(v);
    return NULL;
}

static Json *jparse_object(JP *jp) {
    jp->p++;
    Json *v = jnew(J_OBJ);
    char **keys = NULL;
    Json **vals = NULL;
    int n = 0, cap = 0;
    jskip_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') { jp->p++; return v; }
    for (;;) {
        jskip_ws(jp);
        char *k = jparse_string(jp);
        if (!k) goto fail;
        jskip_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':') { free(k); goto fail; }
        jp->p++;
        jskip_ws(jp);
        Json *val = jparse_value(jp);
        if (!val) { free(k); goto fail; }
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            keys = xrealloc(keys, (size_t)cap * sizeof *keys);
            vals = xrealloc(vals, (size_t)cap * sizeof *vals);
        }
        keys[n] = k;
        vals[n] = val;
        n++;
        jskip_ws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; break; }
        goto fail;
    }
    v->as.obj.keys = keys;
    v->as.obj.vals = vals;
    v->as.obj.n = n;
    return v;
fail:
    for (int i = 0; i < n; i++) { free(keys[i]); json_free(vals[i]); }
    free(keys);
    free(vals);
    json_free(v);
    return NULL;
}

static Json *jparse_value(JP *jp) {
    jskip_ws(jp);
    if (jp->p >= jp->end) return NULL;
    char c = *jp->p;
    if (c == '{') return jparse_object(jp);
    if (c == '[') return jparse_array(jp);
    if (c == '"') return jparse_string_node(jp);
    if (c == 't' && jp->end - jp->p >= 4 && !memcmp(jp->p, "true", 4)) { jp->p += 4; Json *v = jnew(J_BOOL); v->as.b = true; return v; }
    if (c == 'f' && jp->end - jp->p >= 5 && !memcmp(jp->p, "false", 5)) { jp->p += 5; Json *v = jnew(J_BOOL); v->as.b = false; return v; }
    if (c == 'n' && jp->end - jp->p >= 4 && !memcmp(jp->p, "null", 4)) { jp->p += 4; return jnew(J_NULL); }
    if (c == '-' || isdigit((unsigned char)c)) return jparse_number(jp);
    return NULL;
}

Json *json_parse(const char *s, size_t len) {
    JP jp = { s, s + len };
    Json *v = jparse_value(&jp);
    if (!v) return NULL;
    jskip_ws(&jp);
    if (jp.p != jp.end) { json_free(v); return NULL; }
    return v;
}

void json_free(Json *v) {
    if (!v) return;
    switch (v->type) {
    case J_STR: free(v->as.str); break;
    case J_ARR: for (int i = 0; i < v->as.arr.n; i++) json_free(v->as.arr.items[i]); free(v->as.arr.items); break;
    case J_OBJ: for (int i = 0; i < v->as.obj.n; i++) { free(v->as.obj.keys[i]); json_free(v->as.obj.vals[i]); } free(v->as.obj.keys); free(v->as.obj.vals); break;
    default: break;
    }
    free(v);
}

Json *json_obj_get(const Json *v, const char *key) {
    if (!v || v->type != J_OBJ) return NULL;
    for (int i = 0; i < v->as.obj.n; i++)
        if (!strcmp(v->as.obj.keys[i], key)) return v->as.obj.vals[i];
    return NULL;
}

const char *json_str(const Json *v) { return (v && v->type == J_STR) ? v->as.str : NULL; }
double      json_num(const Json *v) { return (v && v->type == J_NUM) ? v->as.num : 0.0; }
Json       *json_arr_get(const Json *v, int i) { return (v && v->type == J_ARR && i >= 0 && i < v->as.arr.n) ? v->as.arr.items[i] : NULL; }

/* ─── encoder ───────────────────────────────────────────────────────── */

void json_write_raw(jbuf *b, const char *s) { jbuf_add(b, s, strlen(s)); }

void json_write_string(jbuf *b, const char *s) {
    jbuf_addc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  jbuf_add(b, "\\\"", 2); break;
        case '\\': jbuf_add(b, "\\\\", 2); break;
        case '\b': jbuf_add(b, "\\b", 2);  break;
        case '\f': jbuf_add(b, "\\f", 2);  break;
        case '\n': jbuf_add(b, "\\n", 2);  break;
        case '\r': jbuf_add(b, "\\r", 2);  break;
        case '\t': jbuf_add(b, "\\t", 2);  break;
        default:
            if (*p < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                jbuf_add(b, esc, strlen(esc));
            } else {
                jbuf_addc(b, (char)*p);
            }
        }
    }
    jbuf_addc(b, '"');
}

void json_write_key(jbuf *b, const char *k) { json_write_string(b, k); jbuf_addc(b, ':'); }

void json_write_int(jbuf *b, long long v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", v);
    jbuf_add(b, buf, strlen(buf));
}

void json_write_bool(jbuf *b, bool v) { jbuf_add(b, v ? "true" : "false", v ? 4 : 5); }
void json_write_null(jbuf *b) { jbuf_add(b, "null", 4); }

void json_emit(jbuf *b, const Json *v) {
    if (!v) { json_write_null(b); return; }
    switch (v->type) {
    case J_NULL: json_write_null(b); break;
    case J_BOOL: json_write_bool(b, v->as.b); break;
    case J_NUM:
        if (v->as.num == (double)(long long)v->as.num)
            json_write_int(b, (long long)v->as.num);
        else {
            char buf[48];
            snprintf(buf, sizeof buf, "%.17g", v->as.num);
            jbuf_add(b, buf, strlen(buf));
        }
        break;
    case J_STR: json_write_string(b, v->as.str); break;
    case J_ARR:
        jbuf_addc(b, '[');
        for (int i = 0; i < v->as.arr.n; i++) { if (i) jbuf_addc(b, ','); json_emit(b, v->as.arr.items[i]); }
        jbuf_addc(b, ']');
        break;
    case J_OBJ:
        jbuf_addc(b, '{');
        for (int i = 0; i < v->as.obj.n; i++) { if (i) jbuf_addc(b, ','); json_write_key(b, v->as.obj.keys[i]); json_emit(b, v->as.obj.vals[i]); }
        jbuf_addc(b, '}');
        break;
    }
}
