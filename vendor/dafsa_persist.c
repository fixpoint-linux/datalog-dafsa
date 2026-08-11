/* dafsa_persist.c — PDWG v3 save/load */
#include "dafsa_internal.h"
/* ─── Persistence ─────────────────────────────────────────────────────── */

/* On-disk format (ROADMAP 1.3): all integers little-endian, explicit byte
 * writes (State/Edge have padding; never fwrite raw structs).
 *
 * Version 4 (2026-02-19): appends a trailing 4-byte CRC32 (IEEE 802.3 /
 * CRC-32) computed over ALL bytes from offset 0 up to (but not including)
 * the CRC field itself.  Readers verify it for v4 files.  v3 files
 * (unchecksummed) remain readable.
 *
 * Version 3 (2026-08-09): widens state-table ntrans from u8 to u16 LE,
 * fixing truncation for states with exactly 256 out-edges.
 *
 * Version 2 (compressed; 2026-08-08): drops the per-state offset column and
 * varint-encodes CSR target ids, shrinking large indexes ~40-58% vs v1.
 * Search semantics are unchanged. v1 is no longer read.
 *
 *   HEADER:  magic[4]="PDWG"; u32 version=4; u32 n_states; u32 n_trans;
 *            u32 initial_id=1; u32 n_final; u32 reserved=0
 *   STATE TABLE: (n_states+1) x u16 LE ntrans (0..65535; entry 0 = 0).
 *            Transition offsets are implied (cumulative), not stored.
 *   FINAL BITMAP: ceil((n_states+1)/8) bytes; bit i set iff reachable state i is final
 *   CSR TRANSITIONS: n_trans x (u8 sym; LEB128 u32 target_id), grouped by state
 *            in state-table order, sorted by sym asc.  Sink 0 -> 0, else new id.
 *   TRAILING CRC32 (v4 only): 4 bytes LE, over every byte above (offset 0 up
 *            to but excluding the CRC itself).
 */

int put_u8(FILE *f, uint8_t v, uint32_t *crc);

int put_uvarint(FILE *f, uint32_t v, uint32_t *crc)
{
    /* LEB128 */
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v != 0) byte |= 0x80;
        if (put_u8(f, byte, crc)) return -1;
    } while (v != 0);
    return 0;
}

int put_u8(FILE *f, uint8_t v, uint32_t *crc)
{
    if (crc)
        *crc = crc32_table[(*crc ^ v) & 0xFF] ^ (*crc >> 8);
    return fputc(v, f) == EOF ? -1 : 0;
}

int put_u16_le(FILE *f, uint16_t v, uint32_t *crc)
{
    if (put_u8(f, (uint8_t)(v & 0xFF), crc) != 0) return -1;
    if (put_u8(f, (uint8_t)((v >> 8) & 0xFF), crc) != 0) return -1;
    return 0;
}

int put_u32_le(FILE *f, uint32_t v, uint32_t *crc)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (put_u8(f, (uint8_t)(v & 0xFF), crc) != 0) return -1;
        v >>= 8;
    }
    return 0;
}

/* Open the directory containing `path` and fsync it, so a prior rename of a
 * file into it is made durable. Returns 0 on success, -1 on error. */
int fsync_dir_of(const char *path)
{
    char *dir = NULL;
    const char *slash;
    int fd, ret = -1;

    if (!path || !*path) return -1;
    /* dirname(path) without modifying path: everything up to the last '/'. */
    slash = strrchr(path, '/');
    if (slash == NULL) {
        dir = (char *)malloc(2);
        if (!dir) return -1;
        dir[0] = '.'; dir[1] = '\0';
    } else if (slash == path) {
        dir = (char *)malloc(2);
        if (!dir) return -1;
        dir[0] = '/'; dir[1] = '\0';
    } else {
        size_t n = (size_t)(slash - path);
        dir = (char *)malloc(n + 1);
        if (!dir) return -1;
        memcpy(dir, path, n);
        dir[n] = '\0';
    }

    fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ret = fsync(fd);
        close(fd);
    }
    free(dir);
    return ret;
}

/* Save a compact, minimal form: BFS-renumber reachable states 1..N (initial
 * -> 1), drop orphans (refcount 0 / unreachable).  Atomic: write path.tmp,
 * fflush, fsync, fclose, rename.  Returns 0 on success, -1 on any error.
 * `d` is const and is never mutated. */
int dafsa_save(const dafsa *d, const char *path)
{
    FILE *f = NULL;
    char *tmp_path = NULL;
    uint32_t *old_to_new = NULL;
    uint32_t *queue = NULL;
    unsigned char *visited = NULL;
    uint32_t n_reach = 0, n_trans = 0, n_final = 0;
    uint32_t head, tail, i, j;
    size_t path_len;
    uint32_t crc = crc32_init();
    int ok = -1;

    if (!d || !path) return -1;

    old_to_new = (uint32_t *)calloc(d->nstates, sizeof(uint32_t));
    queue      = (uint32_t *)malloc(d->nstates * sizeof(uint32_t));
    visited    = (unsigned char *)calloc(d->nstates, 1);
    if (!old_to_new || !queue || !visited) goto out;

    /* BFS from initial, renumber reachable states in BFS order 1..N */
    head = 0; tail = 0;
    queue[tail++] = d->initial;
    visited[d->initial] = 1;
    while (head < tail) {
        uint32_t old = queue[head++];
        const State *s = &d->states[old];
        old_to_new[old] = ++n_reach;
        if (s->is_final) n_final++;
        n_trans += s->ntrans;
        for (j = 0; j < s->ntrans; j++) {
            uint32_t tgt = trans_arr_c(s)[j].target;
            if (!visited[tgt]) {
                visited[tgt] = 1;
                queue[tail++] = tgt;
            }
        }
    }

    /* atomic: write to path.tmp then rename onto path */
    path_len = strlen(path);
    tmp_path = (char *)malloc(path_len + 5);
    if (!tmp_path) goto out;
    snprintf(tmp_path, path_len + 5, "%s.tmp", path);

    f = fopen(tmp_path, "w+b");
    if (!f) goto out;
    /* Large buffered writes: saves ~15M fputc syscalls on a multi-megastate
     * index (default stdio buffer is only 4-8 KB). */
    if (setvbuf(f, NULL, _IOFBF, 1u << 20) != 0) goto fail;

    /* header */
    if (put_u8(f, 'P', &crc) || put_u8(f, 'D', &crc) || put_u8(f, 'W', &crc) || put_u8(f, 'G', &crc))
        goto fail;
    if (put_u32_le(f, DAFSA_PDWG_VERSION, &crc)) goto fail;   /* version */
    if (put_u32_le(f, n_reach, &crc)) goto fail;      /* n_states */
    if (put_u32_le(f, n_trans, &crc)) goto fail;      /* n_trans */
    if (put_u32_le(f, 1, &crc)) goto fail;            /* initial_id */
    if (put_u32_le(f, n_final, &crc)) goto fail;      /* n_final */
    if (put_u32_le(f, 0, &crc)) goto fail;            /* reserved */

    /* state table: (n_states+1) x u16 LE ntrans (entry 0 = 0). Offsets implied. */
    if (put_u16_le(f, 0, &crc)) goto fail;
    for (i = 1; i <= n_reach; i++) {
        const State *s = &d->states[queue[i - 1]];
        if (put_u16_le(f, (uint16_t)s->ntrans, &crc)) goto fail;
    }

    /* final bitmap: ceil((n_states+1)/8) bytes; bit 0 always 0 */
    {
        uint32_t nb = (n_reach + 8) / 8;
        for (i = 0; i < nb; i++) {
            uint8_t byte = 0;
            for (j = 0; j < 8; j++) {
                uint32_t idx = i * 8 + j;
                if (idx >= 1 && idx <= n_reach &&
                    d->states[queue[idx - 1]].is_final)
                    byte |= (uint8_t)(1u << j);
            }
            if (put_u8(f, byte, &crc)) goto fail;
        }
    }

    /* CSR: transitions grouped by state in state-table order (sym asc) */
    for (i = 1; i <= n_reach; i++) {
        const State *s = &d->states[queue[i - 1]];
        {
        const Edge *e = trans_arr_c(s);
        for (j = 0; j < s->ntrans; j++) {
            if (put_u8(f, e[j].sym, &crc)) goto fail;
            if (put_uvarint(f, old_to_new[e[j].target], &crc)) goto fail;
        }
    }
    }

    if (ferror(f)) goto fail;

    /* v4: append the trailing CRC32.  The streaming accumulator `crc` covers
     * every byte written so far (offset 0 up to but excluding the CRC field);
     * finalize it and write the CRC LE.  The CRC field itself is NOT included
     * in the checksum. */
    {
        uint32_t final_crc = crc32_finalize(crc);
        if (put_u32_le(f, final_crc, NULL)) goto fail;   /* CRC itself NOT checksummed */
    }

    /* atomic commit */
    if (fflush(f) != 0) goto fail;
    if (fsync(fileno(f)) != 0) goto fail;
    if (fclose(f) != 0) { f = NULL; goto fail; }
    f = NULL;
    if (rename(tmp_path, path) != 0) goto fail;
    /* fsync the containing directory so the rename itself is durable; a crash
     * after rename but before this point can otherwise lose the rename even
     * though the file data was fsync'd. */
    if (fsync_dir_of(path) != 0) goto fail;

    ok = 0;
    goto out;

fail:
    if (f) fclose(f);
    if (tmp_path) remove(tmp_path);
    ok = -1;

out:
    free(tmp_path);
    free(old_to_new);
    free(queue);
    free(visited);
    return ok;
}

/* Memory-buffer parse helpers for dafsa_load: parse from an in-memory buffer
 * via a cursor instead of per-byte fgetc, which is dramatically faster on
 * large indexes (millions of transitions).  All return -1 on EOF/overflow. */
int mb_u8(const uint8_t **p, const uint8_t *end, uint8_t *out)
{
    if (*p >= end) return -1;
    *out = *(*p)++;
    return 0;
}
int mb_u16(const uint8_t **p, const uint8_t *end, uint16_t *out)
{
    uint8_t lo, hi;
    if (mb_u8(p, end, &lo) || mb_u8(p, end, &hi)) return -1;
    *out = (uint16_t)(lo | ((uint16_t)hi << 8));
    return 0;
}
int mb_u32(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    if (*p + 4 > end) return -1;
    for (i = 0; i < 4; i++)
        v |= ((uint32_t)(*p)[i]) << (8 * i);
    *p += 4;
    *out = v;
    return 0;
}
int mb_uvarint(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    uint32_t v = 0;
    unsigned int shift = 0;
    uint8_t byte;
    for (;;) {
        if (*p >= end) return -1;
        byte = *(*p)++;
        v |= ((uint32_t)(byte & 0x7F)) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
        if (shift > 28) return -1;   /* overflow / malformed */
    }
    *out = v;
    return 0;
}

int mb_skipvarint(const uint8_t **p, const uint8_t *end)
{
    for (;;) {
        uint8_t b;
        if (mb_u8(p, end, &b)) return -1;
        if (!(b & 0x80)) return 0;
    }
}

/* Materialize the on-disk compact form back into a fully mutable DAFSA:
 * rebuilds the incoming-edge lists (refcount + in_head) and the register.
 * Returns the handle, or NULL on any error (partial handle freed). */
dafsa *dafsa_load(const char *path)
{
    return dafsa_load_impl(path, 1);
}

/* Fast read-only load: parses the same on-disk form but skips rebuilding the
 * incoming-edge table and the register, which are only needed for mutation
 * (add/delete/merge).  Lookup and prefix_enum read only states[]/trans[]/
 * is_final, so a search-only handle loads dramatically faster on large
 * indexes (sessions: ~360ms -> ~40ms).  The handle MUST NOT be mutated. */
dafsa *dafsa_load_readonly(const char *path)
{
    return dafsa_load_impl(path, 0);
}

dafsa *dafsa_load_impl(const char *path, int mutable)
{
    int fd = -1;
    dafsa *d = NULL;
    uint8_t *map = NULL;
    uint8_t *final_bits = NULL;
    const uint8_t *p, *end;
    uint32_t version, n_states, n_trans, initial_id, n_final, reserved;
    uint32_t running;
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
    if (fsize == 0) goto fail;
    map = (uint8_t *)mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { map = NULL; goto fail; }
    close(fd);
    fd = -1;
    p = map;
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
    if (n_states == 0) goto fail;                       /* initial must exist */
    if (n_states > DAFSA_MAX_STATES_HARD) goto fail;    /* hard cap: reject corrupt files */
    if ((size_t)n_states + 1 > SIZE_MAX / sizeof(State)) goto fail;

    d = dafsa_create();
    if (!d) goto fail;

    /* grow states array to hold n_states+1 entries.
     * Round capacity up to a power of two so the first `state_new` after a
     * load doesn't immediately double-and-realloc the entire array (which, at
     * 2M+ states, copies ~80MB). */
    if ((size_t)n_states + 1 > d->states_cap) {
        size_t need = (size_t)n_states + 1;
        size_t new_cap = d->states_cap;
        State *new_states;
        while (new_cap < need) new_cap *= 2;
        new_states = (State *)realloc(d->states, new_cap * sizeof(State));
        if (!new_states) goto fail;
        memset(new_states + d->states_cap, 0,
               (new_cap - d->states_cap) * sizeof(State));
        d->states = new_states;
        d->states_cap = new_cap;
    }
    d->nstates = n_states + 1;
    d->initial = 1;

    /* zero sink + live states */
    memset(d->states, 0, (size_t)(n_states + 1) * sizeof(State));

    /* state table: (n_states+1) x u16 LE ntrans (entry 0 = 0). Offsets are
     * implied (cumulative), so we only validate the total. */
    {
        uint16_t sink_nt;
        if (mb_u16(&p, end, &sink_nt)) goto fail;
        if (sink_nt != 0) goto fail;
    }
    for (i = 1; i <= n_states; i++) {
        uint16_t nt;
        if (mb_u16(&p, end, &nt)) goto fail;
        d->states[i].ntrans = nt;
    }
    running = 0;
    for (i = 1; i <= n_states; i++)
        running += d->states[i].ntrans;
    if (running != n_trans) goto fail;

    /* final bitmap */
    bitmap_bytes = (size_t)((n_states + 8) / 8);
    final_bits = (uint8_t *)malloc(bitmap_bytes);
    if (!final_bits) goto fail;
    if (p + bitmap_bytes > end) goto fail;
    memcpy(final_bits, p, bitmap_bytes);
    p += bitmap_bytes;
    {
        uint32_t finals = 0;
        for (i = 1; i <= n_states; i++) {
            if (final_bits[i / 8] & (uint8_t)(1u << (i % 8))) {
                d->states[i].is_final = 1;
                finals++;
            }
        }
        if (finals != n_final) goto fail;
    }

    /* CSR: direct copy into trans[] (already sorted, no trans_add) */
    for (i = 1; i <= n_states; i++) {
        State *s = &d->states[i];
        /* Allocate the sparse transition array for this state. On OOM, fail
         * the load gracefully (dafsa_load returns NULL) rather than aborting.
         * The fail: label frees d->states (and every trans_heap allocated so
         * far via dafsa_free), final_bits, and the mmap view. */
        if (s->ntrans > 0 && trans_reserve(s, s->ntrans) != 0) {
            goto fail;
        }
        {
            Edge *e = trans_arr(s);
            for (j = 0; j < s->ntrans; j++) {
                uint8_t sym;
                uint32_t target;
                if (mb_u8(&p, end, &sym)) goto fail;
                if (mb_uvarint(&p, end, &target)) goto fail;
                if (target > n_states) goto fail;           /* 0 = sink, else 1..N */
                e[j].sym = sym;
                e[j].target = target;
            }
        }
    }
    if (version == 4) {
        /* v4: verify trailing CRC32.  Covered region is [map, p) (p == CSR
         * end).  Stored CRC sits in the final 4 bytes, little-endian. */
        uint32_t stored, calc;
        if (fsize < 32) goto fail;            /* header 28 + CRC 4 */
        if (p + 4 != end) goto fail;          /* no trailing garbage after CRC */
        stored = (uint32_t)map[fsize - 4]
               | ((uint32_t)map[fsize - 3] << 8)
               | ((uint32_t)map[fsize - 2] << 16)
               | ((uint32_t)map[fsize - 1] << 24);
        calc = crc32_compute(map, (size_t)(p - map));
        if (calc != stored) goto fail;
    } else {
        if (p != end) goto fail;  /* v3: reject trailing bytes after CSR */
    }

    /* Rebuild incoming edges + register ONLY for a mutable handle.  Search
     * (lookup / prefix_enum) does not need either; skipping them is the whole
     * point of the read-only fast load path. */
    if (mutable) {
        /* rebuild incoming edges: restores refcount + in_head */
        for (i = 1; i <= n_states; i++) {
            State *s = &d->states[i];
            unsigned int jj;
            const Edge *e = trans_arr_c(s);
            for (jj = 0; jj < s->ntrans; jj++)
                incoming_add(d, i, e[jj].sym, e[jj].target);
        }

        /* rebuild register: sig_compute + reg_insert per live state */
        for (i = 1; i <= n_states; i++) {
            State *s = &d->states[i];
            uint64_t sig = sig_compute(s);
            s->sig = sig;
            reg_insert(d, sig, i);
        }
    }

    if (map) munmap(map, fsize);
    free(final_bits);
    return d;

fail:
    if (fd >= 0) close(fd);
    if (map) munmap(map, fsize);
    dafsa_free(d);
    free(final_bits);
    return NULL;
}
