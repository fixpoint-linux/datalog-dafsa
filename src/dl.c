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
#include "dl_internal.h"
#include "intern.h"
#include "relation.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"
#include "snapshot.h"
#include "regexwalk.h"
#include "tupleset.h"
#include "permindex.h"
#include "magic.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

/* dl_db layout lives in dl_internal.h (shared with internal consumers) */

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
    return dl_open2(dir, NULL);
}

dl_db *dl_open2(const char *dir, int *err_out)
{
    dl_db *db;
    char *rels_path;
    FILE *rf;
    char lock_path[4096];
    int lfd = -1;

    if (!dir) {
        if (err_out) *err_out = -1;
        return NULL;
    }

    /* Create directory if needed */
    mkdir(dir, 0755);

    /* M7: acquire fcntl single-writer lock */
    snprintf(lock_path, sizeof(lock_path), "%s/LOCK", dir);
    lfd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (lfd < 0) {
        if (err_out) *err_out = -1;
        return NULL;
    }

    {
        struct flock fl;
        fl.l_type   = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start  = 0;
        fl.l_len    = 0;  /* whole file */
        fl.l_pid    = 0;

        if (fcntl(lfd, F_SETLK, &fl) != 0) {
            /* Contention: another writer holds the lock */
            close(lfd);
            if (err_out) *err_out = DL_E_LOCKED;
            return NULL;
        }
    }

    db = calloc(1, sizeof(*db));
    if (!db) { close(lfd); if (err_out) *err_out = -1; return NULL; }
    db->lock_fd = lfd;

    db->dir = strdup(dir);
    if (!db->dir) { close(lfd); free(db); if (err_out) *err_out = -1; return NULL; }

    /* M4: set initial snapshot state */
    db->fixpoint_dirty = 0;
    db->snap_version   = 0;
    memset(db->vcache, 0, sizeof(db->vcache));
    db->fault_hook = NULL;
    db->fault_user = NULL;

    /* M8: retained rule AST (empty until dl_load_rules) */
    db->ast_rules = NULL;
    db->n_ast_rules = 0;

    /* Load or create interner */
    {
        char *fwd_path = make_path(db, "symbols", ".dafsa");
        char *rev_path = make_path(db, "symbols", ".array");
        if (!fwd_path || !rev_path) {
            free(fwd_path); free(rev_path);
            close(lfd); free(db->dir); free(db);
            if (err_out) *err_out = -1;
            return NULL;
        }
        db->ir = intern_load(fwd_path, rev_path);
        free(fwd_path);
        free(rev_path);
        if (!db->ir) {
            close(lfd); free(db->dir); free(db);
            if (err_out) *err_out = -1;
            return NULL;
        }
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

    if (err_out) *err_out = 0;
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

    /* M7: compact each relation (save DAFSA + truncate WAL) then save */
    for (i = 0; i < db->nrels; i++) {
        char *path = make_path(db, db->rels[i].name, ".dafsa");
        if (path) {
            rel_compact(db->rels[i].rel, path);
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

    /* M8: free retained rule AST */
    if (db->ast_rules) {
        for (i = 0; i < (size_t)db->n_ast_rules; i++)
            rule_free(db->ast_rules[i]);
        free(db->ast_rules);
    }

    intern_free(db->ir);
    free(db->dir);

    /* M7: release fcntl lock LAST (lock released when fd closes) */
    if (db->lock_fd >= 0) close(db->lock_fd);

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

    /* M8: scoped-eval clones have dir==NULL.  A declaration on such a db
     * means a missed head relation slipped through the transform — fail
     * LOUDLY (return -1) instead of dereferencing NULL in make_path. */
    if (!db->dir) {
        fprintf(stderr, "error: dl_declare_relation('%s') on a filesystem-less "
                "eval clone — transform must pre-declare all head relations\n",
                name);
        return -1;
    }

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

    /* Try to load existing DAFSA with WAL, or create new */
    {
        char *wal_path;
        path = make_path(db, name, ".dafsa");
        wal_path = make_path(db, name, ".wal");
        if (!path || !wal_path) {
            free(path); free(wal_path);
            return -1;
        }

        rel = rel_open_writable(path, wal_path, arity);
        free(path);
        free(wal_path);
        if (!rel) return -1;
    }

    /* Register */
    idx = (int)db->nrels++;
    db->rels[idx].name = strdup(name);
    db->rels[idx].rel  = rel;
    if (!db->rels[idx].name) {
        rel_free(rel);
        db->nrels--;
        return -1;
    }

    /* M7: persist relation metadata immediately so crash-recovery works.
     * Without this, a process that declares a relation and adds facts
     * before crashing would lose the relation declaration on reopen. */
    {
        char *rels_path = make_path(db, "rels", ".txt");
        FILE *rf;
        if (rels_path && (rf = fopen(rels_path, "w"))) {
            size_t j;
            for (j = 0; j < db->nrels; j++) {
                fprintf(rf, "%s:%d\n", db->rels[j].name,
                        (int)rel_arity(db->rels[j].rel));
            }
            fclose(rf);
        }
        free(rels_path);
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

    /* M7: save interner BEFORE relation save (invariant: interner durable
     * before relation, so relation DAFSA never references a sym_id not on disk) */
    {
        char *fwd_path = make_path(db, "symbols", ".dafsa");
        char *rev_path = make_path(db, "symbols", ".array");
        if (fwd_path && rev_path) {
            if (intern_save(db->ir, fwd_path, rev_path) != 0) {
                free(fwd_path); free(rev_path);
                return -1;
            }
        }
        free(fwd_path);
        free(rev_path);
    }

    /* Auto-save relation DAFSA after load */
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

/* ─── Incremental fact API (M7) ────────────────────────────────────────── */
/*
 * dl_add_fact / dl_delete_fact: incremental fact insert/delete with WAL.
 *
 * The caller MUST intern string columns via dl_intern_str() BEFORE calling
 * dl_add_fact — the cols[] array must contain already-interned sym_ids.
 * dl_add_fact will NOT intern strings itself.
 *
 * Ordering invariant (crash safety):
 *   1. If new syms were added (interner is dirty), save interner atomically
 *      BEFORE the WAL record.  This guarantees WAL replay can decode sym_ids.
 *   2. Check rel_exact — skip WAL if fact already present/absent (avoids
 *      WAL bloat; the fact is already durable).
 *   3. WAL-append + fsync (durable before in-memory commit).
 *   4. In-memory add/delete.
 *
 * dl_delete_fact does NOT save the interner (deletes don't create new syms).
 */

static int encode_fact_key(unsigned char *key, size_t *key_len,
                           const uint32_t *cols, uint8_t arity)
{
    uint8_t i;
    if (arity == 0 || arity > 8) return -1;
    for (i = 0; i < arity; i++) {
        uint32_t v = cols[i];
        key[4*i]     = (unsigned char)((v >> 24) & 0xFF);
        key[4*i + 1] = (unsigned char)((v >> 16) & 0xFF);
        key[4*i + 2] = (unsigned char)((v >> 8)  & 0xFF);
        key[4*i + 3] = (unsigned char)(v & 0xFF);
    }
    key[4 * arity] = 0x00;
    *key_len = (size_t)(4 * arity + 1);
    return 0;
}

int dl_add_fact(dl_db *db, const char *rel_name,
                const uint32_t *cols, uint8_t arity)
{
    int idx;
    unsigned char key[33];  /* 4*8+1 */
    size_t key_len;
    int rc;

    if (!db || !rel_name || !cols) return -1;

    idx = find_rel(db, rel_name);
    if (idx < 0) return -1;

    if (arity != rel_arity(db->rels[idx].rel)) return -1;

    if (encode_fact_key(key, &key_len, cols, arity) != 0) return -1;

    /* 1. Interner-before-WAL invariant (M7 BLOCKER fix):
     * If new syms were added (caller interned strings before this call),
     * save the interner BEFORE WAL-append so crash recovery can decode
     * the sym_ids in the WAL. */
    if (intern_is_dirty(db->ir)) {
        char *fwd_path = make_path(db, "symbols", ".dafsa");
        char *rev_path = make_path(db, "symbols", ".array");
        if (!fwd_path || !rev_path) {
            free(fwd_path); free(rev_path);
            return -1;
        }
        if (intern_save(db->ir, fwd_path, rev_path) != 0) {
            free(fwd_path); free(rev_path);
            return -1;
        }
        free(fwd_path);
        free(rev_path);
    }

    /* 2. Duplicate check: if already present, skip WAL (SHOULD-FIX).
     * A duplicate fact is already durable, so no WAL record needed. */
    if (rel_exact(db->rels[idx].rel, cols))
        return 0;

    /* 3. WAL-append ADD + sync (durable before in-memory commit) */
    if (rel_wal_append_add(db->rels[idx].rel, key, (uint32_t)key_len) != 0)
        return -1;

    /* 4. In-memory add */
    rc = rel_add(db->rels[idx].rel, cols);
    if (rc < 0) return -1;

    /* 5. Check compaction threshold: WAL > 25% of DAFSA estimate? */
    {
        uint64_t wal_sz = rel_wal_size(db->rels[idx].rel);
        uint64_t dafsa_sz = rel_dafsa_size(db->rels[idx].rel);
        if (dafsa_sz > 0 && wal_sz > dafsa_sz / 4) {
            char *path = make_path(db, rel_name, ".dafsa");
            if (path) {
                rel_compact(db->rels[idx].rel, path);
                free(path);
            }
        }
    }

    /* M4: facts changed, mark fixpoint dirty */
    db->fixpoint_dirty = 1;

    return 1;  /* added */
}

int dl_delete_fact(dl_db *db, const char *rel_name,
                   const uint32_t *cols, uint8_t arity)
{
    int idx;
    unsigned char key[33];
    size_t key_len;
    int rc;

    if (!db || !rel_name || !cols) return -1;

    idx = find_rel(db, rel_name);
    if (idx < 0) return -1;

    if (arity != rel_arity(db->rels[idx].rel)) return -1;

    if (encode_fact_key(key, &key_len, cols, arity) != 0) return -1;

    /* 1. Absent check: if not present, skip WAL (SHOULD-FIX).
     * No interner save needed — deletes don't create new syms. */
    if (!rel_exact(db->rels[idx].rel, cols))
        return 0;

    /* 2. WAL-append DEL + sync (durable before in-memory commit) */
    if (rel_wal_append_del(db->rels[idx].rel, key, (uint32_t)key_len) != 0)
        return -1;

    /* 3. In-memory delete */
    rc = rel_delete(db->rels[idx].rel, cols);
    if (rc < 0) return -1;

    /* M4: facts changed, mark fixpoint dirty */
    db->fixpoint_dirty = 1;

    return 1;  /* deleted */
}

/* ─── Interner access (M7) ─────────────────────────────────────────────── */

uint32_t dl_intern_str(dl_db *db, const char *str)
{
    if (!db || !db->ir) return 0;
    return intern_str(db->ir, str);
}

const char *dl_intern_str_of(dl_db *db, uint32_t sym_id)
{
    if (!db || !db->ir) return NULL;
    return intern_str_of(db->ir, sym_id);
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

/* ─── M8: AST deep-copy — retain rules for the magic-sets transform ───── */
/*
 * dl_load_rules frees the parsed AST after compiling (rule_free loop).  The
 * magic-sets transform needs that AST at query time, so we deep-copy it into
 * db->ast_rules BEFORE the free loop.  A shallow copy is a use-after-free /
 * silent-wrong-answer hazard: every owned string (pred, args[i]->text,
 * pattern, agg_op->text) must be duplicated, mirroring parser.c's
 * allocation discipline exactly (strdup for text, calloc'd atom/rule).
 */

static token *ast_tok_clone(const token *t)
{
    token *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->kind = t->kind;
    n->ival = t->ival;
    if (t->text) {
        n->text = strdup(t->text);
        if (!n->text) { free(n); return NULL; }
    }
    return n;
}

static atom *ast_atom_clone(const atom *a)
{
    atom *n = calloc(1, sizeof(*n));
    int i;
    if (!n) return NULL;
    n->pred = a->pred ? strdup(a->pred) : NULL;
    n->negated = a->negated;
    n->aggregate = a->aggregate;
    if (a->pattern) {
        n->pattern = strdup(a->pattern);
        if (!n->pattern) goto fail;
    }
    if (a->agg_op) {
        n->agg_op = ast_tok_clone(a->agg_op);
        if (!n->agg_op) goto fail;
    }
    if (a->nargs > 0) {
        n->args = calloc((size_t)a->nargs, sizeof(token *));
        if (!n->args) goto fail;
        n->nargs = a->nargs;
        for (i = 0; i < a->nargs; i++) {
            n->args[i] = ast_tok_clone(a->args[i]);
            if (!n->args[i]) goto fail;
        }
    }
    return n;
fail:
    {
        /* Mirror parser.c atom_free (static) for partial cleanup */
        int j;
        free(n->pred);
        free(n->pattern);
        if (n->args) {
            for (j = 0; j < n->nargs; j++) {
                if (n->args[j]) { free(n->args[j]->text); free(n->args[j]); }
            }
            free(n->args);
        }
        if (n->agg_op) { free(n->agg_op->text); free(n->agg_op); }
        free(n);
    }
    return NULL;
}

static rule *ast_rule_clone(const rule *r)
{
    rule *n = calloc(1, sizeof(*n));
    int i;
    if (!n) return NULL;
    n->has_negation = r->has_negation;
    n->has_aggregate = r->has_aggregate;
    n->head = ast_atom_clone(r->head);
    if (!n->head) { free(n); return NULL; }
    if (r->nbody > 0) {
        n->body = calloc((size_t)r->nbody, sizeof(atom *));
        if (!n->body) { rule_free(n); return NULL; }
        n->nbody = r->nbody;
        for (i = 0; i < r->nbody; i++) {
            n->body[i] = ast_atom_clone(r->body[i]);
            if (!n->body[i]) { rule_free(n); return NULL; }
        }
    }
    return n;
}

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

    /* M8: retain a DEEP copy of the rule AST for the magic-sets transform.
     * Done before the parser AST is freed and before db->crules is mutated,
     * so a failure here leaves db untouched. */
    {
        int i;
        rule **cloned = calloc((size_t)n_rules, sizeof(rule *));
        if (!cloned) {
            for (i = 0; i < n_compiled; i++) compiled_rule_free(new_crules[i]);
            free(new_crules);
            for (i = 0; i < n_rules; i++) rule_free(rules[i]);
            free(rules);
            parse_free(p);
            return -1;
        }
        for (i = 0; i < n_rules; i++) {
            cloned[i] = ast_rule_clone(rules[i]);
            if (!cloned[i]) break;
        }
        if (i < n_rules) {
            int j;
            for (j = 0; j < i; j++) rule_free(cloned[j]);
            free(cloned);
            for (j = 0; j < n_compiled; j++) compiled_rule_free(new_crules[j]);
            free(new_crules);
            for (j = 0; j < n_rules; j++) rule_free(rules[j]);
            free(rules);
            parse_free(p);
            return -1;
        }
        {
            rule **na = realloc(db->ast_rules,
                (size_t)(db->n_ast_rules + n_rules) * sizeof(rule *));
            if (!na) {
                int j;
                for (j = 0; j < n_rules; j++) rule_free(cloned[j]);
                free(cloned);
                for (j = 0; j < n_compiled; j++) compiled_rule_free(new_crules[j]);
                free(new_crules);
                for (j = 0; j < n_rules; j++) rule_free(rules[j]);
                free(rules);
                parse_free(p);
                return -1;
            }
            memcpy(na + db->n_ast_rules, cloned,
                   (size_t)n_rules * sizeof(rule *));
            free(cloned);
            db->ast_rules = na;
            db->n_ast_rules += n_rules;
        }
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

/* ─── M8: magic-sets bound query (scoped re-eval, clone-and-scope) ─────── */

/*
 * Clone-and-scope (plan §6 Option C): build an eval-only dl_db that
 *   - shallow-aliases every EDB relation pointer from src->rels (read-only:
 *     the transform never emits an EDB head, so the VM only reads them),
 *   - gets fresh in-memory rel_create() for the magic/adorned IDB relations
 *     (declared by dl_query_magic before compile_rules),
 *   - aliases the interner (read-mostly; constants are already interned so
 *     no new syms are added),
 *   - has dir==NULL, n_perms==0, no vcache, no lock, snap_version==0 so the
 *     VM can never touch the filesystem.
 *
 * n_aliased (out) = number of borrowed rels (== src->nrels); the fresh rels
 * are appended after them.  eval_db_free needs that boundary to avoid freeing
 * the borrowed EDB relations/names.
 */
static int eval_db_clone(dl_db *src, dl_db *out)
{
    size_t i;
    memset(out, 0, sizeof(*out));
    out->dir = NULL;
    out->ir = src->ir;
    out->lock_fd = -1;
    for (i = 0; i < src->nrels; i++) {
        out->rels[i].name = src->rels[i].name;  /* borrowed — not owned */
        out->rels[i].rel  = src->rels[i].rel;   /* borrowed — not owned */
    }
    out->nrels = src->nrels;
    out->n_perms = 0;
    return 0;
}

static void eval_db_free(dl_db *edb, size_t n_aliased)
{
    size_t i;
    if (!edb) return;

    /* Free compiled magic rules (if any were attached). */
    if (edb->crules) {
        for (i = 0; i < (size_t)edb->n_crules; i++)
            compiled_rule_free(edb->crules[i]);
        free(edb->crules);
    }

    /* Free fresh perm indices (never built from a file). */
    permindex_free_all(edb);

    /* Free only the fresh relations + their names (indices >= n_aliased).
     * The first n_aliased rels/names are borrowed from src — do NOT free. */
    for (i = n_aliased; i < edb->nrels; i++) {
        rel_free(edb->rels[i].rel);
        free(edb->rels[i].name);
    }

    /* ir is borrowed from src; dir is NULL; no intern_save, no rel_compact.
     * The dl_db struct itself is owned by the caller (stack-allocated). */
}

/* Pre-declare one relation into the clone using rel_create (in-memory only,
 * no DAFSA/WAL files).  Returns the index, or -1 on error. */
static int eval_db_declare_inmem(dl_db *edb, const char *name, uint8_t arity)
{
    relation *rel;
    if (edb->nrels >= MAX_RELS) return -1;
    if (find_rel(edb, name) >= 0) return -1;  /* name collision */
    rel = rel_create(arity);
    if (!rel) return -1;
    edb->rels[edb->nrels].name = strdup(name);
    if (!edb->rels[edb->nrels].name) { rel_free(rel); return -1; }
    edb->rels[edb->nrels].rel = rel;
    edb->nrels++;
    return 0;
}

/* Is goal_rel a rule head (i.e. IDB) in the retained AST? */
static int ast_has_head(dl_db *db, const char *pred)
{
    int i;
    for (i = 0; i < db->n_ast_rules; i++) {
        const rule *r = db->ast_rules[i];
        if (r && r->head && r->head->pred &&
            strcmp(r->head->pred, pred) == 0)
            return 1;
    }
    return 0;
}

/* ─── M8: per-position filter adapter (full-scan + filter) ─────────────── */

/* Adapter turning a dl_tuple_cb into a rel_enum_cb that applies a per-position
 * adornment filter: for each position i where adorn[i]=='b', require
 * cols[i]==vals[b] (b = running bound-position counter).  Used to post-filter
 * the fully materialized adorned-goal (or EDB) relation after the scoped
 * fixpoint, since rel_prefix only supports LEADING-prefix binding and cannot
 * express fb/bfb.  Kept inline in dl.c (not relation.h) — small, magic-only. */
typedef struct {
    dl_tuple_cb user_cb;
    void       *user;
    uint8_t     arity;
    char        adorn[9];
    uint32_t    vals[8];
    long        matched;   /* count of tuples that passed the filter */
} magic_filter_ctx;

static int magic_filter_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    magic_filter_ctx *ctx = (magic_filter_ctx *)user;
    uint8_t i, b = 0;
    (void)arity;
    for (i = 0; i < ctx->arity; i++) {
        if (ctx->adorn[i] == 'b') {
            if (cols[i] != ctx->vals[b]) return 0;
            b++;
        }
    }
    ctx->matched++;
    return ctx->user_cb(cols, ctx->arity, ctx->user);
}

long dl_query_magic(dl_db *db, const char *goal_rel,
                    const uint32_t *leading, uint8_t k,
                    dl_tuple_cb cb, void *user)
{
    int goal_idx;
    uint8_t goal_arity;
    char adorn[9];
    uint8_t i;

    if (!db || !goal_rel || !cb) return -1;

    /* Resolve goal + validate k (mirrors the old leading/k entry checks). */
    goal_idx = find_rel(db, goal_rel);
    if (goal_idx < 0) return -1;
    goal_arity = rel_arity(db->rels[goal_idx].rel);
    if (k > goal_arity) return -1;

    /* k==0 → route to dl_query (full materialization). */
    if (k == 0)
        return dl_query(db, goal_rel, cb, user);

    /* Synthesize the leading-prefix adornment and route to the new API. */
    for (i = 0; i < k; i++) adorn[i] = 'b';
    for (i = k; i < goal_arity; i++) adorn[i] = 'f';
    adorn[goal_arity] = '\0';

    return dl_query_magic_adorn(db, goal_rel, adorn, leading, k, cb, user);
}

long dl_query_magic_adorn(dl_db *db, const char *goal_rel,
                          const char *adorn, const uint32_t *vals, uint8_t nvals,
                          dl_tuple_cb cb, void *user)
{
    int goal_idx;
    uint8_t goal_arity;
    size_t alen;
    size_t xi;
    int nb = 0;
    magic_program prog;
    char reject[256];
    dl_db edb;
    compiled_rule **magic_crules = NULL;
    int n_magic = 0;
    size_t n_aliased;
    int d;
    long result = -1;

    if (!db || !goal_rel || !adorn || !cb) return -1;

    /* 1. Validate + resolve goal. */
    goal_idx = find_rel(db, goal_rel);
    if (goal_idx < 0) return -1;
    goal_arity = rel_arity(db->rels[goal_idx].rel);

    /* 2. Validate the adornment: length == arity, chars in {b,f},
     *    nvals == count_b(adorn). */
    alen = strlen(adorn);
    if (alen != goal_arity) {
        fprintf(stderr, "dl_query_magic: adornment length %zu != goal arity %u\n",
                alen, goal_arity);
        return -1;
    }
    for (xi = 0; xi < alen; xi++) {
        if (adorn[xi] != 'b' && adorn[xi] != 'f') {
            fprintf(stderr, "dl_query_magic: bad adornment char '%c'\n",
                    adorn[xi]);
            return -1;
        }
        if (adorn[xi] == 'b') nb++;
    }
    if (nb != (int)nvals) {
        fprintf(stderr, "dl_query_magic: nvals=%u != count_b(adorn)=%d\n",
                nvals, nb);
        return -1;
    }

    /* All-free adorn → route to dl_query (full materialization). */
    if (nvals == 0)
        return dl_query(db, goal_rel, cb, user);

    /* EDB goal (not a rule head) → direct full-scan + per-position filter. */
    if (!ast_has_head(db, goal_rel)) {
        magic_filter_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.user_cb = cb;
        ctx.user = user;
        ctx.arity = goal_arity;
        memcpy(ctx.adorn, adorn, alen);
        ctx.adorn[alen] = '\0';
        memcpy(ctx.vals, vals, (size_t)nvals * sizeof(uint32_t));
        if (rel_prefix(db->rels[goal_idx].rel, NULL, 0,
                       magic_filter_cb, &ctx) < 0)
            return -1;
        return ctx.matched;
    }

    if (db->n_ast_rules <= 0) return -1;  /* no retained AST (shouldn't happen) */

    /* 3. Clone (Option C). */
    eval_db_clone(db, &edb);
    n_aliased = edb.nrels;
    memset(&prog, 0, sizeof(prog));

    /* 4. Transform. */
    if (magic_transform_adorn((const rule *const *)db->ast_rules,
                              db->n_ast_rules, goal_rel, goal_arity,
                              adorn, vals, nvals, db->ir, &prog,
                              reject, sizeof(reject)) != 0) {
        fprintf(stderr, "dl_query_magic: rejected: %s\n", reject);
        goto out_free_edb;
    }

    /* 5. Pre-declare ALL head relations (adorned + magic) in-memory, so
     * compile_rules' dl_declare_relation branch is never reached. */
    for (d = 0; d < prog.n_decls; d++) {
        if (eval_db_declare_inmem(&edb, prog.decls[d].name,
                                  prog.decls[d].arity) != 0) {
            fprintf(stderr, "dl_query_magic: cannot pre-declare '%s'\n",
                    prog.decls[d].name);
            magic_program_free(&prog);
            goto out_free_edb;
        }
    }

    /* 6. Seed the magic goal relation with the bound values (nvals columns,
     *    packed in left-to-right bound-position order). */
    {
        char magic_goal[64];
        int m_idx;
        if (snprintf(magic_goal, sizeof(magic_goal), "magic_%s",
                     prog.adorned_goal) >= (int)sizeof(magic_goal)) {
            fprintf(stderr, "dl_query_magic: magic goal name too long\n");
            magic_program_free(&prog);
            goto out_free_edb;
        }
        m_idx = find_rel(&edb, magic_goal);
        if (m_idx < 0 || rel_arity(edb.rels[m_idx].rel) != nvals) {
            fprintf(stderr, "dl_query_magic: internal: magic goal '%s' "
                    "missing/arity-mismatch\n", magic_goal);
            magic_program_free(&prog);
            goto out_free_edb;
        }
        rel_add(edb.rels[m_idx].rel, vals);
    }

    /* 7. Compile the adorned + magic program against the clone. */
    if (compile_rules(&edb, prog.rules, prog.n_rules,
                      &magic_crules, &n_magic) != 0) {
        fprintf(stderr, "dl_query_magic: compile of adorned program failed\n");
        magic_program_free(&prog);
        goto out_free_edb;
    }

    /* Filesystem-trap backstop: compile_rules must NOT have declared any
     * relation (dir==NULL would have made dl_declare_relation fail, so if it
     * reached here, nrels is still exactly src->nrels + n_decls). */
    if (edb.dir != NULL ||
        edb.nrels != n_aliased + (size_t)prog.n_decls) {
        fprintf(stderr, "dl_query_magic: internal error: compile_rules grew "
                "the eval clone's relation table (missed head relation)\n");
        magic_program_free(&prog);
        goto out_free_crules;
    }

    /* 8. Scoped fixpoint. */
    if (vm_execute(&edb, magic_crules, n_magic) != 0) {
        fprintf(stderr, "dl_query_magic: scoped fixpoint failed\n");
        magic_program_free(&prog);
        goto out_free_crules;
    }

    /* 9. Stream: full-scan + per-position filter on the adorned goal. */
    {
        int a_idx = find_rel(&edb, prog.adorned_goal);
        magic_filter_ctx ctx;
        if (a_idx < 0) {
            fprintf(stderr, "dl_query_magic: internal: adorned goal '%s' "
                    "missing\n", prog.adorned_goal);
            magic_program_free(&prog);
            goto out_free_crules;
        }
        memset(&ctx, 0, sizeof(ctx));
        ctx.user_cb = cb;
        ctx.user = user;
        ctx.arity = goal_arity;
        memcpy(ctx.adorn, adorn, alen);
        ctx.adorn[alen] = '\0';
        memcpy(ctx.vals, vals, (size_t)nvals * sizeof(uint32_t));
        if (rel_prefix(edb.rels[a_idx].rel, NULL, 0,
                       magic_filter_cb, &ctx) < 0)
            result = -1;
        else
            result = ctx.matched;
    }

    magic_program_free(&prog);
out_free_crules:
    {
        int i;
        for (i = 0; i < n_magic; i++) compiled_rule_free(magic_crules[i]);
        free(magic_crules);
    }
out_free_edb:
    eval_db_free(&edb, n_aliased);
    return result;
}

/* ─── M4: snapshot publish ─────────────────────────────────────────────── */

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
