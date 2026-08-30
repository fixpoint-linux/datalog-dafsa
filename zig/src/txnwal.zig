//! txnwal.zig — port of src/txnwal.c (single-transaction write-ahead log).
//!
//! Self-framing record format (this IS the WAL byte-equality gate):
//!   HEADER (16 B): "DTXL" | version:u32LE=1 | flags:u32LE=0
//!                   | header_crc:u32LE (over the first 12 bytes)
//!   RECORD: rel_len:u16LE | rel[rel_len] | op:u8 | key_len:u32LE
//!           | key[key_len] | rec_crc:u32LE
//!   rec_crc = crc32_compute(rel_len||rel||op||key_len||key)
//!   COMMIT marker: rel_len=0, op=TXNWAL_OP_COMMIT (3).
//!
//! Strangler-hybrid ABI: `struct txnwal` is OPAQUE in txnwal.h; the
//! implementation is a native Zig {fd, size} struct (no heap pointers), and
//! every non-static C function is an `export fn` with the exact C name,
//! signature and return semantics.  crc32_compute comes from dafsa_internal.h
//! (@cImport); raw libc syscalls via std.c (typed O_* flags) and @cImport
//! (fstat/mmap/munmap) so framing bytes and EINTR/partial-write handling
//! match the C oracle exactly.
//!
//! Oracle: src/txnwal.c (never modified).

const std = @import("std");
const c = std.c;

const dc = @cImport({
    @cInclude("dafsa_internal.h"); // crc32_compute, fstat/struct_stat, mmap/munmap
    @cInclude("errno.h"); // EINTR, EIO, __errno_location
});

const TXNWAL_OP_ADD: u8 = 1;
const TXNWAL_OP_DEL: u8 = 2;
const TXNWAL_OP_COMMIT: u8 = 3;

/// struct txnwal — opaque to C; native Zig layout { fd, size }.
pub const Txnwal = struct {
    fd: c_int,
    size: u64, // current length in bytes (>= 16)
};

/// typedef int (*txnwal_replay_cb)(const char *rel, uint16_t rel_len,
///     uint8_t op, const unsigned char *key, uint32_t key_len, void *user)
pub const TxnwalReplayCb = ?*const fn (rel: [*c]const u8, rel_len: u16, op: u8, key: [*c]const u8, key_len: u32, user: ?*anyopaque) callconv(.c) c_int;

/// Local strlen (std.c does not re-export it).
fn cstrLen(s: [*c]const u8) usize {
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {}
    return i;
}

// ─── Little-endian readers ────────────────────────────────────────────────

/// Read a little-endian u16 from p; returns <0 on short read.
fn txnwalReadU16(p: [*c]const u8, end: [*c]const u8, out: *u16) c_int {
    if (@intFromPtr(p) + 2 > @intFromPtr(end)) return -1;
    out.* = @as(u16, p[0]) | (@as(u16, p[1]) << 8);
    return 0;
}

/// Read a little-endian u32 from p; returns <0 on short read.
fn txnwalReadU32(p: [*c]const u8, end: [*c]const u8, out: *u32) c_int {
    if (@intFromPtr(p) + 4 > @intFromPtr(end)) return -1;
    out.* = @as(u32, p[0]) |
        (@as(u32, p[1]) << 8) |
        (@as(u32, p[2]) << 16) |
        (@as(u32, p[3]) << 24);
    return 0;
}

// ─── Record validation ────────────────────────────────────────────────────

/// Validate one record at *p with *remaining bytes available.
/// Returns 0 (valid; *p and *remaining advanced), -1 (corrupt), -2 (torn).
/// On a non-zero return, *p is NOT advanced.
fn txnwalValidateRecord(
    p: *[*c]const u8,
    remaining: *usize,
    rel_len: *u16,
    rel: *[*c]const u8,
    op: *u8,
    key: *[*c]const u8,
    key_len: *u32,
    consumed: *u32,
) c_int {
    const head = p.*;
    const rem = remaining.*;
    var rlen: u16 = 0;
    var klen: u32 = 0;
    var o: u8 = 0;

    // Minimum: rel_len(2) + op(1) + key_len(4) + crc(4) = 11 bytes.
    if (rem < 11) return -2;

    if (txnwalReadU16(head, head + rem, &rlen) != 0) return -2;
    if (@as(usize, 2) + @as(usize, rlen) + 1 + 4 + 4 > rem) return -2;

    o = head[2 + @as(usize, rlen)];
    if (o != TXNWAL_OP_ADD and o != TXNWAL_OP_DEL and o != TXNWAL_OP_COMMIT)
        return -1;
    if (o == TXNWAL_OP_COMMIT and rlen != 0) return -1;

    if (txnwalReadU32(head + 2 + @as(usize, rlen) + 1, head + rem, &klen) != 0)
        return -2;

    // Need rel_len(2)+rel(rlen)+op(1)+key_len(4)+key(klen)+crc(4).
    if (@as(usize, 2) + @as(usize, rlen) + 1 + 4 + @as(usize, klen) + 4 > rem)
        return -2;

    // rec_crc over rel_len || rel || op || key_len || key.
    {
        const crc_at = head + 2 + @as(usize, rlen) + 1 + 4 + @as(usize, klen);
        const body: usize = 2 + @as(usize, rlen) + 1 + 4 + @as(usize, klen);
        var stored_crc: u32 = 0;
        if (txnwalReadU32(crc_at, head + rem, &stored_crc) != 0) return -2;
        const calc_crc = dc.crc32_compute(head, body);
        if (calc_crc != stored_crc) return -1;
    }

    rel_len.* = rlen;
    rel.* = head + 2;
    op.* = o;
    key.* = head + 2 + @as(usize, rlen) + 1 + 4;
    key_len.* = klen;
    consumed.* = @intCast(2 + @as(usize, rlen) + 1 + 4 + @as(usize, klen) + 4);
    p.* = head + @as(usize, consumed.*);
    remaining.* = rem - @as(usize, consumed.*);
    return 0;
}

// ─── Header I/O ───────────────────────────────────────────────────────────

/// Write a fresh header and fsync.  Returns 0 on success, -1 on error.
fn txnwalWriteHeader(fd: c_int) c_int {
    var hdr: [16]u8 = undefined;
    hdr[0] = 'D';
    hdr[1] = 'T';
    hdr[2] = 'X';
    hdr[3] = 'L';
    hdr[4] = 1;
    hdr[5] = 0;
    hdr[6] = 0;
    hdr[7] = 0; // version 1 LE
    hdr[8] = 0;
    hdr[9] = 0;
    hdr[10] = 0;
    hdr[11] = 0; // flags 0

    const crc = dc.crc32_compute(&hdr, 12);
    hdr[12] = @truncate(crc);
    hdr[13] = @truncate(crc >> 8);
    hdr[14] = @truncate(crc >> 16);
    hdr[15] = @truncate(crc >> 24);

    const wr = c.write(fd, &hdr, 16);
    if (wr != 16) return -1;
    return if (c.fsync(fd) == 0) 0 else -1;
}

/// Validate the header at `map` (file size `size`).  0 on success, -1 on bad
/// magic / bad version / header CRC mismatch.
fn txnwalValidateHeader(map: [*c]const u8, size: usize) c_int {
    var version: u32 = 0;
    var flags: u32 = 0;
    var stored_crc: u32 = 0;

    if (size < 16) return -1;
    if (map[0] != 'D' or map[1] != 'T' or map[2] != 'X' or map[3] != 'L')
        return -1;
    if (txnwalReadU32(map + 4, map + 16, &version) != 0) return -1;
    if (version != 1) return -1;
    if (txnwalReadU32(map + 8, map + 16, &flags) != 0) return -1;
    // flags validated for framing; value unused (as in C's `(void)flags`)
    const calc_crc = dc.crc32_compute(map, 12);
    if (txnwalReadU32(map + 12, map + 16, &stored_crc) != 0) return -1;
    if (calc_crc != stored_crc) return -1;
    return 0;
}

/// Scan records after the header and return the byte offset of the END of the
/// last valid COMMIT marker into *good_bytes_out.  Stops at the first
/// corrupt/torn record.  Returns 0 (good_bytes always set), -1 on header error.
fn txnwalCommittedEnd(map: [*c]const u8, size: usize, good_bytes_out: *usize) c_int {
    var p: [*c]const u8 = map + 16;
    var remaining: usize = size - 16;
    var committed_end: usize = 16;

    if (txnwalValidateHeader(map, size) != 0) return -1;

    while (remaining > 0) {
        var rel_len: u16 = 0;
        var rel: [*c]const u8 = null;
        var op: u8 = 0;
        var key: [*c]const u8 = null;
        var key_len: u32 = 0;
        var consumed: u32 = 0;
        const rc = txnwalValidateRecord(&p, &remaining, &rel_len, &rel, &op, &key, &key_len, &consumed);
        if (rc != 0) break; // corrupt/torn: stop
        if (op == TXNWAL_OP_COMMIT) {
            committed_end = @intFromPtr(p) - @intFromPtr(map); // end of this COMMIT
        }
    }
    good_bytes_out.* = committed_end;
    return 0;
}

// ─── Write-all helper ─────────────────────────────────────────────────────

/// Write buf fully to fd, retrying on EINTR / partial writes.
fn txnwalWriteAll(fd: c_int, buf: [*]const u8, len: usize) c_int {
    var p: [*]const u8 = buf;
    var left: usize = len;
    while (left > 0) {
        const wr = c.write(fd, p, left);
        if (wr < 0) {
            if (dc.__errno_location().* == dc.EINTR) continue;
            return -1;
        }
        if (wr == 0) {
            dc.__errno_location().* = dc.EIO;
            return -1;
        }
        p += @as(usize, @intCast(wr));
        left -= @as(usize, @intCast(wr));
    }
    return 0;
}

/// Build "<db_dir>/txn.wal" (heap, NUL-terminated).
fn txnwalPath(db_dir: [*c]const u8) [*c]u8 {
    const dlen = cstrLen(db_dir);
    const mem = c.malloc(dlen + 8 + 1) orelse return null;
    const path: [*]u8 = @ptrCast(mem);
    @memcpy(path[0..dlen], db_dir[0..dlen]);
    @memcpy(path[dlen..][0..8], "/txn.wal"); // 8 chars
    path[dlen + 8] = 0; // trailing NUL (memcpy(..., "/txn.wal", 9) in C)
    return @ptrCast(path);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

/// txnwal *txnwal_open_rw(const char *db_dir)
pub export fn txnwal_open_rw(db_dir: [*c]const u8) ?*Txnwal {
    if (db_dir == null) return null;

    const path = txnwalPath(db_dir) orelse return null;
    const fd = c.open(path, .{ .ACCMODE = .RDWR, .CREAT = true, .APPEND = true }, @as(c.mode_t, 0o644));
    c.free(path);
    if (fd < 0) return null;

    var st: dc.struct_stat = undefined;
    if (dc.fstat(fd, &st) != 0) {
        _ = c.close(fd);
        return null;
    }

    const mem = c.calloc(1, @sizeOf(Txnwal)) orelse {
        _ = c.close(fd);
        return null;
    };
    const w: *Txnwal = @ptrCast(@alignCast(mem));
    w.* = std.mem.zeroes(Txnwal);
    w.fd = fd;

    if (st.st_size == 0) {
        if (txnwalWriteHeader(fd) != 0) {
            _ = c.close(fd);
            c.free(mem);
            return null;
        }
        w.size = 16;
        return w;
    }

    // Existing file: validate header, truncate any torn tail (bytes after the
    // last valid COMMIT marker).
    {
        const map_size: usize = @intCast(st.st_size);
        const map = dc.mmap(null, map_size, dc.PROT_READ, dc.MAP_PRIVATE, fd, 0);
        if (map == dc.MAP_FAILED) {
            _ = c.close(fd);
            c.free(mem);
            return null;
        }
        const mapu: [*c]const u8 = @ptrCast(map);

        if (txnwalValidateHeader(mapu, map_size) != 0) {
            // Header-only file (16 B) with a corrupt header: reinitialize.
            if (st.st_size == 16) {
                _ = dc.munmap(map, map_size);
                if (c.ftruncate(fd, 0) != 0 or txnwalWriteHeader(fd) != 0) {
                    _ = c.close(fd);
                    c.free(mem);
                    return null;
                }
                w.size = 16;
                return w;
            }
            _ = dc.munmap(map, map_size);
            _ = c.close(fd);
            c.free(mem);
            return null; // non-empty corrupt header
        }

        var committed_end: usize = 0;
        if (txnwalCommittedEnd(mapu, map_size, &committed_end) != 0) {
            _ = dc.munmap(map, map_size);
            _ = c.close(fd);
            c.free(mem);
            return null;
        }

        if (committed_end < map_size) {
            if (c.ftruncate(fd, @intCast(committed_end)) != 0) {
                _ = dc.munmap(map, map_size);
                _ = c.close(fd);
                c.free(mem);
                return null;
            }
        }

        _ = dc.munmap(map, map_size);
        w.size = @intCast(committed_end);
    }
    return w;
}

/// txnwal *txnwal_open_ro(const char *db_dir)
pub export fn txnwal_open_ro(db_dir: [*c]const u8) ?*Txnwal {
    if (db_dir == null) return null;

    const path = txnwalPath(db_dir) orelse return null;
    const fd = c.open(path, .{ .ACCMODE = .RDONLY });
    c.free(path);
    if (fd < 0) return null;

    var st: dc.struct_stat = undefined;
    if (dc.fstat(fd, &st) != 0) {
        _ = c.close(fd);
        return null;
    }
    if (st.st_size < 16) {
        _ = c.close(fd);
        return null; // header-only/empty
    }

    const mem = c.calloc(1, @sizeOf(Txnwal)) orelse {
        _ = c.close(fd);
        return null;
    };
    const w: *Txnwal = @ptrCast(@alignCast(mem));
    w.* = std.mem.zeroes(Txnwal);
    w.fd = fd;

    {
        const map_size: usize = @intCast(st.st_size);
        const map = dc.mmap(null, map_size, dc.PROT_READ, dc.MAP_PRIVATE, fd, 0);
        if (map == dc.MAP_FAILED) {
            _ = c.close(fd);
            c.free(mem);
            return null;
        }
        const mapu: [*c]const u8 = @ptrCast(map);

        if (txnwalValidateHeader(mapu, map_size) != 0) {
            _ = dc.munmap(map, map_size);
            _ = c.close(fd);
            c.free(mem);
            return null;
        }

        // Scan (not truncate) the committed boundary: replay stops there.
        var committed_end: usize = 0;
        if (txnwalCommittedEnd(mapu, map_size, &committed_end) != 0) {
            _ = dc.munmap(map, map_size);
            _ = c.close(fd);
            c.free(mem);
            return null;
        }

        _ = dc.munmap(map, map_size);
        w.size = @intCast(committed_end);
    }
    return w;
}

// ─── Append ───────────────────────────────────────────────────────────────

/// int txnwal_append_record(txnwal *w, const char *rel, uint16_t rel_len,
///                          uint8_t op, const unsigned char *key,
///                          uint32_t key_len)
pub export fn txnwal_append_record(w: ?*Txnwal, rel: [*c]const u8, rel_len: u16, op: u8, key: [*c]const u8, key_len: u32) c_int {
    const ww = w orelse return -1;
    if (rel == null or key == null) return -1;
    if (op != TXNWAL_OP_ADD and op != TXNWAL_OP_DEL) return -1;
    if (rel_len == 0) return -1; // a record must carry a name

    const body: usize = 2 + @as(usize, rel_len) + 1 + 4 + @as(usize, key_len);
    const total = body + 4; // + rec_crc
    const mem = c.malloc(total) orelse return -1;
    const buf: [*]u8 = @ptrCast(mem);

    buf[0] = @truncate(rel_len);
    buf[1] = @truncate(rel_len >> 8);
    @memcpy(buf[2..][0..@as(usize, rel_len)], rel[0..@as(usize, rel_len)]);
    buf[2 + @as(usize, rel_len)] = op;
    buf[2 + @as(usize, rel_len) + 1] = @truncate(key_len);
    buf[2 + @as(usize, rel_len) + 2] = @truncate(key_len >> 8);
    buf[2 + @as(usize, rel_len) + 3] = @truncate(key_len >> 16);
    buf[2 + @as(usize, rel_len) + 4] = @truncate(key_len >> 24);
    @memcpy(buf[2 + @as(usize, rel_len) + 1 + 4 ..][0..@as(usize, key_len)], key[0..@as(usize, key_len)]);

    const crc = dc.crc32_compute(buf, body);
    buf[body] = @truncate(crc);
    buf[body + 1] = @truncate(crc >> 8);
    buf[body + 2] = @truncate(crc >> 16);
    buf[body + 3] = @truncate(crc >> 24);

    if (txnwalWriteAll(ww.fd, buf, total) != 0) {
        c.free(mem);
        return -1;
    }
    ww.size += @intCast(total);
    c.free(mem);
    return 0;
}

/// int txnwal_append_commit(txnwal *w)
pub export fn txnwal_append_commit(w: ?*Txnwal) c_int {
    const ww = w orelse return -1;
    var buf: [11]u8 = undefined; // rel_len(2)+op(1)+key_len(4)+crc(4)
    buf[0] = 0;
    buf[1] = 0; // rel_len = 0
    buf[2] = TXNWAL_OP_COMMIT;
    buf[3] = 0;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0; // key_len = 0

    const crc = dc.crc32_compute(&buf, 7); // body = rel_len||op||key_len
    buf[7] = @truncate(crc);
    buf[8] = @truncate(crc >> 8);
    buf[9] = @truncate(crc >> 16);
    buf[10] = @truncate(crc >> 24);

    if (txnwalWriteAll(ww.fd, &buf, 11) != 0) return -1;
    ww.size += 11;
    return 0;
}

// ─── Sync / Truncate / Close ──────────────────────────────────────────────

/// int txnwal_sync(txnwal *w)
pub export fn txnwal_sync(w: ?*Txnwal) c_int {
    const ww = w orelse return -1;
    return if (c.fsync(ww.fd) == 0) 0 else -1;
}

/// int txnwal_truncate(txnwal *w, off_t offset)
pub export fn txnwal_truncate(w: ?*Txnwal, offset: c_long) c_int {
    const ww = w orelse return -1;
    var off = offset;
    if (off < 16) off = 16;
    if (c.ftruncate(ww.fd, off) != 0) return -1;
    if (c.fsync(ww.fd) != 0) return -1;
    ww.size = @intCast(off);
    return 0;
}

/// void txnwal_close(txnwal *w)
pub export fn txnwal_close(w: ?*Txnwal) void {
    const ww = w orelse return;
    if (ww.fd >= 0) _ = c.close(ww.fd);
    c.free(@ptrCast(ww));
}

// ─── Replay ───────────────────────────────────────────────────────────────

/// int txnwal_replay(txnwal *w, txnwal_replay_cb cb, void *user,
///                   off_t *good_bytes_out)
pub export fn txnwal_replay(w: ?*Txnwal, cb: TxnwalReplayCb, user: ?*anyopaque, good_bytes_out: ?[*]c_long) c_int {
    const ww = w orelse return -1;
    if (cb == null) return -1;

    var st: dc.struct_stat = undefined;
    if (dc.fstat(ww.fd, &st) != 0) return -1;
    const map_size: usize = @intCast(st.st_size);
    if (map_size < 16) return -1;

    const map = dc.mmap(null, map_size, dc.PROT_READ, dc.MAP_PRIVATE, ww.fd, 0);
    if (map == dc.MAP_FAILED) return -1;
    const mapu: [*c]const u8 = @ptrCast(map);

    var committed_end: usize = 0;
    if (txnwalCommittedEnd(mapu, map_size, &committed_end) != 0) {
        _ = dc.munmap(map, map_size);
        return -1;
    }
    if (good_bytes_out) |out| out[0] = @intCast(committed_end);

    // Deliver only the committed prefix [header_end, committed_end).
    var rc: c_int = 0;
    var p: [*c]const u8 = mapu + 16;
    var remaining: usize = committed_end - 16;
    while (remaining > 0) {
        var rel_len: u16 = 0;
        var rel: [*c]const u8 = null;
        var op: u8 = 0;
        var key: [*c]const u8 = null;
        var key_len: u32 = 0;
        var consumed: u32 = 0;
        if (txnwalValidateRecord(&p, &remaining, &rel_len, &rel, &op, &key, &key_len, &consumed) != 0) {
            // Should not happen: the committed prefix was validated above.
            rc = -1;
            break;
        }
        if (op != TXNWAL_OP_COMMIT) {
            if (cb.?(rel, rel_len, op, key, key_len, user) != 0) {
                rc = -1;
                break;
            }
        }
    }

    _ = dc.munmap(map, map_size);
    return rc;
}

// ─── Tests ────────────────────────────────────────────────────────────────

fn replayCb(rel: [*c]const u8, rel_len: u16, op: u8, key: [*c]const u8, key_len: u32, user: ?*anyopaque) callconv(.c) c_int {
    _ = rel;
    _ = rel_len;
    _ = key;
    _ = key_len;
    const ctx: *struct { ops: usize, last_op: u8 } = @ptrCast(@alignCast(user orelse return 1));
    ctx.ops += 1;
    ctx.last_op = op;
    return 0;
}

test "txnwal append+commit+replay+truncate roundtrip" {
    const dir = "/tmp/datalog_zig_u4_txnwal_test";
    _ = c.mkdir(dir, 0o755); // EEXIST fine

    const w = txnwal_open_rw(dir) orelse return error.OutOfMemory;
    defer txnwal_close(w);

    const rel = "r";
    const key = [_]u8{ 0xDE, 0xAD, 0xBE, 0xEF, 0x00 };
    try std.testing.expectEqual(@as(c_int, 0), txnwal_append_record(w, rel, 1, TXNWAL_OP_ADD, &key, 5));
    try std.testing.expectEqual(@as(c_int, 0), txnwal_append_record(w, rel, 1, TXNWAL_OP_DEL, &key, 5));
    try std.testing.expectEqual(@as(c_int, 0), txnwal_append_commit(w));
    try std.testing.expectEqual(@as(c_int, 0), txnwal_sync(w));

    // Replay the committed prefix.
    var ctx = struct { ops: usize = 0, last_op: u8 = 0 }{};
    var good: c_long = 0;
    try std.testing.expectEqual(@as(c_int, 0), txnwal_replay(w, replayCb, &ctx, @ptrCast(&good)));
    try std.testing.expectEqual(@as(usize, 2), ctx.ops);
    try std.testing.expectEqual(@as(u8, TXNWAL_OP_DEL), ctx.last_op);
    try std.testing.expect(good == @as(c_long, @intCast(w.size)));

    // Truncate to header.
    try std.testing.expectEqual(@as(c_int, 0), txnwal_truncate(w, 16));
    try std.testing.expectEqual(@as(u64, 16), w.size);

    _ = c.unlink(dir ++ "/txn.wal");
    _ = c.rmdir(dir);
}

test "txnwal reopen truncates torn tail (uncommitted records dropped)" {
    const dir = "/tmp/datalog_zig_u4_txnwal_torn";
    _ = c.mkdir(dir, 0o755); // EEXIST fine

    {
        const w = txnwal_open_rw(dir) orelse return error.OutOfMemory;
        defer txnwal_close(w);
        try std.testing.expectEqual(@as(c_int, 0), txnwal_append_record(w, "r", 1, TXNWAL_OP_ADD, &[_]u8{1}, 1));
        try std.testing.expectEqual(@as(c_int, 0), txnwal_append_commit(w));
        // Uncommitted tail after the last COMMIT.
        try std.testing.expectEqual(@as(c_int, 0), txnwal_append_record(w, "r", 1, TXNWAL_OP_ADD, &[_]u8{2}, 1));
        try std.testing.expectEqual(@as(c_int, 0), txnwal_sync(w));
        const size_before = w.size;
        _ = size_before;
    }

    // Reopen: the torn tail is truncated at the COMMIT boundary.
    {
        const w = txnwal_open_rw(dir) orelse return error.OutOfMemory;
        defer txnwal_close(w);
        // Header (16) + ADD record (2+1+1+4+1+4=13) + COMMIT (11) = 40.
        try std.testing.expectEqual(@as(u64, 40), w.size);
    }

    _ = c.unlink(dir ++ "/txn.wal");
    _ = c.rmdir(dir);
}
