/*
 * termstore.c — Hash-consed LIST term store implementation
 *
 * See termstore.h for the encoding contract.  Implementation is an
 * open-addressing hash table keyed by (head, tail) u32 pairs (FNV-1a, linear
 * probe, power-of-two capacity, resize at ~75% load) mapping to a node
 * index; nodes live in parallel head[]/tail[] arrays in handle order.
 *
 * NIL is node 0 (head=tail=0) and is NOT hashed — it is the only node not
 * reachable as a cons cell.  The DAG is well-founded because term_cons only
 * accepts an already-allocated list as its tail.
 */
#include "termstore.h"
#include "util.h"   /* fsync_dir_of_path */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define TERM_INIT_NODES 64
#define TERM_INIT_HASH  128

/* Corruption sanity cap: reject a count header above this many nodes. */
#define TERM_MAX_NODES  (1u << 26)

struct termstore {
    uint32_t *head;     /* node[i].head */
    uint32_t *tail;     /* node[i].tail */
    uint32_t  count;    /* nodes in use (NIL = node 0) */
    uint32_t  cap;      /* capacity of head/tail arrays */
    uint32_t *ht_head;  /* hash key head */
    uint32_t *ht_tail;  /* hash key tail */
    uint32_t *ht_idx;   /* node index + 1 (0 = empty) */
    uint32_t  ht_cap;   /* power of two */
    int       dirty;    /* 1 if new nodes since last save */
};

/* FNV-1a over the 8 bytes of (a, b). */
static uint64_t term_hash(uint32_t a, uint32_t b)
{
    uint64_t h = 14695981039346656037ULL;
    uint32_t v[2];
    int i;
    v[0] = a;
    v[1] = b;
    for (i = 0; i < 2; i++) {
        h ^= (v[i] & 0xFF);         h *= 1099511628211ULL;
        h ^= ((v[i] >> 8) & 0xFF);  h *= 1099511628211ULL;
        h ^= ((v[i] >> 16) & 0xFF); h *= 1099511628211ULL;
        h ^= ((v[i] >> 24) & 0xFF); h *= 1099511628211ULL;
    }
    return h;
}

/* Grow the hash table (power-of-two doubling) and rehash nodes 1..count-1. */
static int ht_grow(termstore *t)
{
    uint32_t nc = t->ht_cap * 2;
    uint32_t *nh, *nt, *ni;
    uint32_t i;
    if (nc < t->ht_cap) return -1;   /* overflow */
    nh = calloc(nc, sizeof(uint32_t));
    nt = calloc(nc, sizeof(uint32_t));
    ni = calloc(nc, sizeof(uint32_t));
    if (!nh || !nt || !ni) { free(nh); free(nt); free(ni); return -1; }
    for (i = 1; i < t->count; i++) {
        uint32_t idx = (uint32_t)(term_hash(t->head[i], t->tail[i]) &
                                  (uint64_t)(nc - 1));
        while (ni[idx] != 0)
            idx = (idx + 1) & (nc - 1);
        nh[idx] = t->head[i];
        nt[idx] = t->tail[i];
        ni[idx] = i + 1;
    }
    free(t->ht_head);
    free(t->ht_tail);
    free(t->ht_idx);
    t->ht_head = nh;
    t->ht_tail = nt;
    t->ht_idx  = ni;
    t->ht_cap  = nc;
    return 0;
}

/* Grow the node arrays (doubling). */
static int nodes_grow(termstore *t)
{
    uint32_t nc = t->cap * 2;
    uint32_t *nh, *nt;
    if (nc < t->cap) return -1;
    nh = realloc(t->head, (size_t)nc * sizeof(uint32_t));
    if (!nh) return -1;
    t->head = nh;
    nt = realloc(t->tail, (size_t)nc * sizeof(uint32_t));
    if (!nt) return -1;   /* head grew; tail failed — head array stays valid */
    t->tail = nt;
    t->cap  = nc;
    return 0;
}

termstore *term_create(void)
{
    termstore *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cap = TERM_INIT_NODES;
    t->ht_cap = TERM_INIT_HASH;
    t->head = malloc((size_t)t->cap * sizeof(uint32_t));
    t->tail = malloc((size_t)t->cap * sizeof(uint32_t));
    t->ht_head = calloc(t->ht_cap, sizeof(uint32_t));
    t->ht_tail = calloc(t->ht_cap, sizeof(uint32_t));
    t->ht_idx  = calloc(t->ht_cap, sizeof(uint32_t));
    if (!t->head || !t->tail || !t->ht_head || !t->ht_tail || !t->ht_idx) {
        free(t->head); free(t->tail);
        free(t->ht_head); free(t->ht_tail); free(t->ht_idx);
        free(t);
        return NULL;
    }
    /* NIL = node 0. */
    t->head[0] = 0;
    t->tail[0] = 0;
    t->count = 1;
    return t;
}

void term_free(termstore *t)
{
    if (!t) return;
    free(t->head);
    free(t->tail);
    free(t->ht_head);
    free(t->ht_tail);
    free(t->ht_idx);
    free(t);
}

int term_is_list(const termstore *t, uint32_t v)
{
    return t && v >= TERM_BASE && (v - TERM_BASE) < t->count;
}

uint32_t term_car(const termstore *t, uint32_t h)
{
    if (!t || !term_is_list(t, h) || h == TERM_NIL) return 0;
    return t->head[h - TERM_BASE];
}

uint32_t term_cdr(const termstore *t, uint32_t h)
{
    if (!t || !term_is_list(t, h) || h == TERM_NIL) return 0;
    return t->tail[h - TERM_BASE];
}

uint32_t term_cons(termstore *t, uint32_t head, uint32_t tail)
{
    uint32_t idx;
    if (!t) return 0;
    if (!term_is_list(t, tail)) return 0;   /* improper tail rejected */

    /* Keep the hash load factor under ~75% for the incoming node. */
    if (t->count + 1 > t->ht_cap - (t->ht_cap >> 2)) {
        if (ht_grow(t) != 0) return 0;
    }

    idx = (uint32_t)(term_hash(head, tail) & (uint64_t)(t->ht_cap - 1));
    while (t->ht_idx[idx] != 0) {
        if (t->ht_head[idx] == head && t->ht_tail[idx] == tail)
            return TERM_BASE + (t->ht_idx[idx] - 1);
        idx = (idx + 1) & (t->ht_cap - 1);
    }

    if (t->count >= t->cap) {
        if (nodes_grow(t) != 0) return 0;
    }

    t->head[t->count] = head;
    t->tail[t->count] = tail;
    t->ht_head[idx] = head;
    t->ht_tail[idx] = tail;
    t->ht_idx[idx]  = t->count + 1;
    t->count++;
    t->dirty = 1;
    return TERM_BASE + (t->count - 1);
}

uint32_t term_append(termstore *t, uint32_t a, uint32_t b)
{
    uint32_t *elems = NULL;
    uint32_t cap = 16, n = 0, cur, i, result;
    if (!t) return 0;
    if (!term_is_list(t, a) || !term_is_list(t, b)) return 0;
    if (a == TERM_NIL) return b;

    elems = malloc((size_t)cap * sizeof(uint32_t));
    if (!elems) return 0;

    /* Collect a's elements (iterative — no deep recursion on long lists). */
    cur = a;
    while (cur != TERM_NIL) {
        if (n >= cap) {
            uint32_t nc = cap * 2;
            uint32_t *ne;
            if (nc < cap) { free(elems); return 0; }
            ne = realloc(elems, (size_t)nc * sizeof(uint32_t));
            if (!ne) { free(elems); return 0; }
            elems = ne;
            cap = nc;
        }
        elems[n++] = term_car(t, cur);
        cur = term_cdr(t, cur);
    }

    /* Right-fold cons onto b (rebuilds the prefix, sharing the b suffix). */
    result = b;
    for (i = n; i > 0; i--) {
        result = term_cons(t, elems[i - 1], result);
        if (result == 0) { free(elems); return 0; }
    }
    free(elems);
    return result;
}

uint32_t term_length(const termstore *t, uint32_t h)
{
    uint32_t n = 0;
    if (!t || !term_is_list(t, h)) return 0;
    while (h != TERM_NIL) {
        n++;
        h = term_cdr(t, h);
    }
    return n;
}

uint32_t term_node_count(const termstore *t)
{
    return t ? t->count : 0;
}

int term_is_dirty(const termstore *t)
{
    return t && t->dirty;
}

void term_clear_dirty(termstore *t)
{
    if (t) t->dirty = 0;
}

int term_save(termstore *t, const char *path)
{
    char tmp[8192];
    FILE *f;
    uint32_t i;

    if (!t || !path) return -1;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) return -1;

    if (fprintf(f, "%u\n", t->count) < 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    for (i = 0; i < t->count; i++) {
        if (fprintf(f, "%u %u\n", t->head[i], t->tail[i]) < 0) {
            fclose(f);
            unlink(tmp);
            return -1;
        }
    }

    if (fflush(f) != 0) { fclose(f); unlink(tmp); return -1; }
    if (fsync(fileno(f)) != 0) { fclose(f); unlink(tmp); return -1; }
    if (fclose(f) != 0) { unlink(tmp); return -1; }

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    if (fsync_dir_of_path(path) != 0) return -1;

    t->dirty = 0;
    return 0;
}

termstore *term_load(const char *path)
{
    termstore *t;
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    uint32_t count = 0, i;

    t = term_create();
    if (!t) return NULL;
    if (!path) return t;

    f = fopen(path, "r");
    if (!f) return t;   /* no file → empty store (backward-compat) */

    /* Count header. */
    if ((len = getline(&line, &cap, f)) > 0) {
        char *end = NULL;
        unsigned long v = strtoul(line, &end, 10);
        if (end == line || v == 0 || v > TERM_MAX_NODES) {
            fclose(f); free(line); term_free(t); return NULL;
        }
        count = (uint32_t)v;
    } else {
        fclose(f); free(line); term_free(t); return NULL;
    }

    /* The file has `count` node lines in handle order: node 0 (NIL, "0 0")
     * is pre-allocated; nodes 1..count-1 are re-interned in order. */
    for (i = 0; i < count; i++) {
        uint32_t h = 0, tl = 0;
        if (getline(&line, &cap, f) <= 0 ||
            sscanf(line, "%u %u", &h, &tl) != 2) {
            fclose(f); free(line); term_free(t); return NULL;
        }
        if (i == 0) {
            if (h != 0 || tl != 0) {   /* NIL row corrupted */
                fclose(f); free(line); term_free(t); return NULL;
            }
            continue;
        }
        if (term_cons(t, h, tl) == 0) {
            fclose(f); free(line); term_free(t); return NULL;
        }
    }

    free(line);
    fclose(f);

    /* The file is canonical (hash-consed): every row interned a fresh node.
     * A mismatch means the file had duplicate rows — corrupt. */
    if (t->count != count) {
        term_free(t);
        return NULL;
    }
    t->dirty = 0;
    return t;
}
