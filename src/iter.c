/* iter.c — Pull-based sorted iterator + merge-join over the DAFSA
 * (range-index follow-up #5).  Replaces the callback fire-and-forget
 * enumeration (rel_prefix's prefix_dfs / view_prefix's view_enum_dfs) with a
 * RESUMABLE cursor.  Uses dafsa_internal.h (precedent: relation.c,
 * snapshot.c, regexwalk.c). */
#include "dl.h"
#include "dl_internal.h"
#include "relation.h"
#include "snapshot.h"
#include "dafsa.h"
#include "dafsa_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_ARITY 8
#define MAX_KEY_LEN (MAX_ARITY * 4 + 1)      /* 33 */
#define DL_ITER_MAX_FRAMES (MAX_KEY_LEN + 1)  /* 34 */

#define DL_ITER_LIVE 1
#define DL_ITER_VIEW 2

/* One DFS frame. `state` = DAFSA state entered at this level; the resume
 * position within that state's outgoing edges is next_edge (LIVE) or cursor
 * (VIEW); the unused one is ignored per `kind`. */
struct dl_iter_frame {
    uint32_t       state;
    uint32_t       next_edge;   /* LIVE only */
    const uint8_t *cursor;      /* VIEW only */
};

struct dl_iter {
    uint8_t kind;        /* DL_ITER_LIVE | DL_ITER_VIEW */
    uint8_t arity;
    uint8_t k;           /* bound leading-column count */
    uint8_t exhausted;   /* 1 once past the last tuple */
    uint8_t nframes;     /* frames in use (>=1 while positioned) */

    uint32_t leading[MAX_ARITY];

    /* Explicit DFS stack. buf[i] = sym on edge frames[i]->frames[i+1]. At a
     * final state depth = nframes-1 = (arity-k)*4 + 1: first (arity-k)*4 bytes
     * of buf are the remaining columns (u32BE), buf[depth-1] is the trailing
     * \0. */
    struct dl_iter_frame frames[DL_ITER_MAX_FRAMES];
    unsigned char buf[MAX_KEY_LEN];

    const dafsa *d;    /* LIVE: rel->d (borrowed; db owns it) */
    dafsa_view  *v;    /* VIEW: owned here; closed in dl_iter_close */
};

/* ─── helpers ─────────────────────────────────────────────────────────── */

/* Local relation lookup duplicating dl.c's static find_rel (keeps dl.c
 * untouched). */
static int iter_find_rel(const dl_db *db, const char *name)
{
    size_t i;
    for (i = 0; i < db->nrels; i++) {
        if (strcmp(db->rels[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

/* Read `arity` u32 columns from a big-endian buffer (mirrors relation.c). */
static void read_cols_be(uint32_t *cols, const unsigned char *buf, uint8_t arity)
{
    uint8_t i;
    for (i = 0; i < arity; i++) {
        cols[i] = ((uint32_t)buf[4*i]     << 24) |
                  ((uint32_t)buf[4*i + 1] << 16) |
                  ((uint32_t)buf[4*i + 2] << 8)  |
                  ((uint32_t)buf[4*i + 3]);
    }
}

/* Walk the k*4-byte prefix of it->leading from the DAFSA root, returning the
 * state index representing that bound, or -1 if the prefix is absent. */
static int iter_walk_prefix(const dl_iter *it)
{
    uint32_t current;
    uint8_t i;

    if (it->kind == DL_ITER_LIVE) {
        current = it->d->initial;
        for (i = 0; i < it->k; i++) {
            uint32_t v = it->leading[i];
            unsigned char col_be[4];
            int b;
            col_be[0] = (unsigned char)((v >> 24) & 0xFF);
            col_be[1] = (unsigned char)((v >> 16) & 0xFF);
            col_be[2] = (unsigned char)((v >> 8)  & 0xFF);
            col_be[3] = (unsigned char)(v & 0xFF);
            for (b = 0; b < 4; b++) {
                int tr = trans_find(&it->d->states[current], col_be[b]);
                if (tr < 0) return -1;
                current = trans_arr_c(&it->d->states[current])[tr].target;
            }
        }
    } else {
        current = it->v->initial;
        for (i = 0; i < it->k; i++) {
            uint32_t v = it->leading[i];
            unsigned char col_be[4];
            int b;
            col_be[0] = (unsigned char)((v >> 24) & 0xFF);
            col_be[1] = (unsigned char)((v >> 16) & 0xFF);
            col_be[2] = (unsigned char)((v >> 8)  & 0xFF);
            col_be[3] = (unsigned char)(v & 0xFF);
            for (b = 0; b < 4; b++) {
                uint32_t target;
                if (view_trans_find(it->v, current, col_be[b], &target) != 0)
                    return -1;
                current = target;
            }
        }
    }
    return (int)current;
}

/* Reset the DFS stack to start from `root` (a DAFSA state, or -1 for an
 * absent prefix -> a VALID EMPTY iterator). */
static void iter_reset_stack(dl_iter *it, int root)
{
    it->exhausted = 0;
    if (root < 0) {
        it->exhausted = 1;
        it->nframes = 0;
        return;
    }
    it->nframes = 1;
    it->frames[0].state = (uint32_t)root;
    if (it->kind == DL_ITER_LIVE)
        it->frames[0].next_edge = 0;
    else
        it->frames[0].cursor = it->v->csr + it->v->state_off[root];
}

/* 1 iff the frame's state is a final (accepting) state. */
static int iter_is_final(const dl_iter *it, const struct dl_iter_frame *f)
{
    if (it->kind == DL_ITER_LIVE)
        return it->d->states[f->state].is_final;
    return (it->v->final_bits[f->state / 8] &
            (uint8_t)(1u << (f->state % 8))) != 0;
}

/* ─── public API ──────────────────────────────────────────────────────── */

dl_iter *dl_iter_open(dl_db *db, const char *rel,
                      const uint32_t *leading, uint8_t k)
{
    dl_iter *it;

    if (!db || !db->dir || !rel) return NULL;
    it = calloc(1, sizeof(*it));
    if (!it) return NULL;

    if (db->snap_version > 0) {
        /* Snapshot path: OWN the mmap view (NOT view_open_cached — the vcache
         * is LRU-evicted and fully invalidated by dl_publish_snapshot, which
         * would dangle a long-lived cursor across iter_next calls). */
        char path[8192];
        uint8_t arity = 0;
        int variadic = 0;
        snprintf(path, sizeof(path), "%s/snapshots/%u",
                 db->dir, db->snap_version);
        if (!manifest_find_rel_ex(path, rel, &arity, &variadic)) {
            free(it); return NULL;               /* unknown rel in snapshot */
        }
        if (variadic) { free(it); return NULL; } /* variadic: rejected */
        if (k > arity) { free(it); return NULL; }
        if (k > 0 && !leading) { free(it); return NULL; }
        snprintf(path, sizeof(path), "%s/snapshots/%u/%s.dafsa",
                 db->dir, db->snap_version, rel);
        it->v = dafsa_view_open(path);
        if (!it->v) { free(it); return NULL; }
        it->kind = DL_ITER_VIEW;
        it->arity = arity;
    } else {
        const rel_entry *e;
        int idx = iter_find_rel(db, rel);
        if (idx < 0) { free(it); return NULL; }  /* unknown rel */
        e = &db->rels[idx];
        if (e->kind == RELK_VARIADIC) { free(it); return NULL; } /* rejected */
        if (k > e->arity) { free(it); return NULL; }
        if (k > 0 && !leading) { free(it); return NULL; }
        it->d = rel_dafsa(e->rel);               /* borrow rel->d (VIEW) */
        if (!it->d) { free(it); return NULL; }
        it->kind = DL_ITER_LIVE;
        it->arity = e->arity;
    }

    it->k = k;
    if (k > 0) memcpy(it->leading, leading, (size_t)k * sizeof(uint32_t));
    iter_reset_stack(it, iter_walk_prefix(it));
    return it;
}

/* LIVE-mode open over an already-resolved relation.  Borrows rel->d and NEVER
 * routes to the snapshot view.  The VM's OP_RANGE must read LIVE: vm_execute
 * materializes rel->d in place even when snap_version > 0 (re-publish), and
 * reading the mmap'd snapshot of a PREVIOUS version would silently
 * mis-evaluate.  Mirrors dl_iter_open's LIVE branch.  Returns NULL on NULL
 * rel / empty dafsa / k > arity / k > 0 && !leading / OOM. */
dl_iter *dl_iter_open_live(relation *rel, const uint32_t *leading, uint8_t k)
{
    dl_iter *it;
    uint8_t arity;

    if (!rel) return NULL;
    arity = rel_arity(rel);
    if (k > arity) return NULL;
    if (k > 0 && !leading) return NULL;

    it = calloc(1, sizeof(*it));
    if (!it) return NULL;

    it->kind = DL_ITER_LIVE;
    it->arity = arity;
    it->d = rel_dafsa(rel);   /* borrow rel->d (db owns it) */
    if (!it->d) { free(it); return NULL; }

    it->k = k;
    if (k > 0) memcpy(it->leading, leading, (size_t)k * sizeof(uint32_t));
    iter_reset_stack(it, iter_walk_prefix(it));
    return it;
}

int dl_iter_seek(dl_iter *it, const uint32_t *leading, uint8_t k)
{
    if (!it) return -1;
    if (k > it->arity) return -1;
    if (k > 0 && !leading) return -1;

    it->k = k;
    if (k > 0) memcpy(it->leading, leading, (size_t)k * sizeof(uint32_t));
    iter_reset_stack(it, iter_walk_prefix(it));
    return 0;
}

int dl_iter_next(dl_iter *it, uint32_t *cols_out)
{
    uint8_t n_rem;

    if (!it || !cols_out) return -1;
    if (it->exhausted) return 0;

    n_rem = (uint8_t)(it->arity - it->k);

    for (;;) {
        struct dl_iter_frame *top = &it->frames[it->nframes - 1];

        if (iter_is_final(it, top)) {
            /* Emit: leading cols ++ remaining cols decoded from buf, then
             * backtrack (pop the final frame). */
            if (it->k > 0)
                memcpy(cols_out, it->leading,
                       (size_t)it->k * sizeof(uint32_t));
            if (n_rem > 0)
                read_cols_be(cols_out + it->k, it->buf, n_rem);
            it->nframes--;
            if (it->nframes == 0) it->exhausted = 1;
            return 1;
        }

        if (it->nframes >= DL_ITER_MAX_FRAMES) return -1; /* depth overflow */

        /* Descend the next outgoing edge (in sorted-symbol order). */
        {
            unsigned char sym;
            uint32_t tgt;

            if (it->kind == DL_ITER_LIVE) {
                const State *s = &it->d->states[top->state];
                const Edge *e;
                if (top->next_edge >= s->ntrans) {
                    /* no more edges: backtrack */
                    it->nframes--;
                    if (it->nframes == 0) { it->exhausted = 1; return 0; }
                    continue;
                }
                e = &trans_arr_c(s)[top->next_edge];
                top->next_edge++;
                sym = e->sym;
                tgt = e->target;
            } else {
                if (view_edge_next(it->v, top->state, &top->cursor,
                                   &sym, &tgt) != 0) {
                    /* no more edges: backtrack */
                    it->nframes--;
                    if (it->nframes == 0) { it->exhausted = 1; return 0; }
                    continue;
                }
            }

            it->buf[it->nframes - 1] = sym;   /* buf[i]=sym on frames[i]->[i+1] */
            it->frames[it->nframes].state = tgt;
            if (it->kind == DL_ITER_LIVE)
                it->frames[it->nframes].next_edge = 0;
            else
                it->frames[it->nframes].cursor =
                    it->v->csr + it->v->state_off[tgt];
            it->nframes++;
        }
    }
}

uint8_t dl_iter_arity(const dl_iter *it)
{
    return it ? it->arity : 0;
}

void dl_iter_close(dl_iter *it)
{
    if (!it) return;
    if (it->kind == DL_ITER_VIEW && it->v)
        dafsa_view_close(it->v);
    free(it);
}

/* ─── merge-join ──────────────────────────────────────────────────────── */

static int mj_keys_equal(const uint32_t *a, const uint32_t *b, uint8_t j)
{
    uint8_t i;
    for (i = 0; i < j; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

long dl_merge_join(dl_iter *l, dl_iter *r, uint8_t jcols,
                   dl_join_cb cb, void *user)
{
    uint32_t lt[MAX_ARITY], rt[MAX_ARITY];
    uint32_t *rbuf = NULL;
    size_t rcap = 0, rcnt = 0;
    uint8_t la, ra, j;
    int lok, rok;
    long emitted = 0;

    if (!l || !r || !cb) return -1;
    la = l->arity;
    ra = r->arity;
    if (jcols == 0 || jcols > la || jcols > ra) return -1;

    lok = dl_iter_next(l, lt);
    rok = dl_iter_next(r, rt);
    if (lok < 0 || rok < 0) goto fail;

    while (lok && rok) {
        int cmp = 0;
        for (j = 0; j < jcols; j++) {
            if (lt[j] < rt[j]) { cmp = -1; break; }
            if (lt[j] > rt[j]) { cmp = 1; break; }
        }

        if (cmp < 0) { lok = dl_iter_next(l, lt); if (lok < 0) goto fail; continue; }
        if (cmp > 0) { rok = dl_iter_next(r, rt); if (rok < 0) goto fail; continue; }

        /* Keys equal: buffer the whole right run, then cross it with the left
         * run.  Both runs advance in sorted order so output stays sorted. */
        rcnt = 0;
        do {
            if (rcnt == rcap) {
                size_t nc = rcap ? rcap * 2 : 64;
                uint32_t *nb = realloc(rbuf, nc * (size_t)ra * sizeof(uint32_t));
                if (!nb) goto fail;
                rbuf = nb;
                rcap = nc;
            }
            memcpy(rbuf + rcnt * ra, rt, (size_t)ra * sizeof(uint32_t));
            rcnt++;
            rok = dl_iter_next(r, rt);
            if (rok < 0) goto fail;
        } while (rok && mj_keys_equal(rt, rbuf, jcols));

        do {
            size_t i;
            for (i = 0; i < rcnt; i++) {
                emitted++;
                if (cb(lt, la, rbuf + i * ra, ra, user) != 0) {
                    free(rbuf);
                    return emitted;              /* early stop */
                }
            }
            lok = dl_iter_next(l, lt);
            if (lok < 0) { free(rbuf); return -1; }
        } while (lok && mj_keys_equal(lt, rbuf, jcols));
    }

    /* Documented contract: both iterators are left exhausted. */
    while (lok) { lok = dl_iter_next(l, lt); if (lok < 0) goto fail; }
    while (rok) { rok = dl_iter_next(r, rt); if (rok < 0) goto fail; }

    free(rbuf);
    return emitted;

fail:
    free(rbuf);
    return -1;
}
