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
 *                         EQ, EQ_CONST, PROJECT, NEG_CHECK, HALT)
 *   eval_nonrecursive() — M1 path: one exec_rule per rule, commits to DAFSA
 *   eval_stratum_recursive() — semi-naive fixpoint using tuple_sets
 *   vm_execute()        — stratify + dispatch
 *   vm_query()          — thin wrapper: execute then stream goal via rel_prefix
 */
#include "vm.h"
#include "relation.h"
#include "regexwalk.h"
#include "tupleset.h"
#include "intern.h"
#include "permindex.h"
#include "snapshot.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── dl_db internal access (must match dl.c prefix) ─────────────────── */

struct dl_db_internal {
    char *dir; void *ir;
    struct { char *name; void *rel; } rels[64];
    size_t nrels;
    void *crules; int n_crules;
    int fixpoint_dirty; uint32_t snap_version;
    view_cache_slot vcache[DL_VIEW_CACHE_SZ];
    void *fault_hook; void *fault_user;
    perm_index_entry perms[MAX_PERMS];
    int n_perms;
};

static void *db_rel(dl_db *db, int idx)
{
    struct dl_db_internal *d = (struct dl_db_internal *)db;
    if (idx < 0 || (size_t)idx >= d->nrels) return NULL;
    return d->rels[idx].rel;
}

static int db_find(dl_db *db, const char *name)
{
    struct dl_db_internal *d = (struct dl_db_internal *)db;
    size_t i;
    for (i = 0; i < d->nrels; i++)
        if (!strcmp(d->rels[i].name, name)) return (int)i;
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
    int i;
    for (i = 0; i < n; i++) {
        long m = exec_rule(db, rules[i], NULL, 0,
                           0 /* commit */, NULL, NULL);
        if (m < 0) return -1;
        (void)m;
        /* M6: IDB DAFSA was populated — mark perms dirty and rebuild
         * so subsequent rules (even within the same stratum) see fresh
         * perm indices for non-leading joins. */
        permindex_mark_dirty(db, rules[i]->head_rel_id);
        if (permindex_build_dirty(db) != 0) return -1;
    }
    return 0;
}

/* ─── Recursive stratum evaluation (semi-naive fixpoint) ───────────────── */

#define FIXPOINT_ERROR_BOUND 10000000

static int eval_stratum_recursive(dl_db *db, compiled_rule **rules, int n)
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
        struct dl_db_internal *di = (struct dl_db_internal *)db;
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
                struct dl_db_internal *_di = (struct dl_db_internal *)db; \
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
        } else {
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
                            /* Full IDB */
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

                    /* Add new tuples to idb and next_delta */
                    {
                        long ci;
                        for (ci = 0; ci < cand.count; ci++) {
                            const uint32_t *t = cand.data +
                                ci * cand.arity;
                            if (!ts_contains(&rd[hdi].idb, t)) {
                                ts_add(&rd[hdi].idb, t);
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
                /* Note: idb is NOT sorted here — it's only used via
                 * ts_contains/ts_add (hash, O(1)) during the fixpoint.
                 * Sorting 125K tuples 500× was the bottleneck. */
            }
        }
    }

    /* ── 6. Materialize: bulk-write idb to DAFSA ───────────────────── */
    for (i = 0; i < nr; i++) {
        relation *rel = (relation *)db_rel(db, rd[i].rel_id);
        if (!rel) continue;

        /* Union pre-existing facts (edb or prior stratum) into idb,
         * then sort and bulk-build the minimal DAFSA.  The union is a
         * no-op for pure-idb relations (start empty). */
        rel_prefix(rel, NULL, 0, ts_sink_cb, &rd[i].idb);
        ts_sort(&rd[i].idb);
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

    /* ── 7. Clean up ───────────────────────────────────────────────── */
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

int vm_execute(dl_db *db, compiled_rule **rules, int n_rules)
{
    int i, s, max_stratum;

    if (!db || !rules || n_rules <= 0) return 0;

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
            if (eval_stratum_recursive(db, strat_rules, sc) != 0)
                return -1;
        }
    }

    return 0;
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
