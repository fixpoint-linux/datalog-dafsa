/* dafsa_view.c — Zero-copy view + prefix enumeration + WAL overlay (M5) */
#include "dafsa_internal.h"

/* ─── Overlay helpers (WAL → layered read view) ──────────────────────── */

/* FNV-1a hash of a byte sequence (same basis as sig_compute). */
static uint64_t overlay_hash_bytes(const uint8_t *data, size_t len)
{
    uint64_t h = FNV_OFFSET;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* Find a payload in a bucket's inner hash table. Returns slot index or -1. */
static int overlay_bucket_find(const struct wal_bucket *b,
                               const uint8_t *payload)
{
    uint64_t h;
    size_t idx;

    if (b->slots_cap == 0) return -1;

    h   = overlay_hash_bytes(payload, 8);
    idx = (size_t)(h & (b->slots_cap - 1));
    for (;;) {
        if (b->slots[idx].state == 0) return -1;
        if (memcmp(b->slots[idx].payload, payload, 8) == 0)
            return (int)idx;
        idx = (idx + 1) & (b->slots_cap - 1);
    }
}

/* Insert or update a payload slot in a bucket. Grows slots if needed. */
static int overlay_bucket_upsert(struct wal_bucket *b,
                                 const uint8_t *payload, uint8_t op)
{
    uint64_t h;
    size_t idx;

    if (b->slots_cap == 0) {
        /* initial alloc: 4 slots */
        b->slots_cap = 4;
        b->slots = calloc(b->slots_cap, sizeof(b->slots[0]));
        if (!b->slots) return -1;
    }

    for (;;) {
        /* Grow proactively at 75 % load factor so probing stays cheap
         * and the inner loop always finds a slot before wrapping. */
        if (b->slots_used * 4 >= b->slots_cap * 3) {
            size_t new_cap = b->slots_cap * 2;
            struct wal_slot *old_slots = b->slots;
            size_t old_cap = b->slots_cap;
            struct wal_slot *new_slots;
            size_t i;

            new_slots = calloc(new_cap, sizeof(new_slots[0]));
            if (!new_slots) return -1;

            b->slots = new_slots;
            b->slots_cap = new_cap;
            b->slots_used = 0;

            /* rehash old entries */
            for (i = 0; i < old_cap; i++) {
                if (old_slots[i].state != 0) {
                    overlay_bucket_upsert(b, old_slots[i].payload,
                                          old_slots[i].state);
                }
            }
            free(old_slots);
            /* fall through: outer loop restarts with larger table */
        }

        /* try to find existing or empty slot */
        h   = overlay_hash_bytes(payload, 8);
        idx = (size_t)(h & (b->slots_cap - 1));
        {
            size_t probed;
            for (probed = 0; probed < b->slots_cap; probed++) {
                if (b->slots[idx].state == 0) {
                    /* empty slot: insert */
                    memcpy(b->slots[idx].payload, payload, 8);
                    b->slots[idx].state = op;
                    b->slots_used++;
                    return 0;
                }
                if (memcmp(b->slots[idx].payload, payload, 8) == 0) {
                    /* existing: overwrite state (last-op-wins) */
                    if (b->slots[idx].state == 0) b->slots_used++;
                    b->slots[idx].state = op;
                    return 0;
                }
                idx = (idx + 1) & (b->slots_cap - 1);
            }
            /* Table is full (all slots occupied, none matching).
             * Outer loop will grow and retry. */
        }
    }
}

/* Free a single bucket. */
static void overlay_bucket_free(struct wal_bucket *b)
{
    free(b->word);
    free(b->slots);
    memset(b, 0, sizeof(*b));
}

/* Free the entire overlay. */
static void overlay_free(struct wal_overlay *ov)
{
    size_t i;
    if (!ov) return;
    for (i = 0; i < ov->buckets_cap; i++) {
        if (ov->buckets[i].word != NULL)
            overlay_bucket_free(&ov->buckets[i]);
    }
    free(ov->buckets);
    free(ov->table);
    free(ov);
}

/* Callback for dafsa_wal_replay: build the overlay from WAL records. */
struct overlay_build_ctx {
    struct wal_overlay *ov;
};

static int overlay_build_cb(uint8_t op, const unsigned char *key,
                            uint32_t key_len, void *user)
{
    struct overlay_build_ctx *ctx = (struct overlay_build_ctx *)user;
    struct wal_overlay *ov = ctx->ov;
    const unsigned char *nul;
    size_t word_len;
    uint64_t h;
    size_t idx;

    /* Split at first 0x00: must have word || 0x00 || 8-byte payload */
    nul = memchr(key, 0x00, key_len);
    if (!nul) return 0;                         /* malformed: skip */
    word_len = (size_t)(nul - key);
    if (key_len - word_len - 1 != 8) return 0;  /* wrong tail length: skip */

    /* Find or create outer bucket */
    h = overlay_hash_bytes(key, word_len);

    /* Grow outer table if needed (> 75% full) */
    if (ov->buckets_used * 4 >= ov->table_cap * 3) {
        size_t new_cap = ov->table_cap ? ov->table_cap * 2 : 1024;
        uint32_t *new_table;
        size_t i;

        new_table = malloc(new_cap * sizeof(uint32_t));
        if (!new_table) return -1;
        for (i = 0; i < new_cap; i++)
            new_table[i] = UINT32_MAX;

        /* Rehash existing buckets */
        for (i = 0; i < ov->buckets_cap; i++) {
            if (ov->buckets[i].word != NULL) {
                uint64_t bh = overlay_hash_bytes(ov->buckets[i].word,
                                                 ov->buckets[i].word_len);
                size_t bi = (size_t)(bh & (new_cap - 1));
                while (new_table[bi] != UINT32_MAX)
                    bi = (bi + 1) & (new_cap - 1);
                new_table[bi] = (uint32_t)i;
            }
        }

        free(ov->table);
        ov->table = new_table;
        ov->table_cap = new_cap;
    }

    /* Probe outer table for matching bucket */
    idx = (size_t)(h & (ov->table_cap - 1));
    for (;;) {
        uint32_t bi = ov->table[idx];
        if (bi == UINT32_MAX) {
            /* New bucket: allocate */
            if (ov->buckets_used >= ov->buckets_cap) {
                size_t new_bcap = ov->buckets_cap ? ov->buckets_cap * 2 : 64;
                struct wal_bucket *nb;
                nb = realloc(ov->buckets, new_bcap * sizeof(nb[0]));
                if (!nb) return -1;
                memset(nb + ov->buckets_cap, 0,
                       (new_bcap - ov->buckets_cap) * sizeof(nb[0]));
                ov->buckets = nb;
                ov->buckets_cap = new_bcap;
            }
            bi = (uint32_t)ov->buckets_used;
            ov->buckets[bi].word = malloc(word_len);
            if (!ov->buckets[bi].word) return -1;
            memcpy(ov->buckets[bi].word, key, word_len);
            ov->buckets[bi].word_len = (uint32_t)word_len;
            ov->buckets[bi].slots = NULL;
            ov->buckets[bi].slots_cap = 0;
            ov->buckets[bi].slots_used = 0;
            ov->table[idx] = bi;
            ov->buckets_used++;
            /* fall through to insert/update slot */
            return overlay_bucket_upsert(&ov->buckets[bi], nul + 1, op);
        }
        /* Check word match */
        if (ov->buckets[bi].word_len == (uint32_t)word_len &&
            memcmp(ov->buckets[bi].word, key, word_len) == 0) {
            return overlay_bucket_upsert(&ov->buckets[bi], nul + 1, op);
        }
        idx = (idx + 1) & (ov->table_cap - 1);
    }
}

/* Load overlay from a WAL file path. Returns NULL on any error.
 * The WAL is opened read-only, validated, replayed into the overlay,
 * then closed.  Uses dafsa_wal_open_ro to ensure a concurrent reader
 * never mutates a writer's WAL. */
static struct wal_overlay *overlay_load(const char *wal_path)
{
    dafsa_wal *w;
    struct wal_overlay *ov;
    struct overlay_build_ctx ctx;

    w = dafsa_wal_open_ro(wal_path);
    if (!w) return NULL;

    ov = calloc(1, sizeof(*ov));
    if (!ov) { dafsa_wal_close(w); return NULL; }

    ctx.ov = ov;
    if (dafsa_wal_replay(w, overlay_build_cb, &ctx) != 0) {
        overlay_free(ov);
        dafsa_wal_close(w);
        return NULL;
    }

    dafsa_wal_close(w);
    return ov;
}

/* Look up a composite key (word || 0x00 || 8-byte payload) in the overlay.
 * Returns: DAFSA_WAL_OP_ADD (1) if present as ADD,
 *          DAFSA_WAL_OP_DEL (2) if tombstoned,
 *          0 if not found in overlay. */
static int overlay_lookup(const struct wal_overlay *ov,
                          const unsigned char *word, size_t word_len,
                          const uint8_t *payload)
{
    uint64_t h;
    size_t idx;
    int slot;

    if (!ov || ov->buckets_used == 0 || ov->table_cap == 0)
        return 0;

    h   = overlay_hash_bytes(word, word_len);
    idx = (size_t)(h & (ov->table_cap - 1));

    for (;;) {
        uint32_t bi = ov->table[idx];
        if (bi == UINT32_MAX) return 0;  /* empty → not found */

        if (ov->buckets[bi].word_len == (uint32_t)word_len &&
            memcmp(ov->buckets[bi].word, word, word_len) == 0) {
            slot = overlay_bucket_find(&ov->buckets[bi], payload);
            if (slot < 0) return 0;
            return (int)ov->buckets[bi].slots[slot].state;
        }

        idx = (idx + 1) & (ov->table_cap - 1);
    }
}

/* Find the overlay bucket for a word. Returns bucket pointer or NULL. */
static const struct wal_bucket *overlay_find_bucket(
    const struct wal_overlay *ov,
    const unsigned char *word, size_t word_len)
{
    uint64_t h;
    size_t idx;

    if (!ov || ov->buckets_used == 0 || ov->table_cap == 0)
        return NULL;

    h   = overlay_hash_bytes(word, word_len);
    idx = (size_t)(h & (ov->table_cap - 1));

    for (;;) {
        uint32_t bi = ov->table[idx];
        if (bi == UINT32_MAX) return NULL;

        if (ov->buckets[bi].word_len == (uint32_t)word_len &&
            memcmp(ov->buckets[bi].word, word, word_len) == 0)
            return &ov->buckets[bi];

        idx = (idx + 1) & (ov->table_cap - 1);
    }
}

/* ─── Zero-copy search-only view ───────────────────────────────────────── */

dafsa_view *dafsa_view_open(const char *path)
{
    int fd = -1;
    dafsa_view *v = NULL;
    uint8_t *map = NULL;
    const uint8_t *p, *end, *ntbl;
    uint32_t version, n_states, n_trans, initial_id, n_final, reserved;
    uint64_t *state_off = NULL;
    size_t bitmap_bytes, fsize = 0;
    uint32_t i, j;

    if (!path) return NULL;

    fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    {
        struct stat st;
        if (fstat(fd, &st) != 0) goto fail;
        fsize = (size_t)st.st_size;
    }
    if (fsize < 28) goto fail;                       /* minimum: header only */
    map = (uint8_t *)mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { map = NULL; goto fail; }
    close(fd);
    fd = -1;
    p   = map;
    end = map + fsize;

    /* header */
    {
        uint8_t magic[4];
        if (mb_u8(&p, end, &magic[0]) || mb_u8(&p, end, &magic[1]) ||
            mb_u8(&p, end, &magic[2]) || mb_u8(&p, end, &magic[3]))
            goto fail;
        if (magic[0] != 'P' || magic[1] != 'D' ||
            magic[2] != 'W' || magic[3] != 'G')
            goto fail;
    }
    if (mb_u32(&p, end, &version) || mb_u32(&p, end, &n_states) ||
        mb_u32(&p, end, &n_trans) || mb_u32(&p, end, &initial_id) ||
        mb_u32(&p, end, &n_final) || mb_u32(&p, end, &reserved))
        goto fail;
    (void)reserved;
    if (version != 3 && version != 4) goto fail;
    if (initial_id != 1) goto fail;
    if (n_states == 0) goto fail;

    /* state table: capture ntbl BEFORE advancing past it */
    ntbl = p;
    {
        uint16_t sink_nt;
        if (mb_u16(&p, end, &sink_nt)) goto fail;
        if (sink_nt != 0) goto fail;
    }
    /* now p points past the sink u16; skip remaining n_states u16 entries */
    if ((size_t)n_states > (size_t)(end - p) / 2) goto fail;
    p += (size_t)n_states * 2;

    /* final bitmap */
    bitmap_bytes = (size_t)((n_states + 8) / 8);
    if (p + bitmap_bytes > end) goto fail;
    {
        /* Validate n_final against popcount of the bitmap */
        const uint8_t *bm = p;
        uint32_t finals = 0;
        for (i = 0; i < bitmap_bytes; i++) {
            uint8_t b = bm[i];
            /* popcount per byte via Brian Kernighan's method */
            while (b) { finals++; b &= (uint8_t)(b - 1); }
        }
        if (finals != n_final) goto fail;
    }
    p += bitmap_bytes;

    /* build state_off: walk the CSR once, reading ntrans from ntbl.
     * Also verify n_trans matches the sum of ntrans values. */
    if ((size_t)n_states + 2 > (size_t)-1 / sizeof(uint64_t)) goto fail;
    state_off = calloc(n_states + 2, sizeof(uint64_t));
    if (!state_off) goto fail;

    {
        const uint8_t *q = p;         /* p now points at first CSR byte */
        uint32_t nt_sum = 0;
        state_off[0] = 0;
        for (i = 1; i <= n_states; i++) {
            /* ntbl points at table start; state i's u16 is at bytes 2*i, 2*i+1 */
            uint16_t nt = (uint16_t)ntbl[(size_t)i * 2]
                        | ((uint16_t)ntbl[(size_t)i * 2 + 1] << 8);
            nt_sum += nt;
            state_off[i] = (uint64_t)(q - p);
            for (j = 0; j < nt; j++) {
                uint32_t tgt;
                if (q >= end) goto fail;
                q++;                                       /* sym byte */
                if (mb_uvarint(&q, end, &tgt)) goto fail;  /* target */
                if (tgt > n_states) goto fail;             /* bounds check */
            }
        }
        state_off[n_states + 1] = (uint64_t)(q - p);
        if (nt_sum != n_trans) goto fail;  /* header n_trans mismatch */
        if (version == 4) {
            /* v4: verify trailing CRC32.  Covered region is [map, q) (q == CSR
             * end).  Stored CRC sits in the final 4 bytes, little-endian. */
            uint32_t stored, calc;
            if (fsize < 32) goto fail;            /* header 28 + CRC 4 */
            if (q + 4 != end) goto fail;          /* no trailing garbage after CRC */
            stored = (uint32_t)map[fsize - 4]
                   | ((uint32_t)map[fsize - 3] << 8)
                   | ((uint32_t)map[fsize - 2] << 16)
                   | ((uint32_t)map[fsize - 1] << 24);
            calc = crc32_compute(map, (size_t)(q - map));
            if (calc != stored) goto fail;
        } else {
            if (q != end) goto fail;      /* v3: CSR must end exactly at EOF */
        }
    }

    v = calloc(1, sizeof(*v));
    if (!v) goto fail;
    v->map        = map;
    v->map_len    = fsize;
    v->n_states   = n_states;
    v->initial    = initial_id;
    v->final_bits = p - bitmap_bytes;  /* bitmap start */
    v->csr        = p;                 /* first CSR byte */
    v->state_off  = state_off;
    v->ov         = NULL;              /* no overlay for plain open */

    return v;

fail:
    if (fd >= 0) close(fd);
    if (map) munmap(map, fsize);
    free(state_off);
    free(v);
    return NULL;
}

void dafsa_view_close(dafsa_view *v)
{
    if (!v) return;
    if (v->ov) overlay_free(v->ov);
    if (v->map) munmap(v->map, v->map_len);
    free(v->state_off);
    free(v);
}

/* ─── Prefix enumeration ──────────────────────────────────────────────── */

/* Recursive DFS from `state`, appending transition bytes into buf.  Calls
 * cb at each final state with the accumulated payload (bytes collected after
 * the 0x00 edge).  Returns non-zero to stop early. */
int enum_dfs(const dafsa *d, unsigned int state, unsigned char *buf,
                    size_t depth, dafsa_enum_cb cb, void *user, long *count)
{
    const State *s = &d->states[state];
    uint32_t j;

    if (s->is_final) {
        (*count)++;
        if (cb(buf, depth, user) != 0) return 1;
    }
    if (depth >= MAX_WORD_LEN) return 0;
    for (j = 0; j < s->ntrans; j++) {
        const Edge *e = &trans_arr_c(s)[j];
        buf[depth] = (unsigned char)e->sym;
        if (enum_dfs(d, e->target, buf, depth + 1,
                     cb, user, count) != 0)
            return 1;
    }
    return 0;
}

/* Enumerate keys of form prefix || 0x00 || payload.  Walks the prefix from
 * the initial state, requires a 0x00 edge next (W\0 semantics), then DFS the
 * payload states calling cb(payload, len).  Returns the number of keys
 * enumerated; 0 if the prefix is absent or not a key boundary. */
long dafsa_prefix_enum(const dafsa *d, const unsigned char *prefix,
                       size_t prefix_len, dafsa_enum_cb cb, void *user)
{
    unsigned int current;
    unsigned char buf[MAX_WORD_LEN];
    size_t i;
    int tr;
    long count = 0;

    if (!d || !cb) return -1;
    if (prefix == NULL && prefix_len > 0) return 0;
    if (prefix_len > MAX_WORD_LEN) return 0;

    current = d->initial;

    /* walk the prefix */
    for (i = 0; i < prefix_len; i++) {
        tr = trans_find(&d->states[current], prefix[i]);
        if (tr < 0) return 0;
        current = trans_arr_c(&d->states[current])[tr].target;
    }

    /* W\0 semantics: a 0x00 edge must exist from the final prefix state */
    tr = trans_find(&d->states[current], 0x00);
    if (tr < 0) return 0;
    current = trans_arr_c(&d->states[current])[tr].target;

    enum_dfs(d, current, buf, 0, cb, user, &count);
    return count;
}

/* ─── Zero-copy view read helpers ──────────────────────────────────────── */

int view_trans_find(const dafsa_view *v, uint32_t s,
                           unsigned char sym, uint32_t *target_out)
{
    const uint8_t *p   = v->csr + v->state_off[s];
    const uint8_t *end = v->csr + v->state_off[s + 1];
    while (p < end) {
        unsigned char e_sym = *p++;
        if (e_sym == sym) {
            if (mb_uvarint(&p, end, target_out) != 0) return -1;
            if (*target_out > v->n_states) return -1;
            return 0;
        }
        if (mb_skipvarint(&p, end) != 0) return -1;
    }
    return -1;
}

int view_edge_next(const dafsa_view *v, uint32_t s,
                          const uint8_t **cursor,
                          unsigned char *sym_out, uint32_t *target_out)
{
    const uint8_t *end = v->csr + v->state_off[s + 1];
    if (*cursor >= end) return -1;
    if (mb_u8(cursor, end, sym_out))    return -1;
    if (mb_uvarint(cursor, end, target_out)) return -1;
    if (*target_out > v->n_states) return -1;
    return 0;
}

/* Recursive DFS for the view, mirroring enum_dfs but reading edges via
 * view_edge_next and checking final_bits directly. */
int view_enum_dfs(const dafsa_view *v, uint32_t state,
                          unsigned char *buf, size_t depth,
                          dafsa_enum_cb cb, void *user, long *count)
{
    const uint8_t *cur;
    unsigned char sym;
    uint32_t tgt;

    if (v->final_bits[state / 8] & (uint8_t)(1u << (state % 8))) {
        (*count)++;
        if (cb(buf, depth, user) != 0) return 1;
    }
    if (depth >= MAX_WORD_LEN) return 0;
    cur = v->csr + v->state_off[state];
    while (view_edge_next(v, state, &cur, &sym, &tgt) == 0) {
        buf[depth] = sym;
        if (view_enum_dfs(v, tgt, buf, depth + 1, cb, user, count) != 0)
            return 1;
    }
    return 0;
}

/* Filtered DFS for layered prefix enumeration. Like view_enum_dfs, but at
 * each final state, checks the overlay bucket: DEL → suppress, ADD → emit
 * + mark slot in `emitted` bitmap. Payloads not 8 bytes long (legacy) are
 * emitted unconditionally. */
static int view_enum_dfs_layered(const dafsa_view *v, uint32_t state,
                                  unsigned char *buf, size_t depth,
                                  const struct wal_bucket *bucket,
                                  uint8_t *emitted,
                                  dafsa_enum_cb cb, void *user, long *count)
{
    const uint8_t *cur;
    unsigned char sym;
    uint32_t tgt;

    if (v->final_bits[state / 8] & (uint8_t)(1u << (state % 8))) {
        int should_emit = 1;

        if (depth == 8 && bucket != NULL) {
            int slot = overlay_bucket_find(bucket, buf);
            if (slot >= 0) {
                uint8_t s = bucket->slots[slot].state;
                if (s == DAFSA_WAL_OP_DEL) {
                    should_emit = 0;
                } else if (s == DAFSA_WAL_OP_ADD && emitted != NULL) {
                    emitted[slot / 8] |= (uint8_t)(1u << (slot % 8));
                }
            }
        }

        if (should_emit) {
            (*count)++;
            if (cb(buf, depth, user) != 0) return 1;
        }
    }

    if (depth >= MAX_WORD_LEN) return 0;
    cur = v->csr + v->state_off[state];
    while (view_edge_next(v, state, &cur, &sym, &tgt) == 0) {
        buf[depth] = sym;
        if (view_enum_dfs_layered(v, tgt, buf, depth + 1,
                                   bucket, emitted, cb, user, count) != 0)
            return 1;
    }
    return 0;
}

/* Layered prefix enumeration: merge base FST + WAL overlay.  Phase A walks
 * the base graph (filtered through overlay); Phase B emits WAL-only ADDs.
 * Does NOT early-return when the prefix is absent from the base — overlay
 * may have entries for words not yet in the base. */
static long view_prefix_enum_layered(const dafsa_view *v,
                                      const unsigned char *prefix,
                                      size_t prefix_len,
                                      dafsa_enum_cb cb, void *user)
{
    uint32_t current;
    size_t i;
    long count = 0;
    const struct wal_bucket *bucket;
    uint8_t *emitted = NULL;
    int base_has_prefix = 1;

    /* Walk prefix in base graph (best-effort) */
    current = v->initial;
    for (i = 0; i < prefix_len; i++) {
        uint32_t target;
        if (view_trans_find(v, current, prefix[i], &target) != 0) {
            base_has_prefix = 0;
            break;
        }
        current = target;
    }

    /* Find overlay bucket for this prefix word */
    bucket = overlay_find_bucket(v->ov, prefix, prefix_len);

    /* Allocate emitted bitmap if there are overlay slots to track */
    if (bucket != NULL && bucket->slots_cap > 0) {
        size_t bm_bytes = (bucket->slots_cap + 7) / 8;
        emitted = calloc(bm_bytes, 1);
        if (!emitted) return -1;
    }

    /* Phase A: base DFS through the 0x00 edge (filtered by overlay) */
    if (base_has_prefix) {
        uint32_t target;
        if (view_trans_find(v, current, 0x00, &target) == 0) {
            unsigned char buf[MAX_WORD_LEN];
            view_enum_dfs_layered(v, target, buf, 0,
                                   bucket, emitted, cb, user, &count);
        }
    }

    /* Phase B: emit WAL-only ADDs (overlay slots not emitted in Phase A) */
    if (bucket != NULL) {
        size_t si;
        for (si = 0; si < bucket->slots_cap; si++) {
            if (bucket->slots[si].state == DAFSA_WAL_OP_ADD) {
                if (emitted == NULL ||
                    !(emitted[si / 8] & (uint8_t)(1u << (si % 8)))) {
                    count++;
                    if (cb(bucket->slots[si].payload, 8, user) != 0) {
                        free(emitted);
                        return count;
                    }
                }
            }
        }
    }

    free(emitted);
    return count;
}

int dafsa_view_lookup_n(const dafsa_view *v,
                         const unsigned char *key, size_t len)
{
    uint32_t current;
    size_t i;

    if (!v) return 0;
    if (key == NULL && len > 0) return 0;

    /* Layered lookup: consult overlay first */
    if (v->ov) {
        const unsigned char *nul = memchr(key, 0x00, len);
        if (nul != NULL) {
            size_t word_len = (size_t)(nul - key);
            if (len - word_len - 1 == 8) {
                int ov_state = overlay_lookup(v->ov, key, word_len, nul + 1);
                if (ov_state == DAFSA_WAL_OP_ADD) return 1;
                if (ov_state == DAFSA_WAL_OP_DEL) return 0;
                /* absent: fall through to base */
            }
        }
    }

    current = v->initial;
    for (i = 0; i < len; i++) {
        uint32_t target;
        if (view_trans_find(v, current, key[i], &target) != 0)
            return 0;
        current = target;
    }
    return (v->final_bits[current / 8] &
            (uint8_t)(1u << (current % 8))) ? 1 : 0;
}

long dafsa_view_prefix_enum(const dafsa_view *v,
                             const unsigned char *prefix, size_t prefix_len,
                             dafsa_enum_cb cb, void *user)
{
    uint32_t current;
    unsigned char buf[MAX_WORD_LEN];
    size_t i;
    long count = 0;

    if (!v || !cb) return -1;
    if (prefix == NULL && prefix_len > 0) return 0;
    if (prefix_len > MAX_WORD_LEN) return 0;

    /* Layered path: merge base + overlay (no early-return on base miss) */
    if (v->ov)
        return view_prefix_enum_layered(v, prefix, prefix_len, cb, user);

    current = v->initial;

    /* walk the prefix */
    for (i = 0; i < prefix_len; i++) {
        uint32_t target;
        if (view_trans_find(v, current, prefix[i], &target) != 0)
            return 0;
        current = target;
    }

    /* W\0 semantics: a 0x00 edge must exist from the final prefix state */
    {
        uint32_t target;
        if (view_trans_find(v, current, 0x00, &target) != 0)
            return 0;
        current = target;
    }

    view_enum_dfs(v, current, buf, 0, cb, user, &count);
    return count;
}

/* ─── Layered open ─────────────────────────────────────────────────────── */

dafsa_view *dafsa_view_open_layered(const char *fst_path, const char *wal_path)
{
    dafsa_view *v;
    struct stat st;

    v = dafsa_view_open(fst_path);
    if (!v) return NULL;

    if (wal_path != NULL) {
        /* Check if WAL file exists and is non-trivial */
        if (stat(wal_path, &st) == 0 && st.st_size >= 16) {
            v->ov = overlay_load(wal_path);
            if (!v->ov) {
                /* Overlay load failed — close view and return NULL */
                dafsa_view_close(v);
                return NULL;
            }
        }
        /* else: wal_path doesn't exist, is empty, or too small —
         * no overlay loaded (not an error) */
    }

    return v;
}

