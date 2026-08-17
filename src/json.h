/* json.h — minimal JSON value tree + writer for the LSP server's JSON-RPC 2.0
   protocol.  Self-contained: the parser produces a heap-allocated tree freed by
   json_free(); the writer appends to a jbuf (a tiny growable byte buffer local
   to this file — datalog has no TmpBuf).  Deliberately NOT tied to the engine:
   incoming LSP messages must outlive any per-evaluation state. */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdbool.h>

/* Tiny growable byte buffer for JSON output (self-contained). */
typedef struct {
    char  *s;
    size_t len;
    size_t cap;
} jbuf;

void jbuf_init(jbuf *b);
void jbuf_free(jbuf *b);

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JsonType;

typedef struct Json Json;
struct Json {
    JsonType type;
    union {
        bool b;
        double num;                 /* J_NUM (integer-valued for whole numbers) */
        char *str;                  /* J_STR (NUL-terminated, malloc'd) */
        struct { Json **items; int n; } arr;
        struct { char **keys; Json **vals; int n; } obj;
    } as;
};

/* Parse `len` bytes of JSON text; returns the root value or NULL on malformed
   input.  The returned tree is heap-allocated; free it with json_free(). */
Json *json_parse(const char *s, size_t len);

/* Recursively free a parsed tree (NULL-safe). */
void json_free(Json *v);

/* Accessors (all NULL-safe). */
Json       *json_obj_get(const Json *v, const char *key); /* NULL if absent/not obj */
const char *json_str(const Json *v);                      /* NULL if not J_STR */
double      json_num(const Json *v);                      /* 0.0 if not J_NUM */
Json       *json_arr_get(const Json *v, int i);           /* NULL if OOB/not arr */

/* ---- writer: appends to a jbuf (caller jbuf_init/free) ---- */
void json_write_raw   (jbuf *b, const char *s);  /* raw token or structural char */
void json_write_string(jbuf *b, const char *s);  /* "..." with JSON escaping */
void json_write_key   (jbuf *b, const char *k);  /* "k": */
void json_write_int   (jbuf *b, long long v);
void json_write_bool  (jbuf *b, bool v);
void json_write_null  (jbuf *b);

/* Re-emit an arbitrary Json value (used to echo a request id verbatim). */
void json_emit(jbuf *b, const Json *v);

#endif /* JSON_H */
