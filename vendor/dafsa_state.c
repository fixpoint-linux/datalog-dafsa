/* dafsa_state.c — State management and transition helpers */
#include "dafsa_internal.h"
/* ─── State management ─────────────────────────────────────────────────── */

unsigned int state_new(dafsa *d)
{
    /* Try the free-list first — recycled slots don't count against the cap. */
    if (d->free_head != 0) {
        unsigned int id = d->free_head;
        /* sig is reused as the next pointer; narrowing from uint64_t to
         * uint32_t is safe because state ids are bounded by 100M. */
        d->free_head = (unsigned int)d->states[id].sig;
        memset(&d->states[id], 0, sizeof(State));
        return id;
    }

    /* Hard limit only fires when we must allocate a genuinely new slot.
     * Free-list exhaustion already handled above. */
    if (d->nstates >= DAFSA_MAX_STATES_HARD) {
        fprintf(stderr, "dafsa: max states exceeded (%u)\n",
                (unsigned)DAFSA_MAX_STATES_HARD);
        abort();
    }

    if (d->nstates >= d->states_cap) {
        size_t new_cap = d->states_cap * 2;
        State *new_states;

        if (new_cap > (size_t)DAFSA_MAX_STATES_HARD + 1)
            new_cap = (size_t)DAFSA_MAX_STATES_HARD + 1;

        new_states = realloc(d->states, new_cap * sizeof(State));
        if (!new_states) {
            fprintf(stderr, "dafsa: OOM growing states\n");
            abort();
        }
        /* Zero-initialize the newly allocated tail */
        memset(new_states + d->states_cap, 0,
               (new_cap - d->states_cap) * sizeof(State));
        d->states = new_states;
        d->states_cap = new_cap;
    }

    {
        unsigned int id = d->nstates++;
        State *s = &d->states[id];
        memset(s, 0, sizeof(*s));
        return id;
    }
}

/* Remove phantom inode entries from children's in_head chains.
 * Called by state_free BEFORE it frees the trans[] array.
 *
 * For each outgoing transition (sym, child) of state sid, unlinks the
 * matching inode from child's in_head chain and decrements child's
 * refcount.  Without this, children accumulate garbage inodes whose
 * parent field points at a now-reused slot — inflating refcounts and
 * risking UB if the reused slot is later merged (the assert at ~line 475
 * would fire on a stale parent). */
void state_detach_from_children(dafsa *d, unsigned int sid)
{
    State *s = &d->states[sid];
    unsigned int j;

    for (j = 0; j < s->ntrans; j++) {
        unsigned char sym   = trans_arr(s)[j].sym;
        unsigned int  child = trans_arr(s)[j].target;
        unsigned int *prev_ptr;
        unsigned int ni;

        /* In a minimal acyclic DFA, child != sid always (no self-loops).
         * Also child could be the equivalent/new_tgt that triggered this
         * merge — that's fine; this just removes sid's phantom contribution. */
        prev_ptr = &d->states[child].in_head;
        ni = *prev_ptr;
        while (ni != 0) {
            Inode *in = &d->inodes[ni];
            if (in->parent == sid && in->sym == sym) {
                /* unlink from child's chain — at most one match per (child,sym) */
                *prev_ptr = in->next;
                d->states[child].refcount--;
                break;
            }
            prev_ptr = &in->next;
            ni = *prev_ptr;
        }
    }
}

/* Release an orphan state slot to the free-list.  Only call when:
 *   refcount == 0 && id != d->initial
 * Detaches phantom inodes from children, frees the transition array,
 * and chains the slot for reuse. */
void state_free(dafsa *d, unsigned int id)
{
    State *s = &d->states[id];

    /* Detach phantom inodes from children BEFORE freeing trans[].
     * state_detach_from_children walks s->trans[], so trans must
     * still be intact here. */
    state_detach_from_children(d, id);

    if (s->trans_heap) {
        free(s->trans_heap);
        s->trans_heap = NULL;
    }
    s->ntrans   = 0;
    s->refcount = 0;
    s->is_final = 0;
    s->in_head  = 0;
    /* Chain into free-list via the `sig` field.
     * State ids fit in 32 bits (bounded by DAFSA_MAX_STATES_HARD=100M),
     * so narrowing the uint64_t sig to uint32_t for the next pointer is safe. */
    s->sig       = d->free_head;
    d->free_head = id;
}

/* ─── Inode allocation ─────────────────────────────────────────────────── */

Inode *inode_alloc(dafsa *d)
{
    if (d->inodes_used + 1 >= d->inodes_cap) {
        size_t new_cap = d->inodes_cap * 2;
        Inode *new_inodes = realloc(d->inodes, new_cap * sizeof(Inode));
        if (!new_inodes) {
            fprintf(stderr, "dafsa: OOM growing inodes\n");
            abort();
        }
        memset(new_inodes + d->inodes_cap, 0,
               (new_cap - d->inodes_cap) * sizeof(Inode));
        d->inodes = new_inodes;
        d->inodes_cap = new_cap;
    }
    return &d->inodes[++d->inodes_used];   /* index 0 = sentinel */
}

/* ─── Scratch arena for add/delete path traversal ──────────────────────── */

/* Ensure the per-handle scratch arrays can hold `len+2` entries.
 * Returns 0 on success, -1 on OOM.  Not reentrant on the same handle. */
int dafsa_ensure_scratch(dafsa *d, size_t len)
{
    size_t need = len + 2;

    if (d->scratch_cap >= need) return 0;

    {
        unsigned int  *new_path;
        unsigned char *new_chars;
        unsigned int  *new_parents;

        /* Use malloc (not realloc) so that the old pointers remain valid
         * until all three allocations have succeeded.  Then swap. */
        new_path    = (unsigned int *)malloc(need * sizeof(unsigned int));
        new_chars   = (unsigned char *)malloc(need * sizeof(unsigned char));
        new_parents = (unsigned int *)malloc(need * sizeof(unsigned int));
        if (!new_path || !new_chars || !new_parents) {
            free(new_path);
            free(new_chars);
            free(new_parents);
            return -1;
        }
        /* Copy old contents (if any) */
        if (d->spath) {
            size_t old_n = d->scratch_cap;
            memcpy(new_path,    d->spath,    old_n * sizeof(unsigned int));
            memcpy(new_chars,   d->schars,   old_n * sizeof(unsigned char));
            memcpy(new_parents, d->sparents, old_n * sizeof(unsigned int));
        }
        free(d->spath);
        free(d->schars);
        free(d->sparents);
        d->spath    = new_path;
        d->schars   = new_chars;
        d->sparents = new_parents;
        d->scratch_cap = need;
    }
    return 0;
}

/* ─── Transition helpers ───────────────────────────────────────────────── */

/* binary search for transition `c`. Returns index or -1. */
int trans_find(const State *s, unsigned char c)
{
    unsigned int n = s->ntrans;
    /* Fast path: most DAFSA states have few transitions; a linear scan (with
     * early exit on the sorted-array invariant) beats binary search there. */
    if (n <= 8) {
        unsigned int i;
        for (i = 0; i < n; i++) {
            unsigned char sy = trans_arr_c(s)[i].sym;
            if (sy == c) return (int)i;
            if (sy >  c) return -1;   /* sorted: no later entry can match */
        }
        return -1;
    }
    {
        int lo = 0, hi = (int)n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (trans_arr_c(s)[mid].sym == c) return mid;
            if (trans_arr_c(s)[mid].sym <  c) lo = mid + 1;
            else                        hi = mid - 1;
        }
        return -1;
    }
}

/* Ensure the state's transition array has capacity for `need` entries.
 * If need <= DAFSA_INLINE_N, the inline array suffices (return 0).
 * Otherwise, promote to a heap-backed TransHeap, growing geometrically
 * (doubling). Returns 0 on success, -1 on OOM. */
int trans_reserve(State *s, unsigned int need)
{
    if (need <= DAFSA_INLINE_N) return 0;   /* fits inline */

    if (!s->trans_heap) {
        /* Promote: allocate heap and copy the ≤4 inline edges over */
        size_t sz = sizeof(TransHeap) + (size_t)need * sizeof(Edge);
        TransHeap *th = (TransHeap *)malloc(sz);
        if (!th) return -1;
        th->cap = need;
        memcpy(th->edges, s->trans, s->ntrans * sizeof(Edge));
        s->trans_heap = th;
        return 0;
    }

    /* Already heap-backed: double until we fit, cap at ALPHABET_SZ */
    if (need <= s->trans_heap->cap) return 0;
    {
        unsigned int new_cap = s->trans_heap->cap;
        /* need is bounded by ALPHABET_SZ (256) so no overflow possible */
        while (new_cap < need) new_cap *= 2;
        if (new_cap > ALPHABET_SZ) new_cap = ALPHABET_SZ;
        {
            size_t sz = sizeof(TransHeap) + (size_t)new_cap * sizeof(Edge);
            TransHeap *th = (TransHeap *)realloc(s->trans_heap, sz);
            if (!th) return -1;
            th->cap = new_cap;
            s->trans_heap = th;
        }
    }
    return 0;
}

/* insert a transition, maintaining sorted order */
void trans_add(State *s, unsigned char c, unsigned int tgt)
{
    Edge *e;

    assert(s->ntrans < ALPHABET_SZ);
    /* Grow on demand (sparse heap array). On OOM, abort like state_new does —
     * the DAFSA cannot continue without the transition table. */
    if (trans_reserve(s, s->ntrans + 1) != 0) {
        fprintf(stderr, "dafsa: OOM growing transitions\n");
        abort();
    }
    /* Fetch AFTER reserve — the reserve may have promoted inline→heap,
     * invalidating any pointer derived from s->trans. */
    e = trans_arr(s);
    {
        int pos = 0;
        while (pos < (int)s->ntrans && e[pos].sym < c)
            pos++;
        if (pos < (int)s->ntrans && e[pos].sym == c) {
            /* update existing */
            e[pos].target = tgt;
            return;
        }
        /* shift right */
        memmove(&e[pos + 1], &e[pos],
                (s->ntrans - (unsigned)pos) * sizeof(Edge));
        e[pos].sym = c;
        e[pos].target = tgt;
        s->ntrans++;
    }
#ifdef DAFSA_DEBUG
    assert(s->trans_heap != NULL || s->ntrans <= DAFSA_INLINE_N);
#endif
}
