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

    /* Header line. */
    if ((len = getline(&line, &cap, f)) <= 0) {
        free(line); fclose(f);
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
            free(line); fclose(f);
            return seterr(errbuf, errcap,
                          "%s:1: header error: relation '%s' has unknown column '%s'; expects %s, got %s",
                          path, rel, name, exp, gots);
        }
        if (used[idx]) {
            free(line); fclose(f);
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
            free(line); fclose(f);
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
            free(line); fclose(f);
            return seterr(errbuf, errcap, "%s:%d: expected %d columns, got %d",
                          path, lineno, (int)r->arity, nf);
        }

        uint32_t cols[DL_SCHEMA_MAX_ARITY];
        for (int i = 0; i < nf; i++) {
            int j = colmap[i];                       /* schema column index */
            char *cell = trim_ws(fields[i]);
            if (r->cols[j] == DLT_NATURAL) {
                /* ^[0-9]+$ and <= 4294967295 */
                const char *p = cell;
                if (!*p) { free(line); fclose(f);
                    return seterr(errbuf, errcap,
                                  "%s:%d:%d: column '%s' expects Natural, got \"\"",
                                  path, lineno, i + 1, dlp_schema_colname(s, rel, j)); }
                unsigned long v = 0;
                for (; *p; p++) {
                    if (*p < '0' || *p > '9') { free(line); fclose(f);
                        return seterr(errbuf, errcap,
                                      "%s:%d:%d: column '%s' expects Natural, got \"%s\"",
                                      path, lineno, i + 1,
                                      dlp_schema_colname(s, rel, j), cell); }
                    v = v * 10 + (unsigned long)(*p - '0');
                    if (v > 4294967295UL) { free(line); fclose(f);
                        return seterr(errbuf, errcap,
                                      "%s:%d:%d: column '%s' expects Natural, got \"%s\"",
                                      path, lineno, i + 1,
                                      dlp_schema_colname(s, rel, j), cell); }
                }
                cols[j] = (uint32_t)v;
            } else {
                /* DLT_TEXT: verbatim (minus a single quote pair), interned. */
                unquote(cell);
                if (db) cols[j] = dl_intern_str(db, cell);
                else cols[j] = 0;
            }
        }

        if (db) {
            if (dl_add_fact(db, rel, cols, r->arity) < 0) {
                free(line); fclose(f);
                return seterr(errbuf, errcap, "%s:%d: dl_add_fact failed", path, lineno);
            }
        }
        fact_count++;
    }

    free(line);
    fclose(f);
    return fact_count;
}
