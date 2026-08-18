/*
 * txnwal.h — Single transaction write-ahead log for the CAS/transaction API
 *
 * A txn WAL is a per-database append-only file (<db_dir>/txn.wal) that
 * records every operation of a transaction followed by a COMMIT marker, so
 * a whole transaction commits (or drops) atomically across relations.
 *
 * Self-framing record format:
 *   HEADER (16 B): magic "DTXL" | version:u32LE=1 | flags:u32LE=0
 *                  | header_crc:u32LE (over the first 12 bytes)
 *   RECORD: rel_len:u16LE | rel[rel_len] | op:u8 | key_len:u32LE
 *           | key[key_len] | rec_crc:u32LE
 *   rec_crc = crc32(rel_len || rel || op || key_len || key)
 *   COMMIT marker record: rel_len=0, op=TXNWAL_OP_COMMIT (rel name empty,
 *   key_len=0).  A committed transaction is exactly the run of records
 *   ending in a valid COMMIT marker.
 *
 * Opcodes: 1=ADD, 2=DEL, 3=COMMIT (see TXNWAL_OP_*).
 *
 * A torn tail (partial record, or valid records with no trailing COMMIT) is
 * never committed: on reopen the committed-prefix scan drops everything after
 * the last valid COMMIT marker.  crc32_compute comes from dafsa_internal.h
 * (the same CRC used by the per-relation WAL), for a consistent framing
 * style.
 *
 * This is a NEW file — do not reuse the per-relation WAL (dafsa_wal); a
 * per-relation WAL cannot express cross-relation atomicity.
 */
#ifndef TXNWAL_H
#define TXNWAL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>   /* off_t */

#ifdef __cplusplus
extern "C" {
#endif

/* On-disk record opcodes. */
#define TXNWAL_OP_ADD    1   /* add a fact */
#define TXNWAL_OP_DEL    2   /* delete a fact */
#define TXNWAL_OP_COMMIT 3   /* transaction commit marker (rel_len=0) */

/* Opaque handle. */
typedef struct txnwal txnwal;

/* Replay callback: invoked once per committed record, in WAL order.  `rel`
 * is rel_len bytes and is NOT NUL-terminated; `key` is key_len raw bytes
 * (the caller decodes it — see dl.c's replay_txn_wal).  Return non-zero to
 * abort the replay. */
typedef int (*txnwal_replay_cb)(const char *rel, uint16_t rel_len,
                                uint8_t op, const unsigned char *key,
                                uint32_t key_len, void *user);

/* Open (or create) <db_dir>/txn.wal read-write.  On open, a torn tail (any
 * bytes after the last valid COMMIT marker) is truncated so subsequent
 * appends start at a clean committed boundary.  Returns NULL on error. */
txnwal *txnwal_open_rw(const char *db_dir);

/* Append one ADD/DEL record for relation `rel` (rel_len bytes, may be any
 * non-zero length; the caller keeps it alive only for the duration of this
 * call).  `op` must be TXNWAL_OP_ADD or TXNWAL_OP_DEL.  Not fsync'd until
 * txnwal_sync.  Returns 0 on success, -1 on error. */
int txnwal_append_record(txnwal *w, const char *rel, uint16_t rel_len,
                         uint8_t op, const unsigned char *key,
                         uint32_t key_len);

/* Append the COMMIT marker record (the last record of a committed txn).
 * Returns 0 on success, -1 on error. */
int txnwal_append_commit(txnwal *w);

/* fsync the WAL so all appended records (including the COMMIT marker) are
 * durable.  Returns 0 on success, -1 on error. */
int txnwal_sync(txnwal *w);

/* Replay the committed prefix (all records up to and including the last
 * valid COMMIT marker) via cb.  Any torn/partial tail beyond the last COMMIT
 * is ignored.  *good_bytes_out is set to the byte offset of the end of the
 * committed prefix (a safe truncation point).  Returns 0 on success, -1 on
 * error. */
int txnwal_replay(txnwal *w, txnwal_replay_cb cb, void *user,
                  off_t *good_bytes_out);

/* Truncate the WAL to `offset` bytes and fsync (used by the caller to
 * consume committed records after replay + compaction).  Returns 0 on
 * success, -1 on error. */
int txnwal_truncate(txnwal *w, off_t offset);

/* Close the handle (NULL-safe). */
void txnwal_close(txnwal *w);

#ifdef __cplusplus
}
#endif

#endif /* TXNWAL_H */
