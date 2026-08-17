/* csv_load.c — typed CSV loader for dlp (S5).
 *
 * Loads a CSV file (header row + data rows) into a relation of a dl_schema,
 * coercing every column against the schema's per-column type:
 *   - header row maps CSV columns to schema columns BY NAME (any order);
 *     unknown / missing / duplicate headers are errors listing both sets;
 *   - DLT_TEXT    — the cell is taken verbatim (minus a single surrounding
 *                   "..." pair) and interned via dl_intern_str;
 *   - DLT_NATURAL — ASCII-trimmed, must match ^[0-9]+$ and be <= 4294967295;
 *   - facts for an IDB (rule-head) relation are rejected (put facts in an
 *     EDB relation).
 *
 * When db == NULL this is a DRY-RUN: validation + coercion only (used by
 * `dlp check`; nothing is written).  When db != NULL the relation must already
 * be declared and validated facts are added via dl_add_fact (used by build).
 *
 * Errors carry `path:LINE:COL:` positions where LINE is a 1-based CSV row
 * (header row == 1) and COL is a 1-based field index.
 *
 * The dl_reldef struct (src/schema.h) stores column TYPES only, not column
 * names, so header-name mapping needs the names retained by the dlp layer.
 * schema_load.c fills dlp_schema_colname() during its walk; csv_load.c uses
 * that to resolve each CSV header to a schema column position.
 */
#include "dlp.h"
#include "dl.h"
#include "termstore.h"
#include "coerce.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Split one CSV line into fields, honouring a single surrounding "..." quote
 * pair around a field (no escaped quotes / multi-line fields — the same
 * contract as the engine's csv_split, dl.c).  Fields are stored into `out`
 * (pointers into `line`, NUL-terminated in place).  Returns the field count. */
static int csv_split(char *line, char **out, int cap) {
    int n = 0;
    char *p = line;
    while (*p) {
        char *start = p;
        int quoted = 0;
        if (*p == '"') { quoted = 1; start++; p++; }
        while (*p) {
            if (*p == '"') { *p = '\0'; p++; break; }
            if (*p == ',' && !quoted) break;
            p++;
        }
        if (*p == ',') { *p = '\0'; p++; }
        if (n < cap) out[n++] = start;
    }
    return n;
}

/* Trim ASCII whitespace in place; returns the leading pointer. */
static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* Strip a single surrounding "..." pair if present (in place). */
static void unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int seterr(char *errbuf, size_t errcap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errcap, fmt, ap);
    va_end(ap);
    return -1;
}

/* Free every compiled regex DFA in rdfs[0..n-1] (NULL-safe, idempotent). */
static void free_rdfs(regex_dfa **rdfs, int n) {
    for (int j = 0; j < n; j++)
        if (rdfs[j]) { regex_dfa_free(rdfs[j]); rdfs[j] = NULL; }
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

/* Parse a Natural (decimal digits only, <= u32 max) from `cell`.  0 on success
 * else -1. */
static int parse_nat(const char *cell, uint32_t *out) {
    const char *p = cell;
    if (!*p) return -1;
    unsigned long v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > 4294967295UL) return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

/* Coerce ONE List/Optional ELEMENT cell string against `elem` (a flat scalar
 * type).  Needs db only for DLT_TEXT interning; when db==NULL (dry-run) a Text
 * element stores 0.  0 on success else -1. */
static int coerce_elem_cell(dl_db *db, dl_coltype elem, char *cell, uint32_t *out) {
    switch (elem) {
    case DLT_NATURAL:
    case DLT_TIMESTAMP:
        return parse_nat(cell, out);
    case DLT_TEXT:
        if (db) *out = dl_intern_str(db, cell);
        else *out = 0;
        return 0;
    case DLT_BOOL:
        return parse_bool(cell, out);
    case DLT_CHAR:
        return parse_char(cell, strlen(cell), out);
    case DLT_DATE:
        return parse_date(cell, out);
    case DLT_SIGNED:
        return parse_signed(cell, out);
    default:
        return -1;
    }
}

/* Split a list-inner string "a,b,c" (already un-bracketed) into elements,
 * honoring a single surrounding '...' quote pair around an element (so a Text
 * element with an embedded comma survives).  Elements are pointers into `s`
 * (NUL-terminated in place).  Returns the TOTAL element count (may exceed
 * `cap`; callers detect overflow and reject). */
static int list_split(char *s, char **out, int cap) {
    int n = 0;
    char *p = s;
    while (*p) {
        char *start = p;
        int quoted = 0;
        if (*p == '\'') { quoted = 1; start++; p++; }
        while (*p) {
            if (*p == '\'') { *p = '\0'; p++; break; }
            if (*p == ',' && !quoted) break;
            p++;
        }
        if (*p == ',') { *p = '\0'; p++; }
        if (n < cap) out[n] = start;
        n++;
    }
    return n;
}


int dlp_csv_load(dl_db *db, const dl_schema *s, const char *rel,
                 const char *path, char *errbuf, size_t errcap) {
    if (errbuf && errcap > 0) errbuf[0] = '\0';
    const dl_reldef *r = dl_schema_find(s, rel);
    if (!r)
        return seterr(errbuf, errcap, "relation '%s' not declared in schema.dhall", rel);
    if (r->is_idb)
        return seterr(errbuf, errcap,
                      "%s:1: relation '%s' is rule-defined (IDB); put facts in an EDB relation",
                      path, rel);

    FILE *f = fopen(path, "rb");
    if (!f) return seterr(errbuf, errcap, "cannot open '%s'", path);

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int lineno = 0;             /* 1-based CSV row; header row == 1 */
    int fact_count = 0;

    /* Compile each regex-constrained Text column's regex ONCE (regex_compile
     * allocates ~49 MiB/DFA — never per-cell).  rdfs[j] is non-NULL iff
     * column j has a regex; freed on every return path via free_rdfs. */
    regex_dfa *rdfs[DL_SCHEMA_MAX_ARITY];
    memset(rdfs, 0, sizeof rdfs);
    for (int j = 0; j < r->arity; j++) {
        if (r->cols[j].tag == DLT_TEXT && r->cols[j].has_regex) {
            rdfs[j] = regex_compile(r->cols[j].regex);
            if (!rdfs[j] || rdfs[j]->errmsg || rdfs[j]->n_states == 0) {
                const char *em = rdfs[j] ? rdfs[j]->errmsg : "compile failed";
                free_rdfs(rdfs, r->arity); free(line); fclose(f);
                return seterr(errbuf, errcap,
                              "%s: bad regex '%s' on column '%s': %s",
                              path, r->cols[j].regex,
                              dlp_schema_colname(s, rel, j), em ? em : "");
            }
        }
    }

    /* Header line. */
    if ((len = getline(&line, &cap, f)) <= 0) {
        free_rdfs(rdfs, r->arity); free(line); fclose(f);
        return seterr(errbuf, errcap, "%s: empty file (no header row)", path);
    }
    lineno = 1;
    if (line[len - 1] == '\n') line[--len] = '\0';
    if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

    char *hdr[DL_SCHEMA_MAX_ARITY];
    int nhdr = csv_split(line, hdr, DL_SCHEMA_MAX_ARITY);

    /* Column map: colmap[i] = schema column index for CSV field i.
       Schema column names come from dlp_schema_colname(). */
    int colmap[DL_SCHEMA_MAX_ARITY];
    int used[DL_SCHEMA_MAX_ARITY];
    memset(used, 0, sizeof used);

    for (int i = 0; i < nhdr; i++) {
        char *name = trim_ws(hdr[i]);
        int idx = -1;
        for (int j = 0; j < r->arity; j++)
            if (strcmp(name, dlp_schema_colname(s, rel, j)) == 0) { idx = j; break; }
        if (idx < 0) {
            /* Header present in CSV but not a schema column name. */
            const char *got[DL_SCHEMA_MAX_ARITY];
            for (int k = 0; k < nhdr; k++) got[k] = trim_ws(hdr[k]);
            char exp[256], gots[256];
            const char *expn[DL_SCHEMA_MAX_ARITY];
            for (int k = 0; k < r->arity; k++) expn[k] = dlp_schema_colname(s, rel, k);
            join_names(exp, sizeof exp, expn, r->arity);
            join_names(gots, sizeof gots, got, nhdr);
            free_rdfs(rdfs, r->arity); free(line); fclose(f);
            return seterr(errbuf, errcap,
                          "%s:1: header error: relation '%s' has unknown column '%s'; expects %s, got %s",
                          path, rel, name, exp, gots);
        }
        if (used[idx]) {
            free_rdfs(rdfs, r->arity); free(line); fclose(f);
            return seterr(errbuf, errcap,
                          "%s:1: header error: relation '%s' has duplicate column '%s'",
                          path, rel, name);
        }
        used[idx] = 1;
        colmap[i] = idx;
    }

    /* Missing columns: a schema column that never appeared in the header. */
    for (int j = 0; j < r->arity; j++) {
        if (!used[j]) {
            const char *got[DL_SCHEMA_MAX_ARITY];
            for (int k = 0; k < nhdr; k++) got[k] = trim_ws(hdr[k]);
            char exp[256], gots[256];
            const char *expn[DL_SCHEMA_MAX_ARITY];
            for (int k = 0; k < r->arity; k++) expn[k] = dlp_schema_colname(s, rel, k);
            join_names(exp, sizeof exp, expn, r->arity);
            join_names(gots, sizeof gots, got, nhdr);
            free_rdfs(rdfs, r->arity); free(line); fclose(f);
            return seterr(errbuf, errcap,
                          "%s:1: header error: relation '%s' is missing column '%s'; expects %s, got %s",
                          path, rel, dlp_schema_colname(s, rel, j), exp, gots);
        }
    }

    /* Data rows. */
    while ((len = getline(&line, &cap, f)) > 0) {
        lineno++;
        if (line[len - 1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        char *s2 = trim_ws(line);
        if (!*s2) continue;     /* skip blank lines */

        char *fields[DL_SCHEMA_MAX_ARITY];
        int nf = csv_split(line, fields, DL_SCHEMA_MAX_ARITY);
        if (nf != r->arity) {
            free_rdfs(rdfs, r->arity); free(line); fclose(f);
            return seterr(errbuf, errcap, "%s:%d: expected %d columns, got %d",
                          path, lineno, (int)r->arity, nf);
        }

        uint32_t cols[DL_SCHEMA_MAX_ARITY];
        for (int i = 0; i < nf; i++) {
            int j = colmap[i];                       /* schema column index */
            switch (r->cols[j].tag) {
            case DLT_NATURAL:
            case DLT_TIMESTAMP: {
                /* Natural / Timestamp: ^[0-9]+$ and <= 4294967295.  Timestamp
                 * is a Natural-valued epoch in the same raw-u32 encoding. */
                char *cell = trim_ws(fields[i]);
                const char *p = cell;
                if (!*p) { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' expects Natural, got \"\"",
                                  path, lineno, i + 1, dlp_schema_colname(s, rel, j)); }
                unsigned long v = 0;
                for (; *p; p++) {
                    if (*p < '0' || *p > '9') { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                        return seterr(errbuf, errcap,
                                      "%s:%d:%d: column '%s' expects Natural, got \"%s\"",
                                      path, lineno, i + 1,
                                      dlp_schema_colname(s, rel, j), cell); }
                    v = v * 10 + (unsigned long)(*p - '0');
                    if (v > 4294967295UL) { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                        return seterr(errbuf, errcap,
                                      "%s:%d:%d: column '%s' expects Natural, got \"%s\"",
                                      path, lineno, i + 1,
                                      dlp_schema_colname(s, rel, j), cell); }
                }
                cols[j] = (uint32_t)v;
                break;
            }
            case DLT_TEXT: {
                /* DLT_TEXT: verbatim (minus a single quote pair), interned.
                 * Do NOT trim whitespace — Text cells are taken as-is.  If the
                 * column has a regex constraint, the raw string must match. */
                char *cell = fields[i];
                unquote(cell);
                if (rdfs[j] && !regex_dfa_full_match(rdfs[j], cell)) {
                    free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' value \"%s\" does not "
                                  "match regex '%s'",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j), cell,
                                  r->cols[j].regex);
                }
                if (db) cols[j] = dl_intern_str(db, cell);
                else cols[j] = 0;
                break;
            }
            case DLT_BOOL: {
                char *cell = trim_ws(fields[i]);
                if (parse_bool(cell, &cols[j]) != 0) { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' expects Bool, got \"%s\"",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j), cell); }
                break;
            }
            case DLT_CHAR: {
                char *cell = trim_ws(fields[i]);
                if (parse_char(cell, strlen(cell), &cols[j]) != 0) { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' expects Char (one UTF-8 scalar), got \"%s\"",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j), cell); }
                break;
            }
            case DLT_DATE: {
                char *cell = trim_ws(fields[i]);
                if (parse_date(cell, &cols[j]) != 0) { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' expects Date (yyyy-mm-dd), got \"%s\"",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j), cell); }
                break;
            }
            case DLT_SIGNED: {
                char *cell = trim_ws(fields[i]);
                if (parse_signed(cell, &cols[j]) != 0) { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' expects Signed, got \"%s\"",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j), cell); }
                break;
            }
            case DLT_LIST: {
                /* A List<elem> cell is a bracketed, QUOTED field "[e1,e2,...]".
                   csv_split keeps a quoted field (with embedded commas) as ONE
                   field and strips the surrounding quote pair, leaving "[...]".
                   Strip the brackets and sub-split on ',' honoring a '...'
                   element quote, then build the cons chain TAIL-FIRST. */
                char *cell = fields[i];
                unquote(cell);
                size_t ln = strlen(cell);
                if (ln < 2 || cell[0] != '[' || cell[ln - 1] != ']') {
                    free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' expects List \"[...]\", got \"%s\"",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j), cell);
                }
                cell[ln - 1] = '\0';
                cell++;                             /* skip '[' */
                char *elems[DLP_LIST_MAX_ELEMS];
                int ne = list_split(cell, elems, DLP_LIST_MAX_ELEMS);
                if (ne > DLP_LIST_MAX_ELEMS) {
                    free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' List has too many elements (cap %d)",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j), DLP_LIST_MAX_ELEMS);
                }
                uint32_t acc = TERM_NIL;
                for (int e = ne - 1; e >= 0; e--) {
                    char *ecell = trim_ws(elems[e]);
                    uint32_t ev;
                    if (coerce_elem_cell(db, r->cols[j].elem, ecell, &ev) != 0) {
                        free_rdfs(rdfs, r->arity); free(line); fclose(f);
                        return seterr(errbuf, errcap,
                                      "%s:%d:%d: column '%s' List element %d is not coercible to its element type",
                                      path, lineno, i + 1,
                                      dlp_schema_colname(s, rel, j), e);
                    }
                    acc = dl_term_cons(db, ev, acc);
                }
                cols[j] = acc;
                break;
            }
            case DLT_OPTIONAL: {
                /* Optional<elem>: empty cell -> None; else coerce elem. */
                char *cell = trim_ws(fields[i]);
                if (!*cell) { cols[j] = DLP_OPT_NONE; break; }
                if (coerce_elem_cell(db, r->cols[j].elem, cell, &cols[j]) != 0) {
                    free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' Optional element is not coercible to its element type",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j));
                }
                break;
            }
            case DLT_ENUM: {
                /* Enum: cell must be one of evalues (case-sensitive), then
                   interned (same as Text). */
                char *cell = trim_ws(fields[i]);
                unquote(cell);
                int ok = 0;
                for (int k = 0; k < r->cols[j].n_evalues; k++)
                    if (strcmp(cell, r->cols[j].evalues[k]) == 0) { ok = 1; break; }
                if (!ok) { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' expects an Enum value from {%s}, got \"%s\"",
                                  path, lineno, i + 1,
                                  dlp_schema_colname(s, rel, j),
                                  r->cols[j].n_evalues > 0 ? r->cols[j].evalues[0] : "",
                                  cell); }
                if (db) cols[j] = dl_intern_str(db, cell);
                else cols[j] = 0;
                break;
            }
            default: { free_rdfs(rdfs, r->arity); free(line); fclose(f);
                return seterr(errbuf, errcap,
                              "%s:%d:%d: column '%s' has an unsupported type for CSV loading",
                              path, lineno, i + 1, dlp_schema_colname(s, rel, j)); }
            }
        }

        /* Enforce per-column min/max value constraints (finish-dlp Item 2). */
        for (int j = 0; j < r->arity; j++) {
            if (!check_minmax(&r->cols[j], cols[j])) {
                char rb[64];
                const dl_colspec *cc = &r->cols[j];
                if (cc->has_min && cc->has_max)
                    snprintf(rb, sizeof rb, "[%lld..%lld]", (long long)cc->min, (long long)cc->max);
                else if (cc->has_min)
                    snprintf(rb, sizeof rb, "[%lld..]", (long long)cc->min);
                else if (cc->has_max)
                    snprintf(rb, sizeof rb, "[..%lld]", (long long)cc->max);
                else
                    rb[0] = '\0';
                free_rdfs(rdfs, r->arity); free(line); fclose(f);
                return seterr(errbuf, errcap,
                              "%s:%d: column '%s' value out of range (allowed %s)",
                              path, lineno, dlp_schema_colname(s, rel, j), rb);
            }
        }

        if (db) {
            if (dl_add_fact(db, rel, cols, r->arity) < 0) {
                free_rdfs(rdfs, r->arity); free(line); fclose(f);
                return seterr(errbuf, errcap, "%s:%d: dl_add_fact failed", path, lineno);
            }
        }
        fact_count++;
    }

    free_rdfs(rdfs, r->arity);
    free(line);
    fclose(f);
    return fact_count;
}
