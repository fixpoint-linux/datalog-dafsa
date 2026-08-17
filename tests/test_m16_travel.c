/*
 * test_m16_travel.c — time-travel / as-of snapshot queries
 *
 * Verifies the additive read-only time-travel surface over the existing
 * versioned snapshot dirs (snapshots/<N>/):
 *
 *   T1  as-of == captured CURRENT: dl_query_version(v) is byte-identical to
 *       dl_query captured at the moment v was current, for versions 1 and 2;
 *       dl_query() still reads the live CURRENT (v2).  Query ORDER is
 *       v2 → v1 → v2 to expose a rel_name-only cache-key bug (a shared cache
 *       would return the wrong version's view for the same rel_name).
 *   T2  immutability: add/delete AFTER v2 does not change dl_query_version(1)
 *       or (2); dl_query() stays on v2 until re-publish.
 *   T3  enumeration: dl_snapshot_versions returns [1,2] ascending; cap
 *       truncation (cap=1 → returns total 2, fills [1]); NULL out sizes.
 *   T4  nonexistent version: dl_query_version(99)==-1 and (0)==-1 (loud,
 *       never silently empty); unknown rel in a valid version == -1.
 *   T5  empty db (no snapshots/): dl_snapshot_versions == 0.
 *   T6  bound as-of: dl_query_bound_version(v1, leading={1}, k=1) == the
 *       {1,*}-prefix of the v1 relation.
 *   T7  retention: set retain=2, publish v3,v4 → [3,4] remain, v1/v2 pruned
 *       (-1); retain=0 restores keep-all.
 *   T8  variadic as-of: dl_query_version over a variadic relation returns
 *       arity-mixed tuples (locks the manifest_find_variants fan-out path).
 *
 * Build: make tests/test_m16_travel (link ALL_OBJS) — see Makefile.
 */
#include "dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %s ... ", name); \
    fflush(stdout); \
} while (0)
#define PASS() do { printf("OK\n"); } while (0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while (0)

/* ─── Fixed-arity tuple buffer (dl_query output, numeric/lex order) ─────── */

typedef struct {
    uint32_t *data;    /* count*arity u32s */
    long      count;
    uint8_t   arity;
    long      cap;
} tset;

static int tset_add(tset *t, const uint32_t *cols, uint8_t arity)
{
    if (t->count >= t->cap) {
        long nc = t->cap ? t->cap * 2 : 256;
        uint32_t *nd = realloc(t->data, (size_t)nc * arity * sizeof(uint32_t));
        if (!nd) return -1;
        t->data = nd;
        t->cap = nc;
    }
    memcpy(t->data + (size_t)t->count * arity, cols,
           (size_t)arity * sizeof(uint32_t));
    t->count++;
    return 0;
}

static void tset_free(tset *t) { free(t->data); memset(t, 0, sizeof(*t)); }

static int sink_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    return tset_add((tset *)user, cols, arity);
}

static int tset_eq(const tset *a, const tset *b)
{
    if (a->count != b->count || a->arity != b->arity) return 0;
    if (a->count == 0) return 1;
    return memcmp(a->data, b->data,
                  (size_t)a->count * a->arity * sizeof(uint32_t)) == 0;
}

/* Enumerate via dl_query (CURRENT routing).  Caller memsets the tset first. */
static long collect(dl_db *db, const char *rel, uint8_t arity, tset *ts)
{
    ts->arity = arity;
    return dl_query(db, rel, sink_cb, ts);
}

/* Enumerate via dl_query_version (as-of routing). */
static long collect_version(dl_db *db, uint32_t version, const char *rel,
                            uint8_t arity, tset *ts)
{
    ts->arity = arity;
    return dl_query_version(db, version, rel, sink_cb, ts);
}

/* ─── Variadic (mixed-arity) collector ──────────────────────────────────── */

#define VCOL_CAP 64

typedef struct {
    uint8_t  ar[VCOL_CAP];
    uint32_t cols[VCOL_CAP][8];
    long     count;
} vcol;

static int vcb(const uint32_t *cols, uint8_t arity, void *user)
{
    vcol *v = (vcol *)user;
    if (v->count >= VCOL_CAP) return -1;
    v->ar[v->count] = arity;
    memcpy(v->cols[v->count], cols, (size_t)arity * sizeof(uint32_t));
    v->count++;
    return 0;
}

static int vcol_has(const vcol *v, uint8_t arity, const uint32_t *cols)
{
    long i;
    for (i = 0; i < v->count; i++)
        if (v->ar[i] == arity &&
            memcmp(v->cols[i], cols, (size_t)arity * sizeof(uint32_t)) == 0)
            return 1;
    return 0;
}

/* ─── DB fixture ───────────────────────────────────────────────────────── */

static const char *BASE = "build-tmp/m16travel";

static dl_db *fresh_db(char *dir_out, size_t cap, const char *name)
{
    char cmd[512];
    snprintf(dir_out, cap, "%s/%s", BASE, name);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir_out);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir_out);
    system(cmd);
    return dl_open(dir_out);
}

/* ─── T1: as-of == captured CURRENT, query order v2 → v1 → v2 ──────────── */

static void t1_cache_order(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t1");
    tset A, B, q;
    uint32_t c[2];

    memset(&A, 0, sizeof(A));
    memset(&B, 0, sizeof(B));
    memset(&q, 0, sizeof(q));

    TEST("T1 as-of == captured CURRENT (query order v2→v1→v2)");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    c[0] = 1; c[1] = 2; dl_add_fact(db, "edge", c, 2);
    c[0] = 2; c[1] = 3; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v1 = A */

    if (collect(db, "edge", 2, &A) < 0) { FAIL("collect A"); goto out; }

    c[0] = 3; c[1] = 4; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v2 = B */

    if (collect(db, "edge", 2, &B) < 0) { FAIL("collect B"); goto out; }

    /* v2 (CURRENT) first, then v1, then v2 again — a rel_name-only shared
     * cache would return v2's view for the v1 query (silent wrong answer). */
    if (collect_version(db, 2, "edge", 2, &q) < 0) { FAIL("qv v2"); goto out; }
    if (!tset_eq(&q, &B)) { FAIL("qv(v2) != B"); goto out; }

    tset_free(&q);
    if (collect_version(db, 1, "edge", 2, &q) < 0) { FAIL("qv v1"); goto out; }
    if (!tset_eq(&q, &A)) { FAIL("qv(v1) != A"); goto out; }

    tset_free(&q);
    if (collect_version(db, 2, "edge", 2, &q) < 0) { FAIL("qv v2 again"); goto out; }
    if (!tset_eq(&q, &B)) { FAIL("qv(v2 again) != B"); goto out; }

    /* live CURRENT routing is untouched: dl_query() still reads v2. */
    tset_free(&q);
    if (collect(db, "edge", 2, &q) < 0) { FAIL("dl_query current"); goto out; }
    if (!tset_eq(&q, &B)) { FAIL("dl_query() != B"); goto out; }

    PASS();
out:
    tset_free(&A); tset_free(&B); tset_free(&q);
    dl_close(db);
}

/* ─── T2: immutability of as-of views ──────────────────────────────────── */

static void t2_immutable(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t2");
    tset A, B, q;
    uint32_t c[2];

    memset(&A, 0, sizeof(A));
    memset(&B, 0, sizeof(B));
    memset(&q, 0, sizeof(q));

    TEST("T2 immutability: post-v2 mutations leave v1/v2 as-of views unchanged");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    c[0] = 1; c[1] = 2; dl_add_fact(db, "edge", c, 2);
    c[0] = 2; c[1] = 3; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v1 = A */
    if (collect(db, "edge", 2, &A) < 0) { FAIL("collect A"); goto out; }

    c[0] = 3; c[1] = 4; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v2 = B */
    if (collect(db, "edge", 2, &B) < 0) { FAIL("collect B"); goto out; }

    /* mutate live: add (4,5), delete (1,2). */
    c[0] = 4; c[1] = 5; dl_add_fact(db, "edge", c, 2);
    c[0] = 1; c[1] = 2; dl_delete_fact(db, "edge", c, 2);

    if (collect_version(db, 1, "edge", 2, &q) < 0) { FAIL("qv v1"); goto out; }
    if (!tset_eq(&q, &A)) { FAIL("v1 changed by later mutations"); goto out; }

    tset_free(&q);
    if (collect_version(db, 2, "edge", 2, &q) < 0) { FAIL("qv v2"); goto out; }
    if (!tset_eq(&q, &B)) { FAIL("v2 changed by later mutations"); goto out; }

    /* dl_query() still reads CURRENT == v2 (add/delete do not re-flip). */
    tset_free(&q);
    if (collect(db, "edge", 2, &q) < 0) { FAIL("dl_query current"); goto out; }
    if (!tset_eq(&q, &B)) { FAIL("dl_query() != B until re-publish"); goto out; }

    PASS();
out:
    tset_free(&A); tset_free(&B); tset_free(&q);
    dl_close(db);
}

/* ─── T3: version enumeration ──────────────────────────────────────────── */

static void t3_enum(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t3");
    uint32_t c[2] = {1, 1};
    uint32_t vers[8];
    long n;

    TEST("T3 enumeration: ascending + cap truncation + NULL sizing");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v1 */
    c[0] = 2; c[1] = 2; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v2 */

    n = dl_snapshot_versions(db, vers, 8);
    if (n != 2 || vers[0] != 1 || vers[1] != 2) { FAIL("enum [1,2]"); goto out; }

    /* cap truncation: returns TOTAL 2, fills only [1]. */
    vers[0] = 0;
    n = dl_snapshot_versions(db, vers, 1);
    if (n != 2 || vers[0] != 1) { FAIL("cap=1 → total 2, fill [1]"); goto out; }

    /* NULL out sizes. */
    if (dl_snapshot_versions(db, NULL, 0) != 2) { FAIL("NULL out sizes"); goto out; }

    PASS();
out:
    dl_close(db);
}

/* ─── T4: nonexistent version is a loud error ──────────────────────────── */

static void t4_nonexistent(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t4");
    tset q;
    uint32_t c[2] = {1, 1};
    uint32_t out[4];

    memset(&q, 0, sizeof(q));

    TEST("T4 nonexistent version/rel: loud -1, never empty");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v1 exists */

    if (dl_query_version(db, 99, "edge", sink_cb, &q) != -1) { FAIL("version 99"); goto out; }
    if (dl_query_version(db, 0, "edge", sink_cb, &q) != -1) { FAIL("version 0"); goto out; }
    if (dl_query_version(db, 1, "nope", sink_cb, &q) != -1) { FAIL("unknown rel in valid version"); goto out; }
    if (dl_query_version(db, 1, NULL, sink_cb, &q) != -1) { FAIL("NULL goal_rel"); goto out; }
    if (dl_query_version(db, 1, "edge", NULL, &q) != -1) { FAIL("NULL cb"); goto out; }
    if (dl_query_bound_version(db, 99, "edge", c, 1, sink_cb, &q) != -1) { FAIL("bound version 99"); goto out; }
    if (dl_snapshot_versions(NULL, out, 4) != -1) { FAIL("NULL db enumeration"); goto out; }

    PASS();
out:
    tset_free(&q);
    dl_close(db);
}

/* ─── T5: empty db has no versions ─────────────────────────────────────── */

static void t5_empty(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t5");

    TEST("T5 empty db: no snapshots/ → 0 versions");

    if (dl_snapshot_versions(db, NULL, 0) != 0) { FAIL("expected 0 versions"); goto out; }

    PASS();
out:
    dl_close(db);
}

/* ─── T6: bound as-of query ────────────────────────────────────────────── */

static void t6_bound(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t6");
    tset q;
    uint32_t c[2];
    uint32_t leading[1] = {1};
    uint32_t exp[2] = {1, 2};

    memset(&q, 0, sizeof(q));

    TEST("T6 bound as-of: leading {1} prefix of v1");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    c[0] = 1; c[1] = 2; dl_add_fact(db, "edge", c, 2);
    c[0] = 2; c[1] = 3; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v1 */

    q.arity = 2;
    if (dl_query_bound_version(db, 1, "edge", leading, 1, sink_cb, &q) != 1) {
        FAIL("bound count"); goto out;
    }
    if (q.count != 1 || memcmp(q.data, exp, 2 * sizeof(uint32_t)) != 0) {
        FAIL("bound tuple"); goto out;
    }

    PASS();
out:
    tset_free(&q);
    dl_close(db);
}

/* ─── T7: retention prune-to-N then restore keep-all ───────────────────── */

static void t7_retention(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t7");
    uint32_t c[2];
    uint32_t vers[8];
    tset q;
    long n;

    memset(&q, 0, sizeof(q));

    TEST("T7 retention: prune-to-2 then restore keep-all");

    assert(dl_declare_relation(db, "edge", 2) == 0);
    c[0] = 1; c[1] = 1; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v1 */
    c[0] = 2; c[1] = 2; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v2 */

    if (dl_set_snapshot_retain(db, 2) != 0) { FAIL("set retain 2"); goto out; }

    c[0] = 3; c[1] = 3; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v3 → prune v1 */
    n = dl_snapshot_versions(db, vers, 8);
    if (n != 2 || vers[0] != 2 || vers[1] != 3) { FAIL("after v3 [2,3]"); goto out; }
    if (dl_query_version(db, 1, "edge", sink_cb, &q) != -1) { FAIL("v1 pruned after v3"); goto out; }

    c[0] = 4; c[1] = 4; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v4 → prune v2 */
    n = dl_snapshot_versions(db, vers, 8);
    if (n != 2 || vers[0] != 3 || vers[1] != 4) { FAIL("after v4 [3,4]"); goto out; }
    if (dl_query_version(db, 1, "edge", sink_cb, &q) != -1) { FAIL("v1 pruned after v4"); goto out; }
    if (dl_query_version(db, 2, "edge", sink_cb, &q) != -1) { FAIL("v2 pruned after v4"); goto out; }
    tset_free(&q);
    if (collect_version(db, 4, "edge", 2, &q) < 0) { FAIL("v4 works"); goto out; }

    /* restore keep-all */
    if (dl_set_snapshot_retain(db, 0) != 0) { FAIL("set retain 0"); goto out; }
    c[0] = 5; c[1] = 5; dl_add_fact(db, "edge", c, 2);
    assert(dl_publish_snapshot(db) == 0);           /* v5 → keep-all */
    n = dl_snapshot_versions(db, vers, 8);
    if (n != 3 || vers[0] != 3 || vers[1] != 4 || vers[2] != 5) {
        FAIL("keep-all restored [3,4,5]"); goto out;
    }

    PASS();
out:
    tset_free(&q);
    dl_close(db);
}

/* ─── T8: variadic as-of query (arity-mixed tuples) ────────────────────── */

static void t8_variadic(char *dir, size_t cap)
{
    dl_db *db = fresh_db(dir, cap, "t8");
    vcol got;
    uint32_t c[8];
    long n;
    uint32_t t2[2] = {1, 2};
    uint32_t t3[3] = {3, 4, 5};

    memset(&got, 0, sizeof(got));

    TEST("T8 variadic as-of: arity-mixed tuples via dl_query_version");

    assert(dl_declare_relation_variadic(db, "v") == 0);
    c[0] = 1; c[1] = 2; dl_add_fact(db, "v", c, 2);
    c[0] = 3; c[1] = 4; c[2] = 5; dl_add_fact(db, "v", c, 3);
    assert(dl_publish_snapshot(db) == 0);           /* v1 */

    n = dl_query_version(db, 1, "v", vcb, &got);
    if (n != 2) { FAIL("variadic count"); goto out; }
    if (!vcol_has(&got, 2, t2) || !vcol_has(&got, 3, t3)) {
        FAIL("variadic tuples missing"); goto out;
    }

    PASS();
out:
    dl_close(db);
}

/* ─── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    char dir[512];

    printf("=== M16 time-travel (as-of) snapshot query tests ===\n");

    t1_cache_order(dir, sizeof(dir));
    t2_immutable(dir, sizeof(dir));
    t3_enum(dir, sizeof(dir));
    t4_nonexistent(dir, sizeof(dir));
    t5_empty(dir, sizeof(dir));
    t6_bound(dir, sizeof(dir));
    t7_retention(dir, sizeof(dir));
    t8_variadic(dir, sizeof(dir));

    printf("\n%d tests run, %d failed.\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
