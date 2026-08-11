/* dafsa_wal.c — Write-ahead log for incremental DAFSA updates (M5)
 *
 * Append-only self-framing record format (index.wal):
 *   HEADER (16 B): magic "DAWL" | version:u32LE=1 | flags:u32LE=0 | header_crc:u32LE
 *   RECORD: op:u8 | key_len:u32LE | key[key_len] | rec_crc:u32LE
 *
 * rec_crc = crc32(op || key_len_le || key)
 * No footer. Torn tail detected by validation failure → ftruncate.
 */
#include "dafsa_internal.h"
#include <errno.h>

/* ─── Shared helpers ────────────────────────────────────────────────────── */

/* Read a little-endian u32 from p; returns <0 on short read. */
static int wal_read_u32(const uint8_t *p, const uint8_t *end, uint32_t *out)
{
    if (p + 4 > end) return -1;
    *out = (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
    return 0;
}

/* Validate one record at *p with *remaining bytes available.
 * Returns:
 *    0  — valid record: *op / *key / *key_len set, *p and *remaining advanced
 *   -1  — corrupt record (bad op, bad key_len, or CRC mismatch): *p NOT advanced
 *   -2  — torn/partial record (not enough bytes for a complete record):
 *         *p NOT advanced
 *
 * This is the single validation function used everywhere — open-time scan,
 * replay, and any future reader.
 */
static int wal_validate_record(const uint8_t **p, size_t *remaining,
                        uint8_t *op, const unsigned char **key,
                        uint32_t *key_len, uint32_t *consumed)
{
    const uint8_t *head = *p;
    size_t rem = *remaining;
    uint32_t klen;

    /* Minimum: op(1) + key_len(4) + key[1] + crc(4) = 10 bytes */
    if (rem < 10) return -2;

    if (head[0] != DAFSA_WAL_OP_ADD && head[0] != DAFSA_WAL_OP_DEL)
        return -1;

    if (wal_read_u32(head + 1, head + rem, &klen) != 0)
        return -2;

    if (klen < 1 || klen > (uint32_t)(MAX_WORD_LEN + 9))
        return -1;

    if ((size_t)klen + 9 > rem)    /* need op(1) + klen(4) + key(klen) + crc(4) = 9+klen */
        return -2;

    /* Validate CRC over op(1) || key_len(4 LE) || key(klen) */
    {
        uint32_t stored_crc, calc_crc;
        unsigned char stack_buf[512];
        unsigned char *tmp = stack_buf;
        const uint8_t *crc_at;

        crc_at = head + 5 + (size_t)klen;
        if (wal_read_u32(crc_at, head + rem, &stored_crc) != 0)
            return -2;

        if (klen + 5 > sizeof(stack_buf)) {
            tmp = malloc((size_t)klen + 5);
            if (!tmp) return -2;
        }

        tmp[0] = head[0];
        tmp[1] = head[1];
        tmp[2] = head[2];
        tmp[3] = head[3];
        tmp[4] = head[4];
        memcpy(tmp + 5, head + 5, klen);

        calc_crc = crc32_compute(tmp, (size_t)klen + 5);

        if (tmp != stack_buf) free(tmp);

        if (calc_crc != stored_crc)
            return -1;
    }

    *op       = head[0];
    *key      = head + 5;
    *key_len  = klen;
    *consumed = (uint32_t)(1 + 4 + (size_t)klen + 4);
    *p       += *consumed;
    *remaining = rem - *consumed;
    return 0;
}

/* ─── Header I/O ────────────────────────────────────────────────────────── */

/* Write a fresh header and fsync. Returns 0 on success, -1 on error. */
static int wal_write_header(int fd)
{
    uint8_t hdr[16];
    uint32_t crc;
    ssize_t wr;

    hdr[0] = 'D'; hdr[1] = 'A'; hdr[2] = 'W'; hdr[3] = 'L';
    hdr[4] = 1; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    hdr[8]  = 0; hdr[9]  = 0; hdr[10] = 0; hdr[11] = 0;

    crc = crc32_compute(hdr, 12);
    hdr[12] = (uint8_t)(crc);
    hdr[13] = (uint8_t)(crc >> 8);
    hdr[14] = (uint8_t)(crc >> 16);
    hdr[15] = (uint8_t)(crc >> 24);

    wr = write(fd, hdr, 16);
    if (wr != 16) return -1;

    return fsync(fd) == 0 ? 0 : -1;
}

/* Validate the header at `map` (size `size`).  On success, scans forward
 * from byte 16 to find the first invalid/torn record and sets *good_bytes_out
 * to the file offset at the START of that first bad record (so it is safe
 * to ftruncate to that offset — it is the last good byte + 1).
 *
 * Returns 0 on success, -1 on hard error (bad magic, bad version, or header
 * CRC mismatch on a non-empty file). */
static int wal_validate_header(const uint8_t *map, size_t size,
                               size_t *good_bytes_out)
{
    uint32_t version, flags, stored_crc, calc_crc;

    if (size < 16) return -1;
    if (map[0] != 'D' || map[1] != 'A' || map[2] != 'W' || map[3] != 'L')
        return -1;

    if (wal_read_u32(map + 4, map + 16, &version) != 0) return -1;
    if (version != 1) return -1;

    if (wal_read_u32(map + 8, map + 16, &flags) != 0) return -1;
    (void)flags;

    calc_crc = crc32_compute(map, 12);
    if (wal_read_u32(map + 12, map + 16, &stored_crc) != 0) return -1;
    if (calc_crc != stored_crc) return -1;  /* hard error on non-empty */

    /* Scan records from byte 16; stop at first invalid record.
     * good_bytes_out = start offset of the first bad record = truncation point. */
    {
        const uint8_t *p = map + 16;
        size_t remaining = size - 16;

        while (remaining > 0) {
            uint8_t op;
            const unsigned char *key;
            uint32_t key_len, consumed;
            const uint8_t *before = p;
            int rc;

            rc = wal_validate_record(&p, &remaining, &op, &key,
                                     &key_len, &consumed);
            if (rc == 0) continue;
            /* rc == -1 or -2: record is bad → truncate at its start */
            *good_bytes_out = (size_t)(before - map);
            return 0;
        }
        *good_bytes_out = size;
    }
    return 0;
}

/* ─── WAL lifecycle ─────────────────────────────────────────────────────── */

/* Writer-side open: O_RDWR|O_CREAT|O_APPEND.  May write a fresh header or
 * ftruncate a torn tail.  Use this for update() / compact() paths only. */
dafsa_wal *dafsa_wal_open_rw(const char *path)
{
    int fd = -1;
    dafsa_wal *w = NULL;
    struct stat st;

    if (!path) return NULL;

    fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return NULL;

    if (fstat(fd, &st) != 0) { close(fd); return NULL; }

    w = calloc(1, sizeof(*w));
    if (!w) { close(fd); return NULL; }
    w->fd = fd;

    if (st.st_size == 0) {
        if (wal_write_header(fd) != 0) {
            close(fd); free(w); return NULL;
        }
        w->size = 16;
    } else {
        uint8_t *map;
        size_t good_bytes;

        map = (uint8_t *)mmap(NULL, (size_t)st.st_size, PROT_READ,
                              MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { close(fd); free(w); return NULL; }

        if (wal_validate_header(map, (size_t)st.st_size, &good_bytes) != 0) {
            munmap(map, (size_t)st.st_size);
            /* Header-only file (16 bytes) with corrupt header: a crash
             * during initial header write left garbage.  Reinitialize. */
            if ((size_t)st.st_size == 16) {
                if (ftruncate(fd, 0) != 0 ||
                    wal_write_header(fd) != 0) {
                    close(fd); free(w); return NULL;
                }
                w->size = 16;
                return w;
            }
            /* Non-empty file with corrupt header: hard error */
            close(fd); free(w); return NULL;
        }

        if (good_bytes < (size_t)st.st_size) {
            if (ftruncate(fd, (off_t)good_bytes) != 0) {
                munmap(map, (size_t)st.st_size);
                close(fd); free(w); return NULL;
            }
        }

        munmap(map, (size_t)st.st_size);
        w->size = (uint64_t)good_bytes;
    }

    return w;
}

/* Reader-side open: O_RDONLY, no O_CREAT, never mutates the file.
 * Validates the header, scans for torn tail but does NOT ftruncate —
 * the self-framing record format already handles torn tails by stopping
 * at the first invalid CRC.  Returns NULL if the file is missing, empty,
 * or has a corrupt header. */
dafsa_wal *dafsa_wal_open_ro(const char *path)
{
    int fd = -1;
    dafsa_wal *w = NULL;
    struct stat st;

    if (!path) return NULL;

    fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    if (fstat(fd, &st) != 0) { close(fd); return NULL; }

    if (st.st_size < 16) {
        /* Too small to contain a valid header — not a usable WAL. */
        close(fd); return NULL;
    }

    w = calloc(1, sizeof(*w));
    if (!w) { close(fd); return NULL; }
    w->fd = fd;

    {
        uint8_t *map;
        size_t good_bytes;

        map = (uint8_t *)mmap(NULL, (size_t)st.st_size, PROT_READ,
                              MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { close(fd); free(w); return NULL; }

        if (wal_validate_header(map, (size_t)st.st_size, &good_bytes) != 0) {
            /* Corrupt header: reader cannot repair — return error. */
            munmap(map, (size_t)st.st_size);
            close(fd); free(w); return NULL;
        }

        munmap(map, (size_t)st.st_size);
        /* Remember where the valid records end; replay stops at the
         * first invalid record anyway, so a torn tail is harmless. */
        w->size = (uint64_t)good_bytes;
    }

    return w;
}

/* Back-compat alias: writer-side open (same as dafsa_wal_open_rw). */
dafsa_wal *dafsa_wal_open(const char *path)
{
    return dafsa_wal_open_rw(path);
}

/* ─── Append ────────────────────────────────────────────────────────────── */

static int wal_append_op(dafsa_wal *w, uint8_t op,
                         const unsigned char *key, uint32_t key_len)
{
    unsigned char *buf;
    uint32_t crc;
    const unsigned char *p;
    size_t remaining, total;

    if (!w || !key) return -1;
    if (key_len < 1 || key_len > (uint32_t)(MAX_WORD_LEN + 9)) return -1;

    total = 1 + 4 + (size_t)key_len + 4;
    buf = malloc(total);
    if (!buf) return -1;

    buf[0] = op;
    buf[1] = (uint8_t)(key_len);
    buf[2] = (uint8_t)(key_len >> 8);
    buf[3] = (uint8_t)(key_len >> 16);
    buf[4] = (uint8_t)(key_len >> 24);
    memcpy(buf + 5, key, key_len);

    crc = crc32_compute(buf, 5 + (size_t)key_len);
    buf[5 + key_len]     = (uint8_t)(crc);
    buf[5 + key_len + 1] = (uint8_t)(crc >> 8);
    buf[5 + key_len + 2] = (uint8_t)(crc >> 16);
    buf[5 + key_len + 3] = (uint8_t)(crc >> 24);

    /* Write via O_APPEND; retry loop for partial writes */
    p = buf;
    remaining = total;
    while (remaining > 0) {
        ssize_t wr = write(w->fd, p, remaining);
        if (wr < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        if (wr == 0) {
            free(buf);
            errno = EIO;
            return -1;
        }
        p += (size_t)wr;
        remaining -= (size_t)wr;
    }

    w->size += (uint64_t)total;
    free(buf);
    return 0;
}

int dafsa_wal_append_add(dafsa_wal *w, const unsigned char *key, uint32_t key_len)
{
    return wal_append_op(w, DAFSA_WAL_OP_ADD, key, key_len);
}

int dafsa_wal_append_del(dafsa_wal *w, const unsigned char *key, uint32_t key_len)
{
    return wal_append_op(w, DAFSA_WAL_OP_DEL, key, key_len);
}

/* ─── Sync / Size ───────────────────────────────────────────────────────── */

int dafsa_wal_sync(dafsa_wal *w)
{
    if (!w) return -1;
    return fsync(w->fd) == 0 ? 0 : -1;
}

uint64_t dafsa_wal_size(const dafsa_wal *w)
{
    if (!w) return 0;
    return w->size;
}

/* ─── Replay ────────────────────────────────────────────────────────────── */

int dafsa_wal_replay(dafsa_wal *w, dafsa_wal_replay_cb cb, void *user)
{
    uint8_t *map;
    const uint8_t *p;
    size_t remaining, map_size;
    struct stat st;

    if (!w || !cb) return -1;

    if (fstat(w->fd, &st) != 0) return -1;
    map_size = (size_t)st.st_size;
    if (map_size < 16) return -1;

    map = (uint8_t *)mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, w->fd, 0);
    if (map == MAP_FAILED) return -1;

    p = map + 16;
    remaining = map_size - 16;

    while (remaining > 0) {
        uint8_t op;
        const unsigned char *key;
        uint32_t key_len, consumed;
        int rc = wal_validate_record(&p, &remaining, &op, &key,
                                     &key_len, &consumed);
        if (rc != 0) break;

        if (cb(op, key, key_len, user) != 0) {
            munmap(map, map_size);
            return -1;
        }
    }

    munmap(map, map_size);
    return 0;
}

/* ─── Close ─────────────────────────────────────────────────────────────── */

void dafsa_wal_close(dafsa_wal *w)
{
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    free(w);
}
