//! snapshot.zig — port of src/snapshot.c (snapshot mmap query path, M4).
//!
//! Implements: view_prefix (zero-copy prefix enumeration via dafsa_view),
//! snapshot_query_scan, view_open_cached (8-slot LRU) + vcache_invalidate,
//! snapshot_read_current, and the manifest helpers, plus the mmap-view
//! pattern/filter/order-statistics entry points.
//!
//! Strangler-hybrid ABI: `struct dl_db` and `view_cache_slot` are EXPOSED
//! concrete structs still dereferenced by C (dl.c) — they are consumed via
//! @cImport("dl_internal.h") (which pulls in dl.h/snapshot.h/permindex.h),
//! NOT redefined here.  dl_tuple_cb comes from the same @cImport as the
//! exact translate-c fn-pointer type.  dafsa_view internals (v->initial/csr/
//! state_off/final_bits, view_trans_find/view_enum_dfs/view_edge_next and
//! the dafsa_view_rank.c order-statistics) come from
//! @cImport("dafsa_internal.h") exactly like the C oracle.  regex_dfa_walk_
//! view/symset_contains come from the ported zig/src/regexwalk.zig (U6).
//!
//! Oracle: src/snapshot.c (never modified).

const std = @import("std");
const c = std.c;

const regexwalk = @import("regexwalk.zig");
const relation = @import("relation.zig");

// dl_internal.h: dl_db, rel_entry, view_cache_slot, DL_VIEW_CACHE_SZ,
// RELK_VARIADIC/MAX_RELS, dl_tuple_cb/dl_join_cb, db_rel_at_arity_ro (dl.c).
// dl_internal.h: dl_db, rel_entry, view_cache_slot, DL_VIEW_CACHE_SZ,
// RELK_VARIADIC/MAX_RELS, dl_tuple_cb/dl_relation_cb, db_rel_at_arity_ro.
// The datalog engine is now Zig; dl_internal.zig hand-declares these
// C-layout structs + extern ABI in place of the removed @cImport.
const dx = @import("dl_internal.zig");

// dafsa_internal.h: dafsa_view, view_trans_find/view_enum_dfs/view_edge_next,
// dafsa_view_{open,close,rank_n,select_n,range_count_n,subtree_counts}.
// The engine is now Zig; dafsa_c.zig hand-declares these types + extern ABI.
const dc = @import("dafsa_c.zig");

// libc decls not in std.c (precedent: intern.zig).
extern "c" fn snprintf(buf: [*c]u8, size: usize, fmt: [*c]const u8, ...) c_int;
extern "c" fn fscanf(stream: *c.FILE, fmt: [*c]const u8, ...) c_int;
extern "c" fn getline(lineptr: *?[*]u8, n: *usize, stream: *c.FILE) isize;
extern "c" fn fputs(s: [*:0]const u8, stream: *c.FILE) c_int;

// ─── Constants ────────────────────────────────────────────────────────────

const MAX_ARITY: u8 = 8;
const MAX_KEY_LEN: usize = @as(usize, MAX_ARITY) * 4 + 1; // 33

// ─── Small C-string helpers (strcmp/strlen/strncmp/atoi stand-ins) ───────

fn strLen(s: [*c]const u8) usize {
    var i: usize = 0;
    while (s[i] != 0) i += 1;
    return i;
}

fn strEq(a: [*c]const u8, b: [*c]const u8) bool {
    var i: usize = 0;
    while (a[i] != 0 and a[i] == b[i]) i += 1;
    return a[i] == b[i];
}

fn strnEq(a: [*c]const u8, b: [*c]const u8, n: usize) bool {
    var i: usize = 0;
    while (i < n) : (i += 1) {
        if (a[i] != b[i]) return false;
        if (a[i] == 0) return true;
    }
    return true;
}

/// C atoi: skip whitespace, optional sign, accumulate digits (stops at the
/// first non-digit, e.g. the ':' in a manifest "3:edb" field).
fn atoiC(s: [*c]const u8) c_int {
    var i: usize = 0;
    while (s[i] == ' ' or s[i] == '\t' or s[i] == '\n' or
        s[i] == '\r' or s[i] == 0x0b or s[i] == 0x0c) i += 1;
    var neg = false;
    if (s[i] == '+' or s[i] == '-') {
        neg = s[i] == '-';
        i += 1;
    }
    var v: c_int = 0;
    while (s[i] >= '0' and s[i] <= '9') : (i += 1) {
        v = v *% 10 +% @as(c_int, s[i] - '0');
    }
    return if (neg) -v else v;
}

// ─── Byte-swap helpers (mirror relation.c:36-60) ──────────────────────────

/// Read `arity` u32 columns from a big-endian buffer.
fn readColsBe(cols: [*]u32, buf: [*]const u8, arity: u8) void {
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        cols[i] = (@as(u32, buf[4 * @as(usize, i)]) << 24) |
            (@as(u32, buf[4 * @as(usize, i) + 1]) << 16) |
            (@as(u32, buf[4 * @as(usize, i) + 2]) << 8) |
            (@as(u32, buf[4 * @as(usize, i) + 3]));
    }
}

// ─── view_prefix — the crux ───────────────────────────────────────────────

const VpCtx = struct {
    leading: ?[*]const u32,
    k: u8,
    arity: u8,
    cb: dx.dl_tuple_cb,
    user: ?*anyopaque,
    count: c_long,
};

/// view_enum_dfs callback: reassemble full tuple and forward to user cb.
fn vpDfsCb(payload: [*c]const u8, payload_len: usize, user: ?*anyopaque) callconv(.c) c_int {
    _ = payload_len;
    const ctx: *VpCtx = @ptrCast(@alignCast(user orelse return -1));
    const n_rem: u8 = ctx.arity - ctx.k;
    var full_tuple: [MAX_ARITY]u32 = undefined;

    // leading columns
    var i: u8 = 0;
    while (i < ctx.k) : (i += 1) {
        full_tuple[i] = ctx.leading.?[i];
    }

    // remaining columns from payload (skip trailing \0)
    if (n_rem > 0) readColsBe(@ptrCast(full_tuple[ctx.k..].ptr), payload, n_rem);

    if (ctx.cb.?(&full_tuple, ctx.arity, ctx.user) != 0)
        return 1;
    return 0;
}

/// Walk k*4 prefix bytes via view_trans_find, then view_enum_dfs from the
/// resulting state.  Mirrors rel_prefix (relation.c:206-251).
///
/// CRITICAL: does NOT call dafsa_view_prefix_enum — that function has the
/// W\0 trap (requires a 0x00 edge before DFS).  We call view_enum_dfs
/// directly from the prefix-walk endpoint, exactly as relation.c's
/// prefix_dfs does.
pub export fn view_prefix(view_handle: ?*anyopaque, arity: u8, leading: ?[*]const u32, k: u8, cb: dx.dl_tuple_cb, user: ?*anyopaque) c_long {
    const v: [*c]const dc.dafsa_view = @ptrCast(@alignCast(view_handle orelse return -1));
    if (cb == null) return -1;
    if (k > arity) return -1;
    if (k > 0 and leading == null) return -1;
    if (arity > MAX_ARITY) return -1;

    var current: c_uint = v.*.initial;

    // Walk the k*4 prefix bytes
    var i: u8 = 0;
    while (i < k) : (i += 1) {
        const vv = leading.?[i];
        const col_be = [4]u8{
            @truncate(vv >> 24),
            @truncate(vv >> 16),
            @truncate(vv >> 8),
            @truncate(vv),
        };
        var b: usize = 0;
        while (b < 4) : (b += 1) {
            var target: c_uint = undefined;
            if (dc.view_trans_find(v, current, col_be[b], &target) != 0)
                return 0;
            current = target;
        }
    }

    // DFS from current state (NO W\0 — call view_enum_dfs directly)
    var ctx = VpCtx{
        .leading = leading,
        .k = k,
        .arity = arity,
        .cb = cb,
        .user = user,
        .count = 0,
    };
    var buf: [MAX_KEY_LEN]u8 = undefined;

    _ = dc.view_enum_dfs(v, current, &buf, 0, vpDfsCb, &ctx, &ctx.count);
    return ctx.count;
}

// ─── snapshot_read_current ────────────────────────────────────────────────

/// uint32_t snapshot_read_current(const char *db_dir)
pub export fn snapshot_read_current(db_dir: [*c]const u8) u32 {
    var path: [4096:0]u8 = undefined;
    _ = snprintf(&path, 4096, "%s/snapshots/CURRENT", db_dir);
    const f = c.fopen(&path, "r") orelse return 0;

    var v: c_ulong = 0;
    if (fscanf(f, "%lu", &v) != 1 or v > 0xFFFFFFFF) {
        _ = c.fclose(f);
        return 0;
    }
    _ = c.fclose(f);
    return @truncate(v);
}

// ─── View cache ───────────────────────────────────────────────────────────

/// void vcache_invalidate(view_cache_slot *vcache)
pub export fn vcache_invalidate(vcache: [*c]dx.view_cache_slot) void {
    var i: usize = 0;
    while (i < dx.DL_VIEW_CACHE_SZ) : (i += 1) {
        const slot = &vcache[i];
        if (slot.*.view) |view| {
            dc.dafsa_view_close(@ptrCast(@alignCast(view)));
            slot.*.view = null;
        }
        slot.*.rel_name[0] = 0;
        slot.*.used = 0;
    }
}

/// strncpy(dst, src, 63); dst[63] = '\0' — strncpy zero-pads the tail.
fn strncpyZeroPad(dst: [*]u8, src: [*c]const u8) void {
    const d = dst;
    var i: usize = 0;
    while (i < 63 and src[i] != 0) : (i += 1) d[i] = src[i];
    while (i < 64) : (i += 1) d[i] = 0;
}

/// void *view_open_cached(view_cache_slot *vcache, const char *rel_name,
///                        const char *sdir) — open or find a cached view.
pub export fn view_open_cached(vcache: [*c]dx.view_cache_slot, rel_name: [*c]const u8, sdir: [*c]const u8) ?*anyopaque {
    var lru_idx: usize = 0;
    var empty_idx: isize = -1;
    var lru_used_val: c_int = 0x7FFFFFFF;

    var i: usize = 0;
    while (i < dx.DL_VIEW_CACHE_SZ) : (i += 1) {
        const slot = &vcache[i];
        if (slot.*.view != null and strEq(@ptrCast(&slot.*.rel_name), rel_name)) {
            slot.*.used += 1;
            return slot.*.view;
        }
        if (slot.*.view == null and empty_idx < 0)
            empty_idx = @intCast(i);
        if (slot.*.used < lru_used_val) {
            lru_used_val = slot.*.used;
            lru_idx = i;
        }
    }

    // Not in cache — open from snapshot dir
    var path: [8192:0]u8 = undefined;
    _ = snprintf(&path, 8192, "%s/%s.dafsa", sdir, rel_name);
    const v = dc.dafsa_view_open(&path) orelse return null;

    var slot_idx: usize = undefined;
    if (empty_idx >= 0) {
        slot_idx = @intCast(empty_idx);
    } else {
        slot_idx = lru_idx;
        if (vcache[slot_idx].view) |old|
            dc.dafsa_view_close(@ptrCast(@alignCast(old)));
    }

    vcache[slot_idx].view = @ptrCast(v);
    vcache[slot_idx].used = 1;
    strncpyZeroPad(@ptrCast(&vcache[slot_idx].rel_name), rel_name);
    return @ptrCast(v);
}

// ─── manifest_find_rel ────────────────────────────────────────────────────

/// int manifest_find_rel_ex(const char *sdir, const char *rel_name,
///                          uint8_t *arity_out, int *variadic_out)
pub export fn manifest_find_rel_ex(sdir: [*c]const u8, rel_name: [*c]const u8, arity_out: [*c]u8, variadic_out: [*c]c_int) c_int {
    var path: [8192:0]u8 = undefined;
    _ = snprintf(&path, 8192, "%s/manifest.txt", sdir);
    const f = c.fopen(&path, "r") orelse return -1;

    var line: ?[*]u8 = null;
    var cap: usize = 0;
    var found: c_int = 0;

    var len: isize = getline(&line, &cap, f);
    while (len > 0) : (len = getline(&line, &cap, f)) {
        var l: usize = @intCast(len);
        const buf = line.?;
        if (buf[l - 1] == '\n') {
            l -= 1;
            buf[l] = 0;
        }
        if (l > 0 and buf[l - 1] == '\r') {
            l -= 1;
            buf[l] = 0;
        }

        if (buf[0] == '#' or buf[0] == 'D') continue;

        var colon: ?usize = null;
        {
            var j: usize = 0;
            while (j < l) : (j += 1) {
                if (buf[j] == ':') {
                    colon = j;
                    break;
                }
            }
        }
        const ci = colon orelse continue;
        buf[ci] = 0;

        if (strEq(buf, rel_name)) {
            const rest: [*c]const u8 = @ptrCast(buf + ci + 1);
            if (rest[0] == '*') {
                // v2 variadic marker: 'name:*:edb|idb'.
                if (variadic_out != null) {
                    variadic_out.* = 1;
                    arity_out.* = 0;
                    found = 1;
                    break;
                }
                // Legacy caller semantics: not found as a fixed relation.
                continue;
            }
            {
                const a = atoiC(rest);
                if (a >= 1 and a <= 8) {
                    arity_out.* = @intCast(a);
                    if (variadic_out != null) variadic_out.* = 0;
                    found = 1;
                    break;
                }
            }
        }
    }

    // Not the plain name: a variadic caller detects the '*' marker above;
    // a per-variant 'name.<a>' line never matches the plain goal name
    // because strcmp compares the whole left side.
    if (line) |lp| c.free(@ptrCast(lp));
    _ = c.fclose(f);
    return found;
}

/// int manifest_find_rel(const char *sdir, const char *rel_name,
///                       uint8_t *arity_out)
pub export fn manifest_find_rel(sdir: [*c]const u8, rel_name: [*c]const u8, arity_out: [*c]u8) c_int {
    return manifest_find_rel_ex(sdir, rel_name, arity_out, null);
}

// ─── manifest_enumerate_rels ──────────────────────────────────────────────

/// Strip a trailing \n / \r (in place, NUL-terminating) — the getline chomp
/// every manifest walker above does inline.
fn chompLine(buf: [*c]u8, len: usize) usize {
    var l = len;
    if (l > 0 and buf[l - 1] == '\n') {
        l -= 1;
        buf[l] = 0;
    }
    if (l > 0 and buf[l - 1] == '\r') {
        l -= 1;
        buf[l] = 0;
    }
    return l;
}

/// One parsed manifest entry.  `name` (NUL-terminated in place) and the
/// arity/idb fields are slices of the getline line buffer — valid only until
/// the next getline.  arity == 0 marks the variadic star base 'name:*'.
const ManifestEntry = struct {
    name: [:0]const u8,
    arity: u8,
    idb: bool,
};

fn strContains(hay: []const u8, needle: []const u8) bool {
    if (needle.len == 0) return true;
    if (hay.len < needle.len) return false;
    var i: usize = 0;
    while (i + needle.len <= hay.len) : (i += 1) {
        if (std.mem.eql(u8, hay[i .. i + needle.len], needle)) return true;
    }
    return false;
}

/// Parse one manifest line into a strict 'name:arity:edb|idb' entry (or a
/// 'name:*' variadic star base).  Returns null for everything the enumerator
/// must skip: comments ('#'), directives ('D'), reserved __PI perm-index
/// names (also recognizable by their trailing ' # perm index' comment), and
/// malformed lines (no flag field, arity outside 1..8).
fn manifestParseLine(buf: [*c]u8, l: usize) ?ManifestEntry {
    if (l == 0 or buf[0] == '#' or buf[0] == 'D') return null;

    var ci: usize = 0;
    while (ci < l) : (ci += 1) {
        if (buf[ci] == ':') break;
    }
    if (ci == 0 or ci == l) return null; // empty name / no arity field
    buf[ci] = 0;
    const name: [:0]const u8 = buf[0..ci :0];

    // reserved perm-index names: '<rel>__PI<hex>__'
    if (strContains(name, "__PI")) return null;

    const rest: [*c]u8 = buf + ci + 1;
    var cj: usize = 0;
    while (rest[cj] != 0 and rest[cj] != ':') cj += 1;
    if (rest[cj] == 0) return null; // no flag field: not the strict shape
    rest[cj] = 0;
    const flag: [*c]const u8 = rest + cj + 1;

    var idb = false;
    if (strEq(flag, "idb")) {
        idb = true;
    } else if (!strEq(flag, "edb")) {
        return null;
    }

    if (rest[0] == '*' and rest[1] == 0)
        return .{ .name = name, .arity = 0, .idb = idb };

    const a = atoiC(rest);
    if (a < 1 or a > MAX_ARITY) return null;
    return .{ .name = name, .arity = @intCast(a), .idb = idb };
}

/// Star-base capacity: db->rels is bounded by MAX_RELS (dl_internal.h), so a
/// manifest can never carry more variadic star bases than this.
const MAX_STAR_BASES: usize = dx.MAX_RELS;

/// Star-base name cap: names longer than this cannot be recorded for
/// variant-line matching, so the walk fails loudly (-1) rather than
/// emitting a variadic's variant lines as fixed relations.  (Relation names
/// are unbounded in principle; every real one is far shorter.)
const MAX_STAR_NAME: usize = 255;

/// Is `name` a per-variant 'base.<d>' line of a collected star base
/// (d a single digit 1..8)?  Same shape manifest_find_variants matches.
fn isVariantOf(name: []const u8, bases: []const [MAX_STAR_NAME + 1]u8, base_lens: []const usize) bool {
    for (bases, base_lens) |*base, bl| {
        if (name.len == bl + 2 and
            std.mem.eql(u8, name[0..bl], base[0..bl]) and
            name[bl] == '.' and
            name[bl + 1] >= '1' and name[bl + 1] <= '8')
            return true;
    }
    return false;
}

/// long manifest_enumerate_rels(const char *sdir, dl_relation_cb cb,
///                              void *user)
///
/// Stream every fixed-arity 'name:arity:edb|idb' entry of `<sdir>/manifest.txt`
/// through `cb` (dl.h dl_relation_cb).  Two passes: the first collects the
/// variadic star-base names ('name:*'), the second emits every fixed entry
/// while skipping the star markers themselves and their per-variant
/// 'name.<d>' lines.  Returns the emitted count, or -1 when the manifest is
/// unreadable or carries a star base this walk cannot represent.
pub export fn manifest_enumerate_rels(sdir: [*c]const u8, cb: dx.dl_relation_cb, user: ?*anyopaque) c_long {
    if (sdir == null or cb == null) return -1;

    var path: [8192:0]u8 = undefined;
    _ = snprintf(&path, 8192, "%s/manifest.txt", sdir);

    // Pass 1: collect star-base names for the pass-2 variant skip.
    var bases: [MAX_STAR_BASES][MAX_STAR_NAME + 1]u8 = undefined;
    var base_lens: [MAX_STAR_BASES]usize = undefined;
    var n_bases: usize = 0;
    {
        const f = c.fopen(&path, "r") orelse return -1;
        defer _ = c.fclose(f);

        var line: ?[*]u8 = null;
        var cap: usize = 0;
        defer if (line) |lp| c.free(@ptrCast(lp));

        var len: isize = getline(&line, &cap, f);
        while (len > 0) : (len = getline(&line, &cap, f)) {
            const ent = manifestParseLine(line.?, chompLine(line.?, @intCast(len))) orelse continue;
            if (ent.arity != 0) continue; // fixed relation: not a base
            if (n_bases >= MAX_STAR_BASES or ent.name.len > MAX_STAR_NAME)
                return -1; // unrepresentable: refuse rather than mis-emit
            @memcpy(bases[n_bases][0..ent.name.len], ent.name);
            base_lens[n_bases] = ent.name.len;
            n_bases += 1;
        }
    }

    // Pass 2: emit every fixed entry, skipping star markers + variants.
    const f = c.fopen(&path, "r") orelse return -1;
    defer _ = c.fclose(f);

    var line: ?[*]u8 = null;
    var cap: usize = 0;
    defer if (line) |lp| c.free(@ptrCast(lp));

    var count: c_long = 0;
    var len: isize = getline(&line, &cap, f);
    while (len > 0) : (len = getline(&line, &cap, f)) {
        const ent = manifestParseLine(line.?, chompLine(line.?, @intCast(len))) orelse continue;
        if (ent.arity == 0) continue; // the star marker itself
        if (isVariantOf(ent.name, bases[0..n_bases], base_lens[0..n_bases])) continue;
        count += 1;
        if (cb.?(ent.name.ptr, ent.arity, if (ent.idb) @as(c_int, 1) else 0, user) != 0)
            return count; // stop early (dl_tuple_cb contract)
    }
    return count;
}

/// void manifest_find_variants(const char *sdir, const char *rel_name,
///                             uint8_t present[9]) — v2: scan the manifest
/// for per-variant lines 'name.<a>:<a>:...' of the variadic relation.
pub export fn manifest_find_variants(sdir: [*c]const u8, rel_name: [*c]const u8, present: [*]u8) void {
    var path: [8192:0]u8 = undefined;
    const nlen = strLen(rel_name);

    @memset(present[0..9], 0);

    _ = snprintf(&path, 8192, "%s/manifest.txt", sdir);
    const f = c.fopen(&path, "r") orelse return;

    var line: ?[*]u8 = null;
    var cap: usize = 0;

    var len: isize = getline(&line, &cap, f);
    while (len > 0) : (len = getline(&line, &cap, f)) {
        var l: usize = @intCast(len);
        const buf = line.?;
        if (buf[l - 1] == '\n') {
            l -= 1;
            buf[l] = 0;
        }
        if (l > 0 and buf[l - 1] == '\r') {
            l -= 1;
            buf[l] = 0;
        }

        if (buf[0] == '#' or buf[0] == 'D') continue;

        var colon: ?usize = null;
        {
            var j: usize = 0;
            while (j < l) : (j += 1) {
                if (buf[j] == ':') {
                    colon = j;
                    break;
                }
            }
        }
        const ci = colon orelse continue;
        buf[ci] = 0;

        // Match '<rel_name>.<d>' with d a single digit 1..8.
        if (ci == nlen + 2 and
            strnEq(buf, rel_name, nlen) and
            buf[nlen] == '.' and
            buf[nlen + 1] >= '1' and buf[nlen + 1] <= '8')
        {
            const a: usize = buf[nlen + 1] - '0';
            const parsed = atoiC(@ptrCast(buf + ci + 1));
            if (parsed == @as(c_int, @intCast(a))) // sanity: variant line carries its arity
                present[a] = 1;
        }
    }

    if (line) |lp| c.free(@ptrCast(lp));
    _ = c.fclose(f);
}

// ─── snapshot_query_scan ──────────────────────────────────────────────────

/// long snapshot_query_scan(const char *db_dir, uint32_t snap_version,
///                          view_cache_slot *vcache, const char *goal_rel,
///                          const uint32_t *leading, uint8_t k,
///                          dl_tuple_cb cb, void *user)
pub export fn snapshot_query_scan(db_dir: [*c]const u8, snap_version: c_uint, vcache: [*c]dx.view_cache_slot, goal_rel: [*c]const u8, leading: ?[*]const u32, k: u8, cb: dx.dl_tuple_cb, user: ?*anyopaque) c_long {
    // Build snapshot dir path
    var sdir: [8192:0]u8 = undefined;
    _ = snprintf(&sdir, 8192, "%s/snapshots/%u", db_dir, snap_version);

    // Resolve arity (and variadic-ness) from the manifest
    var arity: u8 = 0;
    var variadic: c_int = 0;
    if (manifest_find_rel_ex(&sdir, goal_rel, &arity, &variadic) == 0)
        return -1;

    if (variadic != 0) {
        // v2: prefix enumeration fanned out over every PRESENT variant
        // a >= max(k,1) — each variant view walk is the existing mmap'd
        // fixed-width prefix walk (view_prefix); the cb's arity parameter
        // disambiguates tuples across arities.
        var present: [MAX_ARITY + 1]u8 = undefined;
        var total: c_long = 0;

        if (k > MAX_ARITY) return -1;
        if (k > 0 and leading == null) return -1;

        manifest_find_variants(&sdir, goal_rel, &present);
        var a: u8 = if (k > 0) k else 1;
        while (a <= MAX_ARITY) : (a += 1) {
            if (present[a] == 0) continue;
            var vname: [384:0]u8 = undefined;
            if (snprintf(&vname, 384, "%s.%d", goal_rel, @as(c_int, a)) >= 384)
                return -1;
            const v = view_open_cached(vcache, &vname, &sdir) orelse return -1;
            const n = view_prefix(v, a, leading, k, cb, user);
            if (n < 0) return -1;
            total += n;
        }
        return total;
    }

    if (k > arity) return -1;
    if (k > 0 and leading == null) return -1;

    // Open view (from cache if warm)
    const v = view_open_cached(vcache, goal_rel, &sdir) orelse return -1;

    return view_prefix(v, arity, leading, k, cb, user);
}

// ─── Regex pattern walk (mmap view) ───────────────────────────────────────

const VpatCtx = struct {
    arity: u8,
    cb: dx.dl_tuple_cb,
    user: ?*anyopaque,
    count: c_long,
};

fn vpatCb(key_bytes: [*c]const u8, key_len: usize, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *VpatCtx = @ptrCast(@alignCast(user orelse return -1));
    if (key_len != @as(usize, ctx.arity) * 4 + 1) return 0;

    var cols: [MAX_ARITY]u32 = undefined;
    readColsBe(&cols, key_bytes, ctx.arity);

    ctx.count += 1;
    return ctx.cb.?(&cols, ctx.arity, ctx.user);
}

/// long view_pattern(void *view_handle, uint8_t arity,
///                   const struct regex_dfa *dfa, dl_tuple_cb cb, void *user)
pub export fn view_pattern(view_handle: ?*anyopaque, arity: u8, dfa: ?*const regexwalk.regex_dfa, cb: dx.dl_tuple_cb, user: ?*anyopaque) c_long {
    const v: [*c]const dc.dafsa_view = @ptrCast(@alignCast(view_handle orelse return -1));
    if (dfa == null or cb == null) return -1;

    var ctx = VpatCtx{
        .arity = arity,
        .cb = cb,
        .user = user,
        .count = 0,
    };

    // v is this module's @cImport of dafsa_internal.h; regexwalk's is a
    // separate @cImport namespace — cast the identical-layout pointer across
    // (precedent: relation.zig rel_pattern).
    const n = regexwalk.regex_dfa_walk_view(@ptrCast(v), dfa, vpatCb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

// Filter callback context for view_filter_col
const VfilterCtx = struct {
    arity: u8,
    col: u8,
    set: ?*const regexwalk.sym_set,
    cb: dx.dl_tuple_cb,
    user: ?*anyopaque,
    count: c_long,
};

fn vfilterCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *VfilterCtx = @ptrCast(@alignCast(user orelse return -1));
    if (ctx.col < arity and regexwalk.symset_contains(ctx.set, cols[ctx.col]) != 0) {
        ctx.count += 1;
        return ctx.cb.?(cols, ctx.arity, ctx.user);
    }
    return 0;
}

/// long view_filter_col(void *view_handle, uint8_t arity, uint8_t col,
///                      const struct sym_set *set, dl_tuple_cb cb, void *user)
pub export fn view_filter_col(view_handle: ?*anyopaque, arity: u8, col: u8, set: ?*const regexwalk.sym_set, cb: dx.dl_tuple_cb, user: ?*anyopaque) c_long {
    if (view_handle == null or set == null or cb == null) return -1;
    if (col >= arity) return 0; // out of range: no matches

    var ctx = VfilterCtx{
        .arity = arity,
        .col = col,
        .set = set,
        .cb = cb,
        .user = user,
        .count = 0,
    };

    // Enumerate all tuples via view_prefix with k=0 and filter
    const n = view_prefix(view_handle, arity, null, 0, vfilterCb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

// ─── View order-statistics (rank/select/range_count/count) ────────────────

/// Encode arity u32 cols as u32BE + trailing 0x00 (4*arity+1 bytes),
/// identical to relation.c encode_key.
fn viewEncodeKey(buf: *[MAX_KEY_LEN]u8, cols: [*]const u32, arity: u8) usize {
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        const v = cols[i];
        buf[4 * @as(usize, i)] = @truncate(v >> 24);
        buf[4 * @as(usize, i) + 1] = @truncate(v >> 16);
        buf[4 * @as(usize, i) + 2] = @truncate(v >> 8);
        buf[4 * @as(usize, i) + 3] = @truncate(v);
    }
    buf[4 * @as(usize, arity)] = 0x00;
    return 4 * @as(usize, arity) + 1;
}

/// uint64_t view_rank(void *view_handle, uint8_t arity, const uint32_t *cols)
/// Rank of a tuple in the view's total order: the number of keys strictly
/// lexicographically less than `cols`.
pub export fn view_rank(view_handle: ?*anyopaque, arity: u8, cols: ?[*]const u32) u64 {
    const v: [*c]const dc.dafsa_view = @ptrCast(@alignCast(view_handle orelse return 0));
    if (cols == null) return 0;
    if (arity > MAX_ARITY) return 0;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const len = viewEncodeKey(&key, cols.?, arity);
    return dc.dafsa_view_rank_n(v, &key, len);
}

/// int view_select(void *view_handle, uint8_t arity, uint64_t k,
///                 uint32_t *cols_out) — select the k-th (0-indexed) key in
/// the view's total order, decoding its u32 columns into cols_out.
/// Returns 0 on success, or -1 if k is out of range / on OOM (mirrors
/// dl_select's 0/-1 contract).
pub export fn view_select(view_handle: ?*anyopaque, arity: u8, k: u64, cols_out: ?[*]u32) c_int {
    const v: [*c]const dc.dafsa_view = @ptrCast(@alignCast(view_handle orelse return -1));
    if (cols_out == null) return -1;
    if (arity > MAX_ARITY) return -1;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const n = dc.dafsa_view_select_n(v, k, &key, key.len);
    if (n < 0) return -1;
    readColsBe(cols_out.?, &key, arity);
    return 0;
}

/// uint64_t view_range_count(void *view_handle, uint8_t arity,
///                           const uint32_t *lo, const uint32_t *hi)
/// Number of keys in the half-open range [lo, hi).
pub export fn view_range_count(view_handle: ?*anyopaque, arity: u8, lo: ?[*]const u32, hi: ?[*]const u32) u64 {
    const v: [*c]const dc.dafsa_view = @ptrCast(@alignCast(view_handle orelse return 0));
    if (lo == null or hi == null) return 0;
    if (arity > MAX_ARITY) return 0;
    var lo_key: [MAX_KEY_LEN]u8 = undefined;
    var hi_key: [MAX_KEY_LEN]u8 = undefined;
    const lo_len = viewEncodeKey(&lo_key, lo.?, arity);
    const hi_len = viewEncodeKey(&hi_key, hi.?, arity);
    return dc.dafsa_view_range_count_n(v, &lo_key, lo_len, &hi_key, hi_len);
}

/// uint64_t view_count(void *view_handle) — total number of keys in the
/// view (subtree count of the root).
pub export fn view_count(view_handle: ?*anyopaque) u64 {
    const v: [*c]const dc.dafsa_view = @ptrCast(@alignCast(view_handle orelse return 0));
    var counts: [*c]u64 = null;
    const n = dc.dafsa_view_subtree_counts(v, &counts);
    c.free(@ptrCast(counts));
    return n;
}

// ─── tests ────────────────────────────────────────────────────────────────

const testing = std.testing;

const CollectCtx = struct {
    rows: [64][8]u32 = undefined,
    arities: [64]u8 = undefined,
    n: usize = 0,

    fn cb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
        const self: *CollectCtx = @ptrCast(@alignCast(user.?));
        var i: u8 = 0;
        while (i < arity) : (i += 1) self.rows[self.n][i] = cols[i];
        self.arities[self.n] = arity;
        self.n += 1;
        return 0;
    }
};

/// Build a small arity-2 relation {(1,2),(1,3),(2,4)}, save it, and open it
/// as a zero-copy dafsa_view (freed by the caller-provided closer).
fn openTestView(path: [*:0]const u8) ?*dc.dafsa_view {
    const rel = relation.rel_create(2) orelse return null;
    const t1 = [2]u32{ 1, 2 };
    const t2 = [2]u32{ 1, 3 };
    const t3 = [2]u32{ 2, 4 };
    if (relation.rel_add(rel, &t1) < 0) return null;
    if (relation.rel_add(rel, &t2) < 0) return null;
    if (relation.rel_add(rel, &t3) < 0) return null;
    if (relation.rel_save(rel, path) != 0) return null;
    return dc.dafsa_view_open(path);
}

test "manifest_find_rel_ex fixed/variadic/absent + variants" {
    const dir = "/tmp/datalog_zig_u7_snap";
    _ = std.c.mkdir(dir, 0o755); // EEXIST on rerun is fine
    const mfpath = dir ++ "/manifest.txt";
    {
        const f = c.fopen(mfpath, "w") orelse return error.TestUnexpectedResult;
        _ = fputs("# header\n" ++
            "edge:3:edb\n" ++
            "path:*:edb\n" ++
            "edge.2:2:edb\n" ++
            "edge.3:3:idb\n" ++
            "D:some-directive\n", f);
        _ = c.fclose(f);
    }
    defer _ = c.unlink(mfpath);
    defer _ = c.rmdir(dir);

    var arity: u8 = 0;
    var variadic: c_int = 0;

    // fixed arity
    try testing.expectEqual(@as(c_int, 1), manifest_find_rel_ex(dir, "edge", &arity, &variadic));
    try testing.expectEqual(@as(u8, 3), arity);
    try testing.expectEqual(@as(c_int, 0), variadic);

    // legacy wrapper
    try testing.expectEqual(@as(c_int, 1), manifest_find_rel(dir, "edge", &arity));
    try testing.expectEqual(@as(u8, 3), arity);

    // variadic marker: ex reports it, legacy does NOT find it
    try testing.expectEqual(@as(c_int, 1), manifest_find_rel_ex(dir, "path", &arity, &variadic));
    try testing.expectEqual(@as(c_int, 1), variadic);
    try testing.expectEqual(@as(u8, 0), arity);
    try testing.expectEqual(@as(c_int, 0), manifest_find_rel_ex(dir, "path", &arity, null));
    try testing.expectEqual(@as(c_int, 0), manifest_find_rel(dir, "path", &arity));

    // absent relation + absent dir
    try testing.expectEqual(@as(c_int, 0), manifest_find_rel_ex(dir, "missing", &arity, &variadic));
    try testing.expectEqual(@as(c_int, -1), manifest_find_rel_ex("/tmp/datalog_zig_u7_no_such_dir_zz", "edge", &arity, &variadic));

    // variants of the variadic-style name (per-variant lines exist for edge)
    var present: [9]u8 = undefined;
    manifest_find_variants(dir, "edge", &present);
    try testing.expectEqual(@as(u8, 1), present[2]);
    try testing.expectEqual(@as(u8, 1), present[3]);
    var a: usize = 0;
    while (a <= 8) : (a += 1) {
        if (a != 2 and a != 3) try testing.expectEqual(@as(u8, 0), present[a]);
    }
    // sanity check must reject a variant line whose arity field disagrees
    manifest_find_variants(dir, "path", &present);
    try testing.expectEqual(@as(u8, 0), present[0]);
}

test "manifest_enumerate_rels strict shape + star/variant/__PI skips" {
    const dir = "/tmp/datalog_zig_snap_enum";
    _ = std.c.mkdir(dir, 0o755); // EEXIST on rerun is fine
    const mfpath = dir ++ "/manifest.txt";
    {
        const f = c.fopen(mfpath, "w") orelse return error.TestUnexpectedResult;
        _ = fputs("# header\n" ++
            "D:some-directive\n" ++
            "edge:3:edb\n" ++
            "path:*:edb\n" ++
            "path.2:2:edb\n" ++
            "path.5:5:idb\n" ++
            "edge.2:2:edb\n" ++ // NOT a star base's variant (edge is fixed): emitted
            "closure:1:idb\n" ++
            "edge__PI0__:2 # perm index of edge\n" ++
            "weird:9:edb\n" ++ // arity out of range: skipped
            "zero:0:edb\n" ++ // arity out of range: skipped
            "noflag:2\n" ++ // missing flag field: skipped
            "badflag:2:xyz\n" ++ // unknown flag: skipped
            "garbage\n", f); // no colon: skipped
        _ = c.fclose(f);
    }
    defer _ = c.unlink(mfpath);
    defer _ = c.rmdir(dir);

    const Ent = struct { name: [64]u8 = undefined, name_len: usize = 0, arity: u8 = 0, idb: c_int = 0 };
    const Ctx = struct {
        rows: [16]Ent = undefined,
        n: usize = 0,
        stop_at: usize = 0, // 0: never stop (early-stop arm)

        fn cb(name: [*c]const u8, arity: u8, idb: c_int, user: ?*anyopaque) callconv(.c) c_int {
            const self: *@This() = @ptrCast(@alignCast(user.?));
            if (self.n >= self.rows.len) return 1;
            const nm = std.mem.span(@as([*:0]const u8, @ptrCast(name)));
            @memcpy(self.rows[self.n].name[0..nm.len], nm);
            self.rows[self.n].name_len = nm.len;
            self.rows[self.n].arity = arity;
            self.rows[self.n].idb = idb;
            self.n += 1;
            if (self.stop_at != 0 and self.n >= self.stop_at) return 1;
            return 0;
        }

        fn nameOf(self: *const @This(), i: usize) []const u8 {
            return self.rows[i].name[0..self.rows[i].name_len];
        }
    };

    // full enumeration: fixed rels only, in manifest order
    var got = Ctx{};
    try testing.expectEqual(@as(c_long, 3), manifest_enumerate_rels(dir, Ctx.cb, &got));
    try testing.expectEqual(@as(usize, 3), got.n);
    try testing.expectEqualStrings("edge", got.nameOf(0));
    try testing.expectEqual(@as(u8, 3), got.rows[0].arity);
    try testing.expectEqual(@as(c_int, 0), got.rows[0].idb);
    try testing.expectEqualStrings("edge.2", got.nameOf(1)); // fixed rel named like a variant
    try testing.expectEqual(@as(u8, 2), got.rows[1].arity);
    try testing.expectEqualStrings("closure", got.nameOf(2));
    try testing.expectEqual(@as(u8, 1), got.rows[2].arity);
    try testing.expectEqual(@as(c_int, 1), got.rows[2].idb);

    // every skipped shape is absent from the emitted set
    var i: usize = 0;
    while (i < got.n) : (i += 1) {
        const nm = got.nameOf(i);
        try testing.expect(!std.mem.eql(u8, nm, "badflag"));
        try testing.expect(!std.mem.eql(u8, nm, "weird"));
        try testing.expect(!std.mem.eql(u8, nm, "zero"));
        try testing.expect(!std.mem.eql(u8, nm, "noflag"));
        try testing.expect(!std.mem.eql(u8, nm, "garbage"));
        try testing.expect(!std.mem.eql(u8, nm, "path")); // star base itself
        try testing.expect(!std.mem.eql(u8, nm, "path.2")); // its variants
        try testing.expect(!std.mem.eql(u8, nm, "path.5"));
        try testing.expect(std.mem.indexOf(u8, nm, "__PI") == null);
    }

    // early stop: cb returns non-zero at the 2nd entry
    var stopped = Ctx{ .stop_at = 2 };
    try testing.expectEqual(@as(c_long, 2), manifest_enumerate_rels(dir, Ctx.cb, &stopped));
    try testing.expectEqual(@as(usize, 2), stopped.n);

    // unreadable/nonexistent dir + null cb
    try testing.expectEqual(@as(c_long, -1), manifest_enumerate_rels("/tmp/datalog_zig_snap_enum_no_zz", Ctx.cb, &got));
    try testing.expectEqual(@as(c_long, -1), manifest_enumerate_rels(dir, null, &got));
}

test "snapshot_read_current parse + overflow + missing" {
    const dir = "/tmp/datalog_zig_u7_cur";
    _ = std.c.mkdir(dir, 0o755); // EEXIST on rerun is fine
    const snapdir = dir ++ "/snapshots";
    _ = std.c.mkdir(snapdir, 0o755); // EEXIST on rerun is fine
    const cpath = snapdir ++ "/CURRENT";
    defer {
        _ = c.unlink(cpath);
        _ = c.rmdir(snapdir);
        _ = c.rmdir(dir);
    }

    // missing -> 0
    try testing.expectEqual(@as(u32, 0), snapshot_read_current(dir));

    {
        const f = c.fopen(cpath, "w") orelse return error.TestUnexpectedResult;
        _ = fputs("42\n", f);
        _ = c.fclose(f);
    }
    try testing.expectEqual(@as(u32, 42), snapshot_read_current(dir));

    // > 0xFFFFFFFF -> 0
    {
        const f = c.fopen(cpath, "w") orelse return error.TestUnexpectedResult;
        _ = fputs("4294967296\n", f);
        _ = c.fclose(f);
    }
    try testing.expectEqual(@as(u32, 0), snapshot_read_current(dir));

    // parse failure -> 0
    {
        const f = c.fopen(cpath, "w") orelse return error.TestUnexpectedResult;
        _ = fputs("abc\n", f);
        _ = c.fclose(f);
    }
    try testing.expectEqual(@as(u32, 0), snapshot_read_current(dir));
}

test "view_prefix/rank/select/range_count/count over a saved view" {
    const path = "/tmp/datalog_zig_u7_view.dafsa";
    defer _ = c.unlink(path);

    const v = openTestView(path) orelse return error.TestUnexpectedResult;
    defer dc.dafsa_view_close(v);
    const vh: ?*anyopaque = @ptrCast(v);

    // full enumeration (k=0): sorted order
    var got = CollectCtx{};
    try testing.expectEqual(@as(c_long, 3), view_prefix(vh, 2, null, 0, CollectCtx.cb, &got));
    try testing.expectEqual(@as(usize, 3), got.n);
    try testing.expectEqual(@as(u32, 1), got.rows[0][0]);
    try testing.expectEqual(@as(u32, 2), got.rows[0][1]);
    try testing.expectEqual(@as(u32, 3), got.rows[1][1]); // (1,3)
    try testing.expectEqual(@as(u32, 2), got.rows[2][0]);
    try testing.expectEqual(@as(u32, 4), got.rows[2][1]);

    // bound prefix k=1 leading=[1] -> (1,2),(1,3)
    var got2 = CollectCtx{};
    const leading = [1]u32{1};
    try testing.expectEqual(@as(c_long, 2), view_prefix(vh, 2, &leading, 1, CollectCtx.cb, &got2));
    try testing.expectEqual(@as(usize, 2), got2.n);
    try testing.expectEqual(@as(u32, 1), got2.rows[0][0]);
    try testing.expectEqual(@as(u32, 2), got2.rows[0][1]);
    try testing.expectEqual(@as(u32, 3), got2.rows[1][1]);

    // absent prefix -> 0 (valid empty)
    var got3 = CollectCtx{};
    const absent = [1]u32{9};
    try testing.expectEqual(@as(c_long, 0), view_prefix(vh, 2, &absent, 1, CollectCtx.cb, &got3));
    try testing.expectEqual(@as(usize, 0), got3.n);

    // error paths
    try testing.expectEqual(@as(c_long, -1), view_prefix(null, 2, null, 0, CollectCtx.cb, &got3));
    try testing.expectEqual(@as(c_long, -1), view_prefix(vh, 2, null, 0, null, &got3));
    try testing.expectEqual(@as(c_long, -1), view_prefix(vh, 9, null, 0, CollectCtx.cb, &got3)); // arity > MAX_ARITY
    try testing.expectEqual(@as(c_long, -1), view_prefix(vh, 2, null, 3, CollectCtx.cb, &got3)); // k > arity
    try testing.expectEqual(@as(c_long, -1), view_prefix(vh, 2, null, 1, CollectCtx.cb, &got3)); // k>0 !leading

    // order statistics
    const key13 = [2]u32{ 1, 3 };
    const key24 = [2]u32{ 2, 4 };
    try testing.expectEqual(@as(u64, 0), view_rank(vh, 2, &t12));
    try testing.expectEqual(@as(u64, 1), view_rank(vh, 2, &key13));
    try testing.expectEqual(@as(u64, 2), view_rank(vh, 2, &key24));
    try testing.expectEqual(@as(u64, 0), view_rank(null, 2, &key13));

    var out: [2]u32 = undefined;
    try testing.expectEqual(@as(c_int, 0), view_select(vh, 2, 1, &out));
    try testing.expectEqual(@as(u32, 1), out[0]);
    try testing.expectEqual(@as(u32, 3), out[1]);
    try testing.expectEqual(@as(c_int, -1), view_select(vh, 2, 3, &out)); // out of range
    try testing.expectEqual(@as(c_int, -1), view_select(null, 2, 0, &out));

    try testing.expectEqual(@as(u64, 1), view_range_count(vh, 2, &key13, &key24)); // [ (1,3), (2,4) )
    try testing.expectEqual(@as(u64, 2), view_range_count(vh, 2, &t12, &key24)); // [ (1,2), (2,4) ): (1,2),(1,3)
    try testing.expectEqual(@as(u64, 3), view_count(vh));
    try testing.expectEqual(@as(u64, 0), view_count(null));
}
const t12 = [2]u32{ 1, 2 };

test "view_open_cached LRU + vcache_invalidate" {
    // the .dafsa must live INSIDE the snapshot dir the cache opens from
    const dir = "/tmp/datalog_zig_u7_lrudir";
    _ = std.c.mkdir(dir, 0o755); // EEXIST on rerun is fine
    const path = "/tmp/datalog_zig_u7_lrudir/t.dafsa";
    defer _ = c.unlink(path);
    defer _ = c.rmdir(dir);
    const v0 = openTestView(path) orelse return error.TestUnexpectedResult;
    defer dc.dafsa_view_close(v0);

    var vcache: [dx.DL_VIEW_CACHE_SZ]dx.view_cache_slot = std.mem.zeroes([dx.DL_VIEW_CACHE_SZ]dx.view_cache_slot);

    const name = "t";
    const v1 = view_open_cached(&vcache, name, dir) orelse return error.TestUnexpectedResult;
    const v1b = view_open_cached(&vcache, name, dir) orelse return error.TestUnexpectedResult;
    try testing.expectEqual(v1, v1b); // cached hit (used incremented)
    try testing.expectEqual(@as(c_int, 2), vcache[0].used);
    try testing.expectEqual(@as(c_int, 0), vcache[1].used);

    // invalidate closes + zeroes
    vcache_invalidate(&vcache);
    var i: usize = 0;
    while (i < dx.DL_VIEW_CACHE_SZ) : (i += 1) {
        try testing.expect(vcache[i].view == null);
        try testing.expectEqual(@as(c_int, 0), vcache[i].used);
        try testing.expectEqual(@as(u8, 0), vcache[i].rel_name[0]);
    }

    // reopen after invalidate: cache bookkeeping restarted (LRU counters).
    // NOTE: not asserting v2 != v1 — closing + reopening the same file
    // typically mmaps the same address back, deterministically.
    const v2 = view_open_cached(&vcache, name, dir) orelse return error.TestUnexpectedResult;
    _ = v2;
    try testing.expect(vcache[0].view != null);
    try testing.expectEqual(@as(c_int, 1), vcache[0].used);
    vcache_invalidate(&vcache);
}

test "view_filter_col filters on a column value set" {
    const path = "/tmp/datalog_zig_u7_filter.dafsa";
    defer _ = c.unlink(path);
    const v = openTestView(path) orelse return error.TestUnexpectedResult;
    defer dc.dafsa_view_close(v);
    const vh: ?*anyopaque = @ptrCast(v);

    var set: regexwalk.sym_set = std.mem.zeroes(regexwalk.sym_set);
    if (regexwalk.symset_init(&set) != 0) return error.TestUnexpectedResult;
    defer regexwalk.symset_free(&set);
    _ = regexwalk.symset_add(&set, 4);

    var got = CollectCtx{};
    try testing.expectEqual(@as(c_long, 1), view_filter_col(vh, 2, 1, &set, CollectCtx.cb, &got));
    try testing.expectEqual(@as(usize, 1), got.n);
    try testing.expectEqual(@as(u32, 2), got.rows[0][0]);
    try testing.expectEqual(@as(u32, 4), got.rows[0][1]);

    // col out of range -> 0; NULL args -> -1
    try testing.expectEqual(@as(c_long, 0), view_filter_col(vh, 2, 5, &set, CollectCtx.cb, &got));
    try testing.expectEqual(@as(c_long, -1), view_filter_col(null, 2, 0, &set, CollectCtx.cb, &got));
}
