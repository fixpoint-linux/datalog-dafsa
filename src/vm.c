/*
 * vm.c — Bytecode VM interpreter with in-memory fixpoint (M2 perf fix)
 *
 * Architecture (advisor Path B):
 *   During the semi-naive fixpoint, IDB relations and deltas live in
 *   memory as tuple_sets (hash + sorted array).  Each IDB DAFSA is
 *   bulk-built ONCE at stratum end from sorted data — the fast path.
 *   EDB body atoms stay in the DAFSA (read-only during fixpoint).
 *
 *   This avoids the pathological DAFSA clone-on-write churn that
 *   previously caused an ~80s cliff at N=500 chain TC (now < 0.2s).
 *
 * Structure:
 *   exec_rule()         — bytecode interpreter (opcodes: SCAN, LOOKUP,
 *                         EQ, EQ_CONST, PROJECT, NEG_CHECK, HALT, AGG_ACC,
 *                         AGG_EMIT, WALK, LOOKUP_PERM, HASH_JOIN,
 *                         CMP, ARITH, STR_FILTER, STR_LEN, STR_BIND)
 *   eval_nonrecursive() — M1 path: one exec_rule per rule, commits to DAFSA
 *   eval_stratum_recursive() — semi-naive fixpoint using tuple_sets
 *   vm_execute()        — stratify + dispatch
 *   vm_query()          — thin wrapper: execute then stream goal via rel_prefix
 */
#include "vm.h"
#include "dl_internal.h"
#include "relation.h"
#include "regexwalk.h"
#include "tupleset.h"
#include "intern.h"
#include "permindex.h"
#include "snapshot.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── dl_db internal access (authoritative layout in dl_internal.h) ──── */

/* Magic-sets skip-materialize hook.  When a caller (dl_query_magic_adorn)
 * sets vm_nomaterialize=1, the recursive stratum skips the DAFSA bulk-build
 * of the GOAL relation (section 6) and instead hands its idb tuple_set to the
 * caller via vm_export_ts, since the eval clone is torn down immediately after
 * streaming and that DAFSA would never be read again.  ALL OTHER relations are
 * still materialized so higher strata keep seeing correct data.  The globals
 * default to 0/-1/NULL so every non-magic path (dl_compile/dl_query/dl_publish)
 * is byte-identical to before. */
int vm_nomaterialize = 0;
int vm_export_relid = -1;
tuple_set *vm_export_ts = NULL;

/* IVM Slice 3: test observable — counts vm_dred_delete runs (see vm.h). */
int vm_dred_runs = 0;

/* IVM Slice 4: test observable — counts vm_agg_maintain runs (see vm.h). */
int vm_agg_runs = 0;

/* IVM Slice 5: test observable — counts vm_propagate_deltas runs (see vm.h).
 * Lets the bulk-load tests prove the batched-delta path was taken (a silently
 * fell-back full re-eval would also produce correct views). */
int vm_propagate_runs = 0;

static relation *db_rel(dl_db *db, int idx)
{
    if (idx < 0 || (size_t)idx >= db->nrels) return NULL;
    return db->rels[idx].rel;
}

static int db_find(dl_db *db, const char *name)
{
    size_t i;
    for (i = 0; i < db->nrels; i++)
        if (!strcmp(db->rels[i].name, name)) return (int)i;
    return -1;
}

/* ─── Tuple buffer (for frame materialization) ────────────────────────── */

typedef struct {
    uint32_t *data;
    long      count, cap;
    uint8_t   arity;
} tuple_buf;

static int tbuf_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    tuple_buf *tb = (tuple_buf *)user;
    if (tb->arity == 0) tb->arity = arity;
    if (tb->count >= tb->cap) {
        long nc = tb->cap ? tb->cap * 2 : 1024;
        uint32_t *nd = realloc(tb->data,
            (size_t)nc * (size_t)tb->arity * sizeof(uint32_t));
        if (!nd) return 1;
        tb->data = nd;
        tb->cap = nc;
    }
    memcpy(tb->data + (size_t)tb->count * tb->arity, cols,
           (size_t)tb->arity * sizeof(uint32_t));
    tb->count++;
    return 0;
}

static void tbuf_free(tuple_buf *tb)
{
    free(tb->data);
    memset(tb, 0, sizeof(*tb));
}

/* ─── Bindings ────────────────────────────────────────────────────────── */

#define UNBOUND 0xFFFFFFFFUL

typedef struct {
    uint32_t vals[MAX_VARS];
    uint8_t  valid[MAX_VARS];
} bindings;

static void b_init(bindings *b)
{
    memset(b->valid, 0, sizeof(b->valid));
}

static int b_try(bindings *b, uint8_t s, uint32_t v)
{
    if (s >= MAX_VARS) return 0;
    if (b->valid[s]) return b->vals[s] == v;
    b->vals[s] = v;
    b->valid[s] = 1;
    return 1;
}

static uint32_t b_get(const bindings *b, uint8_t s)
{
    return (s < MAX_VARS && b->valid[s]) ? b->vals[s] : UNBOUND;
}

static int b_ok(const bindings *b, uint8_t s)
{
    return s < MAX_VARS && b->valid[s];
}

static void b_save(bindings *dst, const bindings *src)
{
    memcpy(dst->vals, src->vals, sizeof(dst->vals));
    memcpy(dst->valid, src->valid, sizeof(dst->valid));
}

#define b_load b_save

/* ─── Aggregate accumulator (M3) ─────────────────────────────────────── */
/* Gather-all-then-reduce: OP_AGG_ACC accumulates each body binding into an
 * open-addressing hash map keyed by the group columns (FNV-1a, linear
 * probe, resize+rehash — mirrors tupleset.c's hash conventions). */

typedef struct {
    uint32_t *key;      /* group key (n_key u32); NULL when slot empty */
    uint32_t  count;    /* number of tuples in the group */
    uint32_t  sum;      /* wrap-around u32 sum */
    uint32_t  min;      /* min of source */
    uint32_t  max;      /* max of source */
    int       valid;    /* 1 once at least one tuple accumulated */
} agg_bucket;

typedef struct {
    agg_bucket *buckets;
    int         n_buckets;   /* occupied slots */
    int         cap;         /* power of two */
    uint8_t     n_key;       /* number of group key columns */
    uint8_t     op;          /* 0=count 1=sum 2=min 3=max */
    uint8_t     src_slot;    /* source var slot (0xFF for count) */
    uint8_t     res_slot;    /* aggregate result var slot */
    uint8_t     group_slots[8]; /* group-by var slots (length n_key) */
    uint8_t     head_rel_id;
    uint8_t     head_arity;
    uint8_t     head_slots[8];
    int         dry;
    dl_tuple_cb cb;
    void       *user;
} agg_accum;

#define AGG_FNV_OFFSET 14695981039346656037ULL
#define AGG_FNV_PRIME  1099511628211ULL

static uint64_t agg_hash(const uint32_t *key, int n)
{
    uint64_t h = AGG_FNV_OFFSET;
    int i;
    for (i = 0; i < n; i++) {
        uint32_t v = key[i];
        h ^= (v & 0xFF);         h *= AGG_FNV_PRIME;
        h ^= ((v >> 8) & 0xFF);  h *= AGG_FNV_PRIME;
        h ^= ((v >> 16) & 0xFF); h *= AGG_FNV_PRIME;
        h ^= ((v >> 24) & 0xFF); h *= AGG_FNV_PRIME;
    }
    return h;
}

static int agg_init(agg_accum *ac)
{
    memset(ac, 0, sizeof(*ac));
    ac->cap = 16;
    ac->buckets = calloc((size_t)ac->cap, sizeof(agg_bucket));
    return ac->buckets ? 0 : -1;
}

static int agg_grow(agg_accum *ac)
{
    int new_cap = ac->cap * 2;
    agg_bucket *nb = calloc((size_t)new_cap, sizeof(agg_bucket));
    int i;
    if (!nb) return -1;
    for (i = 0; i < ac->cap; i++) {
        agg_bucket *ob = &ac->buckets[i];
        if (ob->key == NULL) continue;
        size_t idx = (size_t)(agg_hash(ob->key, ac->n_key) &
                              (uint64_t)((size_t)new_cap - 1));
        while (nb[idx].key != NULL)
            idx = (idx + 1) & (size_t)(new_cap - 1);
        nb[idx] = *ob;
    }
    free(ac->buckets);
    ac->buckets = nb;
    ac->cap = new_cap;
    return 0;
}

static agg_bucket *agg_find_or_create(agg_accum *ac, const uint32_t *key)
{
    /* Grow when the load factor exceeds ~0.75. */
    if (ac->n_buckets + 1 > ac->cap - (ac->cap >> 2)) {
        if (agg_grow(ac) != 0) return NULL;
    }
    size_t idx = (size_t)(agg_hash(key, ac->n_key) &
                          (uint64_t)((size_t)ac->cap - 1));
    while (ac->buckets[idx].key != NULL) {
        if (memcmp(ac->buckets[idx].key, key,
                   (size_t)ac->n_key * sizeof(uint32_t)) == 0)
            return &ac->buckets[idx];
        idx = (idx + 1) & (size_t)(ac->cap - 1);
    }
    agg_bucket *b = &ac->buckets[idx];
    {
        size_t ksz = (size_t)ac->n_key * sizeof(uint32_t);
        if (ksz < 1) ksz = 1;
        b->key = malloc(ksz);
    }
    if (!b->key) return NULL;
    if (ac->n_key > 0)
        memcpy(b->key, key, (size_t)ac->n_key * sizeof(uint32_t));
    b->count = 0; b->sum = 0; b->min = 0; b->max = 0; b->valid = 0;
    ac->n_buckets++;
    return b;
}

static void agg_free(agg_accum *ac)
{
    int i;
    if (!ac->buckets) return;
    for (i = 0; i < ac->cap; i++)
        if (ac->buckets[i].key) free(ac->buckets[i].key);
    free(ac->buckets);
    memset(ac, 0, sizeof(*ac));
}

/* ─── Frame ───────────────────────────────────────────────────────────── */

#define MAX_FRAMES 8

typedef struct {
    int       ip;       /* instruction that created this frame */
    int       op;       /* OP_SCAN or OP_LOOKUP (or OP_LOOKUP_PERM) */
    tuple_buf tuples;   /* materialized tuples for this frame */
    long      idx;      /* current position in tuples */
    bindings  saved;    /* bindings snapshot at frame entry */
    const uint8_t *perm; /* M6: permutation array for OP_LOOKUP_PERM, NULL otherwise */
} vm_frame;

/* ─── Override ────────────────────────────────────────────────────────── */

typedef struct {
    int              body_idx;  /* which body atom to override */
    const tuple_set *ts;        /* tuple_set to enumerate from (NULL = DAFSA) */
    int              perm_id;   /* M6: perm_id for perm shadow, -1 if none */
} vm_override;

static const tuple_set *find_ov(int body_idx, const vm_override *ov, int n_ov)
{
    int i;
    for (i = 0; i < n_ov; i++)
        if (ov[i].body_idx == body_idx) return ov[i].ts;
    return NULL;
}

/* ─── Row binding ─────────────────────────────────────────────────────── */

/*
 * INVARIANT: vm_instr.slots[c] ALWAYS means "slot for ORIGINAL column c",
 * uniform across every opcode (SCAN, LOOKUP, LOOKUP_PERM, HASH_JOIN).
 * The permutation array is fetched from dl_db at dispatch time; the VM
 * uses it to translate between walker row positions and original columns.
 * Never index slots by perm position directly.
 *
 * For OP_LOOKUP_PERM: the permuted DAFSA stores rows in order
 *   [c_{perm[0]}, c_{perm[1]}, ..., c_{perm[arity-1]}]
 * A row position pp corresponds to original column perm[pp].
 * Binding: slots[perm[pp]] gets row[pp].
 */

/* Perm-aware bind: row positions [sc..ar) bind to slots[perm[sc..ar)].
 * For non-perm paths (perm==NULL), perm[p]==p and this degenerates
 * to standard bind_row. */
static int bind_row_perm(const vm_instr *in, const uint32_t *row,
                         int sc, int ar, const uint8_t *perm, bindings *b)
{
    int pp;
    for (pp = sc; pp < ar; pp++) {
        int oc = perm ? (int)perm[pp] : pp;
        uint8_t s = in->slots[oc];
        if (s == 0xFF) continue;
        if (!b_try(b, s, row[pp])) return 0;
    }
    return 1;
}

/* Perm-aware seek: like seek_valid but uses perm-aware bind. */
static int seek_valid_perm(vm_frame *f, const vm_instr *in, int sc, int ar,
                           const uint8_t *perm, bindings *b)
{
    while (f->idx < f->tuples.count) {
        b_load(b, &f->saved);
        if (bind_row_perm(in, f->tuples.data + f->idx * f->tuples.arity,
                          sc, ar, perm, b))
            return 1;
        f->idx++;
    }
    return 0;
}

/* Non-perm seek: delegates to perm-aware with perm=NULL */
static int seek_valid(vm_frame *f, const vm_instr *in, int sc, int ar,
                      bindings *b)
{
    return seek_valid_perm(f, in, sc, ar, NULL, b);
}

/* Backtrack: advance the current frame, then seek a valid tuple.
 * Pops frames that are exhausted.  Returns 1 if a valid tuple was found
 * (sets *ip to the frame's ip+1), 0 if all frames exhausted. */
static int backtrack(vm_frame *frames, int *sp, bindings *b,
                     const vm_instr *p, int *ip)
{
    while (*sp > 0) {
        vm_frame *f = &frames[*sp - 1];
        f->idx++;
        {
            const vm_instr *fi = &p[f->ip];
            int sc = (f->op == OP_LOOKUP || f->op == OP_LOOKUP_PERM) ? (int)fi->b : 0;
            int found;
            if (f->perm) {
                found = seek_valid_perm(f, fi, sc, (int)f->tuples.arity,
                                        f->perm, b);
            } else {
                found = seek_valid(f, fi, sc, (int)f->tuples.arity, b);
            }
            if (found) {
                *ip = f->ip + 1;
                return 1;
            }
        }
        tbuf_free(&f->tuples);
        (*sp)--;
    }
    return 0;
}

/* ─── Bytecode interpreter ────────────────────────────────────────────── */

/*
 * Execute one compiled rule against the database.
 *
 * Parameters:
 *   db        — database handle
 *   cr        — compiled rule (bytecode)
 *   ov        — overrides: for body_idx -> tuple_set (NULL = use DAFSA)
 *   n_ov      — number of overrides
 *   dry       — 1: collect via cb, do not commit to DAFSA
 *               0: commit to DAFSA via rel_add (non-recursive strata)
 *   cb, user  — tuple callback (for dry mode, or for new-tuple notification)
 *
 * Returns number of tuples produced, or -1 on error.
 */
static long exec_rule(dl_db *db, const compiled_rule *cr,
                      const vm_override *ov, int n_ov,
                      int dry, dl_tuple_cb cb, void *user)
{
    const vm_instr *p = cr->instrs;
    int ni = cr->n_instrs;
    bindings b;
    vm_frame frames[MAX_FRAMES];
    int sp = 0, ip = 0;
    long rc = 0;

    /* M3: locate aggregate instructions (if any) and prepare the accumulator */
    agg_accum acc;
    memset(&acc, 0, sizeof(acc));
    int agg_acc_ip = -1, agg_emit_ip = -1, acc_init = 0;
    {
        int k;
        for (k = 0; k < ni; k++) {
            if (p[k].op == OP_AGG_ACC)      agg_acc_ip = k;
            else if (p[k].op == OP_AGG_EMIT) agg_emit_ip = k;
        }
        if (agg_acc_ip >= 0 && agg_emit_ip >= 0) {
            const vm_instr *ai = &p[agg_acc_ip];
            const vm_instr *ei = &p[agg_emit_ip];
            if (agg_init(&acc) != 0) return -1;
            acc.n_key = ai->a;
            acc.op    = ai->b;
            acc.res_slot = ai->c;
            for (k = 0; k < acc.n_key; k++) acc.group_slots[k] = ai->slots[k];
            acc.src_slot = ai->slots[acc.n_key];
            acc.head_rel_id = ei->a;
            acc.head_arity  = ei->b;
            for (k = 0; k < acc.head_arity; k++) acc.head_slots[k] = ei->slots[k];
            acc.dry  = dry;
            acc.cb   = cb;
            acc.user = user;
            acc_init = 1;
        }
    }

    b_init(&b);
    memset(frames, 0, sizeof(frames));

    while (ip < ni) {
        const vm_instr *in = &p[ip];

        switch (in->op) {

        /* ── OP_SCAN: full scan of a body relation ─────────────────── */
        case OP_SCAN: {
            if (sp >= MAX_FRAMES) return -1;
            vm_frame *f = &frames[sp];
            f->ip = ip;
            f->op = OP_SCAN;
            f->idx = 0;
            memset(&f->tuples, 0, sizeof(f->tuples));

            const tuple_set *ov_ts = find_ov((int)in->body_idx, ov, n_ov);

            if (ov_ts && ov_ts->count > 0) {
                /* Enumerate all tuples from override tuple_set */
                f->tuples.arity = ov_ts->arity;
                f->tuples.count = ov_ts->count;
                f->tuples.cap   = ov_ts->count;
                f->tuples.data  = malloc((size_t)ov_ts->count *
                                         ov_ts->arity * sizeof(uint32_t));
                if (!f->tuples.data) return -1;
                memcpy(f->tuples.data, ov_ts->data,
                       (size_t)ov_ts->count * ov_ts->arity * sizeof(uint32_t));
            } else {
                void *r = db_rel(db, in->a);
                if (!r) { ip = ni; break; }
                f->tuples.arity = in->b;
                rel_prefix((const void *)r, NULL, 0, tbuf_cb, &f->tuples);
            }

            if (f->tuples.count == 0) {
                tbuf_free(&f->tuples);
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }

            b_save(&f->saved, &b);
            if (!seek_valid(f, in, 0, (int)f->tuples.arity, &b)) {
                tbuf_free(&f->tuples);
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            sp++;
            ip++;
            break;
        }

        /* ── OP_WALK: full scan with regex pattern filter ───────────── */
        case OP_WALK: {
            if (sp >= MAX_FRAMES) return -1;
            vm_frame *f = &frames[sp];
            f->ip = ip;
            f->op = OP_SCAN;  /* treated as SCAN for backtrack */
            f->idx = 0;
            memset(&f->tuples, 0, sizeof(f->tuples));

            /* Get the compiled regex DFA from the rule */
            int pat_idx = (int)in->imm;
            if (pat_idx < 0 || pat_idx >= cr->n_patterns) {
                ip = ni; break;
            }
            regex_dfa *dfa = cr->patterns[pat_idx];

            void *r = db_rel(db, in->a);
            if (!r) { ip = ni; break; }
            f->tuples.arity = in->b;

            /* Use rel_pattern to enumerate matching tuples */
            rel_pattern((const void *)r, dfa, tbuf_cb, &f->tuples);

            if (f->tuples.count == 0) {
                tbuf_free(&f->tuples);
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }

            b_save(&f->saved, &b);
            if (!seek_valid(f, in, 0, (int)f->tuples.arity, &b)) {
                tbuf_free(&f->tuples);
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            sp++;
            ip++;
            break;
        }

        /* ── OP_LOOKUP: prefix lookup ─────────────────────────────── */
        case OP_LOOKUP: {
            if (sp >= MAX_FRAMES) return -1;
            vm_frame *f = &frames[sp];
            f->ip = ip;
            f->op = OP_LOOKUP;
            f->idx = 0;
            memset(&f->tuples, 0, sizeof(f->tuples));

            /* Compute prefix from current bindings */
            int k = in->b;
            uint32_t pref[8];
            int pk;
            for (pk = 0; pk < k; pk++)
                pref[pk] = b_get(&b, in->slots[pk]);

            const tuple_set *ov_ts = find_ov((int)in->body_idx, ov, n_ov);

            if (ov_ts && ov_ts->count > 0) {
                /* Use ts_prefix on sorted tuple_set */
                long first;
                long cnt = ts_prefix(ov_ts, pref, (uint8_t)k, &first);
                if (cnt == 0) {
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
                f->tuples.arity = ov_ts->arity;
                f->tuples.count = cnt;
                f->tuples.cap   = cnt;
                f->tuples.data  = malloc((size_t)cnt * ov_ts->arity *
                                         sizeof(uint32_t));
                if (!f->tuples.data) return -1;
                memcpy(f->tuples.data,
                       ov_ts->data + (size_t)first * ov_ts->arity,
                       (size_t)cnt * ov_ts->arity * sizeof(uint32_t));
            } else {
                void *r = db_rel(db, in->a);
                if (!r) {
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
                f->tuples.arity = in->c;
                rel_prefix((const void *)r, pref, (uint8_t)k,
                           tbuf_cb, &f->tuples);
                if (f->tuples.count == 0) {
                    tbuf_free(&f->tuples);
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
            }

            b_save(&f->saved, &b);
            if (!seek_valid(f, in, k, (int)f->tuples.arity, &b)) {
                tbuf_free(&f->tuples);
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            sp++;
            ip++;
            break;
        }

        /* ── OP_LOOKUP_PERM: permuted prefix lookup (M6) ──────────── */
        case OP_LOOKUP_PERM: {
            if (sp >= MAX_FRAMES) return -1;

            /* Fetch permutation from dl_db */
            int perm_id = (int)in->imm;
            int rel_id = (int)in->a;
            int k = (int)in->b;
            int ar = (int)in->c;
            const uint8_t *perm_arr = dl_db_get_perm(db, rel_id, perm_id);
            if (!perm_arr) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }

            vm_frame *f = &frames[sp];
            f->ip = ip;
            f->op = OP_LOOKUP_PERM;
            f->idx = 0;
            f->perm = perm_arr;
            memset(&f->tuples, 0, sizeof(f->tuples));

            /* Build prefix from bindings: pref[pp] = slots[perm[pp]] */
            uint32_t pref[8];
            int pk;
            for (pk = 0; pk < k; pk++) {
                int oc = (int)perm_arr[pk];
                pref[pk] = b_get(&b, in->slots[oc]);
            }

            const tuple_set *ov_ts = find_ov((int)in->body_idx, ov, n_ov);

            if (ov_ts && ov_ts->count > 0) {
                /* Override: use ts_prefix on a permuted shadow (IDB path).
                 * The shadow is already in permuted order, so we prefix
                 * directly on the permuted tuple_set. */
                long first;
                long cnt = ts_prefix(ov_ts, pref, (uint8_t)k, &first);
                if (cnt == 0) {
                    f->perm = NULL;
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
                f->tuples.arity = ov_ts->arity;
                f->tuples.count = cnt;
                f->tuples.cap   = cnt;
                f->tuples.data  = malloc((size_t)cnt * ov_ts->arity *
                                         sizeof(uint32_t));
                if (!f->tuples.data) return -1;
                memcpy(f->tuples.data,
                       ov_ts->data + (size_t)first * ov_ts->arity,
                       (size_t)cnt * ov_ts->arity * sizeof(uint32_t));
            } else {
                /* EDB path: use perm DAFSA */
                void *pr = (void *)dl_db_get_perm_rel(db, rel_id, perm_id);
                if (!pr) {
                    f->perm = NULL;
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
                f->tuples.arity = (uint8_t)ar;
                rel_prefix((const void *)pr, pref, (uint8_t)k,
                           tbuf_cb, &f->tuples);
                if (f->tuples.count == 0) {
                    tbuf_free(&f->tuples);
                    f->perm = NULL;
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
            }

            b_save(&f->saved, &b);
            if (!seek_valid_perm(f, in, k, ar, perm_arr, &b)) {
                tbuf_free(&f->tuples);
                f->perm = NULL;
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            sp++;
            ip++;
            break;
        }

        /* ── OP_HASH_JOIN: in-frame hash join (M6) ────────────────── */
        case OP_HASH_JOIN: {
            /* For now, OP_HASH_JOIN is emitted for recursive IDB body
             * atoms with non-leading joins when no perm index is available.
             * We materialize both sides and hash-join.
             *
             * Layout: a=rel_id, b=k (shared cols), c=arity, imm=perm_id,
             *   slots[c] = slot for original col c. */
            if (sp >= MAX_FRAMES) return -1;

            int rel_id = (int)in->a;
            int k = (int)in->b;
            int ar = (int)in->c;
            int perm_id = (int)in->imm;

            vm_frame *f = &frames[sp];
            f->ip = ip;
            f->op = OP_HASH_JOIN;
            f->idx = 0;
            f->perm = NULL;
            memset(&f->tuples, 0, sizeof(f->tuples));

            /* Try override first */
            const tuple_set *ov_ts = find_ov((int)in->body_idx, ov, n_ov);

            /* Build a hash table keyed by the first k columns of the
             * permuted tuple.  Use an open-addressing hash table
             * (FNV-1a, linear probe — mirrors tupleset.c conventions). */

            /* Collect all tuples from the relation */
            tuple_set all_ts;
            if (ts_init(&all_ts, (uint8_t)ar) != 0) return -1;

            if (ov_ts && ov_ts->count > 0) {
                /* Copy override tuples */
                long ci;
                for (ci = 0; ci < ov_ts->count; ci++) {
                    ts_add(&all_ts,
                           ov_ts->data + (size_t)ci * ov_ts->arity);
                }
            } else {
                void *r = db_rel(db, rel_id);
                if (!r) {
                    ts_free(&all_ts);
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
                rel_prefix((const void *)r, NULL, 0, ts_sink_cb, &all_ts);
                ts_sort(&all_ts);
            }

            if (all_ts.count == 0) {
                ts_free(&all_ts);
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }

            /* Get perm array for re-encoding */
            const uint8_t *perm_arr = NULL;
            if (perm_id >= 0) {
                perm_arr = dl_db_get_perm(db, rel_id, perm_id);
            }

            /* Filter tuples: only those whose prefix columns match
             * the current bindings.  For each matching tuple, produce
             * it as a single-row result set for the frame. */
            {
                /* Collect matching tuples */
                long ci;
                tuple_set match_ts;
                if (ts_init(&match_ts, (uint8_t)ar) != 0) {
                    ts_free(&all_ts);
                    return -1;
                }

                for (ci = 0; ci < all_ts.count; ci++) {
                    const uint32_t *row = all_ts.data +
                        (size_t)ci * (size_t)all_ts.arity;
                    int match = 1;
                    int pk2;
                    for (pk2 = 0; pk2 < k; pk2++) {
                        int oc = perm_arr ? (int)perm_arr[pk2] : pk2;
                        uint32_t bound_val = b_get(&b, in->slots[oc]);
                        if (bound_val != row[oc]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        /* Re-encode through perm if needed */
                        if (perm_arr) {
                            uint32_t prow[8];
                            int jj;
                            for (jj = 0; jj < ar; jj++)
                                prow[jj] = row[perm_arr[jj]];
                            ts_add(&match_ts, prow);
                        } else {
                            ts_add(&match_ts, row);
                        }
                    }
                }

                if (match_ts.count == 0) {
                    ts_free(&match_ts);
                    ts_free(&all_ts);
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }

                ts_sort(&match_ts);

                /* Copy to frame tuples */
                f->tuples.arity = (uint8_t)ar;
                f->tuples.count = match_ts.count;
                f->tuples.cap   = match_ts.count;
                f->tuples.data  = malloc((size_t)match_ts.count *
                                         (size_t)ar * sizeof(uint32_t));
                if (!f->tuples.data) {
                    ts_free(&match_ts);
                    ts_free(&all_ts);
                    return -1;
                }
                memcpy(f->tuples.data, match_ts.data,
                       (size_t)match_ts.count * (size_t)ar * sizeof(uint32_t));
                ts_free(&match_ts);
            }

            ts_free(&all_ts);

            b_save(&f->saved, &b);
            f->perm = perm_arr;

            if (perm_arr) {
                if (!seek_valid_perm(f, in, k, ar, perm_arr, &b)) {
                    tbuf_free(&f->tuples);
                    f->perm = NULL;
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
            } else {
                if (!seek_valid(f, in, k, ar, &b)) {
                    tbuf_free(&f->tuples);
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
            }
            sp++;
            ip++;
            break;
        }

        /* ── OP_NEG_CHECK: stratified negation ────────────────────── */
        case OP_NEG_CHECK: {
            void *r = db_rel(db, in->a);
            if (!r) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            uint32_t cols[8];
            int ar = in->b, all = 1, j;
            for (j = 0; j < ar; j++) {
                uint8_t s = in->slots[j];
                if (!b_ok(&b, s)) { all = 0; break; }
                cols[j] = b_get(&b, s);
            }
            if (!all) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            if (rel_exact((const void *)r, cols)) {
                /* Found in negated relation — backtrack */
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            ip++;
            break;
        }

        /* ── OP_EQ: equality between two variables ────────────────── */
        case OP_EQ: {
            uint8_t va = in->a, vb = in->b;
            uint32_t av = b_get(&b, va), bv = b_get(&b, vb);
            int ab = b_ok(&b, va), bb = b_ok(&b, vb);

            if (ab && bb) {
                if (av != bv) {
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
            } else if (ab && !bb) {
                b_try(&b, vb, av);
            } else if (!ab && bb) {
                b_try(&b, va, bv);
            }
            ip++;
            break;
        }

        /* ── OP_EQ_CONST: equality to constant ────────────────────── */
        case OP_EQ_CONST: {
            uint8_t s = in->a;
            uint32_t cv = in->imm;
            if (b_ok(&b, s)) {
                if (b_get(&b, s) != cv) {
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
            } else {
                b_try(&b, s, cv);
            }
            ip++;
            break;
        }

        /* ── OP_CMP: comparison filter (M9) ─────────────────────────
         * a=lhs slot, b=rhs slot, imm=cmp code 0=LT 1=LE 2=GT 3=GE 4=NE.
         * Both operands MUST be bound (compiler guarantees); on false,
         * backtrack. */
        case OP_CMP: {
            uint32_t av = b_get(&b, in->a), bv = b_get(&b, in->b);
            int pass = 0;
            switch (in->imm) {
                case 0: pass = av <  bv; break;
                case 1: pass = av <= bv; break;
                case 2: pass = av >  bv; break;
                case 3: pass = av >= bv; break;
                case 4: pass = av != bv; break;
                default: pass = 0; break;
            }
            if (!pass) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            ip++;
            break;
        }

        /* ── OP_ARITH: arithmetic bind (M9) ─────────────────────────
         * a=lhs slot, b=rhs slot, c=result temp slot,
         * imm=arith code 0=ADD 1=SUB 2=MUL 3=DIV 4=MOD.
         * Result written via b_try (never overwrite an existing binding);
         * DIV/MOD with rhs==0 backtracks (never a crash); u32 wrap-around. */
        case OP_ARITH: {
            uint32_t av = b_get(&b, in->a), bv = b_get(&b, in->b);
            uint32_t r = 0;
            int ok = 1;
            switch (in->imm) {
                case 0: r = av + bv; break;
                case 1: r = av - bv; break;
                case 2: r = av * bv; break;
                case 3: if (bv == 0) ok = 0; else r = av / bv; break;
                case 4: if (bv == 0) ok = 0; else r = av % bv; break;
                default: ok = 0; break;
            }
            if (!ok) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            if (!b_try(&b, in->c, r)) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            ip++;
            break;
        }

        /* ── OP_STR_FILTER: string filter (M9-strings) ────────────────
         * a=lhs slot, b=rhs slot, imm=0 PREFIX 1 SUFFIX 2 CONTAINS.
         * Both operands are interned symbols.  intern_str_of returns NULL
         * for an out-of-range sym_id (the documented B6 int/symbol
         * collision backstop) -> backtrack (never crash, never
         * mis-evaluate).  false -> backtrack. */
        case OP_STR_FILTER: {
            const char *as = intern_str_of(db->ir, b_get(&b, in->a));
            const char *bs = intern_str_of(db->ir, b_get(&b, in->b));
            int pass = 0;
            if (as && bs) {
                size_t al = strlen(as), bl = strlen(bs);
                switch (in->imm) {
                    case 0: pass = (bl <= al && strncmp(as, bs, bl) == 0);
                            break;
                    case 1: pass = (bl <= al &&
                                    strcmp(as + al - bl, bs) == 0);
                            break;
                    case 2: pass = (strstr(as, bs) != NULL);
                            break;
                    default: pass = 0; break;
                }
            }
            if (!pass) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            ip++;
            break;
        }

        /* ── OP_STR_LEN: string length bind (M9-strings) ──────────────
         * a=operand slot, c=result int temp slot.  operand NULL via
         * intern_str_of -> backtrack; writes BYTE length (strlen) via
         * b_try.  No interning. */
        case OP_STR_LEN: {
            const char *s = intern_str_of(db->ir, b_get(&b, in->a));
            if (!s) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            {
                uint32_t len = (uint32_t)strlen(s);
                if (!b_try(&b, in->c, len)) {
                    if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                    break;
                }
            }
            ip++;
            break;
        }

        /* ── OP_STR_BIND: string-producing bind (M9-strings) ──────────
         * a=lhs slot, b=rhs slot (unused for unary ops), c=result temp
         * slot, imm=0 CONCAT / 1 LOWER / 2 UPPER.  Builds the result into a
         * heap buffer and interns it; intern_str returns 0 for OOM or a
         * result longer than 4096 bytes (intern.c key cap) -> backtrack.
         * This is the ONLY opcode that interns at runtime (db->ir reachable
         * via dl_internal.h).  lower/upper are ASCII case folding. */
        case OP_STR_BIND: {
            uint32_t sid = 0;
            if (in->imm == 0) {
                const char *as = intern_str_of(db->ir, b_get(&b, in->a));
                const char *bs = intern_str_of(db->ir, b_get(&b, in->b));
                if (as && bs) {
                    size_t al = strlen(as), bl = strlen(bs);
                    if (al + bl <= 4096) {
                        char *buf = malloc(al + bl + 1);
                        if (buf) {
                            memcpy(buf, as, al);
                            memcpy(buf + al, bs, bl);
                            buf[al + bl] = '\0';
                            sid = intern_str(db->ir, buf);
                            free(buf);
                        }
                    }
                }
            } else if (in->imm == 1 || in->imm == 2) {
                /* LOWER (1) / UPPER (2): ASCII case folding on one operand. */
                const char *as = intern_str_of(db->ir, b_get(&b, in->a));
                if (as) {
                    size_t al = strlen(as);
                    if (al <= 4096) {
                        char *buf = malloc(al + 1);
                        if (buf) {
                            size_t j;
                            for (j = 0; j < al; j++) {
                                char ch = as[j];
                                if (in->imm == 1) {        /* lower */
                                    if (ch >= 'A' && ch <= 'Z')
                                        ch = (char)(ch + ('a' - 'A'));
                                } else {                  /* upper */
                                    if (ch >= 'a' && ch <= 'z')
                                        ch = (char)(ch - ('a' - 'A'));
                                }
                                buf[j] = ch;
                            }
                            buf[al] = '\0';
                            sid = intern_str(db->ir, buf);
                            free(buf);
                        }
                    }
                }
            }
            if (sid == 0) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            if (!b_try(&b, in->c, sid)) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            ip++;
            break;
        }

        /* ── OP_PROJECT: emit / commit tuple ──────────────────────── */
        case OP_PROJECT: {
            void *r = db_rel(db, in->a);
            if (!r) { ip = ni; break; }
            uint32_t cols[8];
            int j;
            for (j = 0; j < in->b; j++) {
                uint8_t s = in->slots[j];
                if (s == 0xFF) { ip = ni; break; }
                cols[j] = b_get(&b, s);
            }
            if (ip >= ni) break;

            if (dry) {
                rc++;
                if (cb && cb(cols, (uint8_t)in->b, user)) { ip = ni; break; }
            } else {
                int rr = rel_add((void *)r, cols);
                if (rr < 0) { ip = ni; break; }
                if (rr == 1) {
                    rc++;
                    if (cb && cb(cols, (uint8_t)in->b, user)) { ip = ni; break; }
                }
            }
            /* Project always backtracks (each binding produces one tuple) */
            if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
            break;
        }

        /* ── OP_AGG_ACC: accumulate current binding into its group bucket */
        case OP_AGG_ACC: {
            uint32_t key[8];
            int g, all_bound = 1;
            for (g = 0; g < acc.n_key; g++) {
                if (!b_ok(&b, acc.group_slots[g])) { all_bound = 0; break; }
                key[g] = b_get(&b, acc.group_slots[g]);
            }
            if (!all_bound) {
                if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
                break;
            }
            agg_bucket *bk = agg_find_or_create(&acc, key);
            if (!bk) { ip = ni; break; }
            bk->valid = 1;
            bk->count++;
            if (acc.op == 1) {
                bk->sum += b_get(&b, acc.src_slot);   /* u32 wrap-around */
            } else if (acc.op == 2) {
                uint32_t v = b_get(&b, acc.src_slot);
                if (bk->count == 1) bk->min = v;
                else if (v < bk->min) bk->min = v;
            } else if (acc.op == 3) {
                uint32_t v = b_get(&b, acc.src_slot);
                if (bk->count == 1) bk->max = v;
                else if (v > bk->max) bk->max = v;
            }
            /* Aggregate consumes all matching tuples: backtrack to continue */
            if (!backtrack(frames, &sp, &b, p, &ip)) { ip = ni; }
            break;
        }

        /* ── OP_AGG_EMIT: handled after the main loop (never reached
         * via normal ip flow, since OP_AGG_ACC always backtracks) ──── */
        case OP_AGG_EMIT:
            ip++;
            break;

        /* ── OP_HALT ──────────────────────────────────────────────── */
        case OP_HALT:
            ip = ni;
            break;

        default:
            fprintf(stderr, "vm: bad op %d\n", in->op);
            ip = ni;
            break;
        }
    }

    /* M3: emit one output tuple per aggregate group bucket */
    if (acc_init && acc.n_buckets > 0) {
        int bi2;
        for (bi2 = 0; bi2 < acc.cap; bi2++) {
            agg_bucket *bk = &acc.buckets[bi2];
            if (bk->key == NULL) continue;
            uint32_t agg_val;
            switch (acc.op) {
                case 1: agg_val = bk->sum;  break;
                case 2: agg_val = bk->min;  break;
                case 3: agg_val = bk->max;  break;
                default: agg_val = bk->count; break;
            }
            uint32_t cols[8];
            int jj, ok = 1;
            for (jj = 0; jj < acc.head_arity; jj++) {
                uint8_t hs = acc.head_slots[jj];
                if (hs == acc.res_slot) {
                    cols[jj] = agg_val;
                } else {
                    int g2, found = 0;
                    for (g2 = 0; g2 < acc.n_key; g2++) {
                        if (acc.group_slots[g2] == hs) {
                            cols[jj] = bk->key[g2];
                            found = 1;
                            break;
                        }
                    }
                    if (!found) { ok = 0; break; }
                }
            }
            if (!ok) continue;
            if (acc.dry) {
                rc++;
                if (acc.cb && acc.cb(cols, acc.head_arity, acc.user)) break;
            } else {
                void *r = db_rel(db, acc.head_rel_id);
                if (r) {
                    int rr = rel_add((void *)r, cols);
                    if (rr < 0) break;
                    if (rr == 1) {
                        rc++;
                        if (acc.cb && acc.cb(cols, acc.head_arity, acc.user)) break;
                    }
                }
            }
        }
    }
    if (acc_init) agg_free(&acc);

    /* Clean up remaining frames */
    while (sp > 0) {
        tbuf_free(&frames[sp - 1].tuples);
        sp--;
    }

    return rc;
}

/* ─── Candidate collector callback ────────────────────────────────────── */

typedef struct {
    tuple_set *ts;
} cand_ctx;

static int cand_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    cand_ctx *ctx = (cand_ctx *)user;
    (void)arity;
    ts_add(ctx->ts, cols);
    return 0;
}

/* ─── Non-recursive evaluation (M1 path) ──────────────────────────────── */

static int eval_nonrecursive(dl_db *db, compiled_rule **rules, int n)
{
    /* Non-recursive rules must be evaluated to a fixpoint, not a single
     * pass in emit order: a rule chain like r3:-r2; r2:-r1; r1:-edge is
     * only fully materialized if a later-emitted producer has already run.
     * The compiler does not guarantee dependency-first emit order, so loop
     * the whole stratum until a full pass adds no new tuples.  Non-recursive
     * rules form a DAG, so this converges in at most n passes. */
    int pass;
    for (pass = 0; pass <= n; pass++) {
        long added = 0;
        int i;
        int changed = 0;
        for (i = 0; i < n; i++) {
            long m = exec_rule(db, rules[i], NULL, 0,
                               0 /* commit */, NULL, NULL);
            if (m < 0) return -1;
            added += m;
            if (m > 0) changed = 1;
            /* M6: IDB DAFSA was populated — mark perms dirty and rebuild
             * so subsequent rules (even within the same stratum) see fresh
             * perm indices for non-leading joins. */
            if (m > 0) {
                permindex_mark_dirty(db, rules[i]->head_rel_id);
                if (permindex_build_dirty(db) != 0) return -1;
            }
        }
        if (!changed) break;  /* fixpoint reached */
        (void)added;
    }
    return 0;
}

/* ─── Recursive stratum evaluation (semi-naive fixpoint) ───────────────── */

#define FIXPOINT_ERROR_BOUND 10000000

static int eval_stratum_recursive(dl_db *db, compiled_rule **rules, int n,
                                  int ivm)
{
    int i, ri;
    int rp[64], nr = 0;  /* recursive head relation ids */
    int rp_idx[64];       /* rel_id -> index in rp[] (only for recursive heads) */

    /* ── 1. Identify recursive head relations ──────────────────────── */
    {
        int seen[64] = {0};
        for (i = 0; i < n; i++) {
            compiled_rule *cr = rules[i];
            if (!cr->is_recursive) continue;
            int rid = cr->head_rel_id;
            if (rid < 64 && !seen[rid]) {
                seen[rid] = 1;
                rp[nr++] = rid;
            }
        }
        /* Initialize rp_idx: -1 for non-recursive, index for recursive */
        for (i = 0; i < 64; i++) rp_idx[i] = -1;
        for (i = 0; i < nr; i++) rp_idx[rp[i]] = i;
    }

    /* If no recursive heads (shouldn't happen), fall back to non-recursive */
    if (nr == 0)
        return eval_nonrecursive(db, rules, n);

    /* ── 1b. Decide whether the full-idb override must be kept sorted ──
     * A non-delta OP_LOOKUP recursive body atom is evaluated via ts_prefix
     * (binary search, requires SORTED data) over the full-idb override.  idb
     * is grown via ts_add during the loop (unsorted tail).  Any recursive rule
     * with TWO OR MORE recursive body atoms leaves at least one non-delta
     * atom per firing; if any of those is a plain-OP_LOOKUP (leading-bound,
     * e.g. magic-sets' tc__bf(X,Y):-magic_tc__bf(X),tc__bf(X,Z),tc__bf(Z,Y)),
     * then that OP_LOOKUP reads the full idb and needs it sorted, or ts_prefix
     * silently misses tuples (silent wrong answer).  This covers BOTH the
     * two-OP_LOOKUP shape AND the mixed OP_SCAN/OP_LOOKUP shape (a recursive
     * atom with all-fresh variables compiles to OP_SCAN; if its sibling is an
     * OP_LOOKUP, the firing where OP_SCAN is the delta still reads the full
     * idb through OP_LOOKUP).  OP_LOOKUP_PERM reads a per-iteration re-sorted
     * shadow and a lone recursive atom is always the delta (reads the sorted
     * delta, never the unsorted full idb), so neither needs this.
     * We therefore re-sort idb each iteration ONLY when such a rule exists,
     * preserving the M2 perf win (idb left unsorted) for the common
     * single-recursive-atom / all-OP_SCAN shapes. */
    int need_idb_sort = 0;
    for (i = 0; i < n; i++) {
        compiled_rule *cr = rules[i];
        int n_rec = 0, n_lookup = 0, ii;
        if (!cr->is_recursive) continue;
        for (ii = 0; ii < cr->n_instrs; ii++) {
            const vm_instr *in = &cr->instrs[ii];
            if (in->op != OP_LOOKUP && in->op != OP_SCAN
                && in->op != OP_LOOKUP_PERM) continue;
            if (rp_idx[in->a] < 0) continue;  /* not a recursive body atom */
            n_rec++;
            if (in->op == OP_LOOKUP) n_lookup++;
        }
        if (n_rec >= 2 && n_lookup >= 1) { need_idb_sort = 1; break; }
    }

    /* ── 2. Allocate IDB and delta tuple_sets ──────────────────────── */
    typedef struct {
        int        rel_id;
        tuple_set  idb;
        tuple_set  delta;
        tuple_set  next_delta;
        uint8_t    arity;
    } rd_t;

    rd_t *rd = calloc((size_t)nr, sizeof(rd_t));
    if (!rd) return -1;

    for (i = 0; i < nr; i++) {
        void *r = db_rel(db, rp[i]);
        uint8_t ar = r ? rel_arity((const void *)r) : 0;
        rd[i].rel_id = rp[i];
        rd[i].arity  = ar;
        if (ts_init(&rd[i].idb, ar) != 0 ||
            ts_init(&rd[i].delta, ar) != 0 ||
            ts_init(&rd[i].next_delta, ar) != 0) {
            for (ri = 0; ri <= i; ri++) {
                ts_free(&rd[ri].idb);
                ts_free(&rd[ri].delta);
                ts_free(&rd[ri].next_delta);
            }
            free(rd);
            return -1;
        }
    }

    /* ── 2b. M6: Find perms for each recursive IDB and allocate shadows ─ */
    /* perm_idxs[rdi][pi] = db perm_id for the pi-th perm of this IDB */
    int    *perm_count = calloc((size_t)nr, sizeof(int));
    int   **perm_ids   = calloc((size_t)nr, sizeof(int *));
    /* perm_cap for each rdi */
    int    *perm_cap   = calloc((size_t)nr, sizeof(int));
    /* idb_perm_shadows[rdi][pi] = permuted sorted tuple_set shadow */
    tuple_set **idb_perm_shadows = calloc((size_t)nr, sizeof(tuple_set *));
    if (!perm_count || !perm_ids || !perm_cap || !idb_perm_shadows) {
        free(perm_count); free(perm_ids); free(perm_cap); free(idb_perm_shadows);
        for (ri = 0; ri < nr; ri++) {
            ts_free(&rd[ri].idb);
            ts_free(&rd[ri].delta);
            ts_free(&rd[ri].next_delta);
        }
        free(rd);
        return -1;
    }

    {
        dl_db *di = db;
        int pi;
        for (pi = 0; pi < di->n_perms; pi++) {
            int p_rel_id = di->perms[pi].rel_id;
            int rdi = rp_idx[p_rel_id];
            if (rdi < 0) continue;  /* not a recursive IDB head */
            int pc = perm_count[rdi];
            if (pc >= perm_cap[rdi]) {
                int nc = perm_cap[rdi] ? perm_cap[rdi] * 2 : 2;
                int *np = realloc(perm_ids[rdi], (size_t)nc * sizeof(int));
                tuple_set *nt = realloc(idb_perm_shadows[rdi],
                                        (size_t)nc * sizeof(tuple_set));
                if (!np || !nt) {
                    /* OOM — free everything and bail */
                    int rj, pk;
                    for (rj = 0; rj < nr; rj++) {
                        for (pk = 0; pk < perm_count[rj]; pk++)
                            ts_free(&idb_perm_shadows[rj][pk]);
                        free(perm_ids[rj]); free(idb_perm_shadows[rj]);
                        ts_free(&rd[rj].idb);
                        ts_free(&rd[rj].delta);
                        ts_free(&rd[rj].next_delta);
                    }
                    free(perm_count); free(perm_ids); free(perm_cap);
                    free(idb_perm_shadows);
                    free(np); free(nt);
                    free(rd);
                    return -1;
                }
                perm_ids[rdi] = np;
                idb_perm_shadows[rdi] = nt;
                perm_cap[rdi] = nc;
            }
            perm_ids[rdi][pc] = pi;
            if (ts_init(&idb_perm_shadows[rdi][pc], di->perms[pi].arity) != 0) {
                int rj, pk;
                for (rj = 0; rj < nr; rj++) {
                    for (pk = 0; pk < perm_count[rj]; pk++)
                        ts_free(&idb_perm_shadows[rj][pk]);
                    free(perm_ids[rj]); free(idb_perm_shadows[rj]);
                    ts_free(&rd[rj].idb);
                    ts_free(&rd[rj].delta);
                    ts_free(&rd[rj].next_delta);
                }
                free(perm_count); free(perm_ids); free(perm_cap);
                free(idb_perm_shadows); free(rd);
                return -1;
            }
            perm_count[rdi]++;
        }
    }

    /* Helper: rebuild all perm shadows from the current IDB */
    #define REBUILD_PERM_SHADOWS() do { \
        int _rdi, _pi; \
        for (_rdi = 0; _rdi < nr; _rdi++) { \
            for (_pi = 0; _pi < perm_count[_rdi]; _pi++) { \
                int _db_pi = perm_ids[_rdi][_pi]; \
                dl_db *_di = db; \
                const uint8_t *_perm_arr = _di->perms[_db_pi].perm; \
                uint8_t _ar = _di->perms[_db_pi].arity; \
                tuple_set *_shadow = &idb_perm_shadows[_rdi][_pi]; \
                ts_reset(_shadow); \
                long _ci; \
                for (_ci = 0; _ci < rd[_rdi].idb.count; _ci++) { \
                    const uint32_t *_row = rd[_rdi].idb.data \
                        + (size_t)_ci * (size_t)rd[_rdi].idb.arity; \
                    uint32_t _prow[8]; \
                    int _j; \
                    for (_j = 0; _j < (int)_ar; _j++) \
                        _prow[_j] = _row[_perm_arr[_j]]; \
                    ts_add(_shadow, _prow); \
                } \
                ts_sort(_shadow); \
            } \
        } \
    } while(0)

    /* IVM Slice 2: seed idb from the CURRENT VIEW (base ∪ prior derived) so
     * the semi-naive fixpoint starts from the already-materialized state and
     * only re-derives what the new base delta adds.  The full re-eval path
     * (ivm==0) leaves idb empty here — the base-rule seed below fills it. */
    if (ivm) {
        for (i = 0; i < nr; i++) {
            relation *rel = (relation *)db_rel(db, rd[i].rel_id);
            if (rel)
                rel_prefix(rel, NULL, 0, ts_sink_cb, &rd[i].idb);
        }
    }

    /* Non-recursive rules → commit to DAFSA (M1 path).
     * Recursive rules → collect into idb/delta via dry=1 (keep DAFSA
     *   empty so the final materialize step is a fast bulk build). */
    for (i = 0; i < n; i++) {
        compiled_rule *cr = rules[i];
        if (!cr->is_recursive) {
            /* Non-recursive rule in this stratum: standard M1 path */
            long m = exec_rule(db, cr, NULL, 0,
                               0 /* commit */, NULL, NULL);
            if (m < 0) {
                for (ri = 0; ri < nr; ri++) {
                    ts_free(&rd[ri].idb);
                    ts_free(&rd[ri].delta);
                    ts_free(&rd[ri].next_delta);
                }
                free(rd);
                return -1;
            }
            (void)m;
            /* M6: rebuild perms so recursive rules (or higher strata)
             * see fresh non-leading join indices */
            permindex_mark_dirty(db, cr->head_rel_id);
            if (permindex_build_dirty(db) != 0) {
                for (ri = 0; ri < nr; ri++) {
                    ts_free(&rd[ri].idb);
                    ts_free(&rd[ri].delta);
                    ts_free(&rd[ri].next_delta);
                }
                free(rd);
                return -1;
            }
        } else if (!ivm) {
            /* Recursive rule: collect base tuples into idb/delta */
            int hri = cr->head_rel_id;
            int hdi = rp_idx[hri];

            tuple_set seed;
            if (ts_init(&seed, rd[hdi].arity) != 0) {
                for (ri = 0; ri < nr; ri++) {
                    ts_free(&rd[ri].idb);
                    ts_free(&rd[ri].delta);
                    ts_free(&rd[ri].next_delta);
                }
                free(rd);
                return -1;
            }

            cand_ctx ctx;
            ctx.ts = &seed;

            long n = exec_rule(db, cr, NULL, 0,
                               1 /* dry */, cand_cb, &ctx);
            if (n < 0) {
                ts_free(&seed);
                for (ri = 0; ri < nr; ri++) {
                    ts_free(&rd[ri].idb);
                    ts_free(&rd[ri].delta);
                    ts_free(&rd[ri].next_delta);
                }
                free(rd);
                return -1;
            }
            (void)n;

            /* Copy seed tuples into idb and delta */
            {
                long ci;
                for (ci = 0; ci < seed.count; ci++) {
                    const uint32_t *t = seed.data +
                        ci * seed.arity;
                    ts_add(&rd[hdi].idb, t);
                    ts_add(&rd[hdi].delta, t);
                }
            }
            ts_free(&seed);
        } else {
            /* IVM Slice 2: seed DELTA from the pending insert deltas on the
             * rule's BASE (non-recursive-head) body atoms.  Each changed base
             * atom is joined as the delta (semi-naive single-step) against the
             * full current view — recursive atoms read the DAFSA, which still
             * holds the prior fixpoint (= idb), and the other base atoms read
             * the post-insert DAFSA — producing exactly the new head tuples
             * the added base facts can derive.  idb was seeded from the
             * current view above, so only these genuinely-new tuples enter the
             * delta. */
            int hri = cr->head_rel_id;
            int hdi = rp_idx[hri];
            int ii;

            for (ii = 0; ii < cr->n_instrs; ii++) {
                const vm_instr *in = &cr->instrs[ii];
                int br;
                vm_override ov[1];
                tuple_set cand;
                cand_ctx ctx;
                long n_out;
                long ci;

                /* Only override-compatible base-atom opcodes with a pending
                 * +delta.  Recursive atoms (rp_idx >= 0) and relations without
                 * a pending delta are left to read the DAFSA. */
                if (in->op != OP_SCAN && in->op != OP_LOOKUP) continue;
                br = (int)in->a;
                if (br < 0 || br >= MAX_RELS) continue;
                if (rp_idx[br] >= 0) continue;        /* recursive atom */
                if (br >= (int)db->nrels) continue;
                if (!db->delta_pending[br]) continue;
                if (db->delta_pending[br]->count == 0) continue;

                ov[0].body_idx = (int)in->body_idx;
                ov[0].ts       = db->delta_pending[br];
                ov[0].perm_id  = -1;

                if (ts_init(&cand, rd[hdi].arity) != 0) {
                    for (ri = 0; ri < nr; ri++) {
                        ts_free(&rd[ri].idb);
                        ts_free(&rd[ri].delta);
                        ts_free(&rd[ri].next_delta);
                    }
                    free(rd);
                    return -1;
                }
                ctx.ts = &cand;

                n_out = exec_rule(db, cr, ov, 1,
                                  1 /* dry */, cand_cb, &ctx);
                if (n_out < 0) {
                    ts_free(&cand);
                    for (ri = 0; ri < nr; ri++) {
                        ts_free(&rd[ri].idb);
                        ts_free(&rd[ri].delta);
                        ts_free(&rd[ri].next_delta);
                    }
                    free(rd);
                    return -1;
                }

                /* New head tuples (not already in idb) enter BOTH idb (so the
                 * final materialize includes them) and delta (so the fixpoint
                 * loop propagates them transitively) — mirroring the full-mode
                 * base-rule seed. */
                for (ci = 0; ci < cand.count; ci++) {
                    const uint32_t *t = cand.data + ci * cand.arity;
                    if (!ts_contains(&rd[hdi].idb, t)) {
                        ts_add(&rd[hdi].idb, t);
                        ts_add(&rd[hdi].delta, t);
                    }
                }
                ts_free(&cand);
            }
        }
    }

    /* Sort idb and delta for ts_prefix lookups */
    for (i = 0; i < nr; i++) {
        ts_sort(&rd[i].idb);
        ts_sort(&rd[i].delta);
    }

    /* M6: build initial perm shadows from seed idb */
    REBUILD_PERM_SHADOWS();

    /* If all deltas are empty, terminate immediately */
    {
        int any = 0;
        for (i = 0; i < nr; i++)
            if (rd[i].delta.count > 0) { any = 1; break; }
        if (!any) {
            for (ri = 0; ri < nr; ri++) {
                ts_free(&rd[ri].idb);
                ts_free(&rd[ri].delta);
                ts_free(&rd[ri].next_delta);
            }
            free(rd);
            return 0;
        }
    }

    /* ── 5. Fixpoint loop ──────────────────────────────────────────── */
    {
        int iter;
        for (iter = 0; ; iter++) {
            if (iter >= FIXPOINT_ERROR_BOUND) {
                fprintf(stderr, "vm: fixpoint not converged after %d iterations\n",
                        iter);
                for (ri = 0; ri < nr; ri++) {
                    ts_free(&rd[ri].idb);
                    ts_free(&rd[ri].delta);
                    ts_free(&rd[ri].next_delta);
                }
                free(rd);
                /* M6: free perm shadows on error */
                {
                    int _rdi, _pi;
                    for (_rdi = 0; _rdi < nr; _rdi++) {
                        for (_pi = 0; _pi < perm_count[_rdi]; _pi++)
                            ts_free(&idb_perm_shadows[_rdi][_pi]);
                        free(perm_ids[_rdi]);
                        free(idb_perm_shadows[_rdi]);
                    }
                    free(perm_count); free(perm_ids);
                    free(perm_cap); free(idb_perm_shadows);
                }
                return -1;
            }

            /* M6: rebuild perm shadows from current (post-rollover) idb */
            REBUILD_PERM_SHADOWS();

            /* Keep idb sorted for non-delta OP_LOOKUP ts_prefix lookups when
             * the stratum has a recursive rule with ≥2 recursive body atoms
             * AND ≥1 plain-OP_LOOKUP among them (see need_idb_sort above). */
            if (need_idb_sort) {
                for (i = 0; i < nr; i++) ts_sort(&rd[i].idb);
            }

            /* Reset next_delta for this iteration */
            for (i = 0; i < nr; i++)
                ts_reset(&rd[i].next_delta);

            /* For each recursive rule, for each recursive body atom */
            for (ri = 0; ri < n; ri++) {
                compiled_rule *cr = rules[ri];
                if (!cr->is_recursive) continue;

                int hri = cr->head_rel_id;
                int hdi = rp_idx[hri];
                const vm_instr *prog = cr->instrs;
                int ni_instrs = cr->n_instrs;
                int ii;

                for (ii = 0; ii < ni_instrs; ii++) {
                    const vm_instr *in = &prog[ii];
                    if (in->op != OP_SCAN && in->op != OP_LOOKUP
                        && in->op != OP_LOOKUP_PERM)
                        continue;

                    int br = in->a;         /* body relation id */
                    int bdi = rp_idx[br];   /* index in rd[] */
                    if (bdi < 0) continue;  /* not a recursive body atom */
                    if (rd[bdi].delta.count == 0) continue;

                    /* Build override list:
                     * - All recursive body atoms get idb[rel] (full)
                     * - This specific atom gets delta[rel] instead
                     * - OP_LOOKUP_PERM atoms get the perm shadow */
                    vm_override overrides[16];
                    int n_ov = 0;
                    int ji;

                    for (ji = 0; ji < ni_instrs; ji++) {
                        const vm_instr *jin = &prog[ji];
                        if (jin->op != OP_SCAN && jin->op != OP_LOOKUP
                            && jin->op != OP_LOOKUP_PERM)
                            continue;
                        int jbr = jin->a;
                        int jbdi = rp_idx[jbr];
                        if (jbdi < 0) continue;  /* EDB — no override */

                        int is_delta = ((int)jin->body_idx == (int)in->body_idx &&
                                        jbr == br);

                        if (jin->op == OP_LOOKUP_PERM) {
                            /* Provide permuted shadow */
                            int jperm_id = (int)jin->imm;
                            /* Find the shadow for this perm_id */
                            int pk;
                            const tuple_set *shadow_ts = NULL;
                            for (pk = 0; pk < perm_count[jbdi]; pk++) {
                                if (perm_ids[jbdi][pk] == jperm_id) {
                                    shadow_ts = &idb_perm_shadows[jbdi][pk];
                                    break;
                                }
                            }
                            overrides[n_ov].body_idx = (int)jin->body_idx;
                            overrides[n_ov].ts = shadow_ts ? shadow_ts : &rd[jbdi].idb;
                            overrides[n_ov].perm_id = jperm_id;
                        } else if (is_delta) {
                            /* This is the delta atom */
                            overrides[n_ov].body_idx = (int)jin->body_idx;
                            overrides[n_ov].ts = &rd[bdi].delta;
                            overrides[n_ov].perm_id = -1;
                        } else {
                            /* Full IDB.  rd[jbdi].idb is kept sorted when the
                             * stratum has a recursive rule with ≥2 recursive
                             * body atoms AND ≥1 plain-OP_LOOKUP among them
                             * (see need_idb_sort above), so the ts_prefix
                             * binary search in OP_LOOKUP is correct.  For the
                             * common shapes idb stays unsorted, which is safe:
                             * OP_SCAN is order-independent, OP_LOOKUP_PERM
                             * reads a per-iteration re-sorted shadow, and a
                             * single OP_LOOKUP atom is always the delta (never
                             * the non-delta override).  Do NOT unconditionally
                             * re-sort idb here — that was the M2 perf
                             * bottleneck. */
                            overrides[n_ov].body_idx = (int)jin->body_idx;
                            overrides[n_ov].ts = &rd[jbdi].idb;
                            overrides[n_ov].perm_id = -1;
                        }
                        n_ov++;
                    }

                    /* Collect candidates into a temporary tuple_set */
                    tuple_set cand;
                    if (ts_init(&cand, rd[hdi].arity) != 0) {
                        for (i = 0; i < nr; i++) {
                            ts_free(&rd[i].idb);
                            ts_free(&rd[i].delta);
                            ts_free(&rd[i].next_delta);
                        }
                        free(rd);
                        return -1;
                    }

                    cand_ctx ctx;
                    ctx.ts = &cand;

                    long n_out = exec_rule(db, cr, overrides, n_ov,
                                           1 /* dry */, cand_cb, &ctx);
                    if (n_out < 0) {
                        ts_free(&cand);
                        for (i = 0; i < nr; i++) {
                            ts_free(&rd[i].idb);
                            ts_free(&rd[i].delta);
                            ts_free(&rd[i].next_delta);
                        }
                        free(rd);
                        /* M6: free perm shadows on error */
                        {
                            int _rdi, _pi;
                            for (_rdi = 0; _rdi < nr; _rdi++) {
                                for (_pi = 0; _pi < perm_count[_rdi]; _pi++)
                                    ts_free(&idb_perm_shadows[_rdi][_pi]);
                                free(perm_ids[_rdi]);
                                free(idb_perm_shadows[_rdi]);
                            }
                            free(perm_count); free(perm_ids);
                            free(perm_cap); free(idb_perm_shadows);
                        }
                        return -1;
                    }
                    (void)n_out;

                    /* Add new tuples to next_delta only.  idb is FROZEN during
                     * the firing sub-loop so that every non-delta OP_LOOKUP
                     * ts_prefix read (binary search) stays on sorted data; new
                     * tuples are merged into idb at rollover. */
                    {
                        long ci;
                        for (ci = 0; ci < cand.count; ci++) {
                            const uint32_t *t = cand.data +
                                ci * cand.arity;
                            if (!ts_contains(&rd[hdi].idb, t)) {
                                ts_add(&rd[hdi].next_delta, t);
                            }
                        }
                    }

                    ts_free(&cand);
                }
            }

            /* Check termination */
            {
                int empty = 1;
                for (i = 0; i < nr; i++) {
                    if (rd[i].next_delta.count > 0) {
                        empty = 0;
                        break;
                    }
                }
                if (empty) break;
            }

            /* Rollover: sort next_delta → delta, reset next_delta */
            for (i = 0; i < nr; i++) {
                if (rd[i].next_delta.count > 0) {
                    ts_sort(&rd[i].next_delta);
                    /* Merge the iteration's new tuples into idb.  They are
                     * appended unsorted; the top-of-iteration ts_sort (when
                     * need_idb_sort) re-sorts before the next firing round. */
                    {
                        long ci;
                        for (ci = 0; ci < rd[i].next_delta.count; ci++) {
                            ts_add(&rd[i].idb,
                                   rd[i].next_delta.data + ci * rd[i].arity);
                        }
                    }
                    /* Swap delta and next_delta */
                    {
                        tuple_set tmp = rd[i].delta;
                        rd[i].delta = rd[i].next_delta;
                        rd[i].next_delta = tmp;
                    }
                    ts_reset(&rd[i].next_delta);
                } else {
                    /* delta exhausted — free and mark empty */
                    ts_free(&rd[i].delta);
                    memset(&rd[i].delta, 0, sizeof(rd[i].delta));
                    rd[i].delta.arity = rd[i].arity;
                }
                /* Note: idb is NOT sorted here in the rollover — when needed it
                 * is re-sorted at the TOP of each iteration (see need_idb_sort).
                 * idb is otherwise only grown via ts_contains/ts_add (hash, O(1)). */
            }
        }
    }

    /* ── 6. Materialize: bulk-write idb to DAFSA ─────────────────────
     * In the magic path (vm_nomaterialize), the GOAL relation's DAFSA is not
     * needed (the clone is torn down right after streaming); instead export
     * its idb to the caller.  ALL OTHER relations are still materialized so
     * higher strata (which read lower-stratum relations via their DAFSAs)
     * keep seeing correct data. */
    for (i = 0; i < nr; i++) {
        relation *rel = (relation *)db_rel(db, rd[i].rel_id);
        if (!rel) continue;

        /* Union pre-existing facts (edb or prior stratum) into idb,
         * then sort and bulk-build the minimal DAFSA.  The union is a
         * no-op for pure-idb relations (start empty). */
        rel_prefix(rel, NULL, 0, ts_sink_cb, &rd[i].idb);
        ts_sort(&rd[i].idb);
        if (vm_nomaterialize && rd[i].rel_id == vm_export_relid) {
            /* Goal: hand the idb (base facts unioned, sorted) to the caller
             * instead of building a DAFSA.  idb is zeroed so section 7's
             * ts_free is a no-op; delta/next_delta still need freeing. */
            *vm_export_ts = rd[i].idb;
            memset(&rd[i].idb, 0, sizeof(rd[i].idb));
            continue;
        }
        rel_build_from_tupleset(rel, &rd[i].idb);
    }

    /* M6: materialize rebuilt the DAFSA — mark perms dirty and rebuild
     * so higher strata see fresh non-leading join indices for these
     * now-populated recursive IDB relations. */
    {
        int mi;
        for (mi = 0; mi < nr; mi++)
            permindex_mark_dirty(db, rd[mi].rel_id);
        if (permindex_build_dirty(db) != 0) {
            /* Clean up on error */
            for (mi = 0; mi < nr; mi++) {
                ts_free(&rd[mi].idb);
                ts_free(&rd[mi].delta);
                ts_free(&rd[mi].next_delta);
            }
            free(rd);
            {
                int _rdi, _pi;
                for (_rdi = 0; _rdi < nr; _rdi++) {
                    for (_pi = 0; _pi < perm_count[_rdi]; _pi++)
                        ts_free(&idb_perm_shadows[_rdi][_pi]);
                    free(perm_ids[_rdi]);
                    free(idb_perm_shadows[_rdi]);
                }
                free(perm_count); free(perm_ids); free(perm_cap);
                free(idb_perm_shadows);
            }
            #undef REBUILD_PERM_SHADOWS
            return -1;
        }
    }

    /* ── 7. Clean up ─────────────────────────────────────────────────
     * The goal relation (when exported in section 6) has its idb zeroed, so
     * ts_free below is a no-op for it; all others are freed normally. */
    for (i = 0; i < nr; i++) {
        ts_free(&rd[i].idb);
        ts_free(&rd[i].delta);
        ts_free(&rd[i].next_delta);
    }
    free(rd);

    /* M6: free perm shadows */
    {
        int _rdi, _pi;
        for (_rdi = 0; _rdi < nr; _rdi++) {
            for (_pi = 0; _pi < perm_count[_rdi]; _pi++)
                ts_free(&idb_perm_shadows[_rdi][_pi]);
            free(perm_ids[_rdi]);
            free(idb_perm_shadows[_rdi]);
        }
        free(perm_count); free(perm_ids); free(perm_cap);
        free(idb_perm_shadows);
    }
    #undef REBUILD_PERM_SHADOWS

    return 0;
}

/* ─── vm_execute ──────────────────────────────────────────────────────── */

static int vm_execute_mode(dl_db *db, compiled_rule **rules, int n_rules,
                           int ivm)
{
    int i, s, max_stratum;

    if (!db || !rules || n_rules <= 0) return 0;

    /* IVM Slice 0 (deletion-correctness): before re-evaluating, reset every
     * rule-head relation's VIEW to a copy of its BASE, dropping any stale
     * derived tuples from a prior evaluation.  Without this the evaluator is
     * ADD-ONLY: a deleted base fact would leave its previously-derived
     * tuples in the view and the fixpoint would re-union them.  The first
     * reset for a freshly-declared head also SPLITs base off from the view
     * (rel_reset_view), so subsequent base writes no longer alias the view.
     *
     * IVM Slice 2: the insert-only incremental path (ivm==1) SKIPS this reset
     * — the prior derived view is preserved and ADDED to by the delta-seeded
     * semi-naive fixpoint (correct for monotone insert-only recursion). */
    if (!ivm) {
        uint8_t seen[MAX_RELS];
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < n_rules; i++) {
            uint8_t hid = rules[i]->head_rel_id;
            relation *r;
            if (hid >= MAX_RELS) return -1;
            if (seen[hid]) continue;
            seen[hid] = 1;
            r = db_rel(db, hid);
            if (!r) return -1;
            if (rel_reset_view(r) != 0) return -1;
            /* IVM Slice 0: the view DAFSA was just replaced — mark perms dirty
             * so the permindex_build_dirty below rebuilds them from the fresh
             * copy-of-base view, not the stale OLD-view perm DAFSAs. */
            permindex_mark_dirty(db, hid);
        }
    }

    /* M6: build dirty permutation indices before evaluation */
    if (permindex_build_dirty(db) != 0) return -1;

    /* Find max stratum and check if any recursive rules exist */
    max_stratum = 0;
    {
        int any_recursive = 0;
        for (i = 0; i < n_rules; i++) {
            if (rules[i]->stratum > max_stratum)
                max_stratum = rules[i]->stratum;
            if (rules[i]->is_recursive)
                any_recursive = 1;
        }
        /* If no recursive rules at all, use the fast non-recursive path */
        if (!any_recursive)
            return eval_nonrecursive(db, rules, n_rules);
    }

    /* Evaluate stratum by stratum */
    for (s = 0; s <= max_stratum; s++) {
        /* Collect rules in this stratum */
        int sr[256], sc = 0, srec = 0;
        for (i = 0; i < n_rules; i++) {
            if (rules[i]->stratum == s) {
                if (sc < 256) sr[sc++] = i;
                if (rules[i]->is_recursive) srec = 1;
            }
        }

        if (sc == 0) continue;

        if (!srec) {
            /* Non-recursive stratum: M1 path */
            compiled_rule *strat_rules[256];
            for (i = 0; i < sc; i++)
                strat_rules[i] = rules[sr[i]];
            if (eval_nonrecursive(db, strat_rules, sc) != 0)
                return -1;
        } else {
            /* Recursive stratum: semi-naive fixpoint */
            compiled_rule *strat_rules[256];
            for (i = 0; i < sc; i++)
                strat_rules[i] = rules[sr[i]];
            if (eval_stratum_recursive(db, strat_rules, sc, ivm) != 0)
                return -1;
        }
    }

    return 0;
}

int vm_execute(dl_db *db, compiled_rule **rules, int n_rules)
{
    return vm_execute_mode(db, rules, n_rules, 0);
}

int vm_execute_ivm(dl_db *db)
{
    int i;
    if (!db) return -1;
    /* Sort pending deltas so the OP_LOOKUP override's ts_prefix binary search
     * sees sorted data (deltas are captured in insertion order, unsorted). */
    for (i = 0; i < MAX_RELS; i++)
        if (db->delta_pending[i] && db->delta_pending[i]->count > 0)
            ts_sort(db->delta_pending[i]);
    return vm_execute_mode(db, db->crules, db->n_crules, 1);
}

/* ─── vm_query ─────────────────────────────────────────────────────────── */

long vm_query(dl_db *db, compiled_rule **rules, int n_rules,
              const char *goal_rel, dl_tuple_cb cb, void *user)
{
    if (vm_execute(db, rules, n_rules) != 0) return -1;

    int gri = db_find(db, goal_rel);
    if (gri >= 0 && cb) {
        void *r = db_rel(db, gri);
        if (r) {
            long n = rel_prefix((const void *)r, NULL, 0,
                                (rel_enum_cb)cb, user);
            if (n < 0) return -1;
            return n;
        }
    }
    return 0;
}

/* ─── IVM Slice 1/2: insert-only incremental maintenance ──────────────── */
/*
 * Insert-only incremental maintenance.  Two paths, chosen by the publish
 * dispatch on whether the program contains recursive rules:
 *
 *   NON-RECURSIVE (Slice 1): a +delta on relation R is propagated through
 *     every dependent rule whose body reads R by seeding the body join with
 *     the delta as the changed body atom (exec_rule override — semi-naive
 *     single-step).  New head tuples are committed to the head view and
 *     recursively propagated through dependents (a DAG, so it terminates).
 *
 *   RECURSIVE (Slice 2): the stratified evaluator runs WITHOUT resetting the
 *     rule-head views, and each recursive stratum seeds its semi-naive
 *     fixpoint with the CURRENT VIEW (idb) + the new base facts derived from
 *     the pending insert deltas (delta).  The prior view is preserved and
 *     added to; the fixpoint is confluent and the prior view is a subset of
 *     the final result (INSERT-only / monotone), so it converges to the same
 *     view as a full from-scratch re-eval.
 *
 * Correctness is preserved BY CONSTRUCTION: any change outside this class
 * (delete, bulk load, rule load, base fact into a rule-head, or a rule with
 * negation / aggregates / OP_WALK / OP_LOOKUP_PERM / OP_HASH_JOIN) sets
 * db->full_reeval_pending and the publish path runs the FULL fixpoint (the
 * now-correct oracle).  We never propagate when in doubt.
 *
 * The override is valid only for OP_SCAN and OP_LOOKUP body atoms:
 *   - OP_SCAN  override: enumerates the delta tuple_set verbatim.         OK.
 *   - OP_LOOKUP override: ts_prefix on the delta (original column order). OK.
 *   - OP_WALK  override: IGNORED (rel_pattern always scans the DAFSA) — the
 *     delta would be silently dropped.  Excluded by vm_ivm_eligible.
 *   - OP_LOOKUP_PERM override: expects a PERMUTED-order shadow; our deltas
 *     are in original column order — a silent wrong answer.  Excluded.
 *   - OP_HASH_JOIN reads the DAFSA view (stale during the recursive fixpoint
 *     in the no-reset IVM path) — a silent wrong answer.  Excluded
 *     (defensive; the compiler does not currently emit it).
 *   - OP_NEG_CHECK / aggregates: excluded.
 */

int vm_ivm_eligible(dl_db *db)
{
    int i, k;
    uint8_t is_rule_head[MAX_RELS];
    uint8_t is_rec_head[MAX_RELS];
    int     rec_stratum[MAX_RELS];

    if (!db) return 0;

    memset(is_rule_head, 0, sizeof(is_rule_head));
    memset(is_rec_head, 0, sizeof(is_rec_head));
    for (i = 0; i < MAX_RELS; i++) rec_stratum[i] = -1;

    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        uint8_t hid = cr->head_rel_id;
        if (hid >= MAX_RELS) continue;
        is_rule_head[hid] = 1;
        if (cr->is_recursive) {
            is_rec_head[hid] = 1;
            rec_stratum[hid] = cr->stratum;
        }
    }

    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        if (cr->has_aggregate) return 0;
        for (k = 0; k < cr->n_instrs; k++) {
            uint8_t op = cr->instrs[k].op;
            if (op == OP_NEG_CHECK)   return 0;
            if (op == OP_WALK)        return 0;
            if (op == OP_LOOKUP_PERM) return 0;
            if (op == OP_HASH_JOIN)   return 0;

            /* Slice 2 recursive-delta restriction: a RECURSIVE rule's base
             * body atom that reads a RULE-HEAD relation (rather than pure EDB
             * or its own recursive SCC) cannot be tracked by delta_pending —
             * rule-head re-derivation produces no per-insert delta.  Such a
             * program (a non-recursive head or a chained recursive SCC feeding
             * this SCC) must fall back to full re-eval: never silently
             * mis-evaluate. */
            if (!cr->is_recursive) continue;
            if (op != OP_SCAN && op != OP_LOOKUP) continue;
            {
                int R = (int)cr->instrs[k].a;
                if (R < 0 || R >= MAX_RELS) continue;
                if (!is_rule_head[R]) continue;             /* pure EDB */
                if (is_rec_head[R] && rec_stratum[R] == cr->stratum)
                    continue;                               /* same SCC */
                return 0;
            }
        }
    }
    return 1;
}

int vm_has_recursive(dl_db *db)
{
    int i;
    if (!db) return 0;
    for (i = 0; i < db->n_crules; i++)
        if (db->crules[i]->is_recursive) return 1;
    return 0;
}

void vm_clear_deltas(dl_db *db)
{
    int i;
    if (!db) return;
    for (i = 0; i < MAX_RELS; i++) {
        if (db->delta_pending[i]) {
            ts_free(db->delta_pending[i]);
            free(db->delta_pending[i]);
            db->delta_pending[i] = NULL;
        }
    }
}

/* Worklist entry: a pending +delta for one relation. */
typedef struct {
    int        rel_id;
    tuple_set *ts;
} ivm_work;

/* Capture callback: each NEWLY-DERIVED head tuple (exec_rule commit mode calls
 * cb only on rel_add()==1) is sunk into a tuple_set for the next wave. */
typedef struct {
    tuple_set *ts;
    int        err;
} ivm_capture;

static int ivm_capture_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    ivm_capture *cap = (ivm_capture *)user;
    (void)arity;
    if (cap->err) return 1;               /* already failed — stop */
    if (ts_add(cap->ts, cols) < 0) { cap->err = -1; return 1; }
    return 0;
}

int vm_propagate_deltas(dl_db *db)
{
    ivm_work  *queue = NULL;
    size_t     qcap = 0, qn = 0, qh = 0;
    int        i, rc = -1;

    if (!db) return -1;

    vm_propagate_runs++;   /* test observable: prove the insert path was taken */

    /* Seed the queue from pending base deltas, transferring ownership (the
     * tuple_set structs are heap-allocated by the capture path in dl.c). */
    for (i = 0; i < (int)db->nrels; i++) {
        tuple_set *ts = db->delta_pending[i];
        if (!ts) continue;
        db->delta_pending[i] = NULL;      /* ownership moved to the queue */
        if (ts->count == 0) {
            ts_free(ts); free(ts);
            continue;
        }
        ts_sort(ts);                      /* OP_LOOKUP override needs sorted */
        if (qn == qcap) {
            size_t nc = qcap ? qcap * 2 : 16;
            ivm_work *nq = realloc(queue, nc * sizeof(ivm_work));
            if (!nq) { ts_free(ts); free(ts); goto fail; }
            queue = nq; qcap = nc;
        }
        queue[qn].rel_id = i;
        queue[qn].ts = ts;
        qn++;
    }

    while (qh < qn) {
        ivm_work w = queue[qh++];
        int ri;

        for (ri = 0; ri < db->n_crules; ri++) {
            compiled_rule *cr = db->crules[ri];
            int k;

            /* IVM Slice 4: aggregate rules are maintained by vm_agg_maintain
             * (affected-group re-scan), never by the insert worklist — a delta
             * override on their anchor would produce a WRONG partial aggregate. */
            if (cr->has_aggregate) continue;

            for (k = 0; k < cr->n_instrs; k++) {
                const vm_instr *in = &cr->instrs[k];
                uint8_t op = in->op;
                vm_override ov[1];
                ivm_capture cap;
                tuple_set *head_ts;
                relation  *hrel;
                uint8_t    harity;
                long       m;

                /* Only body atoms reading w.rel_id via an override-compatible
                 * opcode.  vm_ivm_eligible() guarantees no WALK/LOOKUP_PERM/
                 * NEG_CHECK, but stay defensive: skip anything not SCAN/LOOKUP. */
                if (op != OP_SCAN && op != OP_LOOKUP) continue;
                if ((int)in->a != w.rel_id) continue;

                hrel = db_rel(db, cr->head_rel_id);
                if (!hrel) goto fail;
                harity = rel_arity(hrel);

                head_ts = malloc(sizeof(*head_ts));
                if (!head_ts) goto fail;
                if (ts_init(head_ts, harity) != 0) { free(head_ts); goto fail; }

                ov[0].body_idx = (int)in->body_idx;
                ov[0].ts       = w.ts;
                ov[0].perm_id  = -1;

                cap.ts  = head_ts;
                cap.err = 0;

                m = exec_rule(db, cr, ov, 1, 0 /* commit */,
                              ivm_capture_cb, &cap);
                if (m < 0 || cap.err) {
                    ts_free(head_ts); free(head_ts);
                    goto fail;
                }

                if (head_ts->count > 0) {
                    /* Perm-index consistency (M6 silent-wrong-answer class):
                     * the head view just grew — mark its perms dirty and
                     * rebuild so dependent rules see fresh perm indices. */
                    permindex_mark_dirty(db, cr->head_rel_id);
                    if (permindex_build_dirty(db) != 0) {
                        ts_free(head_ts); free(head_ts);
                        goto fail;
                    }
                    ts_sort(head_ts);

                    if (qn == qcap) {
                        size_t nc = qcap ? qcap * 2 : 16;
                        ivm_work *nq = realloc(queue, nc * sizeof(ivm_work));
                        if (!nq) { ts_free(head_ts); free(head_ts); goto fail; }
                        queue = nq; qcap = nc;
                    }
                    queue[qn].rel_id = cr->head_rel_id;
                    queue[qn].ts = head_ts;
                    qn++;
                } else {
                    ts_free(head_ts); free(head_ts);
                }
            }
        }

        ts_free(w.ts);
        free(w.ts);
    }

    rc = 0;

fail:
    while (qh < qn) { ts_free(queue[qh].ts); free(queue[qh].ts); qh++; }
    free(queue);
    /* Free any delta_pending not yet seeded into the queue (OOM mid-seed). */
    vm_clear_deltas(db);
    return rc;
}

/* ─── IVM Slice 3: DRed deletion (over-delete + re-derive) ─────────────── */
/*
 * Deletion maintenance for NON-RECURSIVE programs (monotone or stratified
 * negation).  Anything else — recursion, aggregates, OP_WALK /
 * OP_LOOKUP_PERM / OP_HASH_JOIN — is rejected by vm_dred_eligible and the
 * publish path runs the FULL fixpoint (the correctness floor; DRed is an
 * optimization, never a silent mis-evaluation).
 *
 * Algorithm (per publish that has pending changes):
 *
 *   OVER-DELETE (bounded cascade, stratum-ordered):
 *     over[R] starts as the pending -delta of R (the deleted base tuples).
 *     For each rule whose body reads some S with over[S] != NULL via a
 *     POSITIVE OP_SCAN/OP_LOOKUP atom, a dry-run with over[S] substituted
 *     for that atom (the existing exec_rule override) enumerates every head
 *     tuple derivable using an over-deleted tuple — a SUPERSET of the tuples
 *     that actually lose support.  The cascade iterates to a fixpoint within
 *     each stratum (same-stratum rules form a DAG) and moves up the strata
 *     (a rule's body relations are always in stratum <= its head, so lower
 *     strata are final before higher ones cascade).
 *
 *   NEGATION-RESET (conservative, sound):
 *     Retraction through NEGATED body atoms cannot be enumerated by the
 *     positive cascade: a negatively-read relation GROWING (an insert, or a
 *     delete that unblocks a lower-stratum negation) makes !G(args) fail and
 *     silently retracts dependents.  When any change is pending and the
 *     program contains OP_NEG_CHECK, every head of a negation-containing
 *     rule is fully RESET (view = base, all derived dropped) and the reset
 *     propagates to every dependent — their inputs change wholesale.  The
 *     re-derive phase then rebuilds them exactly.  (Monotone programs skip
 *     this entirely and keep the bounded cascade.)
 *
 *   RE-DERIVE (ADD-only fixpoint over the affected cone):
 *     Relations in the transitive dependent cone of any changed relation are
 *     re-derived stratum-by-stratum with eval_nonrecursive: survivors of the
 *     over-delete are re-added, tuples newly derivable — including tuples
 *     unlocked by a negated body atom becoming TRUE after a delete — are
 *     added, and stale tuples stay deleted.  Base facts are never
 *     over-deleted (mixed EDB+IDB: delete from base; if still derivable the
 *     tuple survives in the view, else it vanishes).
 *
 * rel_delete on the view DAFSA is safe for absent keys (dafsa_delete_n
 * returns 0), so over-deleting a tuple that is not present is a no-op.
 */

int vm_dred_eligible(dl_db *db)
{
    int i, k;
    if (!db) return 0;
    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        /* Recursion: retracting a recursive SCC's mutually-dependent tuples
         * soundly is out of the DRed class here — fall back (advisor-approved
         * scope: recursive deletion uses the full re-eval). */
        if (cr->is_recursive)   return 0;
        /* Aggregates: group state (esp. min/max of a deleted extremum) cannot
         * be maintained by over-delete + re-add — fall back (Slice 4). */
        if (cr->has_aggregate) return 0;
        for (k = 0; k < cr->n_instrs; k++) {
            uint8_t op = cr->instrs[k].op;
            /* OP_NEG_CHECK is ALLOWED — see the negation-reset phase above. */
            if (op == OP_WALK)        return 0;  /* rel_pattern: no override */
            if (op == OP_LOOKUP_PERM) return 0;  /* permuted shadow: no override */
            if (op == OP_HASH_JOIN)   return 0;  /* reads DAFSA directly */
        }
    }
    return 1;
}

void vm_clear_deletes(dl_db *db)
{
    int i;
    if (!db) return;
    for (i = 0; i < MAX_RELS; i++) {
        if (db->del_pending[i]) {
            ts_free(db->del_pending[i]);
            free(db->del_pending[i]);
            db->del_pending[i] = NULL;
        }
    }
}

/* Does this rule read relation `rel_id` in a body atom (any opcode whose
 * in->a names a body relation)?  Used for the affected-cone BFS. */
static int dred_rule_reads(const compiled_rule *cr, int rel_id)
{
    int k;
    for (k = 0; k < cr->n_instrs; k++) {
        const vm_instr *in = &cr->instrs[k];
        switch (in->op) {
        case OP_SCAN: case OP_LOOKUP: case OP_NEG_CHECK:
        case OP_WALK: case OP_LOOKUP_PERM: case OP_HASH_JOIN:
            if ((int)in->a == rel_id) return 1;
            break;
        default:
            break;
        }
    }
    return 0;
}

int vm_dred_delete(dl_db *db)
{
    tuple_set *over[MAX_RELS];       /* over-delete set per relation (owned) */
    uint8_t    full_reset[MAX_RELS]; /* 1 = drop ALL derived, re-derive */
    uint8_t    changed[MAX_RELS];    /* 1 = has pending +/- delta */
    uint8_t    affected[MAX_RELS];   /* 1 = in the affected dependent cone */
    uint8_t    is_head[MAX_RELS];
    uint8_t    has_negation = 0;
    int        max_stratum = 0;
    int        any_change = 0;
    int        i, k, s, rc = -1;

    if (!db) return -1;
    vm_dred_runs++;   /* test observable: prove the DRed path was taken */

    memset(over, 0, sizeof(over));
    memset(full_reset, 0, sizeof(full_reset));
    memset(changed, 0, sizeof(changed));
    memset(affected, 0, sizeof(affected));
    memset(is_head, 0, sizeof(is_head));

    /* 0. Sort pending insert deltas (the OP_LOOKUP override binary-searches
     * sorted data; deletes get sorted below when seeded into over[]). */
    for (i = 0; i < MAX_RELS; i++) {
        if (db->delta_pending[i] && db->delta_pending[i]->count > 0) {
            ts_sort(db->delta_pending[i]);
            if (i < (int)db->nrels) changed[i] = 1;
            any_change = 1;
        }
        if (db->del_pending[i] && db->del_pending[i]->count > 0) {
            if (i < (int)db->nrels) changed[i] = 1;
            any_change = 1;
        }
    }

    /* Head bookkeeping + does the program contain negation anywhere? */
    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        int hid = cr->head_rel_id;
        if (cr->has_aggregate) continue;   /* Slice 4: maintained separately */
        if (hid < MAX_RELS) is_head[hid] = 1;
        if (cr->stratum > max_stratum) max_stratum = cr->stratum;
        for (k = 0; k < cr->n_instrs; k++)
            if (cr->instrs[k].op == OP_NEG_CHECK) { has_negation = 1; break; }
    }

    /* 1. Seed over[R] = del_pending[R] (positive-cascade seed: the directly
     *    deleted base tuples). */
    for (i = 0; i < (int)db->nrels; i++) {
        long ci;
        if (!db->del_pending[i] || db->del_pending[i]->count == 0) continue;
        over[i] = malloc(sizeof(tuple_set));
        if (!over[i]) goto out;
        if (ts_init(over[i], rel_arity(db->rels[i].rel)) != 0) {
            free(over[i]); over[i] = NULL; goto out;
        }
        for (ci = 0; ci < db->del_pending[i]->count; ci++)
            ts_add(over[i], db->del_pending[i]->data +
                   ci * db->del_pending[i]->arity);
        ts_sort(over[i]);   /* OP_LOOKUP override needs sorted data */
    }

    /* 2. Negation-reset seed + propagation.  Any pending change can grow a
     *    negatively-read relation (an insert directly, or a delete that
     *    unblocks a lower-stratum negation and adds derived tuples), so when
     *    negation is present EVERY head of a negation-containing rule takes
     *    the conservative full reset.  Propagate to dependents: a rule
     *    reading a reset relation sees its inputs change wholesale. */
    if (has_negation && any_change) {
        for (i = 0; i < db->n_crules; i++) {
            const compiled_rule *cr = db->crules[i];
            int hid = cr->head_rel_id;
            if (cr->has_aggregate) continue;   /* Slice 4: maintained separately */
            if (hid >= MAX_RELS) continue;
            for (k = 0; k < cr->n_instrs; k++)
                if (cr->instrs[k].op == OP_NEG_CHECK) { full_reset[hid] = 1; break; }
        }
        {
            int again = 1;
            while (again) {
                again = 0;
                for (i = 0; i < db->n_crules; i++) {
                    const compiled_rule *cr = db->crules[i];
                    int hid = cr->head_rel_id;
                    if (cr->has_aggregate) continue;  /* Slice 4 */
                    if (hid >= MAX_RELS || full_reset[hid]) continue;
                    for (k = 0; k < cr->n_instrs; k++) {
                        const vm_instr *in = &cr->instrs[k];
                        switch (in->op) {
                        case OP_SCAN: case OP_LOOKUP: case OP_NEG_CHECK:
                        case OP_WALK: case OP_LOOKUP_PERM: case OP_HASH_JOIN:
                            if (in->a < MAX_RELS && full_reset[in->a]) {
                                full_reset[hid] = 1;
                                again = 1;
                            }
                            break;
                        default:
                            break;
                        }
                        if (full_reset[hid]) break;
                    }
                }
            }
        }
    }

    /* 3. Bounded over-delete cascade: stratum by stratum, to a fixpoint
     *    within each stratum.  Heads marked full_reset are skipped (their
     *    whole derived view is dropped anyway); cascading THROUGH a
     *    full_reset body relation is skipped too (its dependents are reset
     *    by propagation). */
    for (s = 0; s <= max_stratum; s++) {
        int again = 1;
        while (again) {
            again = 0;
            for (i = 0; i < db->n_crules; i++) {
                const compiled_rule *cr = db->crules[i];
                int hid = cr->head_rel_id;
                if (cr->has_aggregate) continue;  /* Slice 4: no override */
                if (cr->stratum != s) continue;
                if (hid >= MAX_RELS || full_reset[hid]) continue;
                for (k = 0; k < cr->n_instrs; k++) {
                    const vm_instr *in = &cr->instrs[k];
                    int br;
                    long m;
                    tuple_set cand;
                    cand_ctx ctx;
                    long ci;
                    if (in->op != OP_SCAN && in->op != OP_LOOKUP) continue;
                    br = (int)in->a;
                    if (br < 0 || br >= MAX_RELS) continue;
                    if (!over[br] || over[br]->count == 0) continue;
                    if (full_reset[br]) continue;

                    {
                        vm_override ov[1];
                        ov[0].body_idx = (int)in->body_idx;
                        ov[0].ts       = over[br];
                        ov[0].perm_id  = -1;
                        if (ts_init(&cand, rel_arity(db->rels[hid].rel)) != 0)
                            goto out;
                        ctx.ts = &cand;
                        m = exec_rule(db, cr, ov, 1,
                                      1 /* dry */, cand_cb, &ctx);
                    }
                    if (m < 0) { ts_free(&cand); goto out; }
                    for (ci = 0; ci < cand.count; ci++) {
                        const uint32_t *t = cand.data + ci * cand.arity;
                        if (!over[hid]) {
                            over[hid] = malloc(sizeof(tuple_set));
                            if (!over[hid]) { ts_free(&cand); goto out; }
                            if (ts_init(over[hid], cand.arity) != 0) {
                                free(over[hid]); over[hid] = NULL;
                                ts_free(&cand); goto out;
                            }
                        }
                        {
                            int ar = ts_add(over[hid], t);
                            if (ar < 0) { ts_free(&cand); goto out; }
                            if (ar == 1) again = 1;
                        }
                    }
                    ts_free(&cand);
                    /* Keep over[hid] sorted so a LATER rule in the same
                     * stratum iteration can read it as an OP_LOOKUP override
                     * via ts_prefix (binary search).  The in-stratum fixpoint
                     * grows over[] unsorted; without this, ts_prefix silently
                     * misses tuples and the over-delete cascade is incomplete
                     * -> stale survivors (silent wrong answer).  ts_sort is a
                     * no-op for count<=1 and idempotent (ts_add dedups). */
                    if (over[hid] && over[hid]->count > 1)
                        ts_sort(over[hid]);
                }
            }
        }
        /* Keep every grown over[] sorted for the next stratum's overrides. */
        for (i = 0; i < MAX_RELS; i++)
            if (over[i] && over[i]->count > 0) ts_sort(over[i]);
    }

    /* 4. Apply over-delete to views.  BASE tuples are never over-deleted
     *    (they are durable); rel_delete of an absent key is a safe no-op. */
    for (i = 0; i < (int)db->nrels; i++) {
        long ci;
        int dirtied = 0;
        relation *rel;
        if (!over[i] || over[i]->count == 0) continue;
        if (full_reset[i]) continue;         /* reset drops everything below */
        rel = db->rels[i].rel;
        for (ci = 0; ci < over[i]->count; ci++) {
            const uint32_t *t = over[i]->data + ci * over[i]->arity;
            if (rel_exact_base(rel, t)) continue;   /* durable base fact */
            if (rel_delete(rel, t) == 1) dirtied = 1;
        }
        if (dirtied) permindex_mark_dirty(db, i);
    }

    /* 5. Apply full-reset: drop ALL derived tuples (view = copy of base). */
    for (i = 0; i < (int)db->nrels; i++) {
        if (!full_reset[i] || !is_head[i]) continue;
        if (rel_reset_view(db->rels[i].rel) != 0) goto out;
        permindex_mark_dirty(db, i);
    }
    if (permindex_build_dirty(db) != 0) goto out;

    /* 6. Affected cone: BFS from every changed / over-deleted / reset
     *    relation through the rule dependency graph. */
    {
        int queue[MAX_RELS], qh = 0, qt = 0;
        for (i = 0; i < MAX_RELS; i++) {
            if (changed[i] || full_reset[i]
                || (over[i] && over[i]->count > 0)) {
                affected[i] = 1;
                if (qt < MAX_RELS) queue[qt++] = i;
            }
        }
        while (qh < qt) {
            int r = queue[qh++];
            for (i = 0; i < db->n_crules; i++) {
                const compiled_rule *cr = db->crules[i];
                int hid = cr->head_rel_id;
                if (cr->has_aggregate) continue;  /* Slice 4: not in cone */
                if (hid >= MAX_RELS || affected[hid]) continue;
                if (dred_rule_reads(cr, r)) {
                    affected[hid] = 1;
                    if (qt < MAX_RELS) queue[qt++] = hid;
                }
            }
        }
    }

    /* 7. Re-derive: run the affected rules of each stratum to a fixpoint.
     *    ADD-only: over-deleted survivors come back, newly-derivable tuples
     *    (incl. negation-unlocked ones) are added, stale tuples stay gone.
     *    Stratum order guarantees lower strata are final before higher ones
     *    read them.  Rules whose head is outside the affected cone are
     *    skipped — none of their body inputs changed (by the BFS closure). */
    for (s = 0; s <= max_stratum; s++) {
        compiled_rule *srules[256];
        int sc = 0;
        for (i = 0; i < db->n_crules; i++) {
            const compiled_rule *cr = db->crules[i];
            if (cr->has_aggregate) continue;  /* Slice 4: re-scanned separately */
            if (cr->stratum != s) continue;
            if (cr->head_rel_id < MAX_RELS && !affected[cr->head_rel_id])
                continue;
            if (sc < 256) srules[sc++] = db->crules[i];
        }
        if (sc == 0) continue;
        if (eval_nonrecursive(db, srules, sc) != 0) goto out;
    }

    rc = 0;

out:
    for (i = 0; i < MAX_RELS; i++) {
        if (over[i]) { ts_free(over[i]); free(over[i]); }
    }
    /* Consume the pending changes even on failure: the caller forces a full
     * re-eval on the next publish, which resets every head view to base and
     * recomputes from scratch — the partially-updated views are irrelevant. */
    vm_clear_deltas(db);
    vm_clear_deletes(db);
    return rc;
}

/* ─── IVM Slice 4: aggregates under change ─────────────────────────────── */
/*
 * Aggregates (count/sum/min/max) are non-recursive by construction (the
 * compiler rejects aggregates in recursive rules) and are maintained here
 * by AFFECTED-GROUP RE-SCAN — a single uniform mechanism for all four ops:
 *
 *   On a +delta/-delta of a base relation R that an aggregate rule's anchor
 *   reads, project the delta onto the leading group columns (the group key is
 *   a prefix of the anchor, guaranteed by vm_agg_eligible), collect the
 *   distinct affected group keys, and for EACH re-scan that group over the
 *   CURRENT base (rel_prefix on the leading group columns — O(group), not
 *   O(relation)) recomputing count/sum/min/max.  The head view is then
 *   updated in place: delete the stale head tuple, add the recomputed one, or
 *   drop the head tuple entirely when the group became empty.
 *
 * This uniform re-scan handles the hard cases for free: a deleted extremum is
 * re-found by the re-scan (min/max); an emptied group drops its tuple; a
 * newly-created group materializes; sum wraps exactly like the full eval.
 *
 * Why re-scan instead of a persisted per-group accumulator?  It is uniformly
 * correct for all four ops (no count/sum-vs-min/max special casing), avoids a
 * new persisted per-group data structure (its own hashing, lifecycle, and
 * serialization), and O(group) is the natural cost for the common small-group
 * aggregate workload.  Anything a tractable re-scan cannot cover (joins,
 * filters, negation, non-prefix groups, result-var-not-last heads, derived
 * anchors, non-terminal heads, mixed EDB+IDB heads) is rejected by
 * vm_agg_eligible and falls back to the full fixpoint — NEVER a silent
 * mis-evaluation.
 */

typedef struct {
    uint8_t  op;        /* 0 count, 1 sum, 2 min, 3 max */
    uint8_t  src_col;   /* anchor column of the source var (unused for count) */
    uint32_t count;
    uint32_t sum;
    uint32_t min;
    uint32_t max;
} agg_scan;

static int agg_scan_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    agg_scan *st = (agg_scan *)user;
    (void)arity;
    st->count++;
    if (st->op == 1) {
        st->sum += cols[st->src_col];          /* u32 wrap, like OP_AGG_ACC */
    } else if (st->op == 2) {
        uint32_t v = cols[st->src_col];
        if (st->count == 1) st->min = v;
        else if (v < st->min) st->min = v;
    } else if (st->op == 3) {
        uint32_t v = cols[st->src_col];
        if (st->count == 1) st->max = v;
        else if (v > st->max) st->max = v;
    }
    return 0;
}

typedef struct {
    uint32_t cols[8];
    int      found;
} agg_old;

static int agg_old_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    agg_old *oh = (agg_old *)user;
    if (!oh->found && arity >= 1 && arity <= 8) {
        memcpy(oh->cols, cols, (size_t)arity * sizeof(uint32_t));
        oh->found = 1;
    }
    return 0;
}

static int agg_count_cb(const uint32_t *cols, uint8_t arity, void *user)
{
    long *n = (long *)user;
    (void)cols; (void)arity;
    (*n)++;
    return 0;
}

/* 1 if the relation has NO base facts (pure derived view), 0 if it has base
 * facts, -1 on error.  Used to reject mixed EDB+IDB aggregate heads. */
static int rel_base_empty(dl_db *db, int rel_id)
{
    relation *r = db_rel(db, rel_id);
    long n = 0;
    if (!r) return -1;
    if (rel_prefix_base(r, NULL, 0, agg_count_cb, &n) < 0) return -1;
    return (n == 0) ? 1 : 0;
}

/* 1 if this aggregate rule is incrementally maintainable (see vm_agg_eligible
 * in vm.h for the full contract).  is_head[rel_id] marks every rule head. */
static int agg_rule_tractable(dl_db *db, const compiled_rule *cr,
                              const uint8_t *is_head)
{
    const vm_instr *scan, *acc, *emit;
    int anchor, anchor_arity, n_key, op, res_slot, head_arity;
    int g, c;

    /* Exact minimal bytecode shape [SCAN, AGG_ACC, AGG_EMIT, HALT]: a single
     * positive scan anchor, no joins/filters/negation/patterns. */
    if (cr->n_instrs != 4) return 0;
    if (cr->instrs[0].op != OP_SCAN     ||
        cr->instrs[1].op != OP_AGG_ACC  ||
        cr->instrs[2].op != OP_AGG_EMIT ||
        cr->instrs[3].op != OP_HALT) return 0;

    scan = &cr->instrs[0];
    acc  = &cr->instrs[1];
    emit = &cr->instrs[2];

    anchor       = (int)scan->a;
    anchor_arity = (int)scan->b;
    n_key        = (int)acc->a;
    op           = (int)acc->b;
    res_slot     = (int)acc->c;
    head_arity   = (int)emit->b;

    /* Anchor must be a pure EDB relation (not a rule head): a derived anchor
     * produces no base delta, so group re-scan cannot track it. */
    if (anchor < 0 || anchor >= MAX_RELS) return 0;
    if (anchor >= (int)db->nrels) return 0;
    if (is_head[anchor]) return 0;

    /* The anchor must bind each column to a DISTINCT variable.  A repeated
     * variable (e.g. pair(X,X)) is an intra-anchor equality constraint that
     * rel_prefix(anchor, key, n_key) cannot enforce: the group re-scan would
     * enumerate tuples the full scan rejects (col0==col1), silently
     * over-counting the group.  Reject any shape with a shared slot. */
    for (c = 0; c < anchor_arity; c++) {
        uint8_t sc = scan->slots[c];
        if (sc == 0xFF) continue;   /* unbound column - no constraint */
        for (g = c + 1; g < anchor_arity; g++)
            if (scan->slots[g] == sc) return 0;
    }

    /* Group columns must be a leading prefix of the anchor IN GROUP-KEY
     * ORDER (column g binds group slot g), so rel_prefix(anchor, key, n_key)
     * enumerates exactly the group. */
    if (n_key > anchor_arity) return 0;
    for (g = 0; g < n_key; g++)
        if (scan->slots[g] != acc->slots[g]) return 0;

    /* sum/min/max: the source var must be bound by a column of the anchor. */
    if (op != 0) {
        int src_slot = (int)acc->slots[n_key];
        for (c = 0; c < anchor_arity; c++)
            if ((int)scan->slots[c] == src_slot) break;
        if (c == anchor_arity) return 0;
    }

    /* Head mapping: group columns are the leading head columns (group-key
     * order), result var LAST — so the head tuple is (g0..g_{k-1}, val) and
     * rel_prefix(head, key, n_key) finds the old tuple directly. */
    if (head_arity != n_key + 1) return 0;
    for (g = 0; g < n_key; g++)
        if (emit->slots[g] != acc->slots[g]) return 0;
    if (emit->slots[n_key] != (uint8_t)res_slot) return 0;

    /* Head must be a pure derived view (no base facts). */
    if (rel_base_empty(db, (int)emit->a) != 1) return 0;

    return 1;
}

int vm_agg_eligible(dl_db *db)
{
    uint8_t is_head[MAX_RELS];
    uint8_t agg_head[MAX_RELS];
    int i, k, has_agg = 0;

    if (!db) return 0;

    memset(is_head, 0, sizeof(is_head));
    memset(agg_head, 0, sizeof(agg_head));

    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        if (cr->is_recursive) return 0;
        if (cr->has_aggregate) has_agg = 1;
        if (cr->head_rel_id < MAX_RELS) is_head[cr->head_rel_id] = 1;
    }
    if (!has_agg) return 0;   /* not an aggregate program */

    for (i = 0; i < db->n_crules; i++)
        if (db->crules[i]->has_aggregate &&
            db->crules[i]->head_rel_id < MAX_RELS)
            agg_head[db->crules[i]->head_rel_id] = 1;

    /* An aggregate head must be produced by EXACTLY ONE rule.  The affected-
     * group re-scan updates the head in place assuming one head tuple per
     * group key (rel_prefix finds "the" old tuple); two rules sharing a head
     * (two aggregates, or an aggregate + a non-aggregate) each re-scan and
     * clobber the other's tuple under union semantics.  Reject. */
    {
        int head_count[MAX_RELS];
        memset(head_count, 0, sizeof(head_count));
        for (i = 0; i < db->n_crules; i++) {
            const compiled_rule *cr = db->crules[i];
            if (cr->head_rel_id < MAX_RELS) head_count[cr->head_rel_id]++;
        }
        for (i = 0; i < MAX_RELS; i++)
            if (agg_head[i] && head_count[i] > 1) return 0;
    }

    /* Aggregate heads must be TERMINAL: no rule body reads one. */
    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        for (k = 0; k < cr->n_instrs; k++) {
            uint8_t op = cr->instrs[k].op;
            switch (op) {
            case OP_SCAN: case OP_LOOKUP: case OP_NEG_CHECK:
            case OP_WALK: case OP_LOOKUP_PERM: case OP_HASH_JOIN:
                if (cr->instrs[k].a < MAX_RELS &&
                    agg_head[cr->instrs[k].a]) return 0;
                break;
            default:
                break;
            }
        }
    }

    /* Non-aggregate rules must be free of override-incompatible opcodes
     * (they are maintained by DRed / the insert propagator). */
    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        if (cr->has_aggregate) continue;
        for (k = 0; k < cr->n_instrs; k++) {
            uint8_t op = cr->instrs[k].op;
            if (op == OP_WALK || op == OP_LOOKUP_PERM || op == OP_HASH_JOIN)
                return 0;
        }
    }

    /* Every aggregate rule must be tractable. */
    for (i = 0; i < db->n_crules; i++) {
        const compiled_rule *cr = db->crules[i];
        if (!cr->has_aggregate) continue;
        if (!agg_rule_tractable(db, cr, is_head)) return 0;
    }

    return 1;
}

/* Recompute one aggregate group (or the whole relation for a global
 * aggregate) over the CURRENT base and update the head view in place. */
static int agg_update_group(dl_db *db, const compiled_rule *cr,
                            const uint32_t *group_key)
{
    const vm_instr *scan = &cr->instrs[0];
    const vm_instr *acc  = &cr->instrs[1];
    const vm_instr *emit = &cr->instrs[2];
    int anchor = (int)scan->a;
    int anchor_arity = (int)scan->b;
    int n_key = (int)acc->a;
    int op = (int)acc->b;
    int head = (int)emit->a;
    relation *arel = db_rel(db, anchor);
    relation *hrel = db_rel(db, head);
    agg_scan st;
    agg_old  oh;
    int c, src_col = 0;

    if (!arel || !hrel) return -1;

    if (op != 0) {
        int src_slot = (int)acc->slots[n_key];
        src_col = -1;
        for (c = 0; c < anchor_arity; c++)
            if ((int)scan->slots[c] == src_slot) { src_col = c; break; }
        if (src_col < 0) return -1;   /* eligibility should have caught this */
    }

    memset(&st, 0, sizeof(st));
    st.op = (uint8_t)op;
    st.src_col = (uint8_t)src_col;

    if (rel_prefix(arel, group_key, (uint8_t)n_key, agg_scan_cb, &st) < 0)
        return -1;

    memset(&oh, 0, sizeof(oh));
    if (rel_prefix(hrel, group_key, (uint8_t)n_key, agg_old_cb, &oh) < 0)
        return -1;

    if (st.count == 0) {
        /* Group vanished: drop the old head tuple (if present). */
        if (oh.found && rel_delete(hrel, oh.cols) < 0) return -1;
        return 0;
    }

    {
        uint32_t agg_val, new_tuple[8];
        int gi;
        switch (op) {
        case 1: agg_val = st.sum;  break;
        case 2: agg_val = st.min;  break;
        case 3: agg_val = st.max;  break;
        default: agg_val = st.count; break;
        }
        for (gi = 0; gi < n_key; gi++) new_tuple[gi] = group_key[gi];
        new_tuple[n_key] = agg_val;

        if (rel_exact(hrel, new_tuple)) return 0;   /* unchanged */
        if (oh.found && rel_delete(hrel, oh.cols) < 0) return -1;
        if (rel_add(hrel, new_tuple) < 0) return -1;
    }
    return 0;
}

/* Collect the affected group keys (project the pending +/- deltas onto the
 * leading group columns) and re-scan each.  A global aggregate (n_key==0) is
 * a single group re-scanned whenever the anchor changed at all. */
static int agg_maintain_rule(dl_db *db, const compiled_rule *cr)
{
    const vm_instr *scan = &cr->instrs[0];
    const vm_instr *acc  = &cr->instrs[1];
    int anchor = (int)scan->a;
    int n_key  = (int)acc->a;
    tuple_set groups;
    long gi;
    int which;

    if (n_key == 0) {
        int changed = (db->delta_pending[anchor] &&
                       db->delta_pending[anchor]->count > 0)
                   || (db->del_pending[anchor] &&
                       db->del_pending[anchor]->count > 0);
        if (!changed) return 0;
        return agg_update_group(db, cr, NULL);
    }

    if (ts_init(&groups, (uint8_t)n_key) != 0) return -1;

    for (which = 0; which < 2; which++) {
        tuple_set *ts = (which == 0) ? db->delta_pending[anchor]
                                     : db->del_pending[anchor];
        if (!ts || ts->count == 0) continue;
        for (gi = 0; gi < ts->count; gi++) {
            const uint32_t *t = ts->data + gi * ts->arity;
            if (ts_add(&groups, t) < 0) { ts_free(&groups); return -1; }
        }
    }

    if (groups.count == 0) { ts_free(&groups); return 0; }
    ts_sort(&groups);

    for (gi = 0; gi < groups.count; gi++) {
        const uint32_t *g = groups.data + gi * groups.arity;
        if (agg_update_group(db, cr, g) != 0) { ts_free(&groups); return -1; }
    }
    ts_free(&groups);
    return 0;
}

int vm_agg_maintain(dl_db *db)
{
    int ri, i;
    int has_ins = 0, has_del = 0, has_nonagg = 0, has_neg = 0;

    if (!db) return -1;
    vm_agg_runs++;

    /* 1. Aggregate rules: affected-group re-scan + head update (READS, does
     *    not consume, the pending deltas). */
    for (ri = 0; ri < db->n_crules; ri++) {
        const compiled_rule *cr = db->crules[ri];
        if (!cr->has_aggregate) continue;
        if (agg_maintain_rule(db, cr) != 0) goto fail;
    }

    /* 2. Perm-index consistency: updated aggregate heads mark perms dirty
     *    (defensive — aggregate heads are terminal, but stay consistent with
     *    the M6 silent-wrong-answer guard). */
    for (ri = 0; ri < db->n_crules; ri++) {
        if (!db->crules[ri]->has_aggregate) continue;
        permindex_mark_dirty(db, db->crules[ri]->head_rel_id);
    }
    if (permindex_build_dirty(db) != 0) goto fail;

    /* 3. Non-aggregate part: delegate to DRed (deletes / negation) or the
     *    insert propagator — both skip aggregate rules. */
    for (i = 0; i < MAX_RELS; i++) {
        if (db->delta_pending[i] && db->delta_pending[i]->count > 0) has_ins = 1;
        if (db->del_pending[i] && db->del_pending[i]->count > 0) has_del = 1;
    }
    for (ri = 0; ri < db->n_crules; ri++) {
        const compiled_rule *cr = db->crules[ri];
        int k;
        if (cr->has_aggregate) continue;
        has_nonagg = 1;
        for (k = 0; k < cr->n_instrs; k++)
            if (cr->instrs[k].op == OP_NEG_CHECK) { has_neg = 1; break; }
    }

    if (!has_nonagg || (!has_ins && !has_del)) {
        vm_clear_deltas(db);
        vm_clear_deletes(db);
        return 0;
    }
    if (has_del || (has_ins && has_neg)) {
        if (vm_dred_delete(db) != 0) goto fail;
    } else if (has_ins) {
        if (vm_propagate_deltas(db) != 0) goto fail;
    }
    return 0;

fail:
    vm_clear_deltas(db);
    vm_clear_deletes(db);
    return -1;
}
