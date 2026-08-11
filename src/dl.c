/*
 * dl.c — dl_db integration layer: ties interner + relation layers together
 *
 * Implements the public API from dl.h.  Coordinates between the term
 * interner (string ↔ u32) and per-relation DAFSA fact stores.
 *
 * KNOWN LIMITATION (B6): int/symbol id collision.  A raw integer column
 * value and an interned sym_id share the u32 namespace.  Keep each column
 * type-homogeneous (all ints or all symbols) in M1.
 */

#include "dl.h"
#include "intern.h"
#include "relation.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ─── dl_db struct ────────────────────────────────────────────────────── */

#define MAX_RELS 64  /* enough for M0 */

typedef struct {
    char     *name;
    relation *rel;
} rel_entry;

struct dl_db {
    char      *dir;
    interner  *ir;
    rel_entry  rels[MAX_RELS];
    size_t     nrels;

    /* M1: compiled rules */
    compiled_rule **crules;
    int             n_crules;
};

/* ─── Internal helpers ────────────────────────────────────────────────── */

/* Find a relation by name, return index or -1 */
static int find_rel(dl_db *db, const char *name)
{
    size_t i;
    for (i = 0; i < db->nrels; i++) {
        if (strcmp(db->rels[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

/* Build a file path: db->dir/name.suffix */
static char *make_path(dl_db *db, const char *name, const char *suffix)
{
    size_t dlen = strlen(db->dir);
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    char *path = malloc(dlen + 1 + nlen + slen + 1);
    if (!path) return NULL;
    memcpy(path, db->dir, dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, name, nlen);
    memcpy(path + dlen + 1 + nlen, suffix, slen + 1);
    return path;
}

/* ─── Lifecycle ───────────────────────────────────────────────────────── */

dl_db *dl_open(const char *dir)
{
    dl_db *db;
    char *rels_path;
    FILE *rf;

    if (!dir) return NULL;

    /* Create directory if needed */
    mkdir(dir, 0755);

    db = calloc(1, sizeof(*db));
    if (!db) return NULL;

    db->dir = strdup(dir);
    if (!db->dir) { free(db); return NULL; }

    /* Load or create interner */
    {
        char *fwd_path = make_path(db, "symbols", ".dafsa");
        char *rev_path = make_path(db, "symbols", ".array");
        if (!fwd_path || !rev_path) {
            free(fwd_path); free(rev_path);
            free(db->dir); free(db);
            return NULL;
        }
        db->ir = intern_load(fwd_path, rev_path);
        free(fwd_path);
        free(rev_path);
        if (!db->ir) { free(db->dir); free(db); return NULL; }
    }

    /* Load existing relations from metadata file */
    rels_path = make_path(db, "rels", ".txt");
    if (rels_path) {
        rf = fopen(rels_path, "r");
        if (rf) {
            char *line = NULL;
            size_t cap = 0;
            ssize_t len;
            while ((len = getline(&line, &cap, rf)) > 0) {
                char *colon;
                if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
                if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
                colon = strchr(line, ':');
                if (colon) {
                    *colon = '\0';
                    int arity = atoi(colon + 1);
                    if (arity >= 1 && arity <= 8 && db->nrels < MAX_RELS) {
                        dl_declare_relation(db, line, (uint8_t)arity);
                    }
                }
            }
            free(line);
            fclose(rf);
        }
        free(rels_path);
    }

    return db;
}

void dl_close(dl_db *db)
{
    size_t i;
    if (!db) return;

    /* Save interner */
    {
        char *fwd_path = make_path(db, "symbols", ".dafsa");
        char *rev_path = make_path(db, "symbols", ".array");
        if (fwd_path && rev_path)
            intern_save(db->ir, fwd_path, rev_path);
        free(fwd_path);
        free(rev_path);
    }

    /* Save each relation's DAFSA, and record its name/arity for the metadata
     * write below.  We must capture name/arity BEFORE freeing the relation,
     * or the metadata loop reads freed memory (use-after-free). */
    for (i = 0; i < db->nrels; i++) {
        char *path = make_path(db, db->rels[i].name, ".dafsa");
        if (path) {
            rel_save(db->rels[i].rel, path);
            free(path);
        }
    }

    /* Save relation metadata (name:arity per line) while names/rels are alive */
    {
        char *rels_path = make_path(db, "rels", ".txt");
        FILE *rf;
        if (rels_path && (rf = fopen(rels_path, "w"))) {
            for (i = 0; i < db->nrels; i++) {
                fprintf(rf, "%s:%d\n", db->rels[i].name,
                        (int)rel_arity(db->rels[i].rel));
            }
            fclose(rf);
        }
        free(rels_path);
    }

    /* Now free relations and their names */
    for (i = 0; i < db->nrels; i++) {
        rel_free(db->rels[i].rel);
        free(db->rels[i].name);
    }

    /* Free compiled rules */
    if (db->crules) {
        for (i = 0; i < (size_t)db->n_crules; i++)
            compiled_rule_free(db->crules[i]);
        free(db->crules);
    }

    intern_free(db->ir);
    free(db->dir);
    free(db);
}

/* ─── Schema ──────────────────────────────────────────────────────────── */

int dl_declare_relation(dl_db *db, const char *name, uint8_t arity)
{
    int idx;
    char *path;
    relation *rel;

    if (!db || !name) return -1;
    if (arity == 0 || arity > 8) return -1;
    if (db->nrels >= MAX_RELS) return -1;

    /* Check if already declared */
    idx = find_rel(db, name);
    if (idx >= 0) {
        /* Idempotent: same arity is fine */
        if (rel_arity(db->rels[idx].rel) == arity)
            return 0;
        return -1;  /* arity mismatch */
    }

    /* Try to load existing DAFSA, or create new */
    path = make_path(db, name, ".dafsa");
    if (!path) return -1;

    rel = rel_open(path, arity);
    free(path);
    if (!rel) return -1;

    /* Register */
    idx = (int)db->nrels++;
    db->rels[idx].name = strdup(name);
    db->rels[idx].rel  = rel;
    if (!db->rels[idx].name) {
        rel_free(rel);
        db->nrels--;
        return -1;
    }

    return 0;
}

/* ─── CSV parser ──────────────────────────────────────────────────────── */

/*
 * Simple robust CSV splitter for headerless ground facts.
 * Values: "quoted string" → intern → u32 sym_id
 *         bare integer    → parse as u32 (raw, not interned)
 * Handles: commas, newlines, quoted strings with embedded commas.
 * Does NOT handle: escaped quotes within strings, multiline fields.
 */

/* Parse a single CSV row into an array of strings.
 * Modifies `line` in place (NUL-terminates each field).
 * Returns number of fields, or -1 on error. */
static int csv_split(char *line, char **fields, int max_fields)
{
    int n = 0;
    char *p = line;

    while (*p && n < max_fields) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0' || *p == '\n' || *p == '\r') break;

        if (*p == '"') {
            /* Quoted field */
            p++;  /* skip opening quote */
            fields[n++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') {
                *p = '\0';  /* terminate */
                p++;
            }
            /* After closing quote, expect comma or end */
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ',') p++;
        } else {
            /* Unquoted field */
            fields[n++] = p;
            while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
            if (*p == ',') {
                *p = '\0';
                p++;
            } else {
                /* End of line */
                if (*p) *p = '\0';
            }
        }
    }

    return n;
}

/* Check if a string is a bare integer (optional minus not supported) */
static int is_integer(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
    }
    return 1;
}

/* ─── Fact loading ────────────────────────────────────────────────────── */

int dl_load_facts(dl_db *db, const char *rel_name, const char *csv_path)
{
    FILE *f;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    int idx, loaded = 0;
    uint8_t arity;

    if (!db || !rel_name || !csv_path) return -1;

    idx = find_rel(db, rel_name);
    if (idx < 0) return -1;  /* relation not declared */

    arity = rel_arity(db->rels[idx].rel);

    f = fopen(csv_path, "r");
    if (!f) return -1;

    while ((linelen = getline(&line, &linecap, f)) > 0) {
        char *fields[8];
        int nf;
        uint32_t cols[8];
        int i;

        /* Strip trailing newline */
        if (linelen > 0 && line[linelen - 1] == '\n')
            line[--linelen] = '\0';
        if (linelen > 0 && line[linelen - 1] == '\r')
            line[--linelen] = '\0';

        /* Skip empty lines */
        if (linelen == 0) continue;

        nf = csv_split(line, fields, (int)arity);
        if (nf != (int)arity) {
            /* Wrong number of fields — skip or error? In M0, skip. */
            continue;
        }

        /* Parse each field */
        for (i = 0; i < nf; i++) {
            if (is_integer(fields[i])) {
                /* Raw integer */
                unsigned long val = strtoul(fields[i], NULL, 10);
                if (val > 0xFFFFFFFFUL) {
                    /* Overflow — skip this row */
                    cols[0] = 0; /* force skip */
                    break;
                }
                cols[i] = (uint32_t)val;
            } else {
                /* String: intern it */
                uint32_t sym = intern_str(db->ir, fields[i]);
                if (sym == 0) {
                    /* Intern failed (OOM) — abort */
                    fclose(f);
                    free(line);
                    return -1;
                }
                cols[i] = sym;
            }
        }

        /* Add the fact */
        {
            int rc = rel_add(db->rels[idx].rel, cols);
            if (rc < 0) {
                fclose(f);
                free(line);
                return -1;
            }
            if (rc == 1) loaded++;
        }
    }

    free(line);
    fclose(f);

    /* Auto-save after load */
    {
        char *path = make_path(db, rel_name, ".dafsa");
        if (path) {
            rel_save(db->rels[idx].rel, path);
            free(path);
        }
    }

    return loaded;
}

/* ─── Query primitives ────────────────────────────────────────────────── */

int dl_lookup(dl_db *db, const char *rel_name,
              const uint32_t *cols, uint8_t arity)
{
    int idx;

    if (!db || !rel_name || !cols) return 0;

    idx = find_rel(db, rel_name);
    if (idx < 0) return 0;

    if (arity != rel_arity(db->rels[idx].rel)) return 0;

    return rel_exact(db->rels[idx].rel, cols);
}

long dl_prefix(dl_db *db, const char *rel_name,
               const uint32_t *leading, uint8_t k,
               dl_tuple_cb cb, void *user)
{
    int idx;

    if (!db || !rel_name || !cb) return -1;

    idx = find_rel(db, rel_name);
    if (idx < 0) return -1;

    return rel_prefix(db->rels[idx].rel, leading, k, cb, user);
}

/* ─── Rule loading & compilation (M1) ──────────────────────────────────── */

int dl_load_rules(dl_db *db, const char *dl_source)
{
    parser *p;
    rule **rules;
    int n_rules, n_compiled;
    compiled_rule **new_crules;

    if (!db || !dl_source) return -1;

    p = parse_create(dl_source);
    if (!p) return -1;

    rules = parse_rules(p, &n_rules);
    if (!rules) {
        parse_free(p);
        return -1;
    }

    if (compile_rules(db, rules, n_rules, &new_crules, &n_compiled) != 0) {
        int i;
        for (i = 0; i < n_rules; i++) rule_free(rules[i]);
        free(rules);
        parse_free(p);
        return -1;
    }

    /* Append compiled rules to db's list */
    {
        int new_total = db->n_crules + n_compiled;
        compiled_rule **merged = realloc(db->crules,
            (size_t)new_total * sizeof(compiled_rule *));
        if (!merged) {
            int i;
            for (i = 0; i < n_compiled; i++) compiled_rule_free(new_crules[i]);
            free(new_crules);
            for (i = 0; i < n_rules; i++) rule_free(rules[i]);
            free(rules);
            parse_free(p);
            return -1;
        }
        memcpy(merged + db->n_crules, new_crules,
               (size_t)n_compiled * sizeof(compiled_rule *));
        free(new_crules);
        db->crules = merged;
        db->n_crules = new_total;
    }

    /* Clean up parser AST */
    {
        int i;
        for (i = 0; i < n_rules; i++) rule_free(rules[i]);
        free(rules);
    }
    parse_free(p);

    return 0;
}

int dl_compile(dl_db *db)
{
    if (!db) return -1;
    if (db->n_crules == 0) return 0; /* nothing to compile */
    return vm_execute(db, db->crules, db->n_crules);
}

long dl_query(dl_db *db, const char *goal_rel, dl_tuple_cb cb, void *user)
{
    int ret;

    if (!db) return -1;

    /* Compile and run rules if any are loaded */
    if (db->n_crules > 0) {
        ret = vm_execute(db, db->crules, db->n_crules);
        if (ret != 0) return -1;
    }

    /* Stream the goal relation */
    if (goal_rel && cb) {
        int idx = find_rel(db, goal_rel);
        if (idx < 0) return -1;
        return rel_prefix(db->rels[idx].rel, NULL, 0, cb, user);
    }

    return 0;
}
