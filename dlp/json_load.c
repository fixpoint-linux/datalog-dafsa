/* json_load.c — typed JSON loader for dlp (S6).
 *
 * Loads a JSON data file for a relation of a dl_schema.  File shape is a
 * JSON array of objects:
 *
 *     data/<rel>.json  =>  [ { "col": value, ... }, ... ]
 *
 * The file stem is the relation name; each object key is a column name.  JSON
 * is inherently typed, so unlike the CSV loader (which coerces text cells)
 * this loader enforces STRICT typing against the schema column type:
 *   - DLT_NATURAL — the value must be a JSON number that is integer-valued
 *     and within [0, 4294967295]; anything else (string, bool, null, object,
 *     array, non-integer or out-of-range number) is an error;
 *   - DLT_TEXT    — the value must be a JSON string.
 *
 * Every key in an object must match a schema column (unknown keys are errors
 * listing both sets) and every schema column must be present (missing columns
 * are errors listing both sets).  Facts for an IDB (rule-head) relation are
 * rejected (put facts in an EDB relation).
 *
 * When db == NULL this is a DRY-RUN: validation + coercion only (used by
 * `dlp check`; nothing is written).  When db != NULL the relation must already
 * be declared and validated facts are added via dl_add_fact (used by build).
 *
 * Errors carry `path: element N: column '...'` context (element index is the
 * 0-based array position).
 *
 * The JSON tree is produced by the engine's self-contained parser (src/json.h:
 * json_parse / json_free / json_obj_get / json_str / json_num), which dlp
 * compiles from source (src/json.c added to DLP_ENGINE_SRCS in the Makefile).
 */
#include "dlp.h"
#include "dl.h"
#include "termstore.h"
#include "json.h"
#include "coerce.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int seterr(char *errbuf, size_t errcap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errcap, fmt, ap);
    va_end(ap);
    return -1;
}

/* Coerce ONE List/Optional ELEMENT Json value against `elem` (a flat scalar
 * type).  Needs db only for DLT_TEXT interning (dry-run: 0).  0 on success,
 * -1 on a type/coercion mismatch. */
static int coerce_elem_json(dl_db *db, dl_coltype elem, Json *v, uint32_t *out) {
    switch (elem) {
    case DLT_NATURAL:
    case DLT_TIMESTAMP: {
        if (!v || v->type != J_NUM) return -1;
        double d = v->as.num;
        if (!(d >= 0.0) || d > 4294967295.0 || d != (double)(unsigned long)d) return -1;
        *out = (uint32_t)(unsigned long)d;
        return 0;
    }
    case DLT_TEXT: {
        const char *str = json_str(v);
        if (!str) return -1;
        if (db) *out = dl_intern_str(db, str);
        else *out = 0;
        return 0;
    }
    case DLT_BOOL:
        if (!v || v->type != J_BOOL) return -1;
        *out = v->as.b ? 1u : 0u;
        return 0;
    case DLT_CHAR: {
        const char *str = json_str(v);
        if (!str) return -1;
        return parse_char(str, strlen(str), out);
    }
    case DLT_DATE: {
        const char *str = json_str(v);
        if (!str) return -1;
        return parse_date(str, out);
    }
    case DLT_SIGNED: {
        if (!v || v->type != J_NUM) return -1;
        double d = v->as.num;
        if (d != (double)(long long)d || d < -2147483648.0 || d > 2147483647.0) return -1;
        *out = zigzag((int32_t)(long long)d);
        return 0;
    }
    default:
        return -1;
    }
}

/* Append "[a, b, c]" of `names` (n of them) to a buffer. */
static void join_names(char *buf, size_t cap, const char *const *names, int n) {
    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap > off ? cap - off : 0, "[");
    for (int i = 0; i < n; i++) {
        size_t w = (size_t)snprintf(buf + off, cap > off ? cap - off : 0,
                                    "%s%s", i ? ", " : "", names[i]);
        if (off + w >= cap) break;
        off += w;
    }
    if (off + 1 < cap) { buf[off] = ']'; buf[off + 1] = '\0'; }
}

/* Human-readable name for a JSON value type (used in diagnostics). */
static const char *type_name(const Json *v) {
    if (!v) return "missing";
    switch (v->type) {
    case J_NULL: return "null";
    case J_BOOL: return "boolean";
    case J_NUM:  return "number";
    case J_STR:  return "string";
    case J_ARR:  return "array";
    case J_OBJ:  return "object";
    }
    return "?";
}

/* Read an entire file into a malloc'd NUL-terminated buffer; `*out_len` set
 * to the byte length.  Returns NULL on error. */
static char *read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

int dlp_json_load(dl_db *db, const dl_schema *s, const char *rel,
                  const char *path, char *errbuf, size_t errcap) {
    if (errbuf && errcap > 0) errbuf[0] = '\0';
    const dl_reldef *r = dl_schema_find(s, rel);
    if (!r)
        return seterr(errbuf, errcap, "relation '%s' not declared in schema.dhall", rel);
    if (r->is_idb)
        return seterr(errbuf, errcap,
                      "%s: relation '%s' is rule-defined (IDB); put facts in an EDB relation",
                      path, rel);

    size_t len = 0;
    char *src = read_all(path, &len);
    if (!src)
        return seterr(errbuf, errcap, "cannot open '%s'", path);

    Json *root = json_parse(src, len);
    free(src);
    if (!root)
        return seterr(errbuf, errcap, "%s: malformed JSON", path);

    int rc = -1;
    int fact_count = 0;

    if (root->type != J_ARR) {
        seterr(errbuf, errcap, "%s: expected a JSON array of objects, got %s",
               path, type_name(root));
        goto done;
    }

    int nelem = root->as.arr.n;
    for (int e = 0; e < nelem; e++) {
        Json *el = json_arr_get(root, e);
        if (!el || el->type != J_OBJ) {
            seterr(errbuf, errcap, "%s: element %d is not an object (got %s)",
                   path, e, type_name(el));
            goto done;
        }

        /* Every object key must be a schema column; every schema column must
           be present. */
        int used[DL_SCHEMA_MAX_ARITY];
        memset(used, 0, sizeof used);
        for (int k = 0; k < el->as.obj.n; k++) {
            const char *key = el->as.obj.keys[k];
            int idx = -1;
            for (int j = 0; j < r->arity; j++)
                if (strcmp(key, dlp_schema_colname(s, rel, j)) == 0) { idx = j; break; }
            if (idx < 0) {
                int ngot = el->as.obj.n < DL_SCHEMA_MAX_ARITY ? el->as.obj.n : DL_SCHEMA_MAX_ARITY;
                const char *got[DL_SCHEMA_MAX_ARITY];
                for (int m = 0; m < ngot; m++) got[m] = el->as.obj.keys[m];
                char exp[256], gots[256];
                const char *expn[DL_SCHEMA_MAX_ARITY];
                for (int m = 0; m < r->arity; m++) expn[m] = dlp_schema_colname(s, rel, m);
                join_names(exp, sizeof exp, expn, r->arity);
                join_names(gots, sizeof gots, got, ngot);
                seterr(errbuf, errcap,
                       "%s: element %d: unknown column '%s'; expects %s, got %s",
                       path, e, key, exp, gots);
                goto done;
            }
            used[idx] = 1;
        }
        for (int j = 0; j < r->arity; j++) {
            if (!used[j]) {
                int ngot = el->as.obj.n < DL_SCHEMA_MAX_ARITY ? el->as.obj.n : DL_SCHEMA_MAX_ARITY;
                const char *got[DL_SCHEMA_MAX_ARITY];
                for (int m = 0; m < ngot; m++) got[m] = el->as.obj.keys[m];
                char exp[256], gots[256];
                const char *expn[DL_SCHEMA_MAX_ARITY];
                for (int m = 0; m < r->arity; m++) expn[m] = dlp_schema_colname(s, rel, m);
                join_names(exp, sizeof exp, expn, r->arity);
                join_names(gots, sizeof gots, got, ngot);
                seterr(errbuf, errcap,
                       "%s: element %d: missing column '%s'; expects %s, got %s",
                       path, e, dlp_schema_colname(s, rel, j), exp, gots);
                goto done;
            }
        }

        /* Strictly typed values, cols[] in schema order. */
        uint32_t cols[DL_SCHEMA_MAX_ARITY];
        for (int j = 0; j < r->arity; j++) {
            const char *cname = dlp_schema_colname(s, rel, j);
            Json *v = json_obj_get(el, cname);
            switch (r->cols[j].tag) {
            case DLT_NATURAL:
            case DLT_TIMESTAMP: {
                /* Natural/Timestamp: integer-valued number within u32 (epoch). */
                if (!v || v->type != J_NUM) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Natural, got %s",
                           path, e, cname, type_name(v));
                    goto done;
                }
                double d = v->as.num;
                /* NaN-safe: reject if !(d>=0); then reject out-of-range and
                   non-integer values before casting. */
                if (!(d >= 0.0) || d > 4294967295.0 || d != (double)(unsigned long)d) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Natural, got number",
                           path, e, cname);
                    goto done;
                }
                cols[j] = (uint32_t)(unsigned long)d;
                break;
            }
            case DLT_TEXT: {
                const char *str = json_str(v);
                if (!str) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Text, got %s",
                           path, e, cname, type_name(v));
                    goto done;
                }
                if (db) cols[j] = dl_intern_str(db, str);
                else cols[j] = 0;
                break;
            }
            case DLT_BOOL: {
                /* strict: JSON must be a real boolean true/false */
                if (!v || v->type != J_BOOL) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Bool, got %s",
                           path, e, cname, type_name(v));
                    goto done;
                }
                cols[j] = v->as.b ? 1u : 0u;
                break;
            }
            case DLT_CHAR: {
                const char *str = json_str(v);
                if (!str) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Char (one UTF-8 scalar), got %s",
                           path, e, cname, type_name(v));
                    goto done;
                }
                if (parse_char(str, strlen(str), &cols[j]) != 0) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Char (one UTF-8 scalar), got string",
                           path, e, cname);
                    goto done;
                }
                break;
            }
            case DLT_DATE: {
                const char *str = json_str(v);
                if (!str) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Date (yyyy-mm-dd), got %s",
                           path, e, cname, type_name(v));
                    goto done;
                }
                if (parse_date(str, &cols[j]) != 0) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Date (yyyy-mm-dd), got string",
                           path, e, cname);
                    goto done;
                }
                break;
            }
            case DLT_SIGNED: {
                /* JSON integer (may be negative) within i32 -> zigzag */
                if (!v || v->type != J_NUM) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Signed, got %s",
                           path, e, cname, type_name(v));
                    goto done;
                }
                double d = v->as.num;
                if (d != (double)(long long)d ||
                    d < -2147483648.0 || d > 2147483647.0) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Signed (i32 integer), got number",
                           path, e, cname);
                    goto done;
                }
                int32_t sv = (int32_t)(long long)d;
                cols[j] = zigzag(sv);
                break;
            }
            case DLT_LIST: {
                /* List<elem>: a JSON array; build the cons chain TAIL-FIRST. */
                if (!v || v->type != J_ARR) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects List (JSON array), got %s",
                           path, e, cname, type_name(v));
                    goto done;
                }
                int ne = v->as.arr.n;
                if (ne > DLP_LIST_MAX_ELEMS) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' List has too many elements (cap %d)",
                           path, e, cname, DLP_LIST_MAX_ELEMS);
                    goto done;
                }
                uint32_t acc = TERM_NIL;
                for (int ei = ne - 1; ei >= 0; ei--) {
                    uint32_t ev;
                    if (coerce_elem_json(db, r->cols[j].elem, v->as.arr.items[ei], &ev) != 0) {
                        seterr(errbuf, errcap,
                               "%s: element %d: column '%s' List element %d is not coercible to its element type",
                               path, e, cname, ei);
                        goto done;
                    }
                    acc = dl_term_cons(db, ev, acc);
                }
                cols[j] = acc;
                break;
            }
            case DLT_OPTIONAL: {
                /* Optional<elem>: JSON null -> None; else coerce elem.  A
                   MISSING key is already an error (enforced above). */
                if (v && v->type == J_NULL) { cols[j] = DLP_OPT_NONE; break; }
                if (coerce_elem_json(db, r->cols[j].elem, v, &cols[j]) != 0) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' Optional element is not coercible to its element type",
                           path, e, cname);
                    goto done;
                }
                break;
            }
            case DLT_ENUM: {
                /* Enum: a JSON string that must be one of evalues, interned. */
                const char *estr = json_str(v);
                if (!estr) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects Enum (string), got %s",
                           path, e, cname, type_name(v));
                    goto done;
                }
                int ok = 0;
                for (int k = 0; k < r->cols[j].n_evalues; k++)
                    if (strcmp(estr, r->cols[j].evalues[k]) == 0) { ok = 1; break; }
                if (!ok) {
                    seterr(errbuf, errcap,
                           "%s: element %d: column '%s' expects an Enum value from {%s}, got \"%s\"",
                           path, e, cname,
                           r->cols[j].n_evalues > 0 ? r->cols[j].evalues[0] : "",
                           estr);
                    goto done;
                }
                if (db) cols[j] = dl_intern_str(db, estr);
                else cols[j] = 0;
                break;
            }
            default: {
                seterr(errbuf, errcap,
                       "%s: element %d: column '%s' has an unsupported type for JSON loading",
                       path, e, cname);
                goto done;
            }
            }
        }

        if (db) {
            if (dl_add_fact(db, rel, cols, r->arity) < 0) {
                seterr(errbuf, errcap, "%s: element %d: dl_add_fact failed", path, e);
                goto done;
            }
        }
        fact_count++;
    }
    rc = fact_count;
done:
    json_free(root);
    return rc;
}
