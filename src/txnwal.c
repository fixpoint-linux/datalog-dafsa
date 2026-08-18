/*
 * txnwal.c — Single transaction write-ahead log (CAS/transaction API)
 *
 * Self-framing record format (see txnwal.h):
 *   HEADER (16 B): "DTXL" | version:u32LE=1 | flags:u32LE=0
 *                  | header_crc:u32LE (over the first 12 bytes)
 *   RECORD: rel_len:u16LE | rel[rel_len] | op:u8 | key_len:u32LE
 *           | key[key_len] | rec_crc:u32LE
 *   COMMIT marker: rel_len=0, op=3.
 *
 * Crash recovery: a transaction is committed only when a valid COMMIT marker
 * (rel_len=0, op=3) follows its records.  On open we scan forward from the
 * header and truncate any torn tail (bytes after the last valid COMMIT); on
 * replay we deliver only the committed prefix.  CRC framing mirrors the
 * per-relation WAL (dafsa_wal.c) using the same crc32_compute.
 */
#include "txnwal.h"
#include "dafsa_internal.h"   /* crc32_compute */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

struct txnwal {
    int      fd;
    uint64_t size;   /* current length in bytes (>= 16) */
};

/* ─── Little-endian readers ─────────────────────────────────────────────── */

/* Read a little-endian u16 from p; returns <0 on short read. */
static int txnwal_read_u16(const uint8_t *p, const uint8_t *end, uint16_t *out)
{
    if (p + 2 > end) return -1;
    *out = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return 0;
}

/* Read a little-endian u32 from p; returns <0 on short read. */
static int txnwal_read_u32(const uint8_t *p, const uint8_t *end, uint32_t *out)
{
    if (p + 4 > end) return -1;
    *out = (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
    return 0;
}

/* ─── Record validation ─────────────────────────────────────────────────── */

/* Validate one record at *p with *remaining bytes available.
 * Returns:
 *    0  — valid record: fields set, *p and *remaining advanced
 *   -1  — corrupt record (bad op, bad rel_len/key_len, or CRC mismatch)
 *   -2  — torn/partial record (not enough bytes for a complete record)
 * On a non-zero return, *p is NOT advanced.
 *
 * This is the single validation function used by both the open-time
 * committed-prefix scan and replay, so the committed-prefix boundary is
 * computed identically everywhere (no off-by-one / dup / loss risk). */
static int txnwal_validate_record(const uint8_t **p, size_t *remaining,
                                  uint16_t *rel_len, const char **rel,
                                  uint8_t *op, const unsigned char **key,
                                  uint32_t *key_len, uint32_t *consumed)
{
    const uint8_t *head = *p;
    size_t rem = *remaining;
    uint16_t rlen;
    uint32_t klen;
    uint8_t o;

    /* Minimum: rel_len(2) + op(1) + key_len(4) + crc(4) = 11 bytes. */
    if (rem < 11) return -2;

    if (txnwal_read_u16(head, head + rem, &rlen) != 0) return -2;
    if ((size_t)2 + rlen + 1 + 4 + 4 > rem) return -2;

    o = head[2 + rlen];
    if (o != TXNWAL_OP_ADD && o != TXNWAL_OP_DEL && o != TXNWAL_OP_COMMIT)
        return -1;
    if (o == TXNWAL_OP_COMMIT && rlen != 0) return -1;

    if (txnwal_read_u32(head + 2 + rlen + 1, head + rem, &klen) != 0)
        return -2;

    /* Need rel_len(2)+rel(rlen)+op(1)+key_len(4)+key(klen)+crc(4). */
    if ((size_t)2 + rlen + 1 + 4 + klen + 4 > rem) return -2;

    /* rec_crc over rel_len || rel || op || key_len || key. */
    {
        const uint8_t *crc_at = head + 2 + rlen + 1 + 4 + klen;
        size_t body = (size_t)2 + rlen + 1 + 4 + klen;
        uint32_t stored_crc, calc_crc;
        if (txnwal_read_u32(crc_at, head + rem, &stored_crc) != 0) return -2;
        calc_crc = crc32_compute(head, body);
        if (calc_crc != stored_crc) return -1;
    }

    *rel_len = rlen;
    *rel     = (const char *)(head + 2);
    *op      = o;
    *key     = head + 2 + rlen + 1 + 4;
    *key_len = klen;
    *consumed = (uint32_t)(2 + rlen + 1 + 4 + klen + 4);
    *p += *consumed;
    *remaining = rem - *consumed;
    return 0;
}

/* ─── Header I/O ────────────────────────────────────────────────────────── */

/* Write a fresh header and fsync.  Returns 0 on success, -1 on error. */
static int txnwal_write_header(int fd)
{
    uint8_t hdr[16];
    uint32_t crc;
    ssize_t wr;

    hdr[0] = 'D'; hdr[1] = 'T'; hdr[2] = 'X'; hdr[3] = 'L';
    hdr[4] = 1; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;      /* version 1 LE */
    hdr[8] = 0; hdr[9] = 0; hdr[10] = 0; hdr[11] = 0;    /* flags 0 */

    crc = crc32_compute(hdr, 12);
    hdr[12] = (uint8_t)(crc);
    hdr[13] = (uint8_t)(crc >> 8);
    hdr[14] = (uint8_t)(crc >> 16);
    hdr[15] = (uint8_t)(crc >> 24);

    wr = write(fd, hdr, 16);
    if (wr != 16) return -1;
    return fsync(fd) == 0 ? 0 : -1;
}

/* Validate the header at `map` (file size `size`).  Returns 0 on success
 * (version/flags/crc all good), -1 on a bad magic, bad version, or header
 * CRC mismatch. */
static int txnwal_validate_header(const uint8_t *map, size_t size)
{
    uint32_t version, flags, stored_crc, calc_crc;

    if (size < 16) return -1;
    if (map[0] != 'D' || map[1] != 'T' || map[2] != 'X' || map[3] != 'L')
        return -1;
    if (txnwal_read_u32(map + 4, map + 16, &version) != 0) return -1;
    if (version != 1) return -1;
    if (txnwal_read_u32(map + 8, map + 16, &flags) != 0) return -1;
    (void)flags;
    calc_crc = crc32_compute(map, 12);
    if (txnwal_read_u32(map + 12, map + 16, &stored_crc) != 0) return -1;
    if (calc_crc != stored_crc) return -1;
    return 0;
}

/* Scan records after the header and return the byte offset of the END of the
 * last valid COMMIT marker (the committed-prefix boundary) into
 * *good_bytes_out.  Scanning stops at the first corrupt/torn record, so any
 * records after the last valid COMMIT (an uncommitted tail) are dropped.
 * Returns 0 on success (good_bytes always set), -1 on a header error. */
static int txnwal_committed_end(const uint8_t *map, size_t size,
                                size_t *good_bytes_out)
{
    const uint8_t *p = map + 16;
    size_t remaining = size - 16;
    size_t committed_end = 16;

    if (txnwal_validate_header(map, size) != 0) return -1;

    while (remaining > 0) {
        uint16_t rel_len;
        const char *rel;
        uint8_t op;
        const unsigned char *key;
        uint32_t key_len, consumed;
        int rc = txnwal_validate_record(&p, &remaining, &rel_len, &rel,
                                        &op, &key, &key_len, &consumed);
        (void)rel; (void)key;
        if (rc != 0) break;              /* corrupt/torn: stop */
        if (op == TXNWAL_OP_COMMIT) {
            committed_end = (size_t)(p - map);   /* end of this COMMIT record */
        }
    }
    *good_bytes_out = committed_end;
    return 0;
}

/* ─── Write-all helper ──────────────────────────────────────────────────── */

/* Write buf fully to fd, retrying on EINTR / partial writes. */
static int txnwal_write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t wr = write(fd, p, left);
        if (wr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (wr == 0) { errno = EIO; return -1; }
        p += (size_t)wr;
        left -= (size_t)wr;
    }
    return 0;
}

/* ─── Lifecycle ─────────────────────────────────────────────────────────── */

txnwal *txnwal_open_rw(const char *db_dir)
{
    char *path;
    txnwal *w;
    int fd;
    struct stat st;
    size_t dlen;

    if (!db_dir) return NULL;

    dlen = strlen(db_dir);
    path = malloc(dlen + 8 + 1);          /* "/txn.wal" = 8 chars + NUL */
    if (!path) return NULL;
    memcpy(path, db_dir, dlen);
    memcpy(path + dlen, "/txn.wal", 9);   /* 8 chars + trailing NUL */

    fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    free(path);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }

    w = calloc(1, sizeof(*w));
    if (!w) { close(fd); return NULL; }
    w->fd = fd;

    if (st.st_size == 0) {
        if (txnwal_write_header(fd) != 0) {
            close(fd); free(w); return NULL;
        }
        w->size = 16;
        return w;
    }

    /* Existing file: validate header, truncate any torn tail (bytes after
     * the last valid COMMIT marker) so appends start at a committed
     * boundary. */
    {
        uint8_t *map;
        size_t committed_end;

        map = (uint8_t *)mmap(NULL, (size_t)st.st_size, PROT_READ,
                              MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { close(fd); free(w); return NULL; }

        if (txnwal_validate_header(map, (size_t)st.st_size) != 0) {
            /* Header-only file (16 B) with a corrupt header: a crash during
             * the initial header write left garbage — reinitialize. */
            if ((size_t)st.st_size == 16) {
                munmap(map, (size_t)st.st_size);
                if (ftruncate(fd, 0) != 0 || txnwal_write_header(fd) != 0) {
                    close(fd); free(w); return NULL;
                }
                w->size = 16;
                return w;
            }
            munmap(map, (size_t)st.st_size);
            close(fd); free(w); return NULL;   /* non-empty corrupt header */
        }

        if (txnwal_committed_end(map, (size_t)st.st_size,
                                 &committed_end) != 0) {
            munmap(map, (size_t)st.st_size);
            close(fd); free(w); return NULL;
        }

        if (committed_end < (size_t)st.st_size) {
            if (ftruncate(fd, (off_t)committed_end) != 0) {
                munmap(map, (size_t)st.st_size);
                close(fd); free(w); return NULL;
            }
        }

        munmap(map, (size_t)st.st_size);
        w->size = (uint64_t)committed_end;
    }
    return w;
}

/* ─── Append ────────────────────────────────────────────────────────────── */

int txnwal_append_record(txnwal *w, const char *rel, uint16_t rel_len,
                         uint8_t op, const unsigned char *key,
                         uint32_t key_len)
{
    unsigned char *buf;
    uint32_t crc;
    size_t body, total;

    if (!w || !rel || !key) return -1;
    if (op != TXNWAL_OP_ADD && op != TXNWAL_OP_DEL) return -1;
    if (rel_len == 0) return -1;         /* a record must carry a name */

    body = (size_t)2 + rel_len + 1 + 4 + key_len;
    total = body + 4;                    /* + rec_crc */
    buf = malloc(total);
    if (!buf) return -1;

    buf[0] = (uint8_t)(rel_len);
    buf[1] = (uint8_t)(rel_len >> 8);
    memcpy(buf + 2, rel, rel_len);
    buf[2 + rel_len] = op;
    buf[2 + rel_len + 1] = (uint8_t)(key_len);
    buf[2 + rel_len + 2] = (uint8_t)(key_len >> 8);
    buf[2 + rel_len + 3] = (uint8_t)(key_len >> 16);
    buf[2 + rel_len + 4] = (uint8_t)(key_len >> 24);
    memcpy(buf + 2 + rel_len + 1 + 4, key, key_len);

    crc = crc32_compute(buf, body);
    buf[body]     = (uint8_t)(crc);
    buf[body + 1] = (uint8_t)(crc >> 8);
    buf[body + 2] = (uint8_t)(crc >> 16);
    buf[body + 3] = (uint8_t)(crc >> 24);

    if (txnwal_write_all(w->fd, buf, total) != 0) {
        free(buf);
        return -1;
    }
    w->size += (uint64_t)total;
    free(buf);
    return 0;
}

int txnwal_append_commit(txnwal *w)
{
    unsigned char buf[11];   /* rel_len(2)+op(1)+key_len(4)+crc(4) */
    uint32_t crc;

    if (!w) return -1;
    buf[0] = 0; buf[1] = 0;                /* rel_len = 0 */
    buf[2] = TXNWAL_OP_COMMIT;
    buf[3] = 0; buf[4] = 0; buf[5] = 0; buf[6] = 0;   /* key_len = 0 */

    crc = crc32_compute(buf, 7);           /* body = rel_len||op||key_len */
    buf[7]  = (uint8_t)(crc);
    buf[8]  = (uint8_t)(crc >> 8);
    buf[9]  = (uint8_t)(crc >> 16);
    buf[10] = (uint8_t)(crc >> 24);

    if (txnwal_write_all(w->fd, buf, 11) != 0) return -1;
    w->size += 11;
    return 0;
}

/* ─── Sync / Truncate / Close ───────────────────────────────────────────── */

int txnwal_sync(txnwal *w)
{
    if (!w) return -1;
    return fsync(w->fd) == 0 ? 0 : -1;
}

int txnwal_truncate(txnwal *w, off_t offset)
{
    if (!w) return -1;
    if (offset < 16) offset = 16;
    if (ftruncate(w->fd, offset) != 0) return -1;
    if (fsync(w->fd) != 0) return -1;
    w->size = (uint64_t)offset;
    return 0;
}

void txnwal_close(txnwal *w)
{
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    free(w);
}

/* ─── Replay ────────────────────────────────────────────────────────────── */

int txnwal_replay(txnwal *w, txnwal_replay_cb cb, void *user,
                  off_t *good_bytes_out)
{
    uint8_t *map;
    const uint8_t *p;
    size_t remaining, map_size, committed_end;
    struct stat st;
    int rc = 0;

    if (!w || !cb) return -1;
    if (fstat(w->fd, &st) != 0) return -1;
    map_size = (size_t)st.st_size;
    if (map_size < 16) return -1;

    map = (uint8_t *)mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, w->fd, 0);
    if (map == MAP_FAILED) return -1;

    if (txnwal_committed_end(map, map_size, &committed_end) != 0) {
        munmap(map, map_size);
        return -1;
    }
    if (good_bytes_out) *good_bytes_out = (off_t)committed_end;

    /* Deliver only the committed prefix [header_end, committed_end). */
    p = map + 16;
    remaining = committed_end - 16;
    while (remaining > 0) {
        uint16_t rel_len;
        const char *rel;
        uint8_t op;
        const unsigned char *key;
        uint32_t key_len, consumed;
        if (txnwal_validate_record(&p, &remaining, &rel_len, &rel,
                                   &op, &key, &key_len, &consumed) != 0) {
            /* Should not happen: the committed prefix was validated by the
             * scan above.  Treat as an abort to avoid silently dropping a
             * committed record. */
            rc = -1;
            break;
        }
        if (op != TXNWAL_OP_COMMIT) {
            if (cb(rel, rel_len, op, key, key_len, user) != 0) {
                rc = -1;
                break;
            }
        }
    }

    munmap(map, map_size);
    return rc;
}
