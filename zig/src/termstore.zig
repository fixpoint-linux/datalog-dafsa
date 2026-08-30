//! termstore.zig — port of src/termstore.c (hash-consed LIST term store).
//!
//! Strangler-hybrid ABI: `struct termstore` is OPAQUE to C (termstore.h only
//! forward-declares it), so the implementation is native Zig; every
//! non-static C function is an `export fn` with the exact C name, signature
//! and return-code semantics (0/1 ints, TERM_BASE-relative u32 handles,
//! NULL/0 sentinels).  Hash math uses wrapping ops (`*%`) to match C.
//! Persistence keeps the canonical text format byte-for-byte:
//!   "<count>\n" then "<head> <tail>\n" per node (node 0 = NIL, "0 0").
//!
//! Oracle: src/termstore.c (never modified).

const std = @import("std");
const c = std.c;
const util = @import("util.zig");

const TERM_INIT_NODES: u32 = 64;
const TERM_INIT_HASH: u32 = 128;

/// Corruption sanity cap: reject a count header above this many nodes.
const TERM_MAX_NODES: u64 = 1 << 26;

pub const TERM_BASE: u32 = 0x80000000;
pub const TERM_NIL: u32 = TERM_BASE; // empty list, node 0

/// struct termstore — private (opaque in termstore.h).
const Termstore = struct {
    head: ?[*]u32, // node[i].head
    tail: ?[*]u32, // node[i].tail
    count: u32, // nodes in use (NIL = node 0)
    cap: u32, // capacity of head/tail arrays
    ht_head: ?[*]u32, // hash key head
    ht_tail: ?[*]u32, // hash key tail
    ht_idx: ?[*]u32, // node index + 1 (0 = empty)
    ht_cap: u32, // power of two
    dirty: c_int, // 1 if new nodes since last save
};

/// FNV-1a over the 8 bytes of (a, b).
fn term_hash(a: u32, b: u32) u64 {
    var h: u64 = 14695981039346656037;
    const v = [2]u32{ a, b };
    for (v) |x| {
        h ^= x & 0xFF;
        h *%= 1099511628211;
        h ^= (x >> 8) & 0xFF;
        h *%= 1099511628211;
        h ^= (x >> 16) & 0xFF;
        h *%= 1099511628211;
        h ^= (x >> 24) & 0xFF;
        h *%= 1099511628211;
    }
    return h;
}

/// Grow the hash table (power-of-two doubling) and rehash nodes 1..count-1.
fn ht_grow(t: *Termstore) c_int {
    const nc: u32 = t.ht_cap *% 2;
    if (nc < t.ht_cap) return -1; // overflow
    const mh = c.calloc(nc, @sizeOf(u32)) orelse return -1;
    const mt = c.calloc(nc, @sizeOf(u32)) orelse {
        c.free(mh);
        return -1;
    };
    const mi = c.calloc(nc, @sizeOf(u32)) orelse {
        c.free(mh);
        c.free(mt);
        return -1;
    };
    const nh: [*]u32 = @ptrCast(@alignCast(mh));
    const nt: [*]u32 = @ptrCast(@alignCast(mt));
    const ni: [*]u32 = @ptrCast(@alignCast(mi));
    var i: u32 = 1;
    while (i < t.count) : (i += 1) {
        var idx: u32 = @intCast(term_hash(t.head.?[i], t.tail.?[i]) &
            (@as(u64, nc) - 1));
        while (ni[idx] != 0)
            idx = (idx +% 1) & (nc - 1);
        nh[idx] = t.head.?[i];
        nt[idx] = t.tail.?[i];
        ni[idx] = i + 1;
    }
    c.free(@ptrCast(t.ht_head.?));
    c.free(@ptrCast(t.ht_tail.?));
    c.free(@ptrCast(t.ht_idx.?));
    t.ht_head = nh;
    t.ht_tail = nt;
    t.ht_idx = ni;
    t.ht_cap = nc;
    return 0;
}

/// Grow the node arrays (doubling).
fn nodes_grow(t: *Termstore) c_int {
    const nc: u32 = t.cap *% 2;
    if (nc < t.cap) return -1;
    const mh = c.realloc(@ptrCast(t.head), @as(usize, nc) * @sizeOf(u32)) orelse return -1;
    t.head = @ptrCast(@alignCast(mh));
    const mt = c.realloc(@ptrCast(t.tail), @as(usize, nc) * @sizeOf(u32)) orelse return -1;
    // head grew; tail failed — head array stays valid (as in C)
    t.tail = @ptrCast(@alignCast(mt));
    t.cap = nc;
    return 0;
}

/// termstore *term_create(void) — NIL pre-allocated as node 0; NULL on OOM.
export fn term_create() ?*Termstore {
    const mem = c.calloc(1, @sizeOf(Termstore)) orelse return null;
    const t: *Termstore = @ptrCast(@alignCast(mem));
    t.* = .{
        .head = null,
        .tail = null,
        .count = 0,
        .cap = TERM_INIT_NODES,
        .ht_head = null,
        .ht_tail = null,
        .ht_idx = null,
        .ht_cap = TERM_INIT_HASH,
        .dirty = 0,
    };
    t.head = @ptrCast(@alignCast(c.malloc(@as(usize, t.cap) * @sizeOf(u32))));
    t.tail = @ptrCast(@alignCast(c.malloc(@as(usize, t.cap) * @sizeOf(u32))));
    t.ht_head = @ptrCast(@alignCast(c.calloc(t.ht_cap, @sizeOf(u32))));
    t.ht_tail = @ptrCast(@alignCast(c.calloc(t.ht_cap, @sizeOf(u32))));
    t.ht_idx = @ptrCast(@alignCast(c.calloc(t.ht_cap, @sizeOf(u32))));
    if (t.head == null or t.tail == null or t.ht_head == null or
        t.ht_tail == null or t.ht_idx == null)
    {
        if (t.head) |p| c.free(@ptrCast(p));
        if (t.tail) |p| c.free(@ptrCast(p));
        if (t.ht_head) |p| c.free(@ptrCast(p));
        if (t.ht_tail) |p| c.free(@ptrCast(p));
        if (t.ht_idx) |p| c.free(@ptrCast(p));
        c.free(mem);
        return null;
    }
    // NIL = node 0.
    t.head.?[0] = 0;
    t.tail.?[0] = 0;
    t.count = 1;
    return t;
}

/// void term_free(termstore *t)
export fn term_free(t: ?*Termstore) void {
    const s = t orelse return;
    if (s.head) |p| c.free(@ptrCast(p));
    if (s.tail) |p| c.free(@ptrCast(p));
    if (s.ht_head) |p| c.free(@ptrCast(p));
    if (s.ht_tail) |p| c.free(@ptrCast(p));
    if (s.ht_idx) |p| c.free(@ptrCast(p));
    c.free(@ptrCast(s));
}

/// int term_is_list(const termstore *t, uint32_t v) — EXACT index-range test.
export fn term_is_list(t: ?*const Termstore, v: u32) c_int {
    const s = t orelse return 0;
    return if (v >= TERM_BASE and (v -% TERM_BASE) < s.count) 1 else 0;
}

/// uint32_t term_car(const termstore *t, uint32_t h)
export fn term_car(t: ?*const Termstore, h: u32) u32 {
    const s = t orelse return 0;
    if (term_is_list(s, h) == 0 or h == TERM_NIL) return 0;
    return s.head.?[h - TERM_BASE];
}

/// uint32_t term_cdr(const termstore *t, uint32_t h)
export fn term_cdr(t: ?*const Termstore, h: u32) u32 {
    const s = t orelse return 0;
    if (term_is_list(s, h) == 0 or h == TERM_NIL) return 0;
    return s.tail.?[h - TERM_BASE];
}

/// uint32_t term_cons(termstore *t, uint32_t head, uint32_t tail)
/// Intern the cons cell; tail MUST be a list.  Returns the canonical handle
/// (hash-consed), or 0 on OOM or a non-list tail.
export fn term_cons(t: ?*Termstore, head: u32, tail: u32) u32 {
    const s = t orelse return 0;
    if (term_is_list(s, tail) == 0) return 0; // improper tail rejected

    // Keep the hash load factor under ~75% for the incoming node.
    if (s.count +% 1 > s.ht_cap - (s.ht_cap >> 2)) {
        if (ht_grow(s) != 0) return 0;
    }

    var idx: u32 = @intCast(term_hash(head, tail) & (@as(u64, s.ht_cap) - 1));
    while (s.ht_idx.?[idx] != 0) {
        if (s.ht_head.?[idx] == head and s.ht_tail.?[idx] == tail)
            return TERM_BASE +% (s.ht_idx.?[idx] -% 1);
        idx = (idx +% 1) & (s.ht_cap - 1);
    }

    if (s.count >= s.cap) {
        if (nodes_grow(s) != 0) return 0;
    }

    s.head.?[s.count] = head;
    s.tail.?[s.count] = tail;
    s.ht_head.?[idx] = head;
    s.ht_tail.?[idx] = tail;
    s.ht_idx.?[idx] = s.count +% 1;
    s.count +%= 1;
    s.dirty = 1;
    return TERM_BASE +% (s.count -% 1);
}

/// uint32_t term_append(termstore *t, uint32_t a, uint32_t b)
/// append(a, b) with shared b suffix; append(NIL, b) == b; 0 on error.
export fn term_append(t: ?*Termstore, a: u32, b: u32) u32 {
    const s = t orelse return 0;
    if (term_is_list(s, a) == 0 or term_is_list(s, b) == 0) return 0;
    if (a == TERM_NIL) return b;

    var cap: u32 = 16;
    var n: u32 = 0;
    var elems: [*]u32 = blk: {
        const m = c.malloc(@as(usize, cap) * @sizeOf(u32)) orelse return 0;
        break :blk @ptrCast(@alignCast(m));
    };
    defer c.free(@ptrCast(elems));

    // Collect a's elements (iterative — no deep recursion on long lists).
    var cur = a;
    while (cur != TERM_NIL) {
        if (n >= cap) {
            const nc: u32 = cap *% 2;
            if (nc < cap) return 0;
            const ne = c.realloc(@ptrCast(elems), @as(usize, nc) * @sizeOf(u32)) orelse return 0;
            elems = @ptrCast(@alignCast(ne));
            cap = nc;
        }
        elems[n] = term_car(s, cur);
        n += 1;
        cur = term_cdr(s, cur);
    }

    // Right-fold cons onto b (rebuilds the prefix, sharing the b suffix).
    var result = b;
    var i: u32 = n;
    while (i > 0) : (i -= 1) {
        result = term_cons(s, elems[i - 1], result);
        if (result == 0) return 0;
    }
    return result;
}

/// uint32_t term_length(const termstore *t, uint32_t h) — 0 for NIL /
/// non-list.
export fn term_length(t: ?*const Termstore, h: u32) u32 {
    const s = t orelse return 0;
    if (term_is_list(s, h) == 0) return 0;
    var n: u32 = 0;
    var cur = h;
    while (cur != TERM_NIL) {
        n +%= 1;
        cur = term_cdr(s, cur);
    }
    return n;
}

/// uint32_t term_node_count(const termstore *t) — NIL inclusive.
export fn term_node_count(t: ?*const Termstore) u32 {
    const s = t orelse return 0;
    return s.count;
}

/// int term_is_dirty(const termstore *t)
export fn term_is_dirty(t: ?*const Termstore) c_int {
    const s = t orelse return 0;
    return if (s.dirty != 0) 1 else 0;
}

/// void term_clear_dirty(termstore *t)
export fn term_clear_dirty(t: ?*Termstore) void {
    const s = t orelse return;
    s.dirty = 0;
}

/// Local strlen (std.c does not re-export it).
fn cstrLen(s: [*:0]const u8) usize {
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {}
    return i;
}

/// snprintf(tmp, sizeof(tmp), "%s.tmp", path), see util.zig.
fn tmpNameOf(buf: *[8192:0]u8, path: [*:0]const u8) void {
    const plen = cstrLen(path);
    const max_path = buf.len - 1 - ".tmp".len;
    const n = @min(plen, max_path);
    @memcpy(buf[0..n], path[0..n]);
    @memcpy(buf[n..][0..".tmp".len], ".tmp");
    buf[n + ".tmp".len] = 0;
}

fn writeAll(fd: c_int, bytes: []const u8) c_int {
    var off: usize = 0;
    while (off < bytes.len) {
        const w = c.write(fd, bytes.ptr + off, bytes.len - off);
        if (w < 0) return -1;
        off += @intCast(w);
    }
    return 0;
}

/// int term_save(termstore *t, const char *path)
/// Atomic save (tmp + fsync + rename + dir fsync) of the canonical text
/// format.  Returns 0 on success, -1 on error.
export fn term_save(t: ?*Termstore, path: ?[*:0]const u8) c_int {
    const s = t orelse return -1;
    const path_p = path orelse return -1;

    var tmp: [8192:0]u8 = undefined;
    tmpNameOf(&tmp, path_p);
    const fd = c.open(&tmp, .{ .ACCMODE = .WRONLY, .CREAT = true, .TRUNC = true }, @as(c.mode_t, 0o666));
    if (fd < 0) return -1;

    var wbuf: [32]u8 = undefined;
    var fail = false;

    fail = blk: {
        const line = std.fmt.bufPrint(&wbuf, "{d}\n", .{s.count}) catch break :blk true;
        break :blk writeAll(fd, line) != 0;
    };
    if (!fail) {
        var i: u32 = 0;
        while (i < s.count) : (i += 1) {
            const line = std.fmt.bufPrint(&wbuf, "{d} {d}\n", .{ s.head.?[i], s.tail.?[i] }) catch {
                fail = true;
                break;
            };
            if (writeAll(fd, line) != 0) {
                fail = true;
                break;
            }
        }
    }

    if (fail or c.fsync(fd) != 0) {
        _ = c.close(fd);
        _ = c.unlink(&tmp);
        return -1;
    }
    if (c.close(fd) != 0) {
        _ = c.unlink(&tmp);
        return -1;
    }

    if (c.rename(&tmp, path_p) != 0) {
        _ = c.unlink(&tmp);
        return -1;
    }
    if (util.fsync_dir_of_path(path_p) != 0) return -1;

    s.dirty = 0;
    return 0;
}

// ─── term_load: getline-style line reader + C scanner emulation ──────────

/// getline() equivalent over a raw fd: returns the next line (including its
/// '\n', if any) or null at EOF / read error (with any partial line lost,
/// like getline returning -1).
const LineReader = struct {
    fd: c_int,
    buf: [8192]u8 = undefined,
    start: usize = 0,
    end: usize = 0,
    eof: bool = false,
    line: std.ArrayListUnmanaged(u8) = .empty,
    gpa: std.mem.Allocator = std.heap.c_allocator,

    fn next(self: *LineReader) ?[]const u8 {
        self.line.clearRetainingCapacity();
        while (true) {
            if (self.start >= self.end) {
                if (self.eof) {
                    return if (self.line.items.len > 0) self.line.items else null;
                }
                const n = c.read(self.fd, &self.buf, self.buf.len);
                if (n <= 0) {
                    self.eof = true;
                    return if (self.line.items.len > 0) self.line.items else null;
                }
                self.start = 0;
                self.end = @intCast(n);
            }
            const chunk = self.buf[self.start..self.end];
            if (std.mem.indexOfScalar(u8, chunk, '\n')) |nl| {
                self.line.appendSlice(self.gpa, chunk[0 .. nl + 1]) catch return null;
                self.start += nl + 1;
                return self.line.items;
            }
            self.line.appendSlice(self.gpa, chunk) catch return null;
            self.start = self.end;
        }
    }

    fn deinit(self: *LineReader) void {
        self.line.deinit(self.gpa);
    }
};

fn isSpace(b: u8) bool {
    return b == ' ' or b == '\t' or b == '\n' or b == 0x0b or b == 0x0c or b == '\r';
}

const ScanU = struct { v: u64, any: bool, neg: bool };

/// strtoul(s, &end, 10)-style scan from bytes[pos..]: skips isspace, optional
/// sign, then base-10 digits.  `any` mirrors end != line.  Overflow
/// saturates (strtoul's ULONG_MAX / %u's UINT_MAX behaviour).
fn scanUnsigned(bytes: []const u8, pos: *usize, comptime sat: u64) ScanU {
    var i = pos.*;
    while (i < bytes.len and isSpace(bytes[i])) i += 1;
    var neg = false;
    if (i < bytes.len and (bytes[i] == '+' or bytes[i] == '-')) {
        neg = bytes[i] == '-';
        i += 1;
    }
    var v: u64 = 0;
    var any = false;
    while (i < bytes.len and bytes[i] >= '0' and bytes[i] <= '9') {
        any = true;
        const d: u64 = bytes[i] - '0';
        if (v > (sat - d) / 10) {
            v = sat; // saturate; keep consuming digits
        } else {
            v = v * 10 + d;
        }
        i += 1;
    }
    pos.* = i;
    return .{ .v = v, .any = any, .neg = neg };
}

/// sscanf(line, "%u %u", &h, &tl) — returns null unless both conversions
/// matched.  %u accepts an optional sign and negates modulo 2^32.
fn scanNodeLine(bytes: []const u8) ?struct { h: u32, tl: u32 } {
    var pos: usize = 0;
    const a = scanUnsigned(bytes, &pos, std.math.maxInt(u32));
    if (!a.any) return null;
    const b = scanUnsigned(bytes, &pos, std.math.maxInt(u32));
    if (!b.any) return null;
    const av: u32 = @truncate(if (a.neg) 0 -% a.v else a.v);
    const bv: u32 = @truncate(if (b.neg) 0 -% b.v else b.v);
    return .{ .h = av, .tl = bv };
}

/// termstore *term_load(const char *path)
/// Returns an EMPTY store if path is NULL or the file does not exist
/// (backward-compat); NULL on OOM or a corrupt file.
export fn term_load(path: ?[*:0]const u8) ?*Termstore {
    const t = term_create() orelse return null;
    const path_p = path orelse return t;

    const fd = c.open(path_p, .{ .ACCMODE = .RDONLY });
    if (fd < 0) return t; // no file → empty store (backward-compat)

    var rd = LineReader{ .fd = fd };
    defer rd.deinit();

    // Count header.
    var count: u32 = 0;
    {
        const line = rd.next() orelse {
            _ = c.close(fd);
            term_free(t);
            return null;
        };
        var pos: usize = 0;
        const r = scanUnsigned(line, &pos, std.math.maxInt(u64));
        if (!r.any or r.v == 0 or r.v > TERM_MAX_NODES) {
            _ = c.close(fd);
            term_free(t);
            return null;
        }
        count = @intCast(r.v);
    }

    // The file has `count` node lines in handle order: node 0 (NIL, "0 0")
    // is pre-allocated; nodes 1..count-1 are re-interned in order.
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        const line = rd.next() orelse {
            _ = c.close(fd);
            term_free(t);
            return null;
        };
        const cell = scanNodeLine(line) orelse {
            _ = c.close(fd);
            term_free(t);
            return null;
        };
        if (i == 0) {
            if (cell.h != 0 or cell.tl != 0) { // NIL row corrupted
                _ = c.close(fd);
                term_free(t);
                return null;
            }
            continue;
        }
        if (term_cons(t, cell.h, cell.tl) == 0) {
            _ = c.close(fd);
            term_free(t);
            return null;
        }
    }
    _ = c.close(fd);

    // The file is canonical (hash-consed): every row interned a fresh node.
    // A mismatch means the file had duplicate rows — corrupt.
    if (t.count != count) {
        term_free(t);
        return null;
    }
    t.dirty = 0;
    return t;
}

// ─── Tests ────────────────────────────────────────────────────────────────

test "termstore cons/intern/append/length" {
    const t = term_create() orelse return error.OutOfMemory;
    defer term_free(t);

    try std.testing.expectEqual(@as(u32, 1), term_node_count(t));
    try std.testing.expectEqual(@as(c_int, 0), term_is_dirty(t));

    const nil = TERM_NIL;
    const l1 = term_cons(t, 7, nil); // [7]
    try std.testing.expect(l1 != 0);
    try std.testing.expectEqual(@as(u32, 7), term_car(t, l1));
    try std.testing.expectEqual(nil, term_cdr(t, l1));
    try std.testing.expectEqual(@as(u32, 1), term_length(t, l1));

    const l2 = term_cons(t, 8, l1); // [8 7]
    const l2b = term_cons(t, 8, l1); // hash-consed: same handle
    try std.testing.expectEqual(l2, l2b);
    try std.testing.expectEqual(@as(u32, 3), term_node_count(t));

    // improper tail rejected
    try std.testing.expectEqual(@as(u32, 0), term_cons(t, 1, 42));

    // append shares the suffix
    const la = term_append(t, l1, l2); // [7 8 7]
    try std.testing.expectEqual(@as(u32, 3), term_length(t, la));
    try std.testing.expectEqual(term_cdr(t, la), l2);
    try std.testing.expectEqual(term_append(t, nil, l1), l1);

    // is_list exact range: TERM_BASE valid (NIL), TERM_BASE+count invalid
    try std.testing.expectEqual(@as(c_int, 1), term_is_list(t, nil));
    try std.testing.expectEqual(@as(c_int, 0), term_is_list(t, TERM_BASE + term_node_count(t)));
    try std.testing.expectEqual(@as(c_int, 0), term_is_list(null, nil));
    try std.testing.expectEqual(@as(c_int, 1), term_is_dirty(t));
    term_clear_dirty(t);
    try std.testing.expectEqual(@as(c_int, 0), term_is_dirty(t));
}

test "termstore save/load text format + corruption" {
    const path = "/tmp/datalog_zig_u2_terms_test.bin";
    defer _ = c.unlink(path);

    const t = term_create() orelse return error.OutOfMemory;
    defer term_free(t);
    const nil = TERM_NIL;
    const l1 = term_cons(t, 11, nil);
    _ = term_cons(t, 22, l1);
    _ = term_cons(t, 33, nil);
    try std.testing.expectEqual(@as(c_int, 0), term_save(t, path));
    try std.testing.expectEqual(@as(c_int, 0), term_is_dirty(t));

    // Byte-exact canonical text format: count line + "head tail" lines.
    const fd = c.open(path, .{ .ACCMODE = .RDONLY });
    try std.testing.expect(fd >= 0);
    var buf: [256]u8 = undefined;
    const n: usize = @intCast(c.read(fd, &buf, buf.len));
    _ = c.close(fd);
    try std.testing.expectEqualStrings("4\n0 0\n11 2147483648\n22 2147483649\n33 2147483648\n", buf[0..n]);

    // Load: identical node count, handles re-interned to the same values.
    const t2 = term_load(path) orelse return error.Corrupt;
    defer term_free(t2);
    try std.testing.expectEqual(@as(u32, 4), term_node_count(t2));
    try std.testing.expectEqual(@as(u32, 2), term_length(t2, term_cons(t2, 22, term_cons(t2, 11, TERM_NIL))));
    try std.testing.expectEqual(@as(c_int, 0), term_is_dirty(t2));

    // Missing file → empty store (backward compat).
    const t3 = term_load("/tmp/datalog_zig_u2_terms_nonexistent.bin");
    try std.testing.expect(t3 != null);
    try std.testing.expectEqual(@as(u32, 1), term_node_count(t3.?));
    term_free(t3);

    // Corrupt: bad NIL row / truncated / duplicate rows / bad header.
    inline for (.{ "x\n0 0\n11 0\n22 1\n33 0\n", "4\n1 0\n11 0\n", "4\n0 0\n", "3\n0 0\n11 0\n11 0\n" }) |bad| {
        const bfd = c.open(path, .{ .ACCMODE = .WRONLY, .CREAT = true, .TRUNC = true }, @as(c.mode_t, 0o666));
        var wpos: usize = 0;
        while (wpos < bad.len) {
            const w = c.write(bfd, bad.ptr + wpos, bad.len - wpos);
            wpos += @intCast(w);
        }
        _ = c.close(bfd);
        try std.testing.expect(term_load(path) == null);
    }
}
