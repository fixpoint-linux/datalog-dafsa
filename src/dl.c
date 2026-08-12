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
#include "snapshot.h"
#include "regexwalk.h"
#include "tupleset.h"
#include "permindex.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

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

    /* M4: snapshot support */
    int              fixpoint_dirty;  /* 1 if rules loaded / facts changed
                                         since last compile/publish */
    uint32_t         snap_version;    /* current snapshot version, 0=none */
    view_cache_slot  vcache[DL_VIEW_CACHE_SZ];
    int            (*fault_hook)(dl_fpoint, void *);
    void            *fault_user;

    /* M6: permutation indices */
    perm_index_entry  perms[MAX_PERMS];
    int               n_perms;
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

    /* M4: set initial snapshot state */
    db->fixpoint_dirty = 0;
    db->snap_version   = 0;
    memset(db->vcache, 0, sizeof(db->vcache));
    db->fault_hook = NULL;
    db->fault_user = NULL;

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

    /* M4: detect existing snapshot and fixpoint state */
    db->snap_version = snapshot_read_current(db->dir);
    db->fixpoint_dirty = (db->n_crules > 0) ? 1 : 0;

    return db;
}

void dl_close(dl_db *db)
{
    size_t i;
    if (!db) return;

    /* M4: close all cached snapshot views */
    vcache_invalidate(db->vcache);

    /* M6: free permutation indices */
    permindex_free_all(db);

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

/* M6: check if a name matches the reserved __PI<hex>__ suffix pattern */
static int is_reserved_pi_name(const char *name)
{
    const char *p;
    int hex_digits = 0;

    /* Must contain __PI */
    p = strstr(name, "__PI");
    if (!p) return 0;

    p += 4; /* skip __PI */
    hex_digits = 0;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')) {
        hex_digits++;
        p++;
    }
    if (hex_digits == 0) return 0;
    if (strncmp(p, "__", 2) != 0) return 0;

    return 1;
}

int dl_declare_relation(dl_db *db, const char *name, uint8_t arity)
{
    int idx;
    char *path;
    relation *rel;

    if (!db || !name) return -1;
    if (arity == 0 || arity > 8) return -1;
    if (db->nrels >= MAX_RELS) return -1;

    /* M6: reject reserved permutation index names */
    if (is_reserved_pi_name(name)) {
        fprintf(stderr, "error: relation name '%s' uses reserved __PI<hex>__ suffix\n",
                name);
        return -1;
    }

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
    tuple_set ts;

    if (!db || !rel_name || !csv_path) return -1;

    idx = find_rel(db, rel_name);
    if (idx < 0) return -1;  /* relation not declared */

    arity = rel_arity(db->rels[idx].rel);

    f = fopen(csv_path, "r");
    if (!f) return -1;

    /* Collect new facts in a tuple_set */
    if (ts_init(&ts, arity) != 0) {
        fclose(f);
        return -1;
    }

    /* If the relation already had facts (e.g. loaded from disk on open),
     * union them into ts so we rebuild the combined set. */
    if (rel_prefix(db->rels[idx].rel, NULL, 0,
                   ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        fclose(f);
        return -1;
    }

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
                    ts_free(&ts);
                    fclose(f);
                    free(line);
                    return -1;
                }
                cols[i] = sym;
            }
        }

        /* Add to tuple_set (hash dedup) */
        {
            int rc = ts_add(&ts, cols);
            if (rc < 0) {
                ts_free(&ts);
                fclose(f);
                free(line);
                return -1;
            }
        }
    }

    free(line);
    fclose(f);

    /* Sort and bulk-build the DAFSA */
    ts_sort(&ts);

    if (rel_build_from_tupleset(db->rels[idx].rel, &ts) != 0) {
        ts_free(&ts);
        return -1;
    }

    loaded = (int)ts.count;
    ts_free(&ts);

    /* Auto-save after load */
    {
        char *path = make_path(db, rel_name, ".dafsa");
        if (path) {
            rel_save(db->rels[idx].rel, path);
            free(path);
        }
    }

    /* M4: facts changed, mark fixpoint dirty */
    db->fixpoint_dirty = 1;

    /* M6: mark permutation indices of this relation dirty */
    {
        int pi;
        for (pi = 0; pi < db->n_perms; pi++) {
            if (db->perms[pi].rel_id == idx)
                db->perms[pi].dirty = 1;
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

    /* M4: new rules loaded, mark fixpoint dirty */
    db->fixpoint_dirty = 1;

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

    if (vm_execute(db, db->crules, db->n_crules) != 0)
        return -1;

    /* M4: compilation successful, fixpoint is now clean */
    db->fixpoint_dirty = 0;
    return 0;
}

long dl_query(dl_db *db, const char *goal_rel, dl_tuple_cb cb, void *user)
{
    if (!db) return -1;

    /* M4 snapshot path: if we have a published snapshot, read from mmap */
    if (db->snap_version > 0 && goal_rel && cb) {
        return snapshot_query_scan(db->dir, db->snap_version,
                                   db->vcache,
                                   goal_rel, NULL, 0, cb, user);
    }

    /* Legacy M3 path: compile and run rules, then stream in-memory */
    if (db->n_crules > 0) {
        if (vm_execute(db, db->crules, db->n_crules) != 0)
            return -1;
    }

    if (goal_rel && cb) {
        int idx = find_rel(db, goal_rel);
        if (idx < 0) return -1;
        return rel_prefix(db->rels[idx].rel, NULL, 0, cb, user);
    }

    return 0;
}

/* ─── M4: bound query ──────────────────────────────────────────────────── */

long dl_query_bound(dl_db *db, const char *goal_rel,
                    const uint32_t *leading, uint8_t k,
                    dl_tuple_cb cb, void *user)
{
    if (!db) return -1;

    /* Snapshot path */
    if (db->snap_version > 0 && goal_rel && cb) {
        return snapshot_query_scan(db->dir, db->snap_version,
                                   db->vcache,
                                   goal_rel, leading, k, cb, user);
    }

    /* Legacy path */
    {
        int idx = find_rel(db, goal_rel);
        if (idx < 0) return -1;
        return rel_prefix(db->rels[idx].rel, leading, k, cb, user);
    }
}

/* ─── M4: snapshot publish ─────────────────────────────────────────────── */

/* fsync helpers (mirror dafsa_persist.c) */
static int fsync_dir_of_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir;
    int fd, ret = -1;

    if (!slash)
        slash = path;  /* shouldn't happen */
    if (slash == path) {
        /* path is "/foo" */
        fd = open("/", O_RDONLY | O_DIRECTORY);
        if (fd >= 0) { ret = fsync(fd); close(fd); }
        return ret;
    }
    dir = strndup(path, (size_t)(slash - path));
    if (!dir) return -1;
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { ret = fsync(fd); close(fd); }
    free(dir);
    return ret;
}

static int fsync_dir_path(const char *dirpath)
{
    int fd = open(dirpath, O_RDONLY | O_DIRECTORY);
    int ret = -1;
    if (fd >= 0) { ret = fsync(fd); close(fd); }
    return ret;
}

/* Write a string to a file with atomic rename (tmp+fsync+rename+dir-fsync) */
static int atomic_write_str(const char *path, const char *content)
{
    char tmp[8192];
    int fd;
    size_t len = strlen(content);
    size_t off = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
#pragma GCC diagnostic pop
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    while (off < len) {
        ssize_t w = write(fd, content + off, len - off);
        if (w < 0) { close(fd); remove(tmp); return -1; }
        off += (size_t)w;
    }

    if (fsync(fd) != 0) { close(fd); remove(tmp); return -1; }
    close(fd);

    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    if (fsync_dir_of_path(path) != 0) return -1;
    return 0;
}

/* Best-effort recursive removal of a file or directory (for crash cleanup).
 * No shell (no injection risk from caller-controlled paths, which may contain
 * quotes or spaces).  Uses opendir/readdir recursion — POSIX, no _XOPEN_SOURCE
 * requirement (unlike nftw).  Post-order: children removed before parents. */
static void rm_rf(const char *path)
{
    DIR *d;
    struct dirent *e;

    d = opendir(path);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            char child[4096];
            struct stat st;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
                rm_rf(child);               /* recurse first */
            else
                unlink(child);
        }
        closedir(d);
        rmdir(path);
        return;
    }
    /* Not a directory (or doesn't exist) — treat as a single file. */
    unlink(path);
}

int dl_publish_snapshot(dl_db *db)
{
    char snapshots_dir[4096];
    char tmp_dir[4096];
    char new_dir[4096];
    char current_path[4096];
    char buf[4096];
    uint32_t new_version;
    size_t i;
    int renamed = 0;  /* set once step 5 (rename) succeeds */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

    if (!db) return -1;

    /* 1. Run VM if rules exist and fixpoint is dirty */
    if (db->n_crules > 0 && db->fixpoint_dirty) {
        if (vm_execute(db, db->crules, db->n_crules) != 0)
            return -1;
        db->fixpoint_dirty = 0;
    }

    /* 2. Determine new version */
    new_version = db->snap_version + 1;

    /* 3. Ensure snapshots directory exists */
    snprintf(snapshots_dir, sizeof(snapshots_dir),
             "%s/snapshots", db->dir);
    mkdir(snapshots_dir, 0755);

    /* 4. Build into <db>/snapshots/<new_v>.tmp/ */
    snprintf(tmp_dir, sizeof(tmp_dir),
             "%s/snapshots/%u.tmp", db->dir, new_version);

    /* Clean up any stale .tmp from a previous crash */
    rm_rf(tmp_dir);

    if (mkdir(tmp_dir, 0755) != 0)
        return -1;

    /* 4a. Save interner */
    {
        char fwd[4096], rev[4096];
        snprintf(fwd, sizeof(fwd), "%s/symbols.dafsa", tmp_dir);
        snprintf(rev, sizeof(rev), "%s/symbols.array", tmp_dir);
        if (intern_save(db->ir, fwd, rev) != 0)
            goto fail;
    }

    /* 4b. Save each relation + write manifest */
    {
        char manifest_path[4096];
        FILE *mf;

        snprintf(manifest_path, sizeof(manifest_path),
                 "%s/manifest.txt", tmp_dir);
        mf = fopen(manifest_path, "w");
        if (!mf) goto fail;

        fprintf(mf, "# Datalog-DAFSA snapshot version %u\n", new_version);

        for (i = 0; i < db->nrels; i++) {
            char rel_path[4096];
            uint8_t arity = rel_arity(db->rels[i].rel);

            fprintf(mf, "%s:%d\n", db->rels[i].name, (int)arity);

            snprintf(rel_path, sizeof(rel_path), "%s/%s.dafsa",
                     tmp_dir, db->rels[i].name);
            if (rel_save(db->rels[i].rel, rel_path) != 0) {
                fclose(mf);
                goto fail;
            }

            /* FAULT HOOK: after each relation save */
            if (db->fault_hook &&
                db->fault_hook(DL_FPOINT_AFTER_REL_SAVE,
                               db->fault_user) != 0) {
                fclose(mf);
                goto fail;
            }
        }

        /* M6: save permutation indices */
        {
            int pi;
            for (pi = 0; pi < db->n_perms; pi++) {
                perm_index_entry *pe = &db->perms[pi];
                if (!pe->pidx_rel) continue;
                {
                    char pi_name[128];
                    char pi_path[4096];
                    uint8_t ar = pe->arity;

                    snprintf(pi_name, sizeof(pi_name), "%s__PI%x__",
                             db->rels[pe->rel_id].name, pi);
                    fprintf(mf, "%s:%d # perm index of %s\n",
                            pi_name, (int)ar,
                            db->rels[pe->rel_id].name);

                    snprintf(pi_path, sizeof(pi_path), "%s/%s.dafsa",
                             tmp_dir, pi_name);
                    if (rel_save(pe->pidx_rel, pi_path) != 0) {
                        fclose(mf);
                        goto fail;
                    }
                }
            }
        }

        if (fclose(mf) != 0) goto fail;
    }

    /* 4c. fsync the tmp dir */
    if (fsync_dir_path(tmp_dir) != 0) goto fail;

    /* 5. Atomic rename: .tmp → final dir */
    snprintf(new_dir, sizeof(new_dir),
             "%s/snapshots/%u", db->dir, new_version);

    /* Clean up a stale orphan from a prior crash between rename and the
     * CURRENT flip.  new_version > snap_version, so any existing dir here
     * is uncommitted garbage, not a valid snapshot. */
    rm_rf(new_dir);

    if (rename(tmp_dir, new_dir) != 0) goto fail;
    renamed = 1;
    if (fsync_dir_path(snapshots_dir) != 0) goto fail;

    /* FAULT HOOK: after rename (before CURRENT flip) */
    if (db->fault_hook &&
        db->fault_hook(DL_FPOINT_AFTER_RENAME, db->fault_user) != 0) {
        /* Clean up the already-renamed new_dir */
        rm_rf(new_dir);
        goto fail;
    }

    /* 6. Atomic CURRENT flip */
    snprintf(current_path, sizeof(current_path),
             "%s/snapshots/CURRENT", db->dir);
    snprintf(buf, sizeof(buf), "%u\n", new_version);
    if (atomic_write_str(current_path, buf) != 0)
        goto fail;

    /* 7. Invalidate cache + update snap_version */
    vcache_invalidate(db->vcache);
    db->snap_version = new_version;

#pragma GCC diagnostic pop
    return 0;

fail:
    /* Best-effort cleanup of .tmp dir */
    rm_rf(tmp_dir);
    /* If the rename already happened but a later step (e.g. the CURRENT
     * flip) failed, tmp_dir no longer exists — clean up new_dir instead. */
    if (renamed) rm_rf(new_dir);
#pragma GCC diagnostic pop
    return -1;
}

/* ─── M6: Permutation index API ──────────────────────────────────────── */

int dl_db_declare_perm(dl_db *db, int rel_id, uint8_t arity,
                       const uint8_t perm[8])
{
    int i;

    if (!db || rel_id < 0 || arity == 0 || arity > 8) return -1;
    if (db->n_perms >= MAX_PERMS) return -1;

    /* Check for duplicate: same rel_id + same perm */
    for (i = 0; i < db->n_perms; i++) {
        if (db->perms[i].rel_id == rel_id &&
            db->perms[i].arity == arity &&
            memcmp(db->perms[i].perm, perm, arity) == 0)
            return i;  /* return existing perm_id */
    }

    i = db->n_perms++;
    db->perms[i].rel_id   = rel_id;
    db->perms[i].arity    = arity;
    memcpy(db->perms[i].perm, perm, 8);
    db->perms[i].pidx_rel = NULL;
    db->perms[i].dirty    = 1;  /* build on next exec */
    return i;
}

const uint8_t *dl_db_get_perm(dl_db *db, int rel_id, int perm_id)
{
    if (!db || perm_id < 0 || perm_id >= db->n_perms) return NULL;
    if (db->perms[perm_id].rel_id != rel_id) return NULL;
    return db->perms[perm_id].perm;
}

struct relation *dl_db_get_perm_rel(dl_db *db, int rel_id, int perm_id)
{
    if (!db || perm_id < 0 || perm_id >= db->n_perms) return NULL;
    if (db->perms[perm_id].rel_id != rel_id) return NULL;
    return db->perms[perm_id].pidx_rel;
}

/* ─── Fault hook registration ──────────────────────────────────────────── */

void dl_set_fault_hook(dl_db *db,
                       int (*hook)(dl_fpoint fp, void *user),
                       void *user)
{
    if (!db) return;
    db->fault_hook = hook;
    db->fault_user = user;
}

/* ─── Regex pattern query ──────────────────────────────────────────────── */

long dl_pattern(dl_db *db, const char *rel_name, const struct regex_dfa *dfa,
                dl_tuple_cb cb, void *user)
{
    int idx;

    if (!db || !rel_name || !dfa || !cb) return -1;

    /* Snapshot path */
    if (db->snap_version > 0) {
        char sdir[8192];
        uint8_t arity = 0;
        dafsa_view *v;

        snprintf(sdir, sizeof(sdir), "%s/snapshots/%u",
                 db->dir, db->snap_version);

        if (!manifest_find_rel(sdir, rel_name, &arity))
            return -1;

        v = view_open_cached(db->vcache, rel_name, sdir);
        if (!v) return -1;

        return view_pattern(v, arity, dfa, cb, user);
    }

    /* In-memory path */
    idx = find_rel(db, rel_name);
    if (idx < 0) return -1;

    return rel_pattern(db->rels[idx].rel, dfa,
                       (rel_enum_cb)cb, user);
}
