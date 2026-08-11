/*
 * tupleset.h — In-memory sorted tuple set with O(1) membership
 *
 * A tuple_set is a hash set + sorted array of arity-strided u32 tuples.
 * Hash set (open addressing, FNV-1a) provides O(1) ts_contains / ts_add
 * dedup.  The sorted array supports O(log N) ts_prefix binary search
 * for leading-column prefix enumeration (the Datalog join access pattern).
 *
 * Used by the M2 semi-naive fixpoint to hold IDB relations and deltas
 * in memory during evaluation, avoiding pathological DAFSA clone-on-write
 * churn.  Each IDB DAFSA is bulk-built from the sorted tuple_set ONCE
 * at stratum end.
 */
#ifndef TUPLESET_H
#define TUPLESET_H

#include <stdint.h>
#include <stddef.h>

typedef struct tuple_set {
    uint32_t *data;      /* sorted unique tuples, arity-strided */
    long      count;     /* number of tuples */
    long      cap;       /* capacity in tuples */
    uint8_t   arity;

    /* Open-addressing hash set: slot = 1-based index into data[], 0=empty */
    uint32_t *htab;
    size_t    hcap;      /* capacity (power of two) */
    size_t    hused;     /* occupied slots */
} tuple_set;

/* Initialize an empty tuple set for the given arity (1-8). */
int  ts_init(tuple_set *ts, uint8_t arity);

/* Free all resources. */
void ts_free(tuple_set *ts);

/* O(1) membership test via hash set.  Returns 1 if present, 0 if absent. */
int  ts_contains(const tuple_set *ts, const uint32_t *cols);

/* Add a tuple.  Consults hash set as authority on uniqueness BEFORE
 * appending to the sorted array (so the array never holds duplicates).
 * Returns 1 if added (new), 0 if duplicate, -1 on error. */
int  ts_add(tuple_set *ts, const uint32_t *cols);

/* Binary search on the sorted array for the contiguous range of tuples
 * whose leading k columns equal p[0..k-1].  Returns the count of matching
 * tuples and sets first_idx_out[0] to the first matching index.
 * Returns 0 and sets first_idx_out[0]=0 if no match.
 * The array MUST be sorted (call ts_sort first). */
long ts_prefix(const tuple_set *ts, const uint32_t *p, uint8_t k,
               long first_idx_out[1]);

/* Sort the data array and remove duplicates (safety net). */
void ts_sort(tuple_set *ts);

/* Reset to empty, keeping allocated capacity. */
void ts_reset(tuple_set *ts);

#endif /* TUPLESET_H */
