//! tupleset.zig — port of src/tupleset.c (in-memory sorted tuple set:
//! FNV-1a open-addressing hash + sorted array + binary-search prefix).
//!
//! Strangler-hybrid ABI: `tuple_set` is an EXPOSED struct (tests and other C
//! TUs dereference its fields) and must stay byte-identical to tupleset.h's
//! layout; every non-static C function is an `export fn` with the exact C
//! name/signature/return codes.  Hash math uses wrapping ops (`*%`) to match
//! C exactly.  Allocation goes through libc (malloc/realloc/calloc/free) like
//! the C oracle.
//!
//! Oracle: src/tupleset.c (never modified).

const std = @import("std");
const c = std.c;

// FNV-1a constants (matching vendor/dafsa_internal.h)
const FNV_OFFSET: u64 = 14695981039346656037;
const FNV_PRIME: u64 = 1099511628211;

/// typedef struct tuple_set — MUST stay byte-identical to src/tupleset.h.
pub const tuple_set = extern struct {
    data: ?[*]u32, // sorted unique tuples, arity-strided
    count: c_long, // number of tuples
    cap: c_long, // capacity in tuples
    arity: u8,

    // Open-addressing hash set: slot = 1-based index into data[], 0=empty
    htab: ?[*]u32,
    hcap: usize, // capacity (power of two)
    hused: usize, // occupied slots
};

// ─── Internal helpers ─────────────────────────────────────────────────────

/// FNV-1a hash over arity u32 columns, byte at a time.
fn ts_hash_cols(cols: [*]const u32, arity: u8) u64 {
    var h: u64 = FNV_OFFSET;
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        const v = cols[i];
        h ^= v & 0xFF;
        h *%= FNV_PRIME;
        h ^= (v >> 8) & 0xFF;
        h *%= FNV_PRIME;
        h ^= (v >> 16) & 0xFF;
        h *%= FNV_PRIME;
        h ^= (v >> 24) & 0xFF;
        h *%= FNV_PRIME;
    }
    return h;
}

/// Compare two arity-strided tuples for equality.
fn ts_tuple_eq(a: [*]const u32, b: [*]const u32, arity: u8) bool {
    return std.mem.eql(u32, a[0..arity], b[0..arity]);
}

/// Grow the hash table to at least `need` slots (power of two).
fn ts_hash_grow(ts: *tuple_set, need: usize) c_int {
    var new_cap: usize = 16;
    while (new_cap < need) new_cap *%= 2;
    if (new_cap > (@as(usize, 1) << 30)) return -1; // too large

    const mem = c.calloc(new_cap, @sizeOf(u32)) orelse return -1;
    const new_htab: [*]u32 = @ptrCast(@alignCast(mem));

    // Rehash all existing entries
    if (ts.htab) |htab| {
        var i: usize = 0;
        while (i < ts.hcap) : (i += 1) {
            const slot = htab[i];
            if (slot == 0) continue;
            const cols = ts.data.? + @as(usize, @intCast(slot - 1)) * ts.arity;
            const h = ts_hash_cols(cols, ts.arity);
            var idx: usize = @intCast(h & (new_cap - 1));
            while (new_htab[idx] != 0)
                idx = (idx +% 1) & (new_cap - 1);
            new_htab[idx] = slot;
        }
        c.free(@ptrCast(htab));
    }
    ts.htab = new_htab;
    ts.hcap = new_cap;
    return 0;
}

/// Hash-table lookup: returns 1-based index into data[] if found, 0 if absent.
fn ts_hash_find(ts: *const tuple_set, cols: [*]const u32) u32 {
    if (ts.htab == null or ts.hcap == 0) return 0;
    const htab = ts.htab.?;
    const data = ts.data.?;

    const h = ts_hash_cols(cols, ts.arity);
    var idx: usize = @intCast(h & (ts.hcap - 1));
    while (true) {
        const slot = htab[idx];
        if (slot == 0) return 0; // empty → not found
        if (ts_tuple_eq(cols, data + @as(usize, @intCast(slot - 1)) * ts.arity, ts.arity))
            return slot;
        idx = (idx +% 1) & (ts.hcap - 1);
    }
}

/// Hash-table insert: store 1-based index.  Caller must ensure it's not
/// already present and that there is room.
fn ts_hash_insert(ts: *tuple_set, cols: [*]const u32, idx1: u32) void {
    const htab = ts.htab.?;
    const h = ts_hash_cols(cols, ts.arity);
    var i: usize = @intCast(h & (ts.hcap - 1));
    while (htab[i] != 0)
        i = (i +% 1) & (ts.hcap - 1);
    htab[i] = idx1;
}

// ─── Sorting ──────────────────────────────────────────────────────────────

/// In-place heapsort over arity-strided rows.  Deterministic O(n log n);
/// ties (byte-identical rows) are interchangeable, so any comparison sort
/// gives the identical final array (ts_sort's dedup collapses them anyway).
fn ts_sort_rows(data: [*]u32, arity: u8, n_rows: usize) void {
    if (n_rows < 2) return;

    const Ctx = struct {
        data: [*]u32,
        ar: u8,

        fn rowLess(self: @This(), a: usize, b: usize) bool {
            const x = self.data + a * self.ar;
            const y = self.data + b * self.ar;
            var i: u8 = 0;
            while (i < self.ar) : (i += 1) {
                if (x[i] < y[i]) return true;
                if (x[i] > y[i]) return false;
            }
            return false;
        }

        fn rowSwap(self: @This(), a: usize, b: usize) void {
            const x = self.data + a * self.ar;
            const y = self.data + b * self.ar;
            var i: u8 = 0;
            while (i < self.ar) : (i += 1) {
                const t = x[i];
                x[i] = y[i];
                y[i] = t;
            }
        }

        // sift down; `end` is the exclusive heap bound.
        fn siftDown(self: @This(), root: usize, end: usize) void {
            var r = root;
            while (true) {
                var child = 2 * r + 1;
                if (child >= end) return;
                if (child + 1 < end and self.rowLess(child, child + 1)) child += 1;
                if (!self.rowLess(r, child)) return;
                self.rowSwap(r, child);
                r = child;
            }
        }
    };

    const ctx = Ctx{ .data = data, .ar = arity };
    var i: usize = n_rows / 2;
    while (i > 0) : (i -= 1) ctx.siftDown(i - 1, n_rows);
    var end: usize = n_rows;
    while (end > 1) {
        end -= 1;
        ctx.rowSwap(0, end);
        ctx.siftDown(0, end);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────

/// int ts_init(tuple_set *ts, uint8_t arity) — arity 1..8.
export fn ts_init(ts: ?*tuple_set, arity: u8) c_int {
    const t = ts orelse return -1;
    if (arity == 0 or arity > 8) return -1;
    t.* = std.mem.zeroes(tuple_set);
    t.arity = arity;
    return 0;
}

/// void ts_free(tuple_set *ts)
export fn ts_free(ts: ?*tuple_set) void {
    const t = ts orelse return;
    if (t.data) |d| c.free(@ptrCast(d));
    if (t.htab) |h| c.free(@ptrCast(h));
    t.* = std.mem.zeroes(tuple_set);
}

/// int ts_contains(const tuple_set *ts, const uint32_t *cols)
/// O(1) membership test via hash set.  1 if present, 0 if absent.
export fn ts_contains(ts: ?*const tuple_set, cols: ?[*]const u32) c_int {
    const t = ts orelse return 0;
    const cols_p = cols orelse return 0;
    if (t.count == 0) return 0;
    return if (ts_hash_find(t, cols_p) != 0) 1 else 0;
}

/// int ts_add(tuple_set *ts, const uint32_t *cols)
/// Returns 1 if added (new), 0 if duplicate, -1 on error.
export fn ts_add(ts: ?*tuple_set, cols: ?[*]const u32) c_int {
    const t = ts orelse return -1;
    const cols_p = cols orelse return -1;

    // Check hash first (authority on uniqueness)
    if (ts_hash_find(t, cols_p) != 0)
        return 0; // duplicate

    // Grow data array if needed
    if (t.count >= t.cap) {
        const nc: c_long = if (t.cap != 0) t.cap *% 2 else 1024;
        const old: ?*anyopaque = if (t.data) |d| @ptrCast(d) else null;
        const mem = c.realloc(old, @as(usize, @intCast(nc)) *
            @as(usize, t.arity) * @sizeOf(u32)) orelse return -1;
        t.data = @ptrCast(@alignCast(mem));
        t.cap = nc;
    }

    // Append to sorted array
    const dst = t.data.? + @as(usize, @intCast(t.count)) * t.arity;
    @memcpy(dst[0..t.arity], cols_p[0..t.arity]);
    t.count +%= 1;

    // Grow hash table if needed (load factor 0.75)
    {
        var need: usize = @intFromFloat(@as(f64, @floatFromInt(t.count)) / 0.70);
        if (need < 16) need = 16;
        if (t.hcap == 0 or @as(usize, @intCast(t.count)) > t.hcap - (t.hcap >> 2)) {
            if (ts_hash_grow(t, need) != 0) {
                t.count -%= 1; // roll back
                return -1;
            }
        }
    }

    // Insert into hash table
    ts_hash_insert(t, cols_p, @intCast(t.count)); // 1-based
    t.hused +%= 1;
    return 1;
}

/// long ts_prefix(const tuple_set *ts, const uint32_t *p, uint8_t k,
///                long first_idx_out[1])
/// Binary search for the contiguous range of tuples whose leading k columns
/// equal p[0..k-1].  Returns the match count; sets first_idx_out[0].
/// The array MUST be sorted (call ts_sort first).
export fn ts_prefix(ts: ?*const tuple_set, p: ?[*]const u32, k: u8, first_idx_out: ?[*]c_long) c_long {
    if (first_idx_out) |out| out[0] = 0;
    const t = ts orelse return 0;
    const p_p = p orelse return 0;
    if (t.count == 0 or k > t.arity) return 0;
    const data = t.data.?;
    const ar: usize = t.arity;

    // Binary search for lower bound
    var lo: c_long = 0;
    var hi: c_long = t.count;
    while (lo < hi) {
        const mid: c_long = lo + @divTrunc(hi - lo, 2);
        const r = data + @as(usize, @intCast(mid)) * ar;
        var cmp: c_int = 0;
        var i: u8 = 0;
        while (i < k) : (i += 1) {
            if (r[i] < p_p[i]) {
                cmp = -1;
                break;
            }
            if (r[i] > p_p[i]) {
                cmp = 1;
                break;
            }
        }
        if (cmp < 0) lo = mid + 1 else hi = mid;
    }

    if (lo >= t.count) return 0;

    // Verify match
    {
        const r = data + @as(usize, @intCast(lo)) * ar;
        var i: u8 = 0;
        while (i < k) : (i += 1) {
            if (r[i] != p_p[i]) return 0;
        }
    }

    // Walk backward to find first match
    {
        var first = lo;
        while (first > 0) {
            const r = data + @as(usize, @intCast(first - 1)) * ar;
            var match = true;
            var i: u8 = 0;
            while (i < k) : (i += 1) {
                if (r[i] != p_p[i]) {
                    match = false;
                    break;
                }
            }
            if (!match) break;
            first -= 1;
        }
        if (first_idx_out) |out| out[0] = first;

        // Walk forward to count matches
        var last = lo;
        while (last < t.count) {
            const r = data + @as(usize, @intCast(last)) * ar;
            var match = true;
            var i: u8 = 0;
            while (i < k) : (i += 1) {
                if (r[i] != p_p[i]) {
                    match = false;
                    break;
                }
            }
            if (!match) break;
            last += 1;
        }
        return last - first;
    }
}

/// void ts_sort(tuple_set *ts) — sort rows and dedup (safety net), then
/// rebuild the hash table (positions changed after sort+dedup).
export fn ts_sort(ts: ?*tuple_set) void {
    const t = ts orelse return;
    if (t.count <= 1 or t.arity == 0) return;
    const ar = t.arity;
    const data = t.data.?;

    ts_sort_rows(data, ar, @intCast(t.count));

    // Dedup: compact in-place (safety net; hash should prevent dups)
    {
        var w: c_long = 1;
        var i: c_long = 1;
        while (i < t.count) : (i += 1) {
            const prev = data + @as(usize, @intCast(w - 1)) * ar;
            const cur = data + @as(usize, @intCast(i)) * ar;
            if (!std.mem.eql(u32, prev[0..ar], cur[0..ar])) {
                if (w != i) {
                    const dst = data + @as(usize, @intCast(w)) * ar;
                    @memcpy(dst[0..ar], cur[0..ar]);
                }
                w += 1;
            }
        }
        t.count = w;
    }

    // Rebuild hash table: positions changed after sort+dedup
    if (t.htab) |htab| {
        @memset(htab[0..t.hcap], 0);
        t.hused = 0;
    }
    {
        var i: c_long = 0;
        while (i < t.count) : (i += 1) {
            const cols = data + @as(usize, @intCast(i)) * ar;
            // Ensure hash table is large enough
            var need: usize = @intFromFloat(@as(f64, @floatFromInt(i + 1)) / 0.70);
            if (need < 16) need = 16;
            if (t.hcap == 0 or @as(usize, @intCast(i + 1)) > t.hcap - (t.hcap >> 2)) {
                if (ts_hash_grow(t, need) != 0) return;
            }
            ts_hash_insert(t, cols, @intCast(i + 1));
            t.hused += 1;
        }
    }
}

/// void ts_reset(tuple_set *ts) — reset to empty, keeping allocated capacity.
export fn ts_reset(ts: ?*tuple_set) void {
    const t = ts orelse return;
    t.count = 0;
    if (t.htab) |htab| {
        @memset(htab[0..t.hcap], 0);
        t.hused = 0;
    }
}

// ─── Tests ────────────────────────────────────────────────────────────────

test "tuple_set add/contains/sort/prefix roundtrip" {
    var ts: tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), ts_init(&ts, 2));
    defer ts_free(&ts);

    // Unsorted input; duplicates must return 0.
    const raw = [_]u32{
        5, 2, 3, 9, 1, 1, 5, 2, 7, 7, 7, 0,
    };
    var added: usize = 0;
    var i: usize = 0;
    while (i < raw.len / 2) : (i += 1) {
        const r = ts_add(&ts, raw[i * 2 ..][0..2].ptr);
        try std.testing.expect(r == 1 or r == 0);
        if (r == 1) added += 1;
    }
    try std.testing.expectEqual(@as(usize, 5), added);
    try std.testing.expectEqual(@as(c_long, 5), ts.count);

    try std.testing.expectEqual(@as(c_int, 1), ts_contains(&ts, &.{ 5, 2 }));
    try std.testing.expectEqual(@as(c_int, 0), ts_contains(&ts, &.{ 4, 4 }));

    ts_sort(&ts);
    // Sorted order (oracle-verified): (1,1) (3,9) (5,2) (7,0) (7,7)
    const expected = [_]u32{ 1, 1, 3, 9, 5, 2, 7, 0, 7, 7 };
    try std.testing.expectEqualSlices(u32, &expected, ts.data.?[0..10]);
    try std.testing.expectEqual(@as(c_long, 5), ts.count);

    var first = [1]c_long{-1};
    try std.testing.expectEqual(@as(c_long, 1), ts_prefix(&ts, &.{1}, 1, &first));
    try std.testing.expectEqual(@as(c_long, 0), first[0]); // (1,1) is row 0
    try std.testing.expectEqual(@as(c_long, 0), ts_prefix(&ts, &.{9}, 1, &first));
    try std.testing.expectEqual(@as(c_long, 0), first[0]);
}

test "tuple_set prefix multi-match + reset" {
    var ts: tuple_set = undefined;
    _ = ts_init(&ts, 3);
    defer ts_free(&ts);

    const rows = [_]u32{
        2, 1, 1, 1, 5, 5, 2, 1, 9, 2, 1, 4, 1, 0, 0,
    };
    var i: usize = 0;
    while (i < rows.len / 3) : (i += 1) _ = ts_add(&ts, rows[i * 3 ..][0..3].ptr);
    ts_sort(&ts);

    var first = [1]c_long{0};
    // prefix (2,1): rows (2,1,1) (2,1,4) (2,1,9) — first idx 2 (oracle-verified)
    try std.testing.expectEqual(@as(c_long, 3), ts_prefix(&ts, &.{ 2, 1 }, 2, &first));
    try std.testing.expectEqual(@as(c_long, 2), first[0]);
    try std.testing.expectEqual(@as(c_long, 0), ts_prefix(&ts, &.{ 9, 9 }, 2, &first));
    try std.testing.expectEqual(@as(c_long, 0), first[0]);
    // k > arity and null-safety paths
    try std.testing.expectEqual(@as(c_long, 0), ts_prefix(&ts, &.{1}, 9, &first));
    try std.testing.expectEqual(@as(c_long, 0), ts_prefix(null, &.{1}, 1, &first));
    try std.testing.expectEqual(@as(c_int, 0), ts_contains(null, &.{1}));

    ts_reset(&ts);
    try std.testing.expectEqual(@as(c_long, 0), ts.count);
    try std.testing.expect(ts.data != null); // capacity kept

    // reset then re-add must work (hash table cleared, not freed)
    try std.testing.expectEqual(@as(c_int, 1), ts_add(&ts, &.{ 4, 4, 4 }));
    try std.testing.expectEqual(@as(c_int, 1), ts_contains(&ts, &.{ 4, 4, 4 }));
}
