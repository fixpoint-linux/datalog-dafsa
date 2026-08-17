/* workflow.c — dlp project check/build/query orchestration (S5).
 *
 * Shared pipeline for the three dlp workflow commands:
 *   check  — load schema.dhall, infer EDB/IDB from rule heads, typecheck every
 *            rules file (*.datalog), and dry-run validate every data file
 *            (*.csv).  No writes.
 *   build  — run check; if clean, open dir/.build, declare the EDB relations,
 *            load the CSVs (typed), load the concatenated rules, compile, and
 *            publish a snapshot.
 *   query  — build in-process, parse the goal, materialize the goal relation,
 *            filter on bound positions, and print the FREE columns type-aware.
 *
 * Everything here lives in the dlp layer (dlp sources); no src engine
 * changes.
 */
#include "dlp.h"
#include "dl.h"
#include "parser.h"
#include "typecheck.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ─── Small helpers ────────────────────────────────────────────────────── */

static int seterr(char *errbuf, size_t errcap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errcap, fmt, ap);
    va_end(ap);
    return -1;
}

static int has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Read an entire file into a malloc'd NUL-terminated buffer (NULL on error). */
static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (len == cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) { free(buf); fclose(f); return NULL; } buf = nb; }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* List the files in `dir` matching `suffix` (e.g. ".csv" / ".datalog"),
 * sorted alphabetically.  Returns a malloc'd array of malloc'd basenames
 * (no directory prefix), `*count` set.  Returns NULL on error. */
static char **list_dir(const char *dir, const char *suffix, int *count) {
    DIR *d = opendir(dir);
    if (!d) { *count = -1; return NULL; }
    int cap = 8, n = 0;
    char **out = malloc((size_t)cap * sizeof *out);
    if (!out) { closedir(d); *count = -1; return NULL; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!has_suffix(e->d_name, suffix)) continue;
        if (n == cap) { cap *= 2; char **nb = realloc(out, (size_t)cap * sizeof *out); if (!nb) { goto fail; } out = nb; }
        out[n] = strdup(e->d_name);
        if (!out[n]) goto fail;
        n++;
    }
    closedir(d);
    /* qsort basenames. */
    int cmp(const void *a, const void *b) {
        return strcmp(*(char *const *)a, *(char *const *)b);
    }
    qsort(out, (size_t)n, sizeof *out, cmp);
    *count = n;
    return out;
fail:
    for (int i = 0; i < n; i++) free(out[i]);
    free(out);
    closedir(d);
    *count = -1;
    return NULL;
}

/* ─── EDB/IDB inference + rule typechecking (D1) ───────────────────────── */
/* Parse each rules file (*.datalog) in sorted order, mark every rule-head
 * relation is_idb=1 on the public dl_schema, and typecheck the rules directly
 * (dl_load_rules swallows the diagnostic).  Prints one error line per failing
 * file to stderr and returns the number of files that failed to typecheck. */
static int typecheck_rules_dir(dl_schema *schema, const char *rules_dir, const char *rules_prefix) {
    int nfiles = 0, errs = 0;
    char **files = list_dir(rules_dir, ".datalog", &nfiles);
    if (nfiles < 0) return -1;
    if (nfiles == 0) { free(files); return 0; }

    for (int f = 0; f < nfiles; f++) {
        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", rules_prefix, files[f]) >= (int)sizeof path) { errs++; continue; }
        char *src = read_all(path);
        if (!src) { fprintf(stderr, "rules/%s: cannot read\n", files[f]); errs++; continue; }

        parser *p = parse_create(src);
        int n_rules = 0;
        rule **rules = parse_rules(p, &n_rules);
        if (!rules) {
            const char *msg = parse_last_error(p, NULL);
            fprintf(stderr, "rules/%s: parse error: %s\n", files[f], msg ? msg : "unknown");
            errs++;
            parse_free(p);
            free(src);
            continue;
        }

        /* Mark every rule head as IDB. */
        for (int i = 0; i < n_rules; i++) {
            if (!rules[i] || !rules[i]->head || !rules[i]->head->pred) continue;
            for (int k = 0; k < schema->n_rels; k++)
                if (strcmp(schema->rels[k].name, rules[i]->head->pred) == 0)
                    schema->rels[k].is_idb = 1;
        }

        /* Typecheck this file's rules against the schema. */
        char errbuf[512];
        errbuf[0] = '\0';
        if (dl_typecheck_rules(schema, rules, n_rules, errbuf, sizeof errbuf) != 0) {
            /* Per-file parse => <input>:line:col is file-relative (D4). */
            fprintf(stderr, "rules/%s: %s\n", files[f], errbuf[0] ? errbuf : "typecheck failed");
            errs++;
        }

        for (int i = 0; i < n_rules; i++) rule_free(rules[i]);
        free(rules);
        parse_free(p);
        free(src);
    }

    for (int i = 0; i < nfiles; i++) free(files[i]);
    free(files);
    return errs;
}

/* Dry-run validate every data file (*.csv) (no writes).  Prints errors to
 * stderr and returns the number of failing files. */
static int check_data_dir(const dl_schema *schema, const char *data_dir, const char *data_prefix) {
    int nfiles = 0, errs = 0;
    char **files = list_dir(data_dir, ".csv", &nfiles);
    if (nfiles < 0) return -1;
    for (int f = 0; f < nfiles; f++) {
        /* relation name == CSV stem */
        char rel[256];
        size_t n = strlen(files[f]);
        if (n >= 4) n -= 4;             /* strip ".csv" */
        if (n >= sizeof rel) n = sizeof rel - 1;
        memcpy(rel, files[f], n);
        rel[n] = '\0';
        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", data_prefix, files[f]) >= (int)sizeof path) { errs++; continue; }
        char errbuf[512];
        errbuf[0] = '\0';
        long cnt = dlp_csv_load(NULL, schema, rel, path, errbuf, sizeof errbuf);
        if (cnt < 0) {
            fprintf(stderr, "%s\n", errbuf[0] ? errbuf : "csv load failed");
            errs++;
        }
    }
    for (int i = 0; i < nfiles; i++) free(files[i]);
    free(files);
    return errs;
}

/* Build the concatenated rules source by reading every rules file (*.datalog)
 * in sorted order and joining them with a newline.  Returns malloc'd buffer or
 * NULL on error (a missing rules dir with zero files yields an empty string). */
static char *concat_rules(const char *rules_dir, const char *rules_prefix,
                          int *has_rules) {
    int nfiles = 0;
    char **files = list_dir(rules_dir, ".datalog", &nfiles);
    if (nfiles < 0) return NULL;
    if (has_rules) *has_rules = (nfiles > 0);
    if (nfiles == 0) { free(files); return NULL; }
    size_t total = 1;
    for (int i = 0; i < nfiles; i++) total += strlen(files[i]) + 2048;
    char *buf = malloc(total);
    if (!buf) { for (int i = 0; i < nfiles; i++) free(files[i]); free(files); return NULL; }
    buf[0] = '\0';
    for (int i = 0; i < nfiles; i++) {
        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", rules_prefix, files[i]) >= (int)sizeof path) continue;
        char *src = read_all(path);
        if (src) { strncat(buf, src, total - strlen(buf) - 1); free(src); }
        strncat(buf, "\n", total - strlen(buf) - 1);
    }
    for (int i = 0; i < nfiles; i++) free(files[i]);
    free(files);
    return buf;
}

/* Resolve project-relative paths for schema/data/rules given the project dir. */
static int project_paths(const char *dir, char *schema, size_t sc, char *data, size_t dc,
                         char *rules, size_t rc, char *build, size_t bc, char *errbuf, size_t errcap) {
    if (!dir || !*dir) dir = ".";
    if (strcmp(dir, ".") == 0) {
        snprintf(schema, sc, "schema.dhall");
        snprintf(data, dc, "data");
        snprintf(rules, rc, "rules");
        snprintf(build, bc, ".build");
    } else {
        if (snprintf(schema, sc, "%s/schema.dhall", dir) >= (int)sc ||
            snprintf(data, dc, "%s/data", dir) >= (int)dc ||
            snprintf(rules, rc, "%s/rules", dir) >= (int)rc ||
            snprintf(build, bc, "%s/.build", dir) >= (int)bc)
            return seterr(errbuf, errcap, "project path too long");
    }
    return 0;
}

/* Shared: run the full check (schema load + rules typecheck/IDB + data dry-run).
 * Returns 0 if clean, -1 if any error (already printed to stderr) or a fatal
 * error.  On success, `schema` is filled. */
static int do_check(const char *dir, dl_schema *schema, char *errbuf, size_t errcap) {
    char sp[1024], dp[1024], rp[1024], bp[1024];
    if (project_paths(dir, sp, sizeof sp, dp, sizeof dp, rp, sizeof rp, bp, sizeof bp, errbuf, errcap) != 0)
        return -1;

    if (dlp_schema_load(schema, sp, errbuf, errcap) != 0)
        return -1;  /* main prints the diagnostic; do not fprintf here (double-print) */

    int errs = 0;
    /* rules dir may be absent (no rules) — treat as no files. */
    struct stat st;
    if (stat(rp, &st) == 0) {
        int r = typecheck_rules_dir(schema, rp, rp);
        if (r < 0) { snprintf(errbuf, errcap, "cannot read rules dir '%s'", rp); return -1; }
        errs += r;
    }
    if (stat(dp, &st) == 0) {
        int d = check_data_dir(schema, dp, dp);
        if (d < 0) { snprintf(errbuf, errcap, "cannot read data dir '%s'", dp); return -1; }
        errs += d;
    }
    if (errs > 0) return -1;
    return 0;
}

/* ─── dlp check ────────────────────────────────────────────────────────── */
int dlp_project_check(const char *dir, char *errbuf, size_t errcap) {
    if (errbuf && errcap > 0) errbuf[0] = '\0';
    dl_schema schema;
    return do_check(dir, &schema, errbuf, errcap) == 0 ? 0 : 1;
}

/* ─── dlp build ────────────────────────────────────────────────────────── */
int dlp_project_build(const char *dir, char *errbuf, size_t errcap) {
    if (errbuf && errcap > 0) errbuf[0] = '\0';
    dl_schema schema;
    if (do_check(dir, &schema, errbuf, errcap) != 0)
        return 1;               /* errors already printed; no writes */

    char sp[1024], dp[1024], rp[1024], bp[1024];
    if (project_paths(dir, sp, sizeof sp, dp, sizeof dp, rp, sizeof rp, bp, sizeof bp, errbuf, errcap) != 0)
        return 1;

    dl_db *db = dl_open(bp);
    if (!db) return seterr(errbuf, errcap, "cannot open build dir '%s'", bp);
    if (dl_attach_schema(db, &schema) != 0) {
        dl_close(db);
        return seterr(errbuf, errcap, "cannot attach schema");
    }

    /* Declare ONLY EDB relations — compile_rules auto-declares rule heads,
       and pre-declaring an IDB would break fixpoint IDB treatment. */
    for (int i = 0; i < schema.n_rels; i++) {
        const dl_reldef *r = &schema.rels[i];
        if (r->is_idb) continue;
        if (dl_declare_relation(db, r->name, r->arity) != 0) {
            dl_close(db);
            return seterr(errbuf, errcap, "cannot declare relation '%s'", r->name);
        }
    }

    /* Load CSVs (typed). */
    struct stat st;
    if (stat(dp, &st) == 0) {
        int nfiles = 0;
        char **files = list_dir(dp, ".csv", &nfiles);
        if (nfiles < 0) { dl_close(db); return seterr(errbuf, errcap, "cannot read data dir '%s'", dp); }
        for (int f = 0; f < nfiles; f++) {
            char rel[256];
            size_t n = strlen(files[f]);
            if (n >= 4) n -= 4;
            if (n >= sizeof rel) n = sizeof rel - 1;
            memcpy(rel, files[f], n);
            rel[n] = '\0';
            char path[1024];
            if (snprintf(path, sizeof path, "%s/%s", dp, files[f]) >= (int)sizeof path) { dl_close(db); return seterr(errbuf, errcap, "data path too long"); }
            char e[512];
            e[0] = '\0';
            if (dlp_csv_load(db, &schema, rel, path, e, sizeof e) < 0) {
                fprintf(stderr, "%s\n", e[0] ? e : "csv load failed");
                dl_close(db);
                return 1;
            }
        }
        for (int i = 0; i < nfiles; i++) free(files[i]);
        free(files);
    }

    /* Concatenate + load rules (re-typechecks against attached schema — passes
       since the per-file check already passed).  An EDB-only project (no rule
       files under the rules dir) skips rule load/compile entirely. */
    int has_rules = 0;
    char *rules_src = concat_rules(rp, rp, &has_rules);
    if (!rules_src && has_rules) {
        dl_close(db);
        return seterr(errbuf, errcap, "cannot read rules");
    }
    if (rules_src) {
        int loadrc = dl_load_rules(db, rules_src);
        free(rules_src);
        if (loadrc != 0) { dl_close(db); return seterr(errbuf, errcap, "rule load/compile failed"); }
        if (dl_compile(db) != 0) { dl_close(db); return seterr(errbuf, errcap, "compile failed"); }
    }
    if (dl_publish_snapshot(db) != 0) { dl_close(db); return seterr(errbuf, errcap, "publish failed"); }
    dl_close(db);
    return 0;
}

/* ─── Goal parsing + type-aware row printer (D3) ───────────────────────── */

/* A parsed query goal. */
typedef struct {
    char   rel[DL_SCHEMA_NAME_MAX];
    int    nargs;
    uint8_t bound[DL_SCHEMA_MAX_ARITY];  /* 1 = bound, 0 = free */
    uint32_t vals[DL_SCHEMA_MAX_ARITY];  /* bound values (raw u32 / interned) */
} goal;

/* Parse a goal string:  rel ( arg (, arg)* )?   Free iff first char is in
 * [A-Z_].  A bare integer ([0-9]+ <= 4294967295) is a raw u32 bound value;
 * any other bound arg is interned. */
static int parse_goal(dl_db *db, const char *s, goal *g) {
    memset(g, 0, sizeof *g);
    const char *p = s;
    while (*p && *p != '(') p++;
    size_t rlen = (size_t)(p - s);
    if (rlen == 0 || rlen >= DL_SCHEMA_NAME_MAX) return -1;
    memcpy(g->rel, s, rlen);
    g->rel[rlen] = '\0';
    if (!*p) { g->nargs = 0; return 0; }   /* bare relation name */
    p++;                                    /* skip '(' */
    int i = 0;
    while (i < DL_SCHEMA_MAX_ARITY) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (*p == ')') break;
        const char *start = p;
        while (*p && *p != ',' && *p != ')') p++;
        size_t alen = (size_t)(p - start);
        char arg[256];
        if (alen >= sizeof arg) return -1;
        memcpy(arg, start, alen);
        arg[alen] = '\0';
        int is_var = (alen > 0 && (arg[0] == '_' || isupper((unsigned char)arg[0])));
        if (!is_var) {
            /* bare int? */
            int is_int = 1;
            unsigned long v = 0;
            for (char *q = arg; *q; q++) {
                if (*q < '0' || *q > '9') { is_int = 0; break; }
                v = v * 10 + (unsigned long)(*q - '0');
                if (v > 4294967295UL) { is_int = 0; break; }
            }
            if (is_int && alen > 0) {
                g->bound[i] = 1;
                g->vals[i] = (uint32_t)v;
            } else {
                g->bound[i] = 1;
                g->vals[i] = dl_intern_str(db, arg);
            }
        } else {
            g->bound[i] = 0;
        }
        i++;
        if (*p == ')') break;
    }
    g->nargs = i;
    while (*p && *p != ')') p++;
    if (*p != ')') return -1;
    return 0;
}

/* Print one tuple's FREE columns, type-aware, space-separated. */
typedef struct {
    dl_db        *db;
    const dl_reldef *r;
    const goal   *g;
} print_ctx;

static int print_row(const uint32_t *cols, uint8_t arity, void *user) {
    print_ctx *ctx = (print_ctx *)user;
    (void)arity;
    /* Filter on the bound positions: a row only qualifies if every bound
       column matches its goal value.  (v1 evaluates via full materialization.) */
    for (int i = 0; i < ctx->g->nargs; i++)
        if (ctx->g->bound[i] && cols[i] != ctx->g->vals[i])
            return 0;
    int first = 1;
    int col = 0;
    for (int i = 0; i < ctx->g->nargs; i++) {
        if (ctx->g->bound[i]) continue;
        if (!first) printf(" ");
        first = 0;
        if (ctx->r->cols[i] == DLT_NATURAL)
            printf("%u", cols[i]);
        else {
            const char *s = dl_intern_str_of(ctx->db, cols[i]);
            printf("%s", s ? s : "");
        }
        col++;
    }
    if (col > 0) printf("\n");
    return 0;
}

/* ─── dlp query ────────────────────────────────────────────────────────── */
int dlp_project_query(const char *dir, const char *goal_str, char *errbuf, size_t errcap) {
    if (errbuf && errcap > 0) errbuf[0] = '\0';
    dl_schema schema;
    if (do_check(dir, &schema, errbuf, errcap) != 0)
        return 1;               /* errors already printed; no writes */

    char sp[1024], dp[1024], rp[1024], bp[1024];
    if (project_paths(dir, sp, sizeof sp, dp, sizeof dp, rp, sizeof rp, bp, sizeof bp, errbuf, errcap) != 0)
        return 1;

    dl_db *db = dl_open(bp);
    if (!db) return seterr(errbuf, errcap, "cannot open build dir '%s'", bp);
    if (dl_attach_schema(db, &schema) != 0) { dl_close(db); return seterr(errbuf, errcap, "cannot attach schema"); }

    for (int i = 0; i < schema.n_rels; i++) {
        const dl_reldef *r = &schema.rels[i];
        if (r->is_idb) continue;
        if (dl_declare_relation(db, r->name, r->arity) != 0) { dl_close(db); return seterr(errbuf, errcap, "cannot declare relation '%s'", r->name); }
    }

    struct stat st;
    if (stat(dp, &st) == 0) {
        int nfiles = 0;
        char **files = list_dir(dp, ".csv", &nfiles);
        if (nfiles < 0) { dl_close(db); return seterr(errbuf, errcap, "cannot read data dir '%s'", dp); }
        for (int f = 0; f < nfiles; f++) {
            char rel[256];
            size_t n = strlen(files[f]);
            if (n >= 4) n -= 4;
            if (n >= sizeof rel) n = sizeof rel - 1;
            memcpy(rel, files[f], n);
            rel[n] = '\0';
            char path[1024];
            if (snprintf(path, sizeof path, "%s/%s", dp, files[f]) >= (int)sizeof path) { dl_close(db); return seterr(errbuf, errcap, "data path too long"); }
            char e[512]; e[0] = '\0';
            if (dlp_csv_load(db, &schema, rel, path, e, sizeof e) < 0) {
                fprintf(stderr, "%s\n", e[0] ? e : "csv load failed");
                dl_close(db); return 1;
            }
        }
        for (int i = 0; i < nfiles; i++) free(files[i]);
        free(files);
    }

    int has_rules = 0;
    char *rules_src = concat_rules(rp, rp, &has_rules);
    if (!rules_src && has_rules) { dl_close(db); return seterr(errbuf, errcap, "cannot read rules"); }
    if (rules_src) {
        int loadrc = dl_load_rules(db, rules_src);
        free(rules_src);
        if (loadrc != 0) { dl_close(db); return seterr(errbuf, errcap, "rule load/compile failed"); }
        if (dl_compile(db) != 0) { dl_close(db); return seterr(errbuf, errcap, "compile failed"); }
    }
    if (dl_publish_snapshot(db) != 0) { dl_close(db); return seterr(errbuf, errcap, "publish failed"); }

    /* Parse the goal. */
    goal g;
    if (parse_goal(db, goal_str, &g) != 0) { dl_close(db); return seterr(errbuf, errcap, "bad goal: %s", goal_str); }
    const dl_reldef *r = dl_schema_find(&schema, g.rel);
    if (!r) { dl_close(db); return seterr(errbuf, errcap, "goal relation '%s' not in schema", g.rel); }
    if (g.nargs != r->arity) { dl_close(db); return seterr(errbuf, errcap, "goal '%s' has %d args; relation '%s' has arity %d", goal_str, g.nargs, g.rel, (int)r->arity); }

    print_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.db = db;
    ctx.r = r;
    ctx.g = &g;

    /* A fully-ground goal (no free args) is an exact lookup. */
    int any_free = 0;
    for (int i = 0; i < g.nargs; i++)
        if (!g.bound[i]) { any_free = 1; break; }

    if (!any_free) {
        uint32_t lookup_cols[DL_SCHEMA_MAX_ARITY];
        for (int i = 0; i < g.nargs; i++) lookup_cols[i] = g.vals[i];
        int found = dl_lookup(db, g.rel, lookup_cols, (uint8_t)g.nargs);
        printf("%s\n", found ? "found" : "not found");
        dl_close(db);
        return 0;
    }

    /* Otherwise: evaluate via full materialization, filter on the bound
       positions in the callback (simple + correct for any binding pattern;
       fine for the small worked example). */
    long n = dl_query(db, g.rel, print_row, &ctx);
    if (n < 0) { dl_close(db); return seterr(errbuf, errcap, "query failed"); }

    dl_close(db);
    return 0;
}
