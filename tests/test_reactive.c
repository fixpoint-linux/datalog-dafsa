/*
 * test_reactive.c — Capability 2 slice R1: fired-event observation over the
 * EXISTING delta dispatch (dl_fired_init / dl_set_reactive / dl_fired_step /
 * dl_fired_clear, zig/src/reactive.zig).
 *
 * F1  transitive closure, insert: one step reports EXACTLY the newly-derived
 *     tc tuples; a second step with no new deltas reports ZERO.
 * F2  delete/retraction boundary (stated loudly): a delete-driven step
 *     reports the disappeared tc tuples as DL_REACTIVE_REMOVED events (a
 *     diff, flagged FALLBACK because the cascade takes its full-recompute
 *     branch for a delete against recursive rules); first-class retraction
 *     FIRING is slice R2.
 * F3  full-re-eval fallback flag: a regex-walk program (OP_WALK — outside
 *     the incremental class) still reports the CORRECT diff, flagged.
 * F4  NULL-ISOLATION: the four entry points are session-scoped; every
 *     behaviour of the engine with no session open is unchanged (this is
 *     also the run_zig_suites.sh oracle itself).  API-contract tests pin
 *     the error codes instead.
 * F5  post-clear: the db behaves exactly as before the session; a fresh
 *     session sees only genuinely-new tuples (no state leaks).
 * F6  multi-cycle stepping: deltas accumulate across steps exactly once
 *     each (the baseline advances per step).
 * F7  no-init programmes are untouched (plain consolidations + queries).
 * F8  arity-1 watched head: the callback must SEE and RECORD arity-1
 *     tuples (the old sink silently dropped non-arity-2 events).
 * F9  arity-3 watched head: same, arity 3.
 * F10 >512-tuple event: a step firing 600 tuples must be LOUD in the
 *     harness (overflow flag) while the engine's returned count stays
 *     exact — a truncating sink can never again hide a wrong diff.
 * F11 agg-eligible incremental branch: a grouped-aggregate program steps
 *     through vm_agg_maintain (routing-PROVEN via vm_agg_runs), UNFLAGGED,
 *     reporting both the new and the superseded aggregate rows.
 * F12 quiescent-after-fallback invariant: after ONE fallback-flagged step,
 *     later quiescent steps report 0 with NO flag (the step's cascade
 *     clears fixpoint_dirty/full_reeval_pending exactly like
 *     dl_consolidate), and a plain dl_consolidate between reactive steps
 *     does not desync the session.
 * F13 multi-db sessions: clearing one db's session must never break
 *     another db's (regression for the tombstone-less open-addressing
 *     table: clears of LIVE sessions returned -2 and survivors leaked).
 * F14 early-stop: a callback that aborts mid-stream advances the baseline
 *     to the post-step content exactly once (no leak, no re-fire).
 * F15 two-head early-abort boundary: with BOTH heads watched, an abort on
 *     head A's second event leaves head B's baseline at its pre-step
 *     content, so B's next step reports its own delta exactly once (and A
 *     never re-reports) — the multi-watcher contract in dl.h.
 * F16 variadic-head refusal: arming a variadic rule head (rel_entry.rel is
 *     null by construction) must fail LOUDLY with an R1 diagnostic, not a
 *     silent -1 colliding with the EDB refusal.
 */

#include "dl.h"
#include "vm.h"   /* vm_agg_eligible, vm_agg_runs — routing proof for F11 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>   /* dup/dup2/read/lseek/close/unlink — stderr capture (F16) */

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %s ... ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { printf("OK\n"); } while(0)
#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

/* ─── fired-event sink ───────────────────────────────────────────────────
 * Records events of BOTH kinds separately, at FULL arity (1..EV_MAX_AR),
 * and is LOUD about anything it cannot represent: an unexpected arity or
 * more events than capacity sets a flag the fixture asserts on.  The old
 * sink silently dropped every non-arity-2 tuple, so a wrong diff on an
 * arity-1/3 head was invisible; this one cannot hide it. */

#define EV_CAP     512
#define EV_MAX_AR  3

typedef struct {
    long     n[2];                      /* events recorded, per kind      */
    uint32_t data[2][EV_CAP * EV_MAX_AR];
    uint8_t  arity[2];                  /* arity as REPORTED (0 = none)   */
    int      overflow;                  /* more events than EV_CAP        */
    int      arity_bad;                 /* arity 0 or > EV_MAX_AR seen    */
    int      bad_event;                 /* event kind outside ADDED/REMOVED */
} evset;

#define EV_ADDED   0
#define EV_REMOVED 1

static int fired_cb(int event, const uint32_t *cols, uint8_t arity, void *user)
{
    evset *es = (evset *)user;
    int k;

    if (event != DL_REACTIVE_ADDED && event != DL_REACTIVE_REMOVED) {
        es->bad_event = 1;
        return 0;
    }
    k = (event == DL_REACTIVE_REMOVED);

    if (arity == 0 || arity > EV_MAX_AR) {
        es->arity_bad = 1;
        return 0;
    }
    if (es->arity[k] == 0) {
        es->arity[k] = arity;
    } else if (es->arity[k] != arity) {
        es->arity_bad = 1; /* one kind must carry ONE arity (one relation) */
    }

    if (es->n[k] >= EV_CAP) {
        es->overflow = 1; /* LOUD: the sink is full, the engine count still counts */
        return 0;
    }
    {
        int c;
        for (c = 0; c < arity; c++)
            es->data[k][(size_t)es->n[k] * EV_MAX_AR + c] = cols[c];
    }
    es->n[k]++;
    return 0;
}

/* Early-stop callback: returns non-zero after `stop_after` events of any
 * kind (F14 — the aborted-step contract). */
static int fired_cb_stop_after(int event, const uint32_t *cols, uint8_t arity, void *user)
{
    long *seen = (long *)user;
    (void)event; (void)cols; (void)arity;
    return (++*seen >= 1) ? 1 : 0;
}

/* F15: abort on the FIRST watched head's second event.  The two heads have
 * disjoint value domains (head A < 100, head B >= 100), so the head is
 * identified by value; A is watched first (lower rel_id), so A's events
 * stream before B's. */
typedef struct {
    long a_seen;   /* events belonging to head A (the first watched) */
    long total;    /* every event delivered before the abort        */
} abort_a2;

static int fired_cb_abort_a2(int event, const uint32_t *cols, uint8_t arity, void *user)
{
    abort_a2 *st = (abort_a2 *)user;
    (void)event; (void)arity;
    st->total++;
    if (cols[0] < 100) {          /* head A's value domain */
        st->a_seen++;
        if (st->a_seen >= 2) return 1;
    }
    return 0;
}

static void ev_reset(evset *es)
{
    memset(es, 0, sizeof(*es));
}

static long ev_n(const evset *es, int kind) { return es->n[kind]; }

static int ev_has1(const evset *es, int kind, uint32_t a)
{
    long i;
    for (i = 0; i < es->n[kind]; i++)
        if (es->data[kind][(size_t)i * EV_MAX_AR] == a)
            return 1;
    return 0;
}

static int ev_has2(const evset *es, int kind, uint32_t a, uint32_t b)
{
    long i;
    for (i = 0; i < es->n[kind]; i++)
        if (es->data[kind][(size_t)i * EV_MAX_AR] == a &&
            es->data[kind][(size_t)i * EV_MAX_AR + 1] == b)
            return 1;
    return 0;
}

static int ev_has3(const evset *es, int kind, uint32_t a, uint32_t b, uint32_t c)
{
    long i;
    for (i = 0; i < es->n[kind]; i++)
        if (es->data[kind][(size_t)i * EV_MAX_AR] == a &&
            es->data[kind][(size_t)i * EV_MAX_AR + 1] == b &&
            es->data[kind][(size_t)i * EV_MAX_AR + 2] == c)
            return 1;
    return 0;
}

static long ev_count2(const evset *es, int kind, uint32_t a, uint32_t b)
{
    long i, n = 0;
    for (i = 0; i < es->n[kind]; i++)
        if (es->data[kind][(size_t)i * EV_MAX_AR] == a &&
            es->data[kind][(size_t)i * EV_MAX_AR + 1] == b)
            n++;
    return n;
}

static long ev_count1(const evset *es, int kind, uint32_t a)
{
    long i, n = 0;
    for (i = 0; i < es->n[kind]; i++)
        if (es->data[kind][(size_t)i * EV_MAX_AR] == a)
            n++;
    return n;
}

/* The sink must be clean: no dropped/bad-arity/overflow residue. */
static int ev_clean(const evset *es)
{
    return !es->overflow && !es->arity_bad && !es->bad_event;
}

/* ─── helpers (mirrors test_ivm.c) ─────────────────────────────────────── */

static void rm_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

static void setup_db(dl_db **db_out, const char *name)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "build-tmp/reactive-%s", name);
    rm_dir(dir);
    *db_out = dl_open(dir);
    assert(*db_out);
}

static void teardown_db(dl_db *db, const char *name)
{
    char dir[256];
    dl_close(db);
    snprintf(dir, sizeof(dir), "build-tmp/reactive-%s", name);
    rm_dir(dir);
}

static void declare_and_load(dl_db *db, const char *name, const char *rel,
                             uint8_t arity, const uint32_t *cols, int nrows)
{
    char path[512];
    int i, c;
    FILE *f;

    assert(dl_declare_relation(db, rel, arity) == 0);
    snprintf(path, sizeof(path), "build-tmp/reactive-%s/%s.csv", name, rel);
    f = fopen(path, "w");
    assert(f);
    for (i = 0; i < nrows; i++) {
        for (c = 0; c < arity; c++) {
            if (c > 0) fputc(',', f);
            fprintf(f, "%u", cols[(size_t)i * (size_t)arity + (size_t)c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    assert(dl_load_facts(db, rel, path) == nrows);
}

static void txn_add_ar(dl_db *db, const char *rel, const uint32_t *cols, uint8_t arity)
{
    assert(dl_txn_begin(db) == 0);
    assert(dl_txn_add_fact(db, rel, cols, arity) == 0);
    assert(dl_txn_commit(db) == 0);
}

static void txn_add(dl_db *db, const char *rel, uint32_t a, uint32_t b)
{
    uint32_t cols[2] = {a, b};
    txn_add_ar(db, rel, cols, 2);
}

static void txn_del(dl_db *db, const char *rel, uint32_t a, uint32_t b)
{
    uint32_t cols[2] = {a, b};
    assert(dl_txn_begin(db) == 0);
    assert(dl_txn_delete_fact(db, rel, cols, 2) == 0);
    assert(dl_txn_commit(db) == 0);
}

/* intern-and-add: symbol columns go through the db's interner. */
static uint32_t symid(dl_db *db, const char *s)
{
    uint32_t id = dl_intern_str(db, s);
    assert(id != 0);
    return id;
}

static void txn_add_symbol(dl_db *db, const char *rel, const char *pair)
{
    char buf[128];
    uint32_t cols[2];
    char *colon;
    /* Caller passes "a,b" — split on the comma. */
    snprintf(buf, sizeof(buf), "%s", pair);
    colon = strchr(buf, ',');
    assert(colon);
    *colon = 0;
    cols[0] = symid(db, buf);
    cols[1] = symid(db, colon + 1);
    txn_add_ar(db, rel, cols, 2);
}

/* Bulk-load raw CSV text (symbol facts) into `rel`. */
static void declare_and_load_symbol(dl_db *db, const char *name, const char *rel,
                                    const char *csv, int nrows)
{
    char path[512];
    FILE *f;
    (void)nrows;

    assert(dl_declare_relation(db, rel, 2) == 0);
    snprintf(path, sizeof(path), "build-tmp/reactive-%s/%s.csv", name, rel);
    f = fopen(path, "w");
    assert(f);
    fputs(csv, f);
    fclose(f);
    assert(dl_load_facts(db, rel, path) == 2);
}

/* Load the canonical TC program in the IVM-ELIGIBLE regime: rules are
 * loaded BEFORE the facts, so the compiled join is SCAN(edge)+LOOKUP(tc)
 * (measured: the default greedy body reorder orders by cardinality at
 * compile time — facts-first compiles edge(card>0) before tc(card 0) and
 * emits OP_HASH_JOIN, which the engine classifies IVM-ineligible; the
 * fired diff is identical in both regimes, but only this one is incremental).
 * Settles the initial fixpoint via dl_consolidate. */
static void setup_tc_ivm(dl_db *db, const char *name)
{
    assert(dl_declare_relation(db, "edge", 2) == 0);
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);
    {
        char path[512];
        FILE *f;
        snprintf(path, sizeof(path), "build-tmp/reactive-%s/edge.csv", name);
        f = fopen(path, "w");
        assert(f);
        fprintf(f, "1,2\n2,3\n3,4\n");
        fclose(f);
        assert(dl_load_facts(db, "edge", path) == 3);
    }
    assert(dl_consolidate(db) == 0); /* settle the baseline fixpoint */
    assert(dl_lookup(db, "tc", (uint32_t[]){1,4}, 2) == 1);
}

/* The SAME program in the FACTS-FIRST regime: the compiled join is
 * OP_HASH_JOIN (IVM-ineligible) — used to verify the FALLBACK flag path. */
static void setup_tc_factsfirst(dl_db *db, const char *name)
{
    uint32_t edges[] = {1,2, 2,3, 3,4};
    declare_and_load(db, name, "edge", 2, edges, 3);
    assert(dl_load_rules(db,
        "tc(X,Y):-edge(X,Y).\n"
        "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);
    assert(dl_consolidate(db) == 0);
    assert(dl_lookup(db, "tc", (uint32_t[]){1,4}, 2) == 1);
}

/* ─── F1: insert firing + quiescent second step ───────────────────────── */

static void test_fired_insert(void)
{
    dl_db *db;
    evset es;

    TEST("F1: insert edge(4,5) fires exactly the 4 new tc rows; then 0");

    setup_db(&db, "f1");
    setup_tc_ivm(db, "f1");

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "tc", 1) == 0);

    /* No deltas yet: the first step is quiescent and reports nothing. */
    ev_reset(&es);
    long n0 = dl_fired_step(db, fired_cb, &es);
    if (n0 != 0 || !ev_clean(&es)) {
        printf("  got n=%ld (clean=%d)\n", n0, ev_clean(&es));
        FAIL("first (quiescent) step should report 0");
        teardown_db(db, "f1");
        return;
    }

    txn_add(db, "edge", 4, 5);
    ev_reset(&es);
    long n1 = dl_fired_step(db, fired_cb, &es);
    /* edge 4->5 extends the chain: tc gains (4,5),(3,5),(2,5),(1,5). */
    if (n1 != 4 || ev_n(&es, EV_ADDED) != 4 || ev_n(&es, EV_REMOVED) != 0 ||
        !ev_has2(&es, EV_ADDED, 4, 5) || !ev_has2(&es, EV_ADDED, 1, 5) ||
        ev_has2(&es, EV_ADDED, 1, 4) || !ev_clean(&es)) {
        printf("  got n=%ld added=%ld removed=%ld, expected exactly the 4 new tc rows\n",
               n1, ev_n(&es, EV_ADDED), ev_n(&es, EV_REMOVED));
        FAIL("insert step fired-set wrong");
        teardown_db(db, "f1");
        return;
    }

    /* Second step: no new deltas — ZERO fired. */
    ev_reset(&es);
    long n2 = dl_fired_step(db, fired_cb, &es);
    if (n2 != 0 || ev_n(&es, EV_ADDED) != 0 || !ev_clean(&es)) {
        printf("  got n=%ld, expected 0\n", n2);
        FAIL("second step must report zero");
        teardown_db(db, "f1");
        return;
    }

    teardown_db(db, "f1");
    PASS();
}

/* ─── F2: delete boundary — REMOVE events, flagged FALLBACK ───────────── */

static void test_fired_delete_boundary(void)
{
    dl_db *db;
    evset es;

    TEST("F2: delete edge(1,2) reports removed tc tuples (R1 boundary)");

    setup_db(&db, "f2");
    setup_tc_factsfirst(db, "f2");

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "tc", 1) == 0);

    txn_del(db, "edge", 1, 2);
    ev_reset(&es);
    long n1 = dl_fired_step(db, fired_cb, &es);

    if (n1 < 0 || ev_n(&es, EV_REMOVED) == 0) {
        printf("  got n=%ld removed=%ld\n", n1, ev_n(&es, EV_REMOVED));
        FAIL("delete step must report the retracted diff");
        teardown_db(db, "f2");
        return;
    }
    if (!(n1 & DL_REACTIVE_FALLBACK)) {
        FAIL("delete vs recursive rules must be flagged FALLBACK, never silent");
        teardown_db(db, "f2");
        return;
    }
    if (ev_n(&es, EV_ADDED) != 0 ||
        !ev_has2(&es, EV_REMOVED, 1, 4) ||
        !ev_has2(&es, EV_REMOVED, 1, 3) ||
        !ev_has2(&es, EV_REMOVED, 1, 2) || !ev_clean(&es)) {
        FAIL("removed set missing the tc tuples that lost edge(1,2) support");
        teardown_db(db, "f2");
        return;
    }
    /* tc(2,3), tc(2,4), tc(3,4) survive the delete — never reported. */
    if (ev_has2(&es, EV_REMOVED, 2, 3) || ev_has2(&es, EV_REMOVED, 2, 4) ||
        ev_has2(&es, EV_REMOVED, 3, 4)) {
        FAIL("removed set contains still-derivable tuples");
        teardown_db(db, "f2");
        return;
    }

    teardown_db(db, "f2");
    PASS();
}

/* ─── F3: fallback flag on a full-re-eval program (OP_WALK) ───────────── */

static void test_fired_fallback_flag(void)
{
    dl_db *db;
    evset es;

    TEST("F3: OP_WALK program reports correct diff, FALLBACK-flagged");

    setup_db(&db, "f3");
    {
        /* Facts must be SYMBOLS for the regex-walk atom to match ('.*' runs
         * the pattern over the interned string; integer facts never match). */
        declare_and_load_symbol(db, "f3", "obs", "alpha,beta\ngamma,delta\n", 2);
        assert(dl_load_rules(db,
            "hit(X,Y):-obs(X,Y) ~ 'a.*'.\n") == 0);
        assert(dl_consolidate(db) == 0);
    }

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "hit", 1) == 0);

    ev_reset(&es);
    long n0 = dl_fired_step(db, fired_cb, &es);
    if (n0 != 0) {
        printf("  quiescent step returned %ld\n", n0);
        FAIL("quiescent fallback step should report 0 (no flag on 0)");
        teardown_db(db, "f3");
        return;
    }

    /* Probe 1: a symbol fact that does NOT match 'a.*' — the full re-eval
     * still happens (it is not incremental), so the step must be
     * FALLBACK-flagged even though the correct diff is EMPTY. */
    txn_add_symbol(db, "obs", "zeta,eta");
    ev_reset(&es);
    long n1 = dl_fired_step(db, fired_cb, &es);

    if (n1 < 0 || ev_n(&es, EV_ADDED) != 0 || !ev_clean(&es)) {
        printf("  got n=%ld added=%ld, expected an empty (correct) diff\n",
               n1, ev_n(&es, EV_ADDED));
        FAIL("fallback step diff wrong");
        teardown_db(db, "f3");
        return;
    }
    if (!(n1 & DL_REACTIVE_FALLBACK)) {
        printf("  got n=%ld (flag bit = %ld)\n", n1,
               (n1 & DL_REACTIVE_FALLBACK) ? 1L : 0L);
        FAIL("OP_WALK program must be flagged FALLBACK, never silent");
        teardown_db(db, "f3");
        return;
    }

    /* Probe 2: a NEW matching fact ('a.*' matches adam) — the flag never
     * corrupts the diff: exactly hit(adam,eve) fires, tagged with the
     * interned symbol ids. */
    {
        uint32_t adam = symid(db, "adam"), eve = symid(db, "eve");
        txn_add_symbol(db, "obs", "adam,eve");
        ev_reset(&es);
        long n2 = dl_fired_step(db, fired_cb, &es);
        if (n2 < 0 || !(n2 & DL_REACTIVE_FALLBACK) || ev_n(&es, EV_ADDED) != 1 ||
            !ev_has2(&es, EV_ADDED, adam, eve) || !ev_clean(&es)) {
            printf("  got n=%ld added=%ld\n", n2, ev_n(&es, EV_ADDED));
            FAIL("matching fallback step must fire exactly the new hit");
            teardown_db(db, "f3");
            return;
        }
    }

    teardown_db(db, "f3");
    PASS();
}

/* ─── F4/F5: API contract (null-isolation surface) ─────────────────────── */

static void test_api_contract(void)
{
    dl_db *db;
    evset es;

    TEST("F4/F5: session contract + no state leak across sessions");

    setup_db(&db, "f45");
    setup_tc_ivm(db, "f45");

    /* Step/clear without init: DL_REACTIVE_ERR_NOT_INIT (-2). */
    if (dl_fired_step(db, fired_cb, &es) != -2 ||
        dl_fired_clear(db) != -2) {
        FAIL("step/clear without init must be DL_REACTIVE_ERR_NOT_INIT");
        teardown_db(db, "f45");
        return;
    }
    /* set_reactive outside a session: refused (-1). */
    if (dl_set_reactive(db, "tc", 1) != -1) {
        FAIL("set_reactive outside a session must be refused");
        teardown_db(db, "f45");
        return;
    }

    /* A watched EDB relation is refused loudly at set time. */
    assert(dl_fired_init(db) == 0);
    if (dl_set_reactive(db, "edge", 1) != -1) {
        FAIL("EDB head must be refused at set time");
        teardown_db(db, "f45");
        return;
    }
    if (dl_set_reactive(db, "nosuch", 1) != -1) {
        FAIL("unknown relation must be refused");
        teardown_db(db, "f45");
        return;
    }
    /* Nested init: DL_REACTIVE_ERR_CONFLICT (-5). */
    if (dl_fired_init(db) != -5) {
        FAIL("nested init must be DL_REACTIVE_ERR_CONFLICT");
        teardown_db(db, "f45");
        return;
    }
    assert(dl_fired_clear(db) == 0);

    /* Session 1: quiescent step fires 0 (the baseline is NOW). */
    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "tc", 1) == 0);
    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0 || ev_n(&es, EV_ADDED) != 0) {
        FAIL("a fresh session must baseline at the current content");
        teardown_db(db, "f45");
        return;
    }
    assert(dl_fired_clear(db) == 0);

    /* After clear, plain engine behaviour is exactly as before. */
    txn_add(db, "edge", 4, 5);
    assert(dl_consolidate(db) == 0);
    if (dl_lookup(db, "tc", (uint32_t[]){1,5}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){4,5}, 2) != 1) {
        FAIL("post-clear consolidation must behave normally");
        teardown_db(db, "f45");
        return;
    }

    /* Session 2 (after that growth): sees ONLY the new tuples. */
    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "tc", 1) == 0);
    txn_add(db, "edge", 5, 6);
    ev_reset(&es);
    long n = dl_fired_step(db, fired_cb, &es);
    if (n != 5 || ev_n(&es, EV_ADDED) != 5) {
        printf("  got n=%ld added=%ld, expected exactly 5 new tc rows\n",
               n, ev_n(&es, EV_ADDED));
        FAIL("second session must see only genuinely-new tuples");
        teardown_db(db, "f45");
        return;
    }
    {
        uint32_t want[5][2] = {{5,6},{4,6},{1,6},{2,6},{3,6}};
        int i;
        for (i = 0; i < 5; i++)
            if (ev_count2(&es, EV_ADDED, want[i][0], want[i][1]) != 1) {
                FAIL("new-tuple set wrong (missing or duplicated row)");
                teardown_db(db, "f45");
                return;
            }
    }
    assert(dl_fired_clear(db) == 0);

    teardown_db(db, "f45");
    PASS();
}

/* ─── F6: multi-cycle stepping ─────────────────────────────────────────── */

static void test_multi_step(void)
{
    dl_db *db;
    evset es;

    TEST("F6: three insert cycles — each step reports its own delta once");

    setup_db(&db, "f6");
    setup_tc_ivm(db, "f6");

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "tc", 1) == 0);

    txn_add(db, "edge", 4, 5);      /* +4 tc rows: (4,5),(1,5),(2,5),(3,5) */
    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 4 || ev_n(&es, EV_ADDED) != 4) {
        FAIL("step 1 should fire exactly the 4 new tc rows");
        teardown_db(db, "f6");
        return;
    }

    txn_add(db, "edge", 5, 6);      /* +5 tc rows */
    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 5 || ev_n(&es, EV_ADDED) != 5) {
        FAIL("step 2 should fire exactly the 5 new tc rows");
        teardown_db(db, "f6");
        return;
    }

    ev_reset(&es);                  /* no deltas: zero, never a repeat */
    if (dl_fired_step(db, fired_cb, &es) != 0 || ev_n(&es, EV_ADDED) != 0) {
        FAIL("step 3 (quiescent) must fire zero");
        teardown_db(db, "f6");
        return;
    }

    teardown_db(db, "f6");
    PASS();
}

/* ─── F7: null-isolation — no-init programmes are untouched ────────────── */

static void test_no_init_untouched(void)
{
    dl_db *db;

    TEST("F7: with NO session, consolidation + queries are unchanged");

    setup_db(&db, "f7");
    setup_tc_ivm(db, "f7");

    /* Interleave plain consolidations and txn growth — the exact pattern
     * the pre-reactive engine served — and verify the fixpoint. */
    txn_add(db, "edge", 4, 5);
    assert(dl_consolidate(db) == 0);
    txn_add(db, "edge", 5, 6);
    assert(dl_consolidate(db) == 0);
    assert(dl_consolidate(db) == 0); /* idempotent when quiescent */

    if (dl_lookup(db, "tc", (uint32_t[]){1,6}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){3,6}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){4,6}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){1,5}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){9,9}, 2) != 0) {
        FAIL("plain path diverged without any session");
        teardown_db(db, "f7");
        return;
    }

    teardown_db(db, "f7");
    PASS();
}

/* ─── F8: arity-1 watched head ─────────────────────────────────────────── */

static void test_arity1_head(void)
{
    dl_db *db;
    evset es;

    TEST("F8: arity-1 head n(X):-a(X) — events carry arity 1");

    setup_db(&db, "f8");
    {
        uint32_t a[] = {10, 20};
        declare_and_load(db, "f8", "a", 1, a, 2);
        assert(dl_load_rules(db, "n(X):-a(X).\n") == 0);
        assert(dl_consolidate(db) == 0);
        assert(dl_lookup(db, "n", (uint32_t[]){10}, 1) == 1);
    }

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "n", 1) == 0);

    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0) {   /* quiescent baseline */
        FAIL("fresh session must baseline quiescently");
        teardown_db(db, "f8");
        return;
    }

    txn_add_ar(db, "a", (uint32_t[]){30}, 1);
    ev_reset(&es);
    long n = dl_fired_step(db, fired_cb, &es);
    if (n != 1 || ev_n(&es, EV_ADDED) != 1 || !ev_has1(&es, EV_ADDED, 30) ||
        es.arity[EV_ADDED] != 1 || !ev_clean(&es)) {
        printf("  got n=%ld added=%ld arity=%u clean=%d\n",
               n, ev_n(&es, EV_ADDED), es.arity[EV_ADDED], ev_clean(&es));
        FAIL("arity-1 head must fire exactly n(30) with REPORTED arity 1");
        teardown_db(db, "f8");
        return;
    }
    /* The old sink silently dropped this event (arity != 2) — n would be
     * the only witness; now the recorded tuple is checked too. */

    teardown_db(db, "f8");
    PASS();
}

/* ─── F9: arity-3 watched head ─────────────────────────────────────────── */

static void test_arity3_head(void)
{
    dl_db *db;
    evset es;

    TEST("F9: arity-3 head t3(X,Y,Z):-e3(X,Y,Z) — full tuple recorded");

    setup_db(&db, "f9");
    {
        uint32_t e3[] = {1,2,3, 4,5,6};
        declare_and_load(db, "f9", "e3", 3, e3, 2);
        assert(dl_load_rules(db, "t3(X,Y,Z):-e3(X,Y,Z).\n") == 0);
        assert(dl_consolidate(db) == 0);
        assert(dl_lookup(db, "t3", (uint32_t[]){1,2,3}, 3) == 1);
    }

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "t3", 1) == 0);

    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0) {
        FAIL("fresh session must baseline quiescently");
        teardown_db(db, "f9");
        return;
    }

    txn_add_ar(db, "e3", (uint32_t[]){7,8,9}, 3);
    ev_reset(&es);
    long n = dl_fired_step(db, fired_cb, &es);
    if (n != 1 || ev_n(&es, EV_ADDED) != 1 ||
        !ev_has3(&es, EV_ADDED, 7, 8, 9) ||
        es.arity[EV_ADDED] != 3 || !ev_clean(&es)) {
        printf("  got n=%ld added=%ld arity=%u clean=%d\n",
               n, ev_n(&es, EV_ADDED), es.arity[EV_ADDED], ev_clean(&es));
        FAIL("arity-3 head must fire exactly t3(7,8,9), all 3 columns");
        teardown_db(db, "f9");
        return;
    }
    /* A truncated/partial tuple (e.g. only cols[0] recorded) fails ev_has3. */

    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0 || !ev_clean(&es)) {
        FAIL("quiescent follow-up must fire zero");
        teardown_db(db, "f9");
        return;
    }

    teardown_db(db, "f9");
    PASS();
}

/* ─── F10: >512-tuple event — the sink is LOUD about truncation ────────── */

static void test_big_event_loud(void)
{
    dl_db *db;
    evset es;
    enum { ROWS = 600 };

    TEST("F10: 600-tuple step — overflow flagged LOUD, engine count exact");

    setup_db(&db, "f10");
    {
        /* Rules FIRST (IVM regime), empty edge, settle. */
        assert(dl_declare_relation(db, "edge", 2) == 0);
        assert(dl_load_rules(db, "s(X,Y):-edge(X,Y).\n") == 0);
        assert(dl_consolidate(db) == 0);
    }

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "s", 1) == 0);
    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0) {
        FAIL("fresh session must baseline quiescently");
        teardown_db(db, "f10");
        return;
    }

    /* ONE txn with ROWS inserts -> ONE step firing ROWS events.  The
     * engine's returned count must be EXACT (600) even though the sink
     * caps at EV_CAP=512 — and the sink must SAY SO (overflow), never
     * silently drop.  The old harness reported a clean 512. */
    assert(dl_txn_begin(db) == 0);
    {
        int i;
        for (i = 1; i <= ROWS; i++) {
            uint32_t cols[2] = {(uint32_t)i, 1};
            assert(dl_txn_add_fact(db, "edge", cols, 2) == 0);
        }
    }
    assert(dl_txn_commit(db) == 0);

    ev_reset(&es);
    long n = dl_fired_step(db, fired_cb, &es);
    if (n != ROWS) {
        printf("  got n=%ld, expected exactly %d fired\n", n, ROWS);
        FAIL("engine count must stay exact past the harness capacity");
        teardown_db(db, "f10");
        return;
    }
    if (!es.overflow || ev_n(&es, EV_ADDED) != EV_CAP) {
        printf("  overflow=%d recorded=%ld (cap=%d)\n",
               es.overflow, ev_n(&es, EV_ADDED), EV_CAP);
        FAIL("sink must record up to capacity AND flag the overflow loudly");
        teardown_db(db, "f10");
        return;
    }
    /* What WAS recorded must be correct (a sample across the range). */
    if (!ev_has2(&es, EV_ADDED, 1, 1) || !ev_has2(&es, EV_ADDED, 512, 1)) {
        FAIL("recorded prefix of the big event is wrong");
        teardown_db(db, "f10");
        return;
    }

    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0 || !ev_clean(&es)) {
        FAIL("quiescent follow-up must fire zero");
        teardown_db(db, "f10");
        return;
    }

    teardown_db(db, "f10");
    PASS();
}

/* ─── F11: agg-eligible incremental branch (vm_agg_maintain) ──────────── */

static void test_agg_incremental(void)
{
    dl_db *db;
    evset es;
    int runs0;

    TEST("F11: aggregate program steps through vm_agg_maintain, unflagged");

    setup_db(&db, "f11");
    assert(dl_declare_relation(db, "sale", 2) == 0);
    assert(dl_load_rules(db,
        "cnt(X,N):-sale(X,Y),N=count().\n"
        "tot(X,S):-sale(X,Y),S=sum(Y).\n") == 0);
    if (vm_agg_eligible(db) != 1) {
        FAIL("grouped cnt/tot program should be agg-IVM-eligible");
        teardown_db(db, "f11");
        return;
    }
    assert(dl_compile(db) == 0);

    /* Seed sale = {(1,10),(1,5),(2,3)} through the incremental path. */
    {
        uint32_t s[][2] = {{1,10},{1,5},{2,3}};
        int i;
        for (i = 0; i < 3; i++)
            assert(dl_add_fact(db, "sale", s[i], 2) == 1);
    }
    assert(dl_consolidate(db) == 0);
    if (dl_lookup(db, "cnt", (uint32_t[]){1,2}, 2) != 1 ||
        dl_lookup(db, "tot", (uint32_t[]){1,15}, 2) != 1 ||
        dl_lookup(db, "cnt", (uint32_t[]){2,1}, 2) != 1) {
        FAIL("aggregate seed derivation wrong");
        teardown_db(db, "f11");
        return;
    }

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "cnt", 1) == 0);
    assert(dl_set_reactive(db, "tot", 1) == 0);
    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0) {
        FAIL("fresh session must baseline quiescently");
        teardown_db(db, "f11");
        return;
    }

    /* add sale(2,9): group 2's count 1->2 and total 3->12.  The step must
     * route through vm_agg_maintain (runs+1, routing PROOF — a full
     * re-eval fallback would leave vm_agg_runs untouched) and report the
     * superseded rows as REMOVED and the new ones as ADDED, UNFLAGGED. */
    runs0 = vm_agg_runs;
    txn_add(db, "sale", 2, 9);
    ev_reset(&es);
    long n = dl_fired_step(db, fired_cb, &es);
    if (vm_agg_runs != runs0 + 1) {
        printf("  vm_agg_runs %d -> %d\n", runs0, vm_agg_runs);
        FAIL("step did not route through the aggregate incremental branch");
        teardown_db(db, "f11");
        return;
    }
    if (n < 0 || (n & DL_REACTIVE_FALLBACK)) {
        printf("  got n=%ld flag=%ld\n", n,
               (n & DL_REACTIVE_FALLBACK) ? 1L : 0L);
        FAIL("agg-eligible step must be incremental (never FALLBACK-flagged)");
        teardown_db(db, "f11");
        return;
    }
    if (!ev_has2(&es, EV_ADDED, 2, 2) || !ev_has2(&es, EV_ADDED, 2, 12) ||
        !ev_has2(&es, EV_REMOVED, 2, 1) || !ev_has2(&es, EV_REMOVED, 2, 3) ||
        ev_n(&es, EV_ADDED) != 2 || ev_n(&es, EV_REMOVED) != 2 ||
        !ev_clean(&es)) {
        printf("  added=%ld removed=%ld\n",
               ev_n(&es, EV_ADDED), ev_n(&es, EV_REMOVED));
        FAIL("agg step must report cnt/tot supersede+new exactly");
        teardown_db(db, "f11");
        return;
    }

    /* The views themselves are the recomputed truth. */
    if (dl_lookup(db, "cnt", (uint32_t[]){2,2}, 2) != 1 ||
        dl_lookup(db, "tot", (uint32_t[]){2,12}, 2) != 1 ||
        dl_lookup(db, "cnt", (uint32_t[]){2,1}, 2) != 0) {
        FAIL("post-step aggregate views wrong");
        teardown_db(db, "f11");
        return;
    }

    teardown_db(db, "f11");
    PASS();
}

/* ─── F12: quiescent-after-fallback invariant + plain-consolidate drift ── */

static void test_quiescent_after_fallback(void)
{
    dl_db *db;
    evset es;

    TEST("F12: after ONE fallback step, quiescent steps are 0/no-flag");

    setup_db(&db, "f12");
    setup_tc_factsfirst(db, "f12"); /* OP_HASH_JOIN regime: steps fall back */

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "tc", 1) == 0);

    /* Quiescent before any delta: 0, no flag. */
    ev_reset(&es);
    long q0 = dl_fired_step(db, fired_cb, &es);
    if (q0 != 0 || (q0 & DL_REACTIVE_FALLBACK)) {
        printf("  quiescent-first r=%ld\n", q0);
        FAIL("quiescent step before any delta must be 0 with NO flag");
        teardown_db(db, "f12");
        return;
    }

    /* One delta step: correct diff, flagged (the regime is non-incremental). */
    txn_add(db, "edge", 4, 5);
    ev_reset(&es);
    long s1 = dl_fired_step(db, fired_cb, &es);
    if ((s1 & ~DL_REACTIVE_FALLBACK) != 4 || !(s1 & DL_REACTIVE_FALLBACK) ||
        ev_n(&es, EV_ADDED) != 4) {
        printf("  delta-step r=%ld added=%ld\n", s1, ev_n(&es, EV_ADDED));
        FAIL("facts-first delta step must fire 4 rows FALLBACK-flagged");
        teardown_db(db, "f12");
        return;
    }

    /* THE INVARIANT (review finding 3): the step's cascade must leave the
     * dispatch state as settled as dl_consolidate's — every LATER quiescent
     * step reports 0 with NO flag, instead of re-running vm_execute and
     * re-flagging forever. */
    ev_reset(&es);
    long q1 = dl_fired_step(db, fired_cb, &es);
    if (q1 != 0 || (q1 & DL_REACTIVE_FALLBACK) || ev_n(&es, EV_ADDED) != 0) {
        printf("  quiescent-after r=%ld flag=%ld added=%ld\n",
               q1, (q1 & DL_REACTIVE_FALLBACK) ? 1L : 0L,
               ev_n(&es, EV_ADDED));
        FAIL("quiescent step after a fallback must be 0 with NO flag");
        teardown_db(db, "f12");
        return;
    }
    ev_reset(&es);
    long q2 = dl_fired_step(db, fired_cb, &es);
    if (q2 != 0 || (q2 & DL_REACTIVE_FALLBACK)) {
        FAIL("second quiescent step must also be 0 with NO flag");
        teardown_db(db, "f12");
        return;
    }

    /* Plain dl_consolidate interleaved between reactive steps: no drift.
     * The consolidate derives the 5 new tc rows itself, so the next step's
     * DISPATCH is quiescent (0 re-eval, NO flag) while its DIFF still
     * reports exactly those 5 rows once — the diff boundary is the previous
     * STEP, so nothing derived in between is lost or double-reported. */
    txn_add(db, "edge", 5, 6);
    if (dl_consolidate(db) != 0) {
        FAIL("plain consolidate after reactive steps must succeed");
        teardown_db(db, "f12");
        return;
    }
    ev_reset(&es);
    long s2 = dl_fired_step(db, fired_cb, &es);
    if ((s2 & ~DL_REACTIVE_FALLBACK) != 5 || (s2 & DL_REACTIVE_FALLBACK) ||
        ev_n(&es, EV_ADDED) != 5 || !ev_has2(&es, EV_ADDED, 5, 6)) {
        printf("  post-consolidate step r=%ld added=%ld\n", s2, ev_n(&es, EV_ADDED));
        FAIL("step after plain consolidate: 5 rows once, never flagged");
        teardown_db(db, "f12");
        return;
    }
    ev_reset(&es);
    long q3 = dl_fired_step(db, fired_cb, &es);
    if (q3 != 0 || (q3 & DL_REACTIVE_FALLBACK)) {
        FAIL("step after the post-consolidate step must be quiescent");
        teardown_db(db, "f12");
        return;
    }
    if (dl_lookup(db, "tc", (uint32_t[]){1,6}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){4,6}, 2) != 1) {
        FAIL("plain consolidate did not complete the fixpoint");
        teardown_db(db, "f12");
        return;
    }

    teardown_db(db, "f12");
    PASS();
}

/* ─── F13: multi-db sessions (the tombstone regression) ───────────────── */

static void test_multi_db_sessions(void)
{
    enum { N = 8 };
    dl_db *db[N];
    char name[32];
    int i, bad = 0;

    TEST("F13: clearing one db's session never breaks another db's");

    for (i = 0; i < N; i++) {
        snprintf(name, sizeof(name), "f13-%d", i);
        setup_db(&db[i], name);
        assert(dl_declare_relation(db[i], "edge", 2) == 0);
        assert(dl_load_rules(db[i],
            "tc(X,Y):-edge(X,Y).\n"
            "tc(X,Z):-edge(X,Y),tc(Y,Z).\n") == 0);
        {
            char path[512];
            FILE *f;
            snprintf(path, sizeof(path),
                     "build-tmp/reactive-f13-%d/edge.csv", i);
            f = fopen(path, "w");
            assert(f);
            fputs("1,2\n", f);
            fclose(f);
            assert(dl_load_facts(db[i], "edge", path) == 1);
        }
        assert(dl_consolidate(db[i]) == 0);
        assert(dl_fired_init(db[i]) == 0);
        assert(dl_set_reactive(db[i], "tc", 1) == 0);
    }

    /* Clear the FIRST half — the exact operation that used to punch holes
     * in the (tombstone-less) open-addressing probe runs. */
    for (i = 0; i < N / 2; i++) {
        if (dl_fired_clear(db[i]) != 0) {
            printf("  clear %d FAILED\n", i);
            bad++;
        }
    }

    /* Every survivor's session must still be live and correct. */
    for (i = N / 2; i < N; i++) {
        evset es;
        long n;
        txn_add(db[i], "edge", 2, 3);   /* tc gains (2,3),(1,3) */
        ev_reset(&es);
        n = dl_fired_step(db[i], fired_cb, &es);
        if (n != 2 || !ev_has2(&es, EV_ADDED, 2, 3) ||
            !ev_has2(&es, EV_ADDED, 1, 3) || !ev_clean(&es)) {
            printf("  survivor %d: step r=%ld added=%ld\n",
                   i, n, ev_n(&es, EV_ADDED));
            bad++;
        }
        if (dl_fired_clear(db[i]) != 0) {
            printf("  survivor clear %d FAILED\n", i);
            bad++;
        }
    }

    for (i = 0; i < N; i++) {
        snprintf(name, sizeof(name), "f13-%d", i);
        teardown_db(db[i], name);
    }

    if (bad) {
        FAIL("multi-db session table corrupted (see above)");
        return;
    }
    PASS();
}

/* ─── F14: early-stop advances the baseline exactly once ──────────────── */

static void test_early_stop_baseline(void)
{
    dl_db *db;
    evset es;
    long seen;

    TEST("F14: aborted step advances the baseline once; no re-fire, no gap");

    setup_db(&db, "f14");
    setup_tc_ivm(db, "f14");

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "tc", 1) == 0);

    txn_add(db, "edge", 4, 5);      /* would fire 4 rows */
    seen = 0;
    long n1 = dl_fired_step(db, fired_cb_stop_after, &seen);
    if (n1 != 1 || seen != 1) {
        printf("  aborted step r=%ld seen=%ld, expected exactly 1 delivered\n",
               n1, seen);
        FAIL("early-stop must return only the delivered count");
        teardown_db(db, "f14");
        return;
    }

    /* The baseline advanced to the POST-STEP content: a quiescent follow-up
     * reports ZERO (the undelivered 3 rows are never re-fired), and the
     * NEXT delta still reports exactly its own 5 rows. */
    ev_reset(&es);
    long n2 = dl_fired_step(db, fired_cb, &es);
    if (n2 != 0 || ev_n(&es, EV_ADDED) != 0 || ev_n(&es, EV_REMOVED) != 0) {
        printf("  post-abort step r=%ld added=%ld removed=%ld\n",
               n2, ev_n(&es, EV_ADDED), ev_n(&es, EV_REMOVED));
        FAIL("baseline after an aborted step must be the post-step content");
        teardown_db(db, "f14");
        return;
    }

    txn_add(db, "edge", 5, 6);      /* +5 tc rows */
    ev_reset(&es);
    long n3 = dl_fired_step(db, fired_cb, &es);
    if (n3 != 5 || ev_n(&es, EV_ADDED) != 5 ||
        !ev_has2(&es, EV_ADDED, 5, 6) || !ev_has2(&es, EV_ADDED, 1, 6)) {
        printf("  next delta r=%ld added=%ld\n", n3, ev_n(&es, EV_ADDED));
        FAIL("the step after an aborted step must diff against the advanced baseline");
        teardown_db(db, "f14");
        return;
    }

    /* Several aborted steps in a row (the leak shape from the review):
     * each advances the baseline; none re-fires; the views stay exact. */
    {
        int i;
        for (i = 0; i < 3; i++) {
            uint32_t e[2] = {(uint32_t)(10 + i), (uint32_t)(20 + i)};
            txn_add_ar(db, "edge", e, 2);
            seen = 0;
            long r = dl_fired_step(db, fired_cb_stop_after, &seen);
            if (r < 0 || seen != 1) {
                printf("  aborted loop %d r=%ld seen=%ld\n", i, r, seen);
                FAIL("repeated aborted steps must keep working");
                teardown_db(db, "f14");
                return;
            }
        }
    }
    ev_reset(&es);
    long n4 = dl_fired_step(db, fired_cb, &es);
    if (n4 != 0 || ev_n(&es, EV_ADDED) != 0) {
        FAIL("after repeated aborts a quiescent step must report 0");
        teardown_db(db, "f14");
        return;
    }
    if (dl_lookup(db, "tc", (uint32_t[]){10,20}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){11,21}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){12,22}, 2) != 1 ||
        dl_lookup(db, "tc", (uint32_t[]){1,6}, 2) != 1) {
        FAIL("views diverged across aborted steps");
        teardown_db(db, "f14");
        return;
    }

    teardown_db(db, "f14");
    PASS();
}

/* ─── F15: two-head early-abort boundary (multi-watcher contract) ──────── */

static void test_multi_watch_abort(void)
{
    dl_db *db;
    evset es;
    abort_a2 ab;

    TEST("F15: abort on head A's 2nd event; head B reports its delta once");

    setup_db(&db, "f15");
    /* Declare the two EDBs first, so the rule heads a,b get rel_ids after
     * them IN RULE ORDER — head A is watched (and streamed) FIRST. */
    assert(dl_declare_relation(db, "ea", 1) == 0);
    assert(dl_declare_relation(db, "eb", 1) == 0);
    assert(dl_load_rules(db,
        "a(X):-ea(X).\n"
        "b(X):-eb(X).\n") == 0);

    /* Seed the baseline: ea={11,12}, eb={121,122}. */
    assert(dl_add_fact(db, "ea", (uint32_t[]){11}, 1) == 1);
    assert(dl_add_fact(db, "ea", (uint32_t[]){12}, 1) == 1);
    assert(dl_add_fact(db, "eb", (uint32_t[]){121}, 1) == 1);
    assert(dl_add_fact(db, "eb", (uint32_t[]){122}, 1) == 1);
    assert(dl_consolidate(db) == 0);
    assert(dl_lookup(db, "a", (uint32_t[]){11}, 1) == 1);
    assert(dl_lookup(db, "b", (uint32_t[]){121}, 1) == 1);

    assert(dl_fired_init(db) == 0);
    assert(dl_set_reactive(db, "a", 1) == 0);
    assert(dl_set_reactive(db, "b", 1) == 0);

    /* Fresh session baselines at the current content (nothing to report). */
    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0 || !ev_clean(&es)) {
        FAIL("fresh session must baseline quiescently");
        teardown_db(db, "f15");
        return;
    }

    /* ONE step derives a += {13,14,15} AND b += {123,124}.  The aborting
     * callback stops on head A's SECOND event, so A's baseline advances but
     * B's does not (B's events are never reached). */
    assert(dl_add_fact(db, "ea", (uint32_t[]){13}, 1) == 1);
    assert(dl_add_fact(db, "ea", (uint32_t[]){14}, 1) == 1);
    assert(dl_add_fact(db, "ea", (uint32_t[]){15}, 1) == 1);
    assert(dl_add_fact(db, "eb", (uint32_t[]){123}, 1) == 1);
    assert(dl_add_fact(db, "eb", (uint32_t[]){124}, 1) == 1);
    ab.a_seen = 0;
    ab.total = 0;
    long n1 = dl_fired_step(db, fired_cb_abort_a2, &ab);
    if (n1 != 2 || ab.a_seen != 2 || ab.total != 2) {
        printf("  aborted step r=%ld a_seen=%ld total=%ld, expected 2 delivered (A's first two events)\n",
               n1, ab.a_seen, ab.total);
        FAIL("abort must stop after head A's second event (2 delivered)");
        teardown_db(db, "f15");
        return;
    }

    /* B's baseline is STILL pre-step: the next (quiescent) step reports B's
     * own delta {123,124} EXACTLY once each — and A never re-reports. */
    ev_reset(&es);
    long n2 = dl_fired_step(db, fired_cb, &es);
    if (n2 != 2 || ev_n(&es, EV_ADDED) != 2 || ev_n(&es, EV_REMOVED) != 0 ||
        ev_count1(&es, EV_ADDED, 123) != 1 || ev_count1(&es, EV_ADDED, 124) != 1 ||
        es.arity[EV_ADDED] != 1 || !ev_clean(&es)) {
        printf("  post-abort step r=%ld added=%ld removed=%ld clean=%d\n",
               n2, ev_n(&es, EV_ADDED), ev_n(&es, EV_REMOVED), ev_clean(&es));
        FAIL("head B must report its own delta exactly once");
        teardown_db(db, "f15");
        return;
    }
    /* Head A's aborted-step tuples are never re-reported. */
    if (ev_has1(&es, EV_ADDED, 13) || ev_has1(&es, EV_ADDED, 14) ||
        ev_has1(&es, EV_ADDED, 15)) {
        FAIL("head A must not re-report the tuples of its aborted step");
        teardown_db(db, "f15");
        return;
    }

    /* B's baseline has now advanced too: a further quiescent step is 0. */
    ev_reset(&es);
    if (dl_fired_step(db, fired_cb, &es) != 0 || !ev_clean(&es)) {
        FAIL("step after B's catch-up must be quiescent");
        teardown_db(db, "f15");
        return;
    }

    /* Both views hold the full derived content. */
    if (dl_lookup(db, "a", (uint32_t[]){13}, 1) != 1 ||
        dl_lookup(db, "a", (uint32_t[]){14}, 1) != 1 ||
        dl_lookup(db, "a", (uint32_t[]){15}, 1) != 1 ||
        dl_lookup(db, "b", (uint32_t[]){123}, 1) != 1 ||
        dl_lookup(db, "b", (uint32_t[]){124}, 1) != 1) {
        FAIL("views must hold the full derived content across the aborted step");
        teardown_db(db, "f15");
        return;
    }

    teardown_db(db, "f15");
    PASS();
}

/* ─── F16: arming a variadic rule head is refused LOUDLY ───────────────── */

/* Run dl_set_reactive with stderr redirected to a temp file and return both
 * the result and the captured diagnostic text — the "loud" contract (a
 * diagnostic IS emitted, not a silent -1). */
static int set_reactive_capture(dl_db *db, const char *rel, int on,
                                char *out, size_t outsz)
{
    char tmpl[] = "build-tmp/reactive-stderr.XXXXXX";
    int fd, saved;
    int rc;
    ssize_t n;

    fd = mkstemp(tmpl);
    assert(fd >= 0);
    saved = dup(2);
    assert(saved >= 0);
    fflush(stderr);
    assert(dup2(fd, 2) >= 0);
    rc = dl_set_reactive(db, rel, on);
    fflush(stderr);
    assert(dup2(saved, 2) >= 0);
    close(saved);
    assert(lseek(fd, 0, SEEK_SET) >= 0);
    n = read(fd, out, (ssize_t)(outsz - 1));
    assert(n >= 0);
    out[n] = 0;
    close(fd);
    unlink(tmpl);
    return rc;
}

static void test_variadic_head_refused(void)
{
    dl_db *db;
    char err[512];

    TEST("F16: arming a variadic rule head fails with a diagnostic");

    setup_db(&db, "f16");
    /* A variadic relation (arity 0) that is ALSO a rule head: the compiler
     * materializes a fixed variant for the head, but the rel_entry stays
     * RELK_VARIADIC with rel == null — arming it must be refused loudly
     * (R1 does not support variadic heads), not silently return -1. */
    assert(dl_declare_relation(db, "e", 1) == 0);
    assert(dl_declare_relation_variadic(db, "vh") == 0);
    assert(dl_load_rules(db, "vh(X):-e(X).\n") == 0);
    assert(dl_consolidate(db) == 0);

    assert(dl_fired_init(db) == 0);
    err[0] = 0;
    int rc = set_reactive_capture(db, "vh", 1, err, sizeof(err));
    if (rc != -1) {
        printf("  rc=%d, expected -1\n", rc);
        FAIL("arming a variadic head must be refused");
        teardown_db(db, "f16");
        return;
    }
    if (!strstr(err, "variadic") || !strstr(err, "R1")) {
        printf("  stderr: '%s'\n", err);
        FAIL("variadic refusal must be LOUD (stderr diagnostic naming R1)");
        teardown_db(db, "f16");
        return;
    }
    teardown_db(db, "f16");
    PASS();
}

int main(void)
{
    printf("REACTIVE (Capability 2, slice R1):\n");

    test_fired_insert();
    test_fired_delete_boundary();
    test_fired_fallback_flag();
    test_api_contract();
    test_multi_step();
    test_no_init_untouched();
    test_arity1_head();
    test_arity3_head();
    test_big_event_loud();
    test_agg_incremental();
    test_quiescent_after_fallback();
    test_multi_db_sessions();
    test_early_stop_baseline();
    test_multi_watch_abort();
    test_variadic_head_refused();

    printf("REACTIVE: %d/%d passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
