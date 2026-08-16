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
#include "vrelation.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"
#include "snapshot.h"
#include "regexwalk.h"
#include "tupleset.h"
#include "permindex.h"
#include "magic.h"
#include "topdown.h"
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

/* Magic-sets skip-materialize hook (defined in vm.c): let the scoped fixpoint
 * skip the post-fixpoint DAFSA bulk-build and export the adorned-goal idb
 * directly, since the eval clone is torn down right after streaming. */
extern int vm_nomaterialize;
extern int vm_export_relid;
extern tuple_set *vm_export_ts;

/* Forward decls (defined in the Schema section below). */
static int  dl_declare_relation_kind(dl_db *db, const char *name,
                                     uint8_t arity, int is_idb);
static void write_rels_txt(dl_db *db);

/* IVM Slice 5: forward decls for the batched-delta capture in dl_load_facts
 * (defined below, in the Incremental fact API section). */
static int  ivm_rel_is_head(dl_db *db, int rel_id);
static int  ivm_capture_delta(dl_db *db, int rel_id,
                              const uint32_t *cols, uint8_t arity);

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

/* Build a VARIADIC VARIANT file path: db->dir/name.<a><suffix>.
 * Predicate names cannot contain '.' (parser.c is_pred_char), so these
 * paths never collide with a user relation's own files (dl_declare also
 * guards the pathological C-API case of a fixed name "x.<d>"). */
static char *make_vpath(dl_db *db, const char *name, uint8_t a,
                        const char *suffix)
{
    size_t dlen = strlen(db->dir);
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    char *path = malloc(dlen + 1 + nlen + 2 + slen + 1);
    if (!path) return NULL;
    memcpy(path, db->dir, dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, name, nlen);
    path[dlen + 1 + nlen] = '.';
    path[dlen + 2 + nlen] = (char)('0' + (a % 10));
    memcpy(path + dlen + 3 + nlen, suffix, slen + 1);
    return path;
}

/* ─── v2 variable arity: rel_entry dispatch helpers ────────────────────── */

int db_entry_is_variadic(const rel_entry *e)
{
    return (e && e->kind == RELK_VARIADIC) ? 1 : 0;
}

int db_has_variadic(const dl_db *db)
{
    size_t i;
    if (!db) return 0;
    for (i = 0; i < db->nrels; i++)
        if (db->rels[i].kind == RELK_VARIADIC)
            return 1;
    return 0;
}

/* v2-lists: 1 if ANY compiled rule emits a list builtin opcode.  List
 * VALUES as pure data (a list-literal constant -> OP_EQ_CONST) do NOT count
 * — only the construction/decomposition builtins do. */
int db_has_list_builtin(const dl_db *db)
{
    int i, k;
    if (!db) return 0;
    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        for (k = 0; k < cr->n_instrs; k++) {
            uint8_t op = cr->instrs[k].op;
            if (op == OP_LIST_CONS || op == OP_LIST_CAR ||
                op == OP_LIST_CDR || op == OP_LIST_APPEND ||
                op == OP_LIST_MEMBER)
                return 1;
        }
    }
    return 0;
}

relation *db_entry_variant_ro(const rel_entry *e, uint8_t arity)
{
    if (!e) return NULL;
    if (e->kind == RELK_VARIADIC)
        return vrel_variant_or_null(e->vrel, arity);   /* absent = empty */
    return (e->arity == arity) ? e->rel : NULL;
}

relation *db_entry_variant_rw(rel_entry *e, uint8_t arity)
{
    if (!e) return NULL;
    if (e->kind == RELK_VARIADIC)
        return vrel_variant(e->vrel, arity);           /* get-or-create */
    return (e->arity == arity) ? e->rel : NULL;
}

relation *db_rel_at_arity_ro(const dl_db *db, int rel_id, uint8_t arity)
{
    if (!db || rel_id < 0 || (size_t)rel_id >= db->nrels) return NULL;
    return db_entry_variant_ro(&db->rels[rel_id], arity);
}

relation *db_rel_at_arity_rw(dl_db *db, int rel_id, uint8_t arity)
{
    if (!db || rel_id < 0 || (size_t)rel_id >= db->nrels) return NULL;
    return db_entry_variant_rw(&db->rels[rel_id], arity);
}

/* Does any on-disk file exist for variant a of `name`?  (declare-time scan) */
static int variant_files_exist(dl_db *db, const char *name, uint8_t a)
{
    static const char *suffixes[3] = {".dafsa", ".wal", ".base.dafsa"};
    struct stat st;
    int i;
    for (i = 0; i < 3; i++) {
        char *p = make_vpath(db, name, a, suffixes[i]);
        int exists = (p && stat(p, &st) == 0);
        free(p);
        if (exists) return 1;
    }
    return 0;
}

/* A variadic variant's rule-head-ness is derived from FILE EXISTENCE:
 * <name>.<a>.base.dafsa present => open as an idb (split base/view)
 * variant, else as an EDB variant (base aliases view).  rels.txt carries a
 * single edb|idb flag for the WHOLE variadic relation, which cannot
 * express per-variant state — so the base file is the durable per-variant
 * marker (dl_close writes it for every idb variant, exactly like fixed
 * rule-head relations). */
static int variant_is_idb_on_disk(dl_db *db, const char *name, uint8_t a)
{
    struct stat st;
    char *p = make_vpath(db, name, a, ".base.dafsa");
    int exists = (p && stat(p, &st) == 0);
    free(p);
    return exists;
}

/* Get-or-open (durable, WAL-backed) variant a of a variadic entry.  The
 * open replays + compacts any existing WAL, mirroring the fixed declare
 * path.  On a filesystem-less eval clone (dir==NULL) the variant is created
 * in memory instead. */
static relation *variadic_open_variant(dl_db *db, rel_entry *e, uint8_t a)
{
    relation *r;
    char *dafsa, *wal, *base = NULL;
    int is_idb;

    if (!db || !e || e->kind != RELK_VARIADIC) return NULL;
    if (a == 0 || a > MAX_VAR_ARITY) return NULL;
    if (!db->dir) return vrel_variant(e->vrel, a);   /* eval clone */

    r = vrel_variant_or_null(e->vrel, a);
    if (r) return r;

    dafsa = make_vpath(db, e->name, a, ".dafsa");
    wal   = make_vpath(db, e->name, a, ".wal");
    is_idb = variant_is_idb_on_disk(db, e->name, a);
    if (is_idb)
        base = make_vpath(db, e->name, a, ".base.dafsa");

    if (!dafsa || !wal || (is_idb && !base)) {
        free(dafsa); free(wal); free(base);
        return NULL;
    }

    r = is_idb ? rel_open_writable_idb(base, dafsa, wal, a)
               : rel_open_writable(dafsa, wal, a);

    free(dafsa);
    free(wal);
    free(base);

    if (!r) return NULL;
    if (vrel_attach(e->vrel, a, r) != 0) {
        rel_free(r);
        return NULL;
    }
    return r;
}

relation *dl_ensure_variant(dl_db *db, int rel_id, uint8_t arity)
{
    if (!db || rel_id < 0 || (size_t)rel_id >= db->nrels) return NULL;
    if (db->rels[rel_id].kind != RELK_VARIADIC)
        return db_entry_variant_ro(&db->rels[rel_id], arity);
    return variadic_open_variant(db, &db->rels[rel_id], arity);
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

    /* v2-lists: load the list term store.  A pre-lists DB has no terms.bin
     * and opens with an EMPTY store (NIL only). */
    {
        char *tpath = make_path(db, "terms", ".bin");
        if (!tpath) {
            intern_free(db->ir);
            close(lfd); free(db->dir); free(db);
            if (err_out) *err_out = -1;
            return NULL;
        }
        db->terms = term_load(tpath);
        free(tpath);
        if (!db->terms) {
            intern_free(db->ir);
            close(lfd); free(db->dir); free(db);
            if (err_out) *err_out = -1;
            return NULL;
        }
    }

    /* Load existing relations from metadata file.  Read the WHOLE file into
     * a temporary array BEFORE declaring any relation: declaring rewrites
     * rels.txt, so declaring inside the read loop would truncate the file
     * mid-read and silently drop relations. */
    rels_path = make_path(db, "rels", ".txt");
    if (rels_path) {
        rf = fopen(rels_path, "r");
        if (rf) {
            char *line = NULL;
            size_t cap = 0;
            ssize_t len;
            char rel_names[MAX_RELS][256];
            uint8_t rel_arities[MAX_RELS];
            uint8_t rel_idb[MAX_RELS];
            size_t n_meta = 0;

            while ((len = getline(&line, &cap, rf)) > 0 && n_meta < MAX_RELS) {
                char *colon;
                if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
                if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
                colon = strchr(line, ':');
                if (colon) {
                    *colon = '\0';
                    char *rest = colon + 1;
                    int arity = atoi(rest);
                    int is_variadic = (rest[0] == '*');
                    int is_idb = 0;
                    char *colon2 = strchr(rest, ':');
                    if (colon2 && strcmp(colon2 + 1, "idb") == 0) is_idb = 1;
                    /* v2 backward-compat: a 'name:*:edb|idb' variadic marker
                     * is accepted here; arity 1..8 lines parse identically to
                     * v1 (old binaries simply skip the '*' line — atoi("*")
                     * is 0, outside 1..8).  The flag is advisory for
                     * variadic lines: per-variant edb|idb state is derived
                     * from <name>.<a>.base.dafsa file existence. */
                    if (line[0] != '\0' &&
                        (is_variadic || (arity >= 1 && arity <= 8))) {
                        snprintf(rel_names[n_meta], sizeof(rel_names[n_meta]),
                                 "%s", line);
                        /* arity 0 carries the VARIADIC kind downstream */
                        rel_arities[n_meta] = (uint8_t)(is_variadic ? 0 : arity);
                        rel_idb[n_meta] = (uint8_t)is_idb;
                        n_meta++;
                    }
                }
            }
            free(line);
            fclose(rf);

            for (size_t mi = 0; mi < n_meta; mi++) {
                dl_declare_relation_kind(db, rel_names[mi],
                                         rel_arities[mi], rel_idb[mi]);
            }
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

    /* IVM Slice 1/3: free any pending delta/delete tuple_sets */
    vm_clear_deltas(db);
    vm_clear_deletes(db);

    /* Save interner */
    {
        char *fwd_path = make_path(db, "symbols", ".dafsa");
        char *rev_path = make_path(db, "symbols", ".array");
        if (fwd_path && rev_path)
            intern_save(db->ir, fwd_path, rev_path);
        free(fwd_path);
        free(rev_path);
    }

    /* v2-lists: save the term store AFTER the interner and BEFORE any
     * relation DAFSA that references list handles (the same ordering
     * invariant as the interner-before-relation rule). */
    {
        char *tpath = make_path(db, "terms", ".bin");
        if (tpath) {
            term_save(db->terms, tpath);
            free(tpath);
        }
    }

    /* M7/IVM: compact each relation's BASE (save + truncate WAL).  Rule-head
     * (IDB) relations additionally persist their VIEW to <name>.dafsa>.
     * v2: a VARIADIC relation does the same PER VARIANT under
     * <name>.<a>.dafsa — an idb variant's base file doubles as the durable
     * per-variant edb|idb marker consulted at reopen. */
    for (i = 0; i < db->nrels; i++) {
        char *path;
        if (db->rels[i].kind == RELK_VARIADIC) {
            uint8_t a;
            for (a = 1; a <= MAX_VAR_ARITY; a++) {
                relation *vr = vrel_variant_or_null(db->rels[i].vrel, a);
                if (!vr) continue;
                if (rel_is_idb(vr)) {
                    path = make_vpath(db, db->rels[i].name, a, ".base.dafsa");
                    if (path) {
                        rel_compact(vr, path);
                        free(path);
                    }
                    path = make_vpath(db, db->rels[i].name, a, ".dafsa");
                    if (path) {
                        rel_save(vr, path);
                        free(path);
                    }
                } else {
                    path = make_vpath(db, db->rels[i].name, a, ".dafsa");
                    if (path) {
                        rel_compact(vr, path);
                        free(path);
                    }
                }
            }
        } else if (rel_is_idb(db->rels[i].rel)) {
            path = make_path(db, db->rels[i].name, ".base.dafsa");
            if (path) {
                rel_compact(db->rels[i].rel, path);
                free(path);
            }
            path = make_path(db, db->rels[i].name, ".dafsa");
            if (path) {
                rel_save(db->rels[i].rel, path);
                free(path);
            }
        } else {
            path = make_path(db, db->rels[i].name, ".dafsa");
            if (path) {
                rel_compact(db->rels[i].rel, path);
                free(path);
            }
        }
    }

    /* Save relation metadata (name:arity:edb|idb) while names/rels are alive */
    write_rels_txt(db);

    /* Now free relations and their names */
    for (i = 0; i < db->nrels; i++) {
        if (db->rels[i].kind == RELK_VARIADIC)
            vrel_free(db->rels[i].vrel);   /* frees every present variant */
        else
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
    term_free(db->terms);
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

/* Persist relation metadata (name:arity:edb|idb per line).  The edb|idb
 * flag records whether the relation is a rule head whose base facts live in
 * <name>.base.dafsa (idb) or a pure-EDB relation with a single <name>.dafsa
 * (base == view). */
static void write_rels_txt(dl_db *db)
{
    char *rels_path = make_path(db, "rels", ".txt");
    char *buf;
    char *p;
    size_t total = 1;  /* trailing NUL */
    size_t i;

    if (!rels_path) return;

    /* Size the buffer: each line is "name:arity:edb|idb\n" — arity is one
     * digit (1-8), the flag is 3 chars, so strlen(name) + 16 is ample. */
    for (i = 0; i < db->nrels; i++)
        total += strlen(db->rels[i].name) + 16;

    buf = malloc(total);
    if (!buf) { free(rels_path); return; }

    p = buf;
    for (i = 0; i < db->nrels; i++) {
        int n;
        if (db->rels[i].kind == RELK_VARIADIC) {
            /* v2 variadic marker: 'name:*:edb|idb'.  Older binaries skip
             * this line (their arity 1..8 check rejects it); the flag is
             * advisory — per-variant edb|idb state is derived at reopen
             * from <name>.<a>.base.dafsa file existence. */
            n = snprintf(p, total - (size_t)(p - buf), "%s:*:%s\n",
                         db->rels[i].name,
                         vrel_any_idb(db->rels[i].vrel) ? "idb" : "edb");
        } else {
            n = snprintf(p, total - (size_t)(p - buf), "%s:%d:%s\n",
                         db->rels[i].name,
                         (int)rel_arity(db->rels[i].rel),
                         rel_is_idb(db->rels[i].rel) ? "idb" : "edb");
        }
        if (n < 0) { free(buf); free(rels_path); return; }
        p += n;
    }

    /* IVM Slice 5 (persistence polish): atomic write — tmp + fsync + rename +
     * dir-fsync (the same pattern dafsa_save uses).  A crash mid-write under
     * the old fopen("w") truncate-then-write could leave a corrupt rels.txt,
     * and with the edb|idb flag a relation could be reopened with the WRONG
     * kind (edb as idb or vice versa).  atomic_write_str never exposes a
     * partial file: readers see either the old or the new rels.txt, never a
     * truncated one. */
    atomic_write_str(rels_path, buf);
    free(buf);
    free(rels_path);
}

/* Does `name` look like a variadic-variant file stem "<x>.<d>" with d a
 * single digit 1..8?  (Parser identifiers cannot contain '.', so only C-API
 * callers can produce such names — the guard below keeps their files from
 * aliasing a variadic relation's variant files.) */
static int is_variant_stem(const char *name, char stem[256])
{
    size_t len = strlen(name);
    if (len < 3 || len > 254) return 0;
    if (name[len - 2] != '.') return 0;
    if (name[len - 1] < '1' || name[len - 1] > '8') return 0;
    memcpy(stem, name, len - 2);
    stem[len - 2] = '\0';
    return 1;
}

/* Declare a relation, optionally as a rule head (is_idb).  arity == 0
 * declares a VARIADIC relation (dl_declare_relation_variadic). */
static int dl_declare_relation_kind(dl_db *db, const char *name,
                                    uint8_t arity, int is_idb)
{
    int idx;
    char *path;
    relation *rel;
    vrelation *vrel = NULL;

    if (!db || !name) return -1;
    if (arity > 8) return -1;   /* 0 = variadic (v2) */

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

    /* v2: keep variadic variant files (<x>.<a>.dafsa) from aliasing the
     * files of another declared relation (only reachable via the C API,
     * since parser identifiers cannot contain '.'). */
    if (arity == 0) {
        /* declaring variadic `name`: no fixed "<name>.<d>" may exist */
        size_t i;
        for (i = 0; i < db->nrels; i++) {
            if (db->rels[i].kind != RELK_VARIADIC && db->rels[i].name[0]) {
                char stem[256];
                if (is_variant_stem(db->rels[i].name, stem) &&
                    strcmp(stem, name) == 0) {
                    fprintf(stderr, "error: variadic relation '%s' collides "
                            "with variant files of '%s'\n",
                            name, db->rels[i].name);
                    return -1;
                }
            }
        }
    } else {
        /* declaring fixed `name` shaped "<x>.<d>": `x` may not be variadic */
        char stem[256];
        if (is_variant_stem(name, stem)) {
            int sidx = find_rel(db, stem);
            if (sidx >= 0 && db->rels[sidx].kind == RELK_VARIADIC) {
                fprintf(stderr, "error: relation name '%s' collides with "
                        "variant files of variadic '%s'\n", name, stem);
                return -1;
            }
        }
    }

    /* Check if already declared */
    idx = find_rel(db, name);
    if (idx >= 0) {
        if (arity == 0) {
            /* Idempotent variadic redeclare */
            if (db->rels[idx].kind == RELK_VARIADIC) return 0;
            return -1;  /* exists as fixed: kind mismatch */
        }
        if (db->rels[idx].kind == RELK_VARIADIC) return -1;  /* kind mismatch */
        /* Idempotent: same arity is fine */
        if (rel_arity(db->rels[idx].rel) == arity)
            return 0;
        return -1;  /* arity mismatch */
    }

    if (db->nrels >= MAX_RELS) return -1;

    if (arity == 0) {
        /* ── VARIADIC: create the family and open every variant that has
         * files on disk (lazy from here on — new arities materialize on
         * first add/load/compile via variadic_open_variant). */
        uint8_t a;
        rel_entry tmp;
        vrel = vrel_create();
        if (!vrel) return -1;
        memset(&tmp, 0, sizeof(tmp));
        tmp.name = (char *)name;   /* borrowed: variadic_open_variant only
                                      reads it to build file paths */
        tmp.kind = RELK_VARIADIC;
        tmp.vrel = vrel;
        for (a = 1; a <= MAX_VAR_ARITY; a++) {
            if (!variant_files_exist(db, name, a)) continue;
            if (!variadic_open_variant(db, &tmp, a)) {
                vrel_free(vrel);
                return -1;
            }
        }
    } else {
        /* Try to load existing DAFSA with WAL, or create new */
        char *wal_path;
        char *base_path;
        path = make_path(db, name, ".dafsa");
        wal_path = make_path(db, name, ".wal");
        base_path = is_idb ? make_path(db, name, ".base.dafsa") : NULL;
        if (!path || !wal_path || (is_idb && !base_path)) {
            free(path); free(wal_path); free(base_path);
            return -1;
        }

        if (is_idb)
            rel = rel_open_writable_idb(base_path, path, wal_path, arity);
        else
            rel = rel_open_writable(path, wal_path, arity);
        free(path);
        free(wal_path);
        free(base_path);
        if (!rel) return -1;
    }

    /* Register */
    idx = (int)db->nrels++;
    db->rels[idx].name = strdup(name);
    db->rels[idx].kind = (arity == 0) ? RELK_VARIADIC : RELK_FIXED;
    db->rels[idx].arity = arity;
    db->rels[idx].rel  = (arity == 0) ? NULL : rel;
    db->rels[idx].vrel = (arity == 0) ? vrel : NULL;
    if (!db->rels[idx].name ||
        (arity == 0 ? !db->rels[idx].vrel : !db->rels[idx].rel)) {
        if (arity == 0) vrel_free(db->rels[idx].vrel);
        else rel_free(db->rels[idx].rel);
        free(db->rels[idx].name);
        db->nrels--;
        return -1;
    }

    /* M7: persist relation metadata immediately so crash-recovery works.
     * Without this, a process that declares a relation and adds facts
     * before crashing would lose the relation declaration on reopen. */
    write_rels_txt(db);

    return 0;
}

int dl_declare_relation(dl_db *db, const char *name, uint8_t arity)
{
    return dl_declare_relation_kind(db, name, arity, 0);
}

int dl_declare_relation_variadic(dl_db *db, const char *name)
{
    return dl_declare_relation_kind(db, name, 0, 0);
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

/* v2: variadic bulk load.  The CSV has VARIABLE-WIDTH rows — each row's
 * field count (1..8) IS that fact's arity, routed to the matching variant
 * (rows with 0 or >8 fields are skipped, mirroring the fixed loader's
 * skip-on-mismatch policy).  Per-arity tuple_sets union the pre-existing
 * variant BASE with the new rows; each touched variant is bulk-rebuilt and
 * base-saved exactly like the fixed loader.  Returns the total facts loaded
 * across all arities, or -1 on error. */
static int dl_load_facts_variadic(dl_db *db, int idx,
                                  const char *rel_name, const char *csv_path)
{
    FILE *f;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    rel_entry *e = &db->rels[idx];
    tuple_set ts[MAX_VAR_ARITY + 1];
    int have[MAX_VAR_ARITY + 1];
    long loaded = 0;
    long n_new = 0;
    int a;
    int rc = -1;

    memset(ts, 0, sizeof(ts));
    for (a = 0; a <= MAX_VAR_ARITY; a++) have[a] = 0;

    f = fopen(csv_path, "r");
    if (!f) return -1;

    while ((linelen = getline(&line, &linecap, f)) > 0) {
        char *fields[9];   /* one extra: csv_split TRUNCATES at max_fields,
                              so a >8-field row would otherwise be silently
                              misread as arity 8 with its tail dropped —
                              scan with cap 9 and reject nf==9 instead. */
        int nf;
        uint32_t cols[8];
        int i;

        if (linelen > 0 && line[linelen - 1] == '\n')
            line[--linelen] = '\0';
        if (linelen > 0 && line[linelen - 1] == '\r')
            line[--linelen] = '\0';
        if (linelen == 0) continue;

        /* Variable-width row: field count = arity. */
        nf = csv_split(line, fields, 9);
        if (nf < 1 || nf > 8) continue;   /* empty or >8 fields: skip */

        for (i = 0; i < nf; i++) {
            if (is_integer(fields[i])) {
                unsigned long val = strtoul(fields[i], NULL, 10);
                if (val > 0xFFFFFFFFUL) break;   /* overflow: skip row */
                cols[i] = (uint32_t)val;
            } else {
                uint32_t sym = intern_str(db->ir, fields[i]);
                if (sym == 0) goto out;          /* OOM: abort */
                cols[i] = sym;
            }
        }
        if (i < nf) continue;                    /* row marked for skip */

        if (!have[nf]) {
            relation *vr = vrel_variant_or_null(e->vrel, (uint8_t)nf);
            if (ts_init(&ts[nf], (uint8_t)nf) != 0) goto out;
            have[nf] = 1;
            /* Union the variant's pre-existing BASE (e.g. loaded from disk
             * on open) so the rebuild produces the combined set. */
            if (vr && rel_prefix_base(vr, NULL, 0, ts_sink_cb, &ts[nf]) < 0)
                goto out;
        }
        {
            int trc = ts_add(&ts[nf], cols);
            if (trc < 0) goto out;
            if (trc == 1) n_new++;
        }
    }

    /* M7 invariant: interner durable BEFORE any relation DAFSA that
     * references the (possibly new) sym_ids. */
    {
        char *fwd_path = make_path(db, "symbols", ".dafsa");
        char *rev_path = make_path(db, "symbols", ".array");
        if (fwd_path && rev_path) {
            if (intern_save(db->ir, fwd_path, rev_path) != 0) {
                free(fwd_path); free(rev_path);
                goto out;
            }
        }
        free(fwd_path);
        free(rev_path);
    }

    /* v2-lists: term store durable before any relation DAFSA referencing
     * list handles. */
    {
        char *tpath = make_path(db, "terms", ".bin");
        if (tpath) {
            if (term_save(db->terms, tpath) != 0) {
                free(tpath);
                goto out;
            }
            free(tpath);
        }
    }

    for (a = 1; a <= MAX_VAR_ARITY; a++) {
        relation *vr;
        char *path;

        if (!have[a]) continue;

        vr = variadic_open_variant(db, e, (uint8_t)a);   /* may materialize */
        if (!vr) goto out;

        ts_sort(&ts[a]);
        if (rel_build_base_from_tupleset(vr, &ts[a]) != 0)
            goto out;
        loaded += ts[a].count;

        /* Auto-save the variant's BASE (atomic via dafsa_save) — bulk load
         * bypasses the WAL; the base DAFSA is the durable home. */
        path = rel_is_idb(vr)
             ? make_vpath(db, rel_name, (uint8_t)a, ".base.dafsa")
             : make_vpath(db, rel_name, (uint8_t)a, ".dafsa");
        if (path) {
            rel_save_base(vr, path);
            free(path);
        }
    }

    rc = (int)loaded;

    if (n_new > 0) {
        /* v2 gating: a variadic relation is outside every incremental class
         * (a single-arity delta_pending cannot represent mixed arities) —
         * force the FULL fixpoint at the next publish.  Never silently
         * mis-evaluate. */
        db->full_reeval_pending = 1;
        db->fixpoint_dirty = 1;
        permindex_mark_dirty(db, idx);
    }

out:
    free(line);
    fclose(f);
    for (a = 1; a <= MAX_VAR_ARITY; a++)
        if (have[a]) ts_free(&ts[a]);
    return rc;
}

int dl_load_facts(dl_db *db, const char *rel_name, const char *csv_path)
{
    FILE *f;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    int idx, loaded = 0;
    int delta_failed = 0;
    int n_new = 0;
    uint8_t arity;
    tuple_set ts;
    tuple_set delta;

    if (!db || !rel_name || !csv_path) return -1;

    idx = find_rel(db, rel_name);
    if (idx < 0) return -1;  /* relation not declared */

    /* v2: variable-width rows routed per-arity. */
    if (db->rels[idx].kind == RELK_VARIADIC)
        return dl_load_facts_variadic(db, idx, rel_name, csv_path);

    arity = rel_arity(db->rels[idx].rel);

    f = fopen(csv_path, "r");
    if (!f) return -1;

    /* Collect new facts in a tuple_set (ts = base ∪ new for the bulk rebuild);
     * delta = the newly-added facts (the +delta batch for IVM). */
    if (ts_init(&ts, arity) != 0) {
        fclose(f);
        return -1;
    }
    if (ts_init(&delta, arity) != 0) {
        ts_free(&ts);
        fclose(f);
        return -1;
    }

    /* If the relation already had BASE facts (e.g. loaded from disk on open),
     * union them into ts so we rebuild the combined set. */
    if (rel_prefix_base(db->rels[idx].rel, NULL, 0,
                        ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        ts_free(&delta);
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
                    ts_free(&delta);
                    fclose(f);
                    free(line);
                    return -1;
                }
                cols[i] = sym;
            }
        }

        /* Add to tuple_set (hash dedup).  ts already holds the pre-existing
         * BASE facts, so ts_add returns 1 only for a genuinely NEW fact (not in
         * base, not already added by this CSV) — capture it as a +delta. */
        {
            int rc = ts_add(&ts, cols);
            if (rc < 0) {
                ts_free(&ts);
                ts_free(&delta);
                fclose(f);
                free(line);
                return -1;
            }
            if (rc == 1) {
                n_new++;
                if (!delta_failed && ts_add(&delta, cols) < 0)
                    delta_failed = 1;   /* OOM: fall back to full re-eval */
            }
        }
    }

    free(line);
    fclose(f);

    /* Sort and bulk-build the DAFSA. */
    ts_sort(&ts);

    if (rel_build_base_from_tupleset(db->rels[idx].rel, &ts) != 0) {
        ts_free(&ts);
        ts_free(&delta);
        return -1;
    }

    loaded = (int)ts.count;
    ts_free(&ts);

    /* M7: save interner BEFORE relation save (invariant: interner durable
     * before relation, so relation DAFSA never references a sym_id not on
     * disk).  Bulk load interns string columns inline, so any new syms must be
     * durable before the base DAFSA that references them. */
    {
        char *fwd_path = make_path(db, "symbols", ".dafsa");
        char *rev_path = make_path(db, "symbols", ".array");
        if (fwd_path && rev_path) {
            if (intern_save(db->ir, fwd_path, rev_path) != 0) {
                free(fwd_path); free(rev_path);
                ts_free(&delta);
                return -1;
            }
        }
        free(fwd_path);
        free(rev_path);
    }

    /* v2-lists: term store durable before the relation base DAFSA. */
    {
        char *tpath = make_path(db, "terms", ".bin");
        if (tpath) {
            if (term_save(db->terms, tpath) != 0) {
                free(tpath);
                ts_free(&delta);
                return -1;
            }
            free(tpath);
        }
    }

    /* Auto-save the relation's BASE DAFSA after load (atomic via dafsa_save).
     * Bulk load bypasses the WAL — the base DAFSA is the durable home for the
     * loaded facts (a documented durability difference vs dl_add_fact). */
    {
        char *path = rel_is_idb(db->rels[idx].rel)
                   ? make_path(db, rel_name, ".base.dafsa")
                   : make_path(db, rel_name, ".dafsa");
        if (path) {
            rel_save_base(db->rels[idx].rel, path);
            free(path);
        }
    }

    /* IVM Slice 5: bulk load is a BATCHED DELTA.  The newly-loaded facts are
     * captured as a single +delta batch and propagated by the existing IVM
     * machinery at the next publish (vm_propagate_deltas / the recursive seed
     * / aggregate maintenance / DRed) instead of unconditionally forcing a
     * full re-eval.  Only the cases outside the incremental classes fall back:
     *   - loading into a RULE-HEAD relation (mixed EDB+IDB: the fact must
     *     appear in the view AND propagate) → full re-eval;
     *   - OOM while capturing the delta → full re-eval (never leave derived
     *     views stale). */
    if (n_new > 0) {
        if (delta_failed || ivm_rel_is_head(db, idx)) {
            db->full_reeval_pending = 1;
        } else {
            long di;
            for (di = 0; di < delta.count; di++) {
                if (ivm_capture_delta(db, idx,
                        delta.data + (size_t)di * arity, arity) != 0) {
                    /* OOM: the base is already committed (rebuilt + saved).
                     * Fall back to a full re-eval at publish rather than a
                     * partial propagate (never silently mis-evaluate). */
                    db->full_reeval_pending = 1;
                    break;
                }
            }
        }
        ts_free(&delta);

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
    } else {
        ts_free(&delta);
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

/* IVM Slice 1: is rel_id a rule head (and therefore a derived view)? */
static int ivm_rel_is_head(dl_db *db, int rel_id)
{
    int i;
    for (i = 0; i < db->n_crules; i++)
        if ((int)db->crules[i]->head_rel_id == rel_id) return 1;
    return 0;
}

/* IVM Slice 1: append a +delta for rel_id to db->delta_pending (lazy-init).
 * Returns 0 on success (added or duplicate), -1 on error. */
static int ivm_capture_delta(dl_db *db, int rel_id,
                             const uint32_t *cols, uint8_t arity)
{
    tuple_set *ts = db->delta_pending[rel_id];
    int rc;
    if (!ts) {
        ts = malloc(sizeof(*ts));
        if (!ts) return -1;
        if (ts_init(ts, arity) != 0) { free(ts); return -1; }
        db->delta_pending[rel_id] = ts;
    }
    rc = ts_add(ts, cols);
    return (rc < 0) ? -1 : 0;
}

/* IVM Slice 3: append a -delta for rel_id to db->del_pending (lazy-init).
 * Returns 0 on success (added or duplicate), -1 on error. */
static int ivm_capture_delete(dl_db *db, int rel_id,
                              const uint32_t *cols, uint8_t arity)
{
    tuple_set *ts = db->del_pending[rel_id];
    int rc;
    if (!ts) {
        ts = malloc(sizeof(*ts));
        if (!ts) return -1;
        if (ts_init(ts, arity) != 0) { free(ts); return -1; }
        db->del_pending[rel_id] = ts;
    }
    rc = ts_add(ts, cols);
    return (rc < 0) ? -1 : 0;
}

/* v2: variadic add — routes to (get-or-opens) the WAL-backed variant for
 * `arity`, mirroring the fixed path's interner-before-WAL / dup-skip /
 * WAL-append / in-memory-commit / compaction-threshold ordering exactly. */
static int dl_add_fact_variadic(dl_db *db, int idx, const char *rel_name,
                                const uint32_t *cols, uint8_t arity)
{
    relation *vr;
    unsigned char key[33];  /* 4*8+1 */
    size_t key_len;
    int rc;

    if (arity == 0 || arity > 8) return -1;

    vr = variadic_open_variant(db, &db->rels[idx], arity);
    if (!vr) return -1;

    if (encode_fact_key(key, &key_len, cols, arity) != 0) return -1;

    /* 1. Interner-before-WAL invariant (see the fixed path). */
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

    /* v2-lists: the term store must be durable before a WAL that references
     * a list handle (same invariant as the interner). */
    if (term_is_dirty(db->terms)) {
        char *tpath = make_path(db, "terms", ".bin");
        if (!tpath) return -1;
        if (term_save(db->terms, tpath) != 0) { free(tpath); return -1; }
        free(tpath);
    }

    /* 2. Duplicate check against the variant BASE. */
    if (rel_exact_base(vr, cols))
        return 0;

    /* 3. WAL-append ADD + sync (durable before in-memory commit). */
    if (rel_wal_append_add(vr, key, (uint32_t)key_len) != 0)
        return -1;

    /* 4. In-memory add to the variant BASE. */
    rc = rel_add_base(vr, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(db, idx);
        /* v2 gating: variadic is outside every incremental class (a
         * single-arity delta_pending cannot represent a variadic relation)
         * — force the full fixpoint at the next publish.  Never silently
         * mis-evaluate. */
        db->full_reeval_pending = 1;
    }

    /* 5. Compaction threshold (per variant). */
    {
        uint64_t wal_sz = rel_wal_size(vr);
        uint64_t dafsa_sz = rel_dafsa_size(vr);
        if (dafsa_sz > 0 && wal_sz > dafsa_sz / 4) {
            char *path = rel_is_idb(vr)
                       ? make_vpath(db, rel_name, arity, ".base.dafsa")
                       : make_vpath(db, rel_name, arity, ".dafsa");
            if (path) {
                rel_compact(vr, path);
                free(path);
            }
        }
    }

    db->fixpoint_dirty = 1;

    return 1;  /* added */
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

    /* v2: route to the arity's variant. */
    if (db->rels[idx].kind == RELK_VARIADIC)
        return dl_add_fact_variadic(db, idx, rel_name, cols, arity);

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

    /* v2-lists: the term store must be durable before a WAL that references
     * a list handle (same invariant as the interner). */
    if (term_is_dirty(db->terms)) {
        char *tpath = make_path(db, "terms", ".bin");
        if (!tpath) return -1;
        if (term_save(db->terms, tpath) != 0) { free(tpath); return -1; }
        free(tpath);
    }

    /* 2. Duplicate check against BASE: if already present, skip WAL
     * (SHOULD-FIX). A duplicate base fact is already durable. */
    if (rel_exact_base(db->rels[idx].rel, cols))
        return 0;

    /* 3. WAL-append ADD + sync (durable before in-memory commit) */
    if (rel_wal_append_add(db->rels[idx].rel, key, (uint32_t)key_len) != 0)
        return -1;

    /* 4. In-memory add to BASE */
    rc = rel_add_base(db->rels[idx].rel, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        /* M6: the base grew — any perm index on this relation is stale until
         * the next permindex_build_dirty (perm DAFSAs are not delta-aware). */
        permindex_mark_dirty(db, idx);
    }

    /* 4a. IVM Slice 1: capture the insert delta (AFTER the in-memory commit,
     * per the interner-before-WAL ordering note — no interner interaction
     * here).  A base fact added to a RULE-HEAD relation is the mixed EDB+IDB
     * case (the fact must appear in the view AND propagate) — out of the
     * Slice 1 class, so force a full re-eval instead of a wrong propagate. */
    if (rc == 1) {
        if (ivm_rel_is_head(db, idx)) {
            db->full_reeval_pending = 1;
        } else if (ivm_capture_delta(db, idx, cols, arity) != 0) {
            /* OOM capturing the delta: the base fact is already committed
             * (base + WAL).  Fall back to a full re-eval at publish rather
             * than leaving downstream derived views stale (never silently
             * mis-evaluate). */
            db->full_reeval_pending = 1;
        }
    }

    /* 5. Check compaction threshold: WAL > 25% of BASE estimate? */
    {
        uint64_t wal_sz = rel_wal_size(db->rels[idx].rel);
        uint64_t dafsa_sz = rel_dafsa_size(db->rels[idx].rel);
        if (dafsa_sz > 0 && wal_sz > dafsa_sz / 4) {
            char *path = rel_is_idb(db->rels[idx].rel)
                       ? make_path(db, rel_name, ".base.dafsa")
                       : make_path(db, rel_name, ".dafsa");
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

/* v2: variadic delete — routes to the variant for `arity`; a successful
 * delete forces the full fixpoint (variadic is outside the DRed class). */
static int dl_delete_fact_variadic(dl_db *db, int idx,
                                   const uint32_t *cols, uint8_t arity)
{
    relation *vr;
    unsigned char key[33];  /* 4*8+1 */
    size_t key_len;
    int rc;

    if (arity == 0 || arity > 8) return -1;

    vr = variadic_open_variant(db, &db->rels[idx], arity);
    if (!vr) return -1;

    if (encode_fact_key(key, &key_len, cols, arity) != 0) return -1;

    /* 1. Absent check against the variant BASE: if not present, skip WAL. */
    if (!rel_exact_base(vr, cols))
        return 0;

    /* 2. WAL-append DEL + sync (durable before in-memory commit). */
    if (rel_wal_append_del(vr, key, (uint32_t)key_len) != 0)
        return -1;

    /* 3. In-memory delete from the variant BASE. */
    rc = rel_delete_base(vr, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(db, idx);
        /* v2 gating: variadic deletions are outside DRed (no single-arity
         * del_pending) — force the full fixpoint (correctness floor). */
        db->full_reeval_pending = 1;
    }

    db->fixpoint_dirty = 1;

    return 1;  /* deleted */
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

    /* v2: route to the arity's variant. */
    if (db->rels[idx].kind == RELK_VARIADIC)
        return dl_delete_fact_variadic(db, idx, cols, arity);

    if (arity != rel_arity(db->rels[idx].rel)) return -1;

    if (encode_fact_key(key, &key_len, cols, arity) != 0) return -1;

    /* 1. Absent check against BASE: if not present, skip WAL (SHOULD-FIX).
     * No interner save needed — deletes don't create new syms. */
    if (!rel_exact_base(db->rels[idx].rel, cols))
        return 0;

    /* 2. WAL-append DEL + sync (durable before in-memory commit) */
    if (rel_wal_append_del(db->rels[idx].rel, key, (uint32_t)key_len) != 0)
        return -1;

    /* 3. In-memory delete from BASE */
    rc = rel_delete_base(db->rels[idx].rel, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        /* M6: the base shrank — any perm index on this relation is stale
         * until the next permindex_build_dirty (perm DAFSAs are not
         * delta-aware).  Without this, a delete could leave stale tuples
         * visible through a non-leading (OP_LOOKUP_PERM) join. */
        permindex_mark_dirty(db, idx);
    }

    /* IVM Slice 3: record the -delta.  If the program is DRed-eligible
     * (non-recursive, no aggregates, no WALK/LOOKUP_PERM/HASH_JOIN), the
     * publish path maintains the derived views incrementally via DRed
     * (over-delete + re-derive); otherwise the dispatch falls back to the
     * full fixpoint (the correctness floor).  An OOM capturing the delta
     * forces the fallback — never silently leave derived views stale. */
    if (ivm_capture_delete(db, idx, cols, arity) != 0)
        db->full_reeval_pending = 1;

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

    /* v2: exact membership against the arity's variant. */
    if (db->rels[idx].kind == RELK_VARIADIC) {
        if (arity == 0 || arity > 8) return 0;
        return vrel_exact(db->rels[idx].vrel, cols, arity);
    }

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

    /* v2: prefix enumeration fanned out over variants a >= k (each is the
     * existing fixed-width byte-prefix walk; the cb's arity parameter
     * disambiguates tuples across arities). */
    if (db->rels[idx].kind == RELK_VARIADIC)
        return vrel_prefix(db->rels[idx].vrel, leading, k, cb, user);

    return rel_prefix(db->rels[idx].rel, leading, k, cb, user);
}

/* ─── Order statistics (Tier-2) ───────────────────────────────────────── */

/* Resolve a FIXED-arity relation for the order-statistics API.  Returns NULL
 * on any error (NULL/unknown rel, variadic relation, arity mismatch) — these
 * all map to a -1/0/sentinel rejection at the call site (never silently
 * mis-evaluated). */
static relation *ordstats_rel_ro(const dl_db *db, const char *rel_name,
                                 uint8_t arity)
{
    int idx;

    if (!db || !rel_name) return NULL;
    idx = find_rel((dl_db *)db, rel_name);
    if (idx < 0) return NULL;
    if (db->rels[idx].kind == RELK_VARIADIC) return NULL;      /* rejected this slice */
    if (db->rels[idx].arity != arity) return NULL;             /* arity mismatch */
    return db->rels[idx].rel;
}

uint64_t dl_rank(dl_db *db, const char *rel_name, const uint32_t *cols,
                 uint8_t arity)
{
    relation *rel;
    if (!cols) return UINT64_MAX;
    rel = ordstats_rel_ro(db, rel_name, arity);
    if (!rel) return UINT64_MAX;
    return rel_rank(rel, cols);
}

int dl_select(dl_db *db, const char *rel_name, uint64_t k,
              uint32_t *cols_out, uint8_t arity)
{
    relation *rel;
    if (!cols_out) return -1;
    rel = ordstats_rel_ro(db, rel_name, arity);
    if (!rel) return -1;
    return rel_select(rel, k, cols_out);
}

uint64_t dl_range_count(dl_db *db, const char *rel_name, const uint32_t *lo,
                        const uint32_t *hi, uint8_t arity)
{
    relation *rel;
    if (!lo || !hi) return UINT64_MAX;
    rel = ordstats_rel_ro(db, rel_name, arity);
    if (!rel) return UINT64_MAX;
    return rel_range_count(rel, lo, hi);
}

uint64_t dl_count(dl_db *db, const char *rel_name)
{
    relation *rel;

    if (!db || !rel_name) return UINT64_MAX;
    {
        int idx = find_rel(db, rel_name);
        if (idx < 0) return UINT64_MAX;
        if (db->rels[idx].kind == RELK_VARIADIC) return UINT64_MAX;
        rel = db->rels[idx].rel;
    }
    return rel_count_subtree(rel);
}

uint64_t dl_rank_bound(dl_db *db, const char *rel_name,
                       const uint32_t *leading, uint8_t k,
                       const uint32_t *cols, uint8_t arity)
{
    relation *rel;
    if (!cols) return UINT64_MAX;
    if (k > 0 && !leading) return UINT64_MAX;
    rel = ordstats_rel_ro(db, rel_name, arity);
    if (!rel) return UINT64_MAX;
    return rel_rank_bound(rel, leading, k, cols);
}

int dl_select_bound(dl_db *db, const char *rel_name,
                    const uint32_t *leading, uint8_t k, uint64_t idx,
                    uint32_t *cols_out, uint8_t arity)
{
    relation *rel;
    if (!cols_out) return -1;
    if (k > 0 && !leading) return -1;
    rel = ordstats_rel_ro(db, rel_name, arity);
    if (!rel) return -1;
    return rel_select_bound(rel, leading, k, idx, cols_out);
}

uint64_t dl_range_count_bound(dl_db *db, const char *rel_name,
                              const uint32_t *leading, uint8_t k,
                              const uint32_t *lo, const uint32_t *hi,
                              uint8_t arity)
{
    relation *rel;
    if (!lo || !hi) return UINT64_MAX;
    if (k > 0 && !leading) return UINT64_MAX;
    rel = ordstats_rel_ro(db, rel_name, arity);
    if (!rel) return UINT64_MAX;
    return rel_range_count_bound(rel, leading, k, lo, hi);
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

static void ast_tok_free(token *t);

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
    if (t->nchildren > 0) {
        int i;
        n->children = calloc((size_t)t->nchildren, sizeof(token *));
        if (!n->children) { free(n->text); free(n); return NULL; }
        n->nchildren = t->nchildren;
        for (i = 0; i < t->nchildren; i++) {
            n->children[i] = ast_tok_clone(t->children[i]);
            if (!n->children[i]) { ast_tok_free(n); return NULL; }
        }
    }
    if (t->tail) {
        n->tail = ast_tok_clone(t->tail);
        if (!n->tail) { ast_tok_free(n); return NULL; }
    }
    return n;
}

static void ast_tok_free(token *t)
{
    int i;
    if (!t) return;
    if (t->children) {
        for (i = 0; i < t->nchildren; i++)
            ast_tok_free(t->children[i]);
        free(t->children);
    }
    ast_tok_free(t->tail);
    free(t->text);
    free(t);
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
    if (a->arith) {
        n->arith = expr_clone(a->arith);
        if (!n->arith) goto fail;
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
            for (j = 0; j < n->nargs; j++)
                ast_tok_free(n->args[j]);
            free(n->args);
        }
        ast_tok_free(n->agg_op);
        expr_free(n->arith);
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

    /* IVM Slice 1: the rule set changed — pending deltas can no longer be
     * propagated against the old program, so force a full re-eval. */
    db->full_reeval_pending = 1;

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

    /* M4: compilation successful, fixpoint is now clean.  IVM Slice 1/3: the
     * full re-eval consumed every pending change — drop deltas + deletes +
     * the full-reeval flag so subsequent changes can propagate incrementally. */
    db->fixpoint_dirty = 0;
    db->full_reeval_pending = 0;
    vm_clear_deltas(db);
    vm_clear_deletes(db);
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
        /* v2: stream every variant (cb carries each tuple's arity). */
        if (db->rels[idx].kind == RELK_VARIADIC)
            return vrel_prefix(db->rels[idx].vrel, NULL, 0, cb, user);
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
        /* v2: prefix fan-out over variants a >= k. */
        if (db->rels[idx].kind == RELK_VARIADIC)
            return vrel_prefix(db->rels[idx].vrel, leading, k, cb, user);
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
    out->terms = src->terms;   /* v2-lists: borrowed — list handles shared */
    out->lock_fd = -1;
    for (i = 0; i < src->nrels; i++) {
        out->rels[i].name  = src->rels[i].name;   /* borrowed — not owned */
        out->rels[i].kind  = src->rels[i].kind;   /* v2: keep the kind tag */
        out->rels[i].arity = src->rels[i].arity;
        out->rels[i].rel   = src->rels[i].rel;    /* borrowed — not owned */
        out->rels[i].vrel  = src->rels[i].vrel;   /* borrowed — not owned */
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
        if (edb->rels[i].kind == RELK_VARIADIC)
            vrel_free(edb->rels[i].vrel);
        else
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
    edb->rels[edb->nrels].kind  = RELK_FIXED;
    edb->rels[edb->nrels].arity = arity;
    edb->rels[edb->nrels].rel   = rel;
    edb->rels[edb->nrels].vrel  = NULL;
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

    /* v2: a variadic goal has no single arity to adorn — and any program
     * containing a variadic relation is outside the magic-sets class.
     * REJECT loudly (consistent with the existing rejection list: never
     * silently mis-evaluate). */
    if (db_has_variadic(db)) {
        fprintf(stderr, "dl_query_magic: rejected: program contains a "
                "variadic relation (outside the magic-sets class)\n");
        return -1;
    }
    /* v2-lists: list-builtin programs are outside the magic-sets class
     * (never silently mis-evaluated) — route to the full fixpoint. */
    if (db_has_list_builtin(db)) {
        fprintf(stderr, "dl_query_magic: rejected: program uses a list "
                "builtin (outside the magic-sets class)\n");
        return -1;
    }

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

    /* v2: variadic relations are outside the magic-sets class (a variadic
     * goal has no single arity to adorn; the transform's arity-keyed
     * adorned variants assume fixed-width predicates).  REJECT loudly. */
    if (db_has_variadic(db)) {
        fprintf(stderr, "dl_query_magic: rejected: program contains a "
                "variadic relation (outside the magic-sets class)\n");
        return -1;
    }
    /* v2-lists: list-builtin programs are outside the magic-sets class
     * (never silently mis-evaluated) — route to the full fixpoint. */
    if (db_has_list_builtin(db)) {
        fprintf(stderr, "dl_query_magic: rejected: program uses a list "
                "builtin (outside the magic-sets class)\n");
        return -1;
    }

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

    /* Negation/aggregate soundness: a negated or aggregated body atom in a
     * reachable rule is evaluated against the FULL materialization of its
     * predicate — the clone shallow-aliases src's relations, and a negated
     * non-closure IDB atom (or an aggregate over one) must see the compiled
     * tuples, not an empty relation.  If the fixpoint is dirty and any rule
     * uses negation/aggregate, materialize src first (mirrors dl_query's
     * compile-on-query behavior).  This is the ONLY case dl_query_magic
     * mutates the db: all-positive programs (T11/T27) stay non-mutating. */
    if (db->fixpoint_dirty) {
        int ri;
        for (ri = 0; ri < db->n_ast_rules; ri++) {
            const rule *R = db->ast_rules[ri];
            if (R && (R->has_negation || R->has_aggregate)) {
                if (dl_compile(db) != 0) return -1;
                break;
            }
        }
    }

    /* 3. Clone (Option C). */
    eval_db_clone(db, &edb);
    n_aliased = edb.nrels;
    memset(&prog, 0, sizeof(prog));

    /* 4. Transform. */
    if (magic_transform_adorn((const rule *const *)db->ast_rules,
                              db->n_ast_rules, goal_rel, goal_arity,
                              adorn, vals, nvals, db->nrels, db->ir, &prog,
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

    /* 8. Scoped fixpoint (skip-materialize: the eval clone is torn down
     *    immediately after streaming, so skip the DAFSA bulk-build and
     *    export the adorned-goal idb tuple_set via the vm_* hook). */
    {
        int a_idx = find_rel(&edb, prog.adorned_goal);
        if (a_idx < 0) {
            fprintf(stderr, "dl_query_magic: internal: adorned goal '%s' "
                    "missing\n", prog.adorned_goal);
            magic_program_free(&prog);
            goto out_free_crules;
        }

        tuple_set goal_ts;
        memset(&goal_ts, 0, sizeof(goal_ts));
        vm_nomaterialize = 1;
        vm_export_relid  = a_idx;
        vm_export_ts     = &goal_ts;

        int exec_rc = vm_execute(&edb, magic_crules, n_magic);

        /* Reset the hook regardless of the fixpoint result. */
        vm_nomaterialize = 0;
        vm_export_relid  = -1;
        vm_export_ts     = NULL;

        if (exec_rc != 0) {
            fprintf(stderr, "dl_query_magic: scoped fixpoint failed\n");
            ts_free(&goal_ts);   /* no-op unless the goal was exported */
            magic_program_free(&prog);
            goto out_free_crules;
        }

        /* 9. Stream the fully-materialized adorned goal through the
         * per-position filter.  Two cases:
         *   - The goal is a recursive head → its idb was exported to goal_ts
         *     (arity set) by the skip-materialize hook in section 6.
         *   - The goal is non-recursive (aggregate / negation / projection) →
         *     it was materialized via the M1 path; stream its DAFSA. */
        magic_filter_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.user_cb = cb;
        ctx.user = user;
        ctx.arity = goal_arity;
        memcpy(ctx.adorn, adorn, alen);
        ctx.adorn[alen] = '\0';
        memcpy(ctx.vals, vals, (size_t)nvals * sizeof(uint32_t));

        if (goal_ts.arity > 0) {
            /* Exported recursive-goal idb (already sorted). */
            long ci;
            for (ci = 0; ci < goal_ts.count; ci++) {
                const uint32_t *t = goal_ts.data + (size_t)ci * goal_ts.arity;
                if (magic_filter_cb(t, goal_ts.arity, &ctx) != 0) break;
            }
            result = ctx.matched;
            ts_free(&goal_ts);
        } else {
            /* Non-recursive goal: DAFSA materialized via M1. */
            if (rel_prefix(edb.rels[a_idx].rel, NULL, 0,
                           magic_filter_cb, &ctx) < 0)
                result = -1;
            else
                result = ctx.matched;
        }
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

/* ─── top-down / QSQ bound query (opt-in 4th path) ─────────────────────── */

long dl_query_topdown(dl_db *db, const char *goal_rel,
                      const uint32_t *leading, uint8_t k,
                      dl_tuple_cb cb, void *user)
{
    int goal_idx;
    uint8_t goal_arity;
    char adorn[9];
    uint8_t i;

    if (!db || !goal_rel || !cb) return -1;

    if (db_has_variadic(db)) {
        fprintf(stderr, "dl_query_topdown: rejected: program contains a "
                "variadic relation (outside the magic-sets class)\n");
        return -1;
    }
    if (db_has_list_builtin(db)) {
        fprintf(stderr, "dl_query_topdown: rejected: program uses a list "
                "builtin (outside the magic-sets class)\n");
        return -1;
    }

    goal_idx = find_rel(db, goal_rel);
    if (goal_idx < 0) return -1;
    goal_arity = rel_arity(db->rels[goal_idx].rel);
    if (k > goal_arity) return -1;
    if (k == 0)
        return dl_query(db, goal_rel, cb, user);

    for (i = 0; i < k; i++) adorn[i] = 'b';
    for (i = k; i < goal_arity; i++) adorn[i] = 'f';
    adorn[goal_arity] = '\0';

    return dl_query_topdown_adorn(db, goal_rel, adorn, leading, k, cb, user);
}

long dl_query_topdown_adorn(dl_db *db, const char *goal_rel,
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
    compiled_rule **td_crules = NULL;
    int n_td = 0;
    size_t n_aliased;
    int d;
    long result = -1;
    int goal_variant_id = -1;

    if (!db || !goal_rel || !adorn || !cb) return -1;

    /* 1. Validate + resolve goal. */
    goal_idx = find_rel(db, goal_rel);
    if (goal_idx < 0) return -1;
    goal_arity = rel_arity(db->rels[goal_idx].rel);

    if (db_has_variadic(db)) {
        fprintf(stderr, "dl_query_topdown: rejected: program contains a "
                "variadic relation (outside the magic-sets class)\n");
        return -1;
    }
    if (db_has_list_builtin(db)) {
        fprintf(stderr, "dl_query_topdown: rejected: program uses a list "
                "builtin (outside the magic-sets class)\n");
        return -1;
    }

    /* 2. Validate the adornment (mirrors dl_query_magic_adorn). */
    alen = strlen(adorn);
    if (alen != goal_arity) {
        fprintf(stderr, "dl_query_topdown: adornment length %zu != goal arity %u\n",
                alen, goal_arity);
        return -1;
    }
    for (xi = 0; xi < alen; xi++) {
        if (adorn[xi] != 'b' && adorn[xi] != 'f') {
            fprintf(stderr, "dl_query_topdown: bad adornment char '%c'\n",
                    adorn[xi]);
            return -1;
        }
        if (adorn[xi] == 'b') nb++;
    }
    if (nb != (int)nvals) {
        fprintf(stderr, "dl_query_topdown: nvals=%u != count_b(adorn)=%d\n",
                nvals, nb);
        return -1;
    }

    /* All-free adorn → route to dl_query. */
    if (nvals == 0)
        return dl_query(db, goal_rel, cb, user);

    /* EDB goal → direct full-scan + per-position filter. */
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

    if (db->n_ast_rules <= 0) return -1;

    /* Negation/aggregate soundness (mirrors dl_query_magic_adorn). */
    if (db->fixpoint_dirty) {
        int ri;
        for (ri = 0; ri < db->n_ast_rules; ri++) {
            const rule *R = db->ast_rules[ri];
            if (R && (R->has_negation || R->has_aggregate)) {
                if (dl_compile(db) != 0) return -1;
                break;
            }
        }
    }

    /* 3. Clone (Option C). */
    eval_db_clone(db, &edb);
    n_aliased = edb.nrels;
    memset(&prog, 0, sizeof(prog));

    /* 4. Transform. */
    if (magic_transform_adorn((const rule *const *)db->ast_rules,
                              db->n_ast_rules, goal_rel, goal_arity,
                              adorn, vals, nvals, db->nrels, db->ir, &prog,
                              reject, sizeof(reject)) != 0) {
        fprintf(stderr, "dl_query_topdown: rejected: %s\n", reject);
        goto out_free_edb;
    }

    /* 5. Pre-declare ALL head relations (adorned + magic). */
    for (d = 0; d < prog.n_decls; d++) {
        if (eval_db_declare_inmem(&edb, prog.decls[d].name,
                                  prog.decls[d].arity) != 0) {
            fprintf(stderr, "dl_query_topdown: cannot pre-declare '%s'\n",
                    prog.decls[d].name);
            magic_program_free(&prog);
            goto out_free_edb;
        }
    }

    /* 6. Compile with g_reorder=0 / g_bushy=0 so compiled body_idx == AST
     *    body position and recursive atoms stay override-compatible. */
    {
        int saved_reorder = g_reorder;
        int saved_bushy = g_bushy;
        int compile_rc;
        g_reorder = 0;
        g_bushy = 0;
        compile_rc = compile_rules(&edb, prog.rules, prog.n_rules,
                                   &td_crules, &n_td);
        g_reorder = saved_reorder;
        g_bushy = saved_bushy;
        if (compile_rc != 0) {
            fprintf(stderr, "dl_query_topdown: compile of adorned program failed\n");
            magic_program_free(&prog);
            goto out_free_edb;
        }
    }

    /* Build the permutation indices the compiler declared (OP_LOOKUP_PERM
     * over EDB atoms — non-leading adornments).  The forward fixpoint builds
     * these inside vm_execute; the top-down driver uses vm_exec_rule directly,
     * so it must build them explicitly or an EDB OP_LOOKUP_PERM reads a NULL
     * perm DAFSA (silent empty result). */
    if (permindex_build_dirty(&edb) != 0) {
        fprintf(stderr, "dl_query_topdown: perm index build failed\n");
        magic_program_free(&prog);
        goto out_free_crules;
    }

    /* Filesystem-trap backstop. */
    if (edb.dir != NULL ||
        edb.nrels != n_aliased + (size_t)prog.n_decls) {
        fprintf(stderr, "dl_query_topdown: internal error: compile_rules grew "
                "the eval clone's relation table\n");
        magic_program_free(&prog);
        goto out_free_crules;
    }

    /* 7. Locate the goal variant (adorned_goal among the decl pairs). */
    for (d = 0; d < prog.n_decls; d += 2) {
        if (strcmp(prog.decls[d].name, prog.adorned_goal) == 0) {
            goal_variant_id = d / 2;
            break;
        }
    }
    if (goal_variant_id < 0) {
        fprintf(stderr, "dl_query_topdown: internal: adorned goal '%s' missing\n",
                prog.adorned_goal);
        magic_program_free(&prog);
        goto out_free_crules;
    }

    /* 8. Top-down / QSQ evaluation (streams the goal memo directly). */
    result = td_eval(&edb, &prog, td_crules, n_td,
                     goal_variant_id, vals, cb, user);

    magic_program_free(&prog);
out_free_crules:
    {
        int i;
        for (i = 0; i < n_td; i++) compiled_rule_free(td_crules[i]);
        free(td_crules);
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

    /* 1. Materialize derived views if rules exist and the fixpoint is dirty.
     * IVM Slice 1/3 dispatch —
     *   full_reeval_pending, or a change outside every incremental class
     *                                       → FULL fixpoint (oracle)
     *   pending DELETE deltas on a DRed-eligible program (or inserts on a
     *   program that is only DRed-eligible, e.g. stratified negation)
     *                                       → DRed over-delete + re-derive
     *   pending insert deltas + eligible    → delta propagation (Slice 1/2)
     *   else                                → pure save
     * Any change outside the incremental classes MUST end in the first
     * branch (full_reeval_pending is set at the change site), so we never
     * wrongly propagate. */
    if (db->n_crules > 0 && db->fixpoint_dirty) {
        int has_del = 0, has_ins = 0, has_agg = 0, dri;
        for (dri = 0; dri < MAX_RELS; dri++) {
            if (db->del_pending[dri] && db->del_pending[dri]->count > 0)
                has_del = 1;
            if (db->delta_pending[dri] && db->delta_pending[dri]->count > 0)
                has_ins = 1;
        }
        for (dri = 0; dri < db->n_crules; dri++)
            if (db->crules[dri]->has_aggregate) has_agg = 1;
        if (db->full_reeval_pending) {
            if (vm_execute(db, db->crules, db->n_crules) != 0)
                return -1;
            vm_clear_deltas(db);   /* full re-eval consumed pending changes */
            vm_clear_deletes(db);
        } else if (has_agg) {
            /* IVM Slice 4: aggregates under change.  Incremental affected-
             * group re-scan for count/sum/min/max; anything the maintenance
             * cannot handle soundly falls back to the full fixpoint (the
             * correctness floor — never silently mis-evaluate). */
            if (vm_agg_eligible(db)) {
                if (vm_agg_maintain(db) != 0) {
                    db->full_reeval_pending = 1;
                    return -1;
                }
            } else {
                if (vm_execute(db, db->crules, db->n_crules) != 0)
                    return -1;
                vm_clear_deltas(db);
                vm_clear_deletes(db);
            }
        } else if (has_del && !vm_dred_eligible(db)) {
            if (vm_execute(db, db->crules, db->n_crules) != 0)
                return -1;
            vm_clear_deltas(db);   /* full re-eval consumed pending changes */
            vm_clear_deletes(db);
        } else if (has_ins && !vm_ivm_eligible(db) && !vm_dred_eligible(db)) {
            if (vm_execute(db, db->crules, db->n_crules) != 0)
                return -1;
            vm_clear_deltas(db);   /* full re-eval consumed pending changes */
            vm_clear_deletes(db);
        } else if (has_del || (has_ins && !vm_ivm_eligible(db))) {
            /* IVM Slice 3: DRed over-delete + re-derive.  Consumes both the
             * pending deletes and any co-pending inserts. */
            if (vm_dred_delete(db) != 0) {
                /* The view may be partially updated and the pending changes
                 * were consumed.  Force a full re-eval on the next publish
                 * (never leave a silently-incomplete view). */
                db->full_reeval_pending = 1;
                return -1;
            }
        } else if (!vm_ivm_eligible(db)) {
            /* Defensive: dirty with no tracked deltas on an ineligible
             * program — run the full fixpoint (the correctness floor). */
            if (vm_execute(db, db->crules, db->n_crules) != 0)
                return -1;
            vm_clear_deltas(db);
            vm_clear_deletes(db);
        } else if (vm_has_recursive(db)) {
            /* IVM Slice 2: recursive insert IVM — seed the semi-naive fixpoint
             * with the current view + the pending insert deltas (never reset
             * the view on this path). */
            if (vm_execute_ivm(db) != 0) {
                /* The view may be partially updated and the delta seed did not
                 * converge cleanly.  Force a full re-eval on the next publish
                 * (never leave a silently-incomplete recursive view). */
                db->full_reeval_pending = 1;
                vm_clear_deltas(db);
                return -1;
            }
            vm_clear_deltas(db);   /* delta seed consumed the pending changes */
        } else if (vm_propagate_deltas(db) != 0) {
            /* Propagation failed part-way (OOM): the view may be partially
             * updated and delta_pending was cleared.  Force a full re-eval
             * on the next publish instead of leaving a silently-incomplete
             * view (never silently mis-evaluate). */
            db->full_reeval_pending = 1;
            return -1;
        }
        db->full_reeval_pending = 0;
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

    /* 4a'. Save the list term store (list handles) BEFORE the relations that
     * reference them (same ordering invariant as the interner). */
    {
        char tpath[4096];
        snprintf(tpath, sizeof(tpath), "%s/terms.bin", tmp_dir);
        if (term_save(db->terms, tpath) != 0)
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

            if (db->rels[i].kind == RELK_VARIADIC) {
                /* v2: one marker line 'name:*:edb|idb' (old readers skip it)
                 * plus, per PRESENT variant, an ordinary fixed-arity line
                 * 'name.<a>:<a>:edb|idb' + 'name.<a>.dafsa' — the per-variant
                 * lines are exactly what the snapshot query path fans out
                 * over (and harmless phantoms for old readers). */
                uint8_t a;
                fprintf(mf, "%s:*:%s\n", db->rels[i].name,
                        vrel_any_idb(db->rels[i].vrel) ? "idb" : "edb");
                for (a = 1; a <= MAX_VAR_ARITY; a++) {
                    relation *vr = vrel_variant_or_null(db->rels[i].vrel, a);
                    char vname[384];
                    if (!vr) continue;
                    if (snprintf(vname, sizeof(vname), "%s.%d",
                                 db->rels[i].name, (int)a) >=
                        (int)sizeof(vname))
                        { fclose(mf); goto fail; }
                    fprintf(mf, "%s:%d:%s\n", vname, (int)a,
                            rel_is_idb(vr) ? "idb" : "edb");
                    snprintf(rel_path, sizeof(rel_path), "%s/%s.dafsa",
                             tmp_dir, vname);
                    if (rel_save(vr, rel_path) != 0) {
                        fclose(mf);
                        goto fail;
                    }
                    if (db->fault_hook &&
                        db->fault_hook(DL_FPOINT_AFTER_REL_SAVE,
                                       db->fault_user) != 0) {
                        fclose(mf);
                        goto fail;
                    }
                }
                continue;
            }

            uint8_t arity = rel_arity(db->rels[i].rel);

            fprintf(mf, "%s:%d:%s\n", db->rels[i].name, (int)arity,
                    rel_is_idb(db->rels[i].rel) ? "idb" : "edb");

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
        int variadic = 0;
        dafsa_view *v;

        snprintf(sdir, sizeof(sdir), "%s/snapshots/%u",
                 db->dir, db->snap_version);

        if (!manifest_find_rel_ex(sdir, rel_name, &arity, &variadic))
            return -1;

        if (variadic) {
            /* v2: pattern walk fanned out over the per-variant views. */
            uint8_t present[MAX_VAR_ARITY + 1];
            long total = 0;
            uint8_t a;
            manifest_find_variants(sdir, rel_name, present);
            for (a = 1; a <= MAX_VAR_ARITY; a++) {
                char vname[384];
                long n;
                if (!present[a]) continue;
                if (snprintf(vname, sizeof(vname), "%s.%d",
                             rel_name, (int)a) >= (int)sizeof(vname))
                    return -1;
                v = view_open_cached(db->vcache, vname, sdir);
                if (!v) return -1;
                n = view_pattern(v, a, dfa, cb, user);
                if (n < 0) return -1;
                total += n;
            }
            return total;
        }

        v = view_open_cached(db->vcache, rel_name, sdir);
        if (!v) return -1;

        return view_pattern(v, arity, dfa, cb, user);
    }

    /* In-memory path */
    idx = find_rel(db, rel_name);
    if (idx < 0) return -1;

    /* v2: pattern walk fanned out over present variants. */
    if (db->rels[idx].kind == RELK_VARIADIC)
        return vrel_pattern(db->rels[idx].vrel, dfa, (rel_enum_cb)cb, user);

    return rel_pattern(db->rels[idx].rel, dfa,
                       (rel_enum_cb)cb, user);
}
