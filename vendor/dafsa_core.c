/* dafsa_core.c — Core DAFSA algorithms */
#include "dafsa_internal.h"
/* ─── Incoming-edge tracking ───────────────────────────────────────────── */

void incoming_add(dafsa *d, unsigned int src, unsigned char c,
                          unsigned int dst)
{
    Inode *in = inode_alloc(d);   /* may realloc d->inodes */
    in->parent = src;
    in->sym = c;
    in->next  = d->states[dst].in_head;   /* re-fetched by index, safe */
    d->states[dst].in_head = d->inodes_used;
    d->states[dst].refcount++;
}

/* Redirect ALL incoming edges that point to old_tgt to point to new_tgt.
 * Also fix up the parents' transition tables.
 *
 * IMPORTANT: This function holds State* and Inode* across its loop body
 * but no realloc-triggering call is made inside the loop, so they remain
 * valid. Callers must re-fetch any State* / Inode* they hold across the
 * call to this function (which they do -- callers use indices). */
void incoming_redirect(dafsa *d, unsigned int old_tgt,
                               unsigned int new_tgt)
{
    State *old_s = &d->states[old_tgt];  /* fetched once; no realloc in body */
    State *new_s = &d->states[new_tgt];
    unsigned int ni = old_s->in_head;

    while (ni != 0) {
        Inode *in = &d->inodes[ni];
        State  *parent = &d->states[in->parent];
        int pos;

        /* update parent's transition */
        pos = trans_find(parent, in->sym);
        assert(pos >= 0);
        trans_arr(parent)[pos].target = new_tgt;
        parent->sig = 0;  /* invalidate, will recompute */

        /* update refcounts */
        old_s->refcount--;
        new_s->refcount++;

        /* move the inode to new_tgt's list */
        {
            unsigned int next = in->next;
            in->next = new_s->in_head;
            new_s->in_head = ni;
            ni = next;
        }
    }
    old_s->in_head = 0;

    /* If this state is now an orphan, free it for reuse */
    if (old_s->refcount == 0 && old_tgt != d->initial)
        state_free(d, old_tgt);
}

/* Redirect a single incoming edge: parent's transition via sym from
 * old_tgt to new_tgt. Updates the transition table, inode list, and
 * refcounts. Used during clone-on-write.
 *
 * Pointer safety: accesses d->states[] and d->inodes[] by index/re-fetch
 * each iteration. No realloc-triggering call in the loop body. */
void incoming_redirect_one(dafsa *d, unsigned int parent,
                                   unsigned char sym,
                                   unsigned int old_tgt,
                                   unsigned int new_tgt)
{
    int pos;

    /* update parent's transition */
    pos = trans_find(&d->states[parent], sym);
    assert(pos >= 0 && trans_arr(&d->states[parent])[pos].target == old_tgt);
    trans_arr(&d->states[parent])[pos].target = new_tgt;
    d->states[parent].sig = 0;

    /* update refcounts */
    d->states[old_tgt].refcount--;
    d->states[new_tgt].refcount++;

    /* move the inode from old_tgt's list to new_tgt's list */
    {
        unsigned int *prev_ptr = &d->states[old_tgt].in_head;
        unsigned int ni = *prev_ptr;

        while (ni != 0) {
            /* Re-fetch inode each iteration (safe -- no realloc in loop) */
            Inode *in = &d->inodes[ni];
            if (in->parent == parent && in->sym == sym) {
                /* unlink from old_tgt */
                *prev_ptr = in->next;
                /* link into new_tgt */
                in->next = d->states[new_tgt].in_head;
                d->states[new_tgt].in_head = ni;

                /* If old_tgt is now an orphan, free it for reuse */
                if (d->states[old_tgt].refcount == 0 && old_tgt != d->initial)
                    state_free(d, old_tgt);

                return;
            }
            prev_ptr = &in->next;
            ni = *prev_ptr;
        }
    }
    /* Not found -- shouldn't happen if bookkeeping is correct */
    assert(0 && "incoming_redirect_one: inode not found");
}

/* ─── Signature computation (FNV-1a) ───────────────────────────────────── */

/* IMPORTANT (2026-08-09): the target-hashing formula changed from a
 * 4-byte little-endian loop to a single 32-bit xor + multiply.
 * Signatures computed before this change are incompatible — any
 * in-flight mutable handle must be reloaded from disk before
 * mutation (the nightly build provides this escape hatch). */
uint64_t sig_compute(const State *s)
{
    uint64_t h = FNV_OFFSET;

    /* hash the final flag */
    h ^= s->is_final ? 1 : 0;
    h *= FNV_PRIME;

    /* hash each transition: sym + 32-bit target in one step */
    {
        unsigned int i;
        for (i = 0; i < s->ntrans; i++) {
            h ^= trans_arr_c(s)[i].sym;
            h *= FNV_PRIME;
            h ^= trans_arr_c(s)[i].target;    /* xor the full 32-bit target once */
            h *= FNV_PRIME;
        }
    }
    return h;
}

/* ─── Register (equivalence-class map) ────────────────────────────────── */

unsigned int reg_lookup(dafsa *d, uint64_t sig)
{
    size_t idx;

    if (sig == 0) return 0;  /* 0 = invalid/empty */

    idx = (size_t)(sig % d->reg_cap);
    while (d->reg_keys[idx] != 0) {
        if (d->reg_keys[idx] == sig)
            return d->reg_vals[idx];
        idx = (idx + 1) % d->reg_cap;
        d->reg_probes++;
    }
    return 0;  /* not found */
}

/* Non-counting variant for invariant-checker / read-only paths that
 * must not perturb reg_probes.  Identical to reg_lookup except it
 * does not increment d->reg_probes.  Only used inside DAFSA_DEBUG. */
#ifdef DAFSA_DEBUG
unsigned int reg_lookup_no_count(const dafsa *d, uint64_t sig)
{
    size_t idx;

    if (sig == 0) return 0;

    idx = (size_t)(sig % d->reg_cap);
    while (d->reg_keys[idx] != 0) {
        if (d->reg_keys[idx] == sig)
            return d->reg_vals[idx];
        idx = (idx + 1) % d->reg_cap;
    }
    return 0;
}
#endif

/* Grow the register: double capacity -> next prime, rehash all entries. */
void reg_grow(dafsa *d)
{
    size_t new_cap = next_prime(d->reg_cap * 2);
    uint64_t *new_keys;
    uint32_t *new_vals;
    size_t i;

    new_keys = calloc(new_cap, sizeof(uint64_t));
    new_vals = calloc(new_cap, sizeof(uint32_t));
    if (!new_keys || !new_vals) {
        free(new_keys);
        free(new_vals);
        fprintf(stderr, "dafsa: OOM growing register\n");
        abort();
    }

    /* Rehash all existing entries */
    for (i = 0; i < d->reg_cap; i++) {
        uint64_t sig = d->reg_keys[i];
        if (sig == 0) continue;
        {
            size_t idx = (size_t)(sig % new_cap);
            while (new_keys[idx] != 0)
                idx = (idx + 1) % new_cap;
            new_keys[idx] = sig;
            new_vals[idx] = d->reg_vals[i];
        }
    }

    free(d->reg_keys);
    free(d->reg_vals);
    d->reg_keys = new_keys;
    d->reg_vals = new_vals;
    d->reg_cap   = new_cap;
    /* reg_used is unchanged (same number of entries) */
}

void reg_insert(dafsa *d, uint64_t sig, unsigned int id)
{
    size_t idx;

    if (sig == 0) return;

    /* Grow if load factor would exceed 0.7 after insert */
    if ((d->reg_used + 1) * 10 > d->reg_cap * 7)
        reg_grow(d);

    idx = (size_t)(sig % d->reg_cap);
    while (d->reg_keys[idx] != 0) {
        /* overwrite if re-inserting the same state */
        if (d->reg_keys[idx] == sig) {
            d->reg_vals[idx] = id;
            return;
        }
        idx = (idx + 1) % d->reg_cap;
        d->reg_probes++;
    }
    d->reg_keys[idx] = sig;
    d->reg_vals[idx] = id;
    d->reg_used++;
}

/* ─── Clone-on-write ──────────────────────────────────────────────────── */

/* Clone state `sid`, return the clone's id. The clone gets refcount=0
 * initially, and all its outgoing transitions add incoming edges to their
 * targets (via incoming_add). The caller is responsible for redirecting
 * the appropriate single incoming edge from the parent.
 *
 * Pointer safety: state_new() may realloc d->states, so we fetch src/dst
 * AFTER state_new. incoming_add() may realloc d->inodes, but dst/src point
 * into d->states (separate allocation), so they remain valid. */
unsigned int clone_state(dafsa *d, unsigned int sid)
{
    unsigned int new_id = state_new(d);   /* MAY REALLOC d->states */

    /* Re-fetch AFTER state_new: the old &d->states[sid] would be stale */
    State *src = &d->states[sid];
    State *dst = &d->states[new_id];

    dst->is_final = src->is_final;
    dst->ntrans = src->ntrans;
    if (src->ntrans > 0) {
        /* Allocate the clone's transition array (sparse). On OOM, abort. */
        if (trans_reserve(dst, src->ntrans) != 0) {
            fprintf(stderr, "dafsa: OOM cloning transitions\n");
            abort();
        }
        memcpy(trans_arr(dst), trans_arr_c(src), src->ntrans * sizeof(Edge));
    }
    dst->sig = src->sig;

    /* Register incoming edges for all of the clone's outgoing transitions.
     * incoming_add may realloc d->inodes, but dst is in d->states (safe). */
    {
        unsigned int i;
        for (i = 0; i < dst->ntrans; i++) {
            incoming_add(d, new_id, trans_arr_c(dst)[i].sym,
                         trans_arr_c(dst)[i].target);
        }
    }

    return new_id;
}

/* ─── Core: replace_or_register ────────────────────────────────────────── */

/* After modifying state `sid`, call this to either register it (if its
 * signature is unique) or merge it with an existing equivalent state.
 *
 * `parent` is the state that transitions to `sid`.
 * After a merge, parent's signature is invalidated; confluence_path
 * will re-register it.
 *
 * Returns 1 if a merge occurred (the caller may need to rebuild the register),
 * 0 otherwise.
 *
 * Pointer safety: s = &d->states[sid] is fetched once. reg_lookup and
 * reg_insert may realloc reg_keys/reg_vals (separate allocation), so s
 * remains valid. incoming_redirect also does not realloc states/inodes. */
int replace_or_register(dafsa *d, unsigned int sid,
                               unsigned int parent)
{
    State *s = &d->states[sid];  /* fetched once; states not realloc'd here */
    uint64_t new_sig;
    unsigned int equivalent;

    /* Compute (or recompute) signature */
    new_sig = sig_compute(s);
    s->sig = new_sig;

    equivalent = reg_lookup(d, new_sig);
    /* A register entry is only a valid merge target if the state it names is
     * still live (refcount > 0, or the initial state) AND still carries that
     * exact signature. Otherwise the entry is stale (the state was merged away
     * or its structure changed) and must be ignored — this is what makes
     * incremental register maintenance correct without a full reg_rebuild on
     * every operation. */
    if (equivalent != 0 && equivalent != sid &&
        (d->states[equivalent].refcount != 0 || equivalent == d->initial) &&
        d->states[equivalent].sig == new_sig) {
        /* --- MERGE: sid into equivalent --- */
        incoming_redirect(d, sid, equivalent);

        /* Repoint the register entry: this signature now belongs to the
         * surviving state `equivalent`, not the merged-away `sid`.  Leaving
         * the stale sid entry would make a later state with the same signature
         * merge into a dead id. */
        reg_insert(d, new_sig, equivalent);

        /* parent's transition was updated by incoming_redirect.
         * Now parent's signature is dirty.
         * parent may be 0 when called for the root — writes to
         * sentinel state 0 are harmless (unused slot). */
        d->states[parent].sig = 0;

        /* sid was freed by incoming_redirect — nothing more to do here */
        return 1;
    } else {
        /* --- REGISTER: this signature is unique (or its entry was stale) --- */
        reg_insert(d, new_sig, sid);
        return 0;
    }
}

/* ─── Confluence along the path ────────────────────────────────────────── */

/* After adding/deleting a word, the path from root to the final state
 * may contain states that need to be re-registered. Process bottom-up.
 *
 * path[i]     = state id
 * parents[i]  = path[i-1]  (parents[0] is unused)
 * len         = number of states on the path
 *
 * Returns 1 if any state on the path was merged (caller may need to rebuild
 * the register), 0 otherwise.
 *
 * Pointer safety: operates entirely on indices in the path[]/parents[]
 * stack arrays. replace_or_register re-fetches State * internally. */
int confluence_path(dafsa *d, unsigned int *path,
                           unsigned int *parents,
                           unsigned int len)
{
    int i;
    int merged = 0;
    for (i = (int)len - 1; i >= 1; i--) {
        unsigned int child  = path[i];
        unsigned int parent = parents[i];
        if (replace_or_register(d, child, parent)) merged = 1;
    }
    /* Also register the root if it was modified */
    if (replace_or_register(d, path[0], 0)) merged = 1;
    return merged;
}

/* ─── Add word (length-delimited) ──────────────────────────────────────── */

int dafsa_add_n(dafsa *d, const unsigned char *key, size_t len)
{
    unsigned int path_len;
    unsigned int current;
    unsigned int pos;

    if (d) d->subtree_valid = 0;   /* coarse invalidation: DAG restructured below */

    if (len == 0) {
        /* Empty string: the initial state becomes final */
        if (d->states[d->initial].is_final) {
#ifdef DAFSA_DEBUG
            dafsa_check_invariants(d);
#endif
            return 0;
        }
        d->states[d->initial].is_final = 1;
        replace_or_register(d, d->initial, 0);
#ifdef DAFSA_DEBUG
        dafsa_check_invariants(d);
#endif
        return 1;
    }
    if (key == NULL) return -1;   /* defensive: non-empty key must be non-NULL */
    if (len > MAX_WORD_LEN) return -1;   /* hard guard: path arrays are bounded */

    if (dafsa_ensure_scratch(d, len) != 0) return -1;

    /* --- Phase 1: Traverse existing path --- */
    current  = d->initial;
    path_len = 0;

    d->spath[path_len]    = current;
    d->schars[path_len]   = 0;
    d->sparents[path_len] = 0;
    path_len++;

    for (pos = 0; pos < len; pos++) {
        unsigned char c = key[pos];
        int tr = trans_find(&d->states[current], c);
        if (tr < 0) break;  /* divergence point */

        {
            unsigned int next = trans_arr_c(&d->states[current])[tr].target;
            /* Prefetch the next iteration's state (and its overflow slab) so
             * the two dependent random loads overlap the memory latency of the
             * current iteration. Safe: read-only hint; bounds checked. */
            if (pos + 1 < len) {
                DAFSA_PREFETCH(&d->states[next]);
                if (d->states[next].trans_heap)
                    DAFSA_PREFETCH(d->states[next].trans_heap);
            }
            current = next;
            d->spath[path_len]    = current;
            d->schars[path_len]   = c;
            d->sparents[path_len] = d->spath[path_len - 1];
            path_len++;
        }
    }

    /* --- Check: word already present? --- */
    if (pos == len && d->states[current].is_final) {
#ifdef DAFSA_DEBUG
        dafsa_check_invariants(d);
#endif
        return 0;  /* already in the DAFSA */
    }

    /* --- Clone-on-write: make the prefix path private (ascending) ---
     * Clone every shared state along the path from root toward the leaf.
     * Each redirect targets the already-private parent path[di-1], so it
     * never affects other words that share a sub-automaton.  This is required
     * when re-adding a word whose (deleted) ghost branch is still shared with
     * another word, as well as when adding a fresh suffix at a divergence. */
    {
        unsigned int di;
        for (di = 1; di < path_len; di++) {
            unsigned int sid = d->spath[di];
            if (d->states[sid].refcount > 1) {
                unsigned int clone = clone_state(d, sid);
                unsigned int parent = d->spath[di - 1];
                unsigned char pc    = d->schars[di];

                /* clone_state may realloc states; re-fetch via indices */
                incoming_redirect_one(d, parent, pc, sid, clone);

                /* Update path (and the parent pointer of the next element) */
                d->spath[di] = clone;
                if (di + 1 < path_len)
                    d->sparents[di + 1] = clone;
            }
        }
        current = d->spath[path_len - 1];
    }

    /* --- Phase 2: Add suffix from the divergence point --- */
    if (pos < len) {
        unsigned int i;
        for (i = pos; i < len; i++) {
            unsigned char c = key[i];
            unsigned int next;

            next = state_new(d);   /* MAY REALLOC states */
            /* re-fetch state via index -- current is an index, safe */
            trans_add(&d->states[current], c, next);
            d->states[current].sig = 0;  /* dirty */

            incoming_add(d, current, c, next);  /* MAY REALLOC inodes */

            d->spath[path_len]    = next;
            d->schars[path_len]   = c;
            d->sparents[path_len] = current;
            path_len++;

            current = next;
        }
    }

    /* --- Phase 3: Mark final and confluence --- */
    d->states[current].is_final = 1;
    d->states[current].sig = 0;  /* dirty */

    /* Stale register entries are validated at lookup in replace_or_register
     * (live state + matching signature), so no full register rebuild is needed
     * here — rebuilding on every add is O(nstates) and makes bulk builds
     * O(N^2). */
    confluence_path(d, d->spath, d->sparents, path_len);

#ifdef DAFSA_DEBUG
    dafsa_check_invariants(d);
#endif
    return 1;
}

/* ─── Delete word (length-delimited) ───────────────────────────────────── */

int dafsa_delete_n(dafsa *d, const unsigned char *key, size_t len)
{
    unsigned int path_len;
    unsigned int current;
    unsigned int i;
    int di;

    if (d) d->subtree_valid = 0;   /* coarse invalidation: DAG restructured below */

    if (len == 0) {
        if (!d->states[d->initial].is_final) {
#ifdef DAFSA_DEBUG
            dafsa_check_invariants(d);
#endif
            return 0;
        }
        d->states[d->initial].is_final = 0;
        d->states[d->initial].sig = 0;
        replace_or_register(d, d->initial, 0);
#ifdef DAFSA_DEBUG
        dafsa_check_invariants(d);
#endif
        return 1;
    }
    if (key == NULL) return -1;   /* defensive: non-empty key must be non-NULL */
    if (len > MAX_WORD_LEN) return -1;   /* hard guard: path arrays are bounded */

    if (dafsa_ensure_scratch(d, len) != 0) return -1;

    /* --- Phase 1: Traverse to the final state --- */
    current  = d->initial;
    path_len = 0;

    d->spath[path_len]    = current;
    d->schars[path_len]   = 0;
    d->sparents[path_len] = 0;
    path_len++;

    for (i = 0; i < len; i++) {
        unsigned char c = key[i];
        int tr = trans_find(&d->states[current], c);
        if (tr < 0) {
#ifdef DAFSA_DEBUG
            dafsa_check_invariants(d);
#endif
            return 0;  /* not present */
        }
        {
            unsigned int next = trans_arr_c(&d->states[current])[tr].target;
            /* Prefetch the next iteration's state + overflow slab (see add_n). */
            if ((unsigned)(i + 1) < len) {
                DAFSA_PREFETCH(&d->states[next]);
                if (d->states[next].trans_heap)
                    DAFSA_PREFETCH(d->states[next].trans_heap);
            }
            current = next;
        }
        d->spath[path_len]    = current;
        d->schars[path_len]   = c;
        d->sparents[path_len] = d->spath[path_len - 1];
        path_len++;
    }

    if (!d->states[current].is_final) {
#ifdef DAFSA_DEBUG
        dafsa_check_invariants(d);
#endif
        return 0;  /* word is a prefix but not a word */
    }

    /* --- Phase 2: Clone-on-write, bottom-up ---
     * Walk the path from the root toward the leaf (ascending).  At each step
     * the parent (path[di-1]) is already private -- either it was cloned in
     * the previous iteration (refcount == 1) or it had refcount == 1 to begin
     * with -- so redirecting its single edge on the path cannot affect any
     * other word.  (A descending walk would redirect the edge of a still-shared
     * parent and corrupt words that share the sub-automaton.) */
    {
        for (di = 1; di < (int)path_len; di++) {
            unsigned int sid = d->spath[di];
            if (d->states[sid].refcount > 1) {
                unsigned int clone = clone_state(d, sid);
                /* parent is the (possibly just-cloned) previous path state;
                 * must re-read path[di-1], NOT the stale parents[] snapshot */
                unsigned int parent = d->spath[di - 1];
                unsigned char pc    = d->schars[di];

                /* clone_state may realloc states; re-fetch via indices */
                incoming_redirect_one(d, parent, pc, sid, clone);

                /* Update path -- and current if this is the final state */
                d->spath[di] = clone;
                if (di < (int)path_len - 1)
                    d->sparents[di + 1] = clone;
                if (di == (int)path_len - 1)
                    current = clone;
            }
        }
    }

    /* --- Phase 3: Unmark final and confluence --- */
    d->states[current].is_final = 0;
    d->states[current].sig = 0;

    /* Stale register entries (from merged-away/dead states) are validated at
     * lookup in replace_or_register, so no full register rebuild is needed. */
    confluence_path(d, d->spath, d->sparents, path_len);

#ifdef DAFSA_DEBUG
    dafsa_check_invariants(d);
#endif
    return 1;
}

/* ─── Lookup (length-delimited) ────────────────────────────────────────── */

int dafsa_lookup_n(const dafsa *d, const unsigned char *key, size_t len)
{
    unsigned int current = d->initial;
    size_t i;

    if (key == NULL && len > 0) return 0;  /* defensive */

    for (i = 0; i < len; i++) {
        int tr = trans_find(&d->states[current], key[i]);
        if (tr < 0) return 0;
        {
            unsigned int next = trans_arr_c(&d->states[current])[tr].target;
            /* Prefetch the next iteration's state + overflow slab (see add_n). */
            if (i + 1 < len) {
                DAFSA_PREFETCH(&d->states[next]);
                if (d->states[next].trans_heap)
                    DAFSA_PREFETCH(d->states[next].trans_heap);
            }
            current = next;
        }
    }
    return d->states[current].is_final;
}

/* ─── NUL-terminated convenience wrappers ──────────────────────────────── */

int dafsa_add(dafsa *d, const unsigned char *word)
{
    return dafsa_add_n(d, word, strlen((const char *)word));
}

int dafsa_lookup(const dafsa *d, const unsigned char *word)
{
    return dafsa_lookup_n(d, word, strlen((const char *)word));
}

int dafsa_delete(dafsa *d, const unsigned char *word)
{
    return dafsa_delete_n(d, word, strlen((const char *)word));
}
