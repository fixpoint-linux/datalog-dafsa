//! intern.zig — port of src/intern.c (string ↔ u32 symbol-id interner).
//!
//! Forward map: DAFSA keyed  utf8_bytes \0 sym_id_u32BE  — a pure DERIVED
//! cache of the reverse array (the source of truth); it is never persisted
//! (intern_save writes only symbols.array) and rebuilt lazily from rev[].
//! Reverse map: growable array of strdup'd char*, indexed by sym_id-1.
//! Sym ids are 1-based; 0 = not-found sentinel.
//!
//! Strangler-hybrid ABI: `struct interner` is OPAQUE to C (intern.h only
//! forward-declares it), so the implementation is a native Zig struct; every
//! non-static C function is an `export fn` with the exact C name, signature
//! and return semantics.  The dafsa engine stays C in the hybrid .so
//! (vendor/dafsa/*.c) and is addressed through raw extern bindings.  All
//! allocation and file I/O goes through raw libc (calloc/realloc/free/strdup,
//! FILE*, getline, fputc) so syscall/partial-write/error semantics match the
//! C oracle byte-for-byte.
//!
//! Oracle: src/intern.c (never modified).  NULL ir/str inputs crash in C and
//! are left unchecked here for the same reason.

const std = @import("std");
const c = std.c;
const util = @import("util.zig");

/// dafsa_internal.h: MAX_WORD_LEN guard (intern-side, on str \0 id_u32BE).
const MAX_WORD_LEN: usize = 65536;

const INTERN_REV_INIT_CAP: u32 = 256;

// ─── dafsa C API (stays C in the hybrid .so) ─────────────────────────────

const dafsa = opaque {};

const DafsaEnumCb = ?*const fn (?[*]const u8, usize, ?*anyopaque) callconv(.c) c_int;

extern "c" fn dafsa_create() ?*dafsa;
extern "c" fn dafsa_free(d: ?*dafsa) void;
extern "c" fn dafsa_add_n(d: *dafsa, key: [*]const u8, len: usize) c_int;
extern "c" fn dafsa_prefix_enum(d: *const dafsa, prefix: [*]const u8, prefix_len: usize, cb: DafsaEnumCb, user: ?*anyopaque) c_long;

// std.c does not re-export these.
extern "c" fn strdup(s: [*:0]const u8) ?[*:0]u8;
extern "c" fn strcmp(a: [*:0]const u8, b: [*:0]const u8) c_int;
extern "c" fn fputc(ch: c_int, stream: *c.FILE) c_int;
extern "c" fn fflush(stream: *c.FILE) c_int;
extern "c" fn fileno(stream: *c.FILE) c_int;
extern "c" fn getline(lineptr: *?[*]u8, n: *usize, stream: *c.FILE) isize;

/// struct interner — private (opaque in intern.h).
const Interner = struct {
    fwd: ?*dafsa, // forward DAFSA (NULL until lazily built)
    rev: ?[*]?[*:0]u8, // reverse array: rev[sym_id-1] -> string
    rev_cap: u32, // capacity of rev
    next_id: u32, // next sym_id to allocate (1-based)
    dirty: c_int, // 1 if new syms added since last save
    fwd_path: ?[*:0]u8, // on-disk path of the fwd DAFSA (kept, unused)
    fwd_ro: c_int, // 1 if fwd must NOT be mutated
    fwd_gen: u32, // next_id the in-memory fwd was built from
};

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

// ─── Callback for dafsa_prefix_enum: capture the first sym_id ────────────

const CaptureCtx = struct {
    id: u32,
    found: c_int,
};

fn captureCB(payload: ?[*]const u8, payload_len: usize, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *CaptureCtx = @ptrCast(@alignCast(user.?));
    // payload is the 4-byte big-endian sym_id
    if (payload_len >= 4) {
        const p = payload.?;
        ctx.id = (@as(u32, p[0]) << 24) | (@as(u32, p[1]) << 16) |
            (@as(u32, p[2]) << 8) | @as(u32, p[3]);
        ctx.found = 1;
    }
    return 1; // stop after first match
}

/// key = str \0 id_u32BE — the layout both ensureFwd and intern_str use.
fn fillKey(k: [*]u8, s: [*:0]const u8, slen: usize, id: u32) void {
    @memcpy(k[0..slen], s[0..slen]);
    k[slen] = 0x00;
    k[slen + 1] = @truncate(id >> 24);
    k[slen + 2] = @truncate(id >> 16);
    k[slen + 3] = @truncate(id >> 8);
    k[slen + 4] = @truncate(id);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────

/// interner *intern_create(void)
pub export fn intern_create() ?*Interner {
    const mem = c.calloc(1, @sizeOf(Interner)) orelse return null;
    const ir: *Interner = @ptrCast(@alignCast(mem));
    ir.* = .{
        .fwd = null,
        .rev = null,
        .rev_cap = INTERN_REV_INIT_CAP,
        .next_id = 1, // 1-based; 0 = not-found
        .dirty = 0,
        .fwd_path = null,
        .fwd_ro = 0,
        .fwd_gen = 0,
    };

    ir.fwd = dafsa_create() orelse {
        c.free(mem);
        return null;
    };

    const rm = c.calloc(ir.rev_cap, @sizeOf(?[*:0]u8)) orelse {
        dafsa_free(ir.fwd);
        c.free(mem);
        return null;
    };
    ir.rev = @ptrCast(@alignCast(rm));
    return ir;
}

/// void intern_free(interner *ir)
pub export fn intern_free(ir: ?*Interner) void {
    const s = ir orelse return;
    dafsa_free(s.fwd);
    if (s.fwd_path) |p| c.free(@ptrCast(p));
    if (s.rev) |rev| {
        var i: u32 = 0;
        const n = s.next_id -% 1;
        while (i < n and i < s.rev_cap) : (i += 1) {
            if (rev[i]) |str| c.free(@ptrCast(str));
        }
        c.free(@ptrCast(rev));
    }
    c.free(@ptrCast(s));
}

// ─── Core ops ────────────────────────────────────────────────────────────

/// Materialize ir->fwd on first need (lazy forward-DAFSA build).
///
/// want_mutable=false: a search-only handle suffices (fwd_ro=1).
/// want_mutable=true: rebuild a fully mutable DAFSA from the reverse array
/// so dafsa_add_n can grow it.
///
/// Returns 0 on success, -1 on OOM.
fn ensureFwd(ir: *Interner, want_mutable: bool) c_int {
    if (ir.fwd != null and (!want_mutable or ir.fwd_ro == 0) and
        ir.fwd_gen == ir.next_id) return 0; // already satisfies the request

    if (ir.fwd) |d| {
        // stale (gen mismatch) or wrong-mode handle; replace with a rebuild
        dafsa_free(d);
        ir.fwd = null;
    }

    // Rebuild the forward DAFSA from the reverse array (the source of truth).
    // The on-disk symbols.dafsa is a pure derived cache and is never trusted:
    // intern_save writes only symbols.array, so the file may be stale or
    // absent.  For each rev[i] (sym_id i+1) build the same key intern_str
    // constructs — str \0 id_u32BE.
    const d = dafsa_create() orelse return -1;
    var i: u32 = 0;
    const n = ir.next_id -% 1;
    while (i < n and i < ir.rev_cap) : (i += 1) {
        const s = ir.rev.?[i] orelse continue;
        const slen = cstrLen(s);
        const key_len = slen + 1 + 4;
        if (key_len > MAX_WORD_LEN) continue; // already-validated at add time
        const km = c.malloc(key_len) orelse {
            dafsa_free(d);
            return -1;
        };
        const key: [*]u8 = @ptrCast(km);
        fillKey(key, s, slen, i + 1);
        if (dafsa_add_n(d, key, key_len) < 0) {
            c.free(km);
            dafsa_free(d);
            return -1;
        }
        c.free(km);
    }
    ir.fwd = d;
    ir.fwd_ro = if (want_mutable) 0 else 1;
    ir.fwd_gen = ir.next_id;
    return 0;
}

/// NON-MUTATING lookup: walk the forward DAFSA if it is materialized and
/// current (fwd_gen == next_id), else linear-scan the reverse array.  Returns
/// 0 if absent.  Never allocates / never marks dirty.
fn lookup(ir: *Interner, str: [*:0]const u8) u32 {
    if (ir.fwd != null and ir.fwd_gen == ir.next_id) {
        const slen = cstrLen(str);
        var ctx = CaptureCtx{ .id = 0, .found = 0 };
        _ = dafsa_prefix_enum(ir.fwd.?, str, slen, captureCB, &ctx);
        return if (ctx.found != 0) ctx.id else 0;
    }

    var i: u32 = 0;
    const n = ir.next_id -% 1;
    while (i < n and i < ir.rev_cap) : (i += 1) {
        if (ir.rev.?[i]) |s| {
            if (strcmp(s, str) == 0) return i + 1;
        }
    }
    return 0;
}

/// uint32_t intern_str_find(interner *ir, const char *str)
pub export fn intern_str_find(ir: ?*Interner, str: ?[*:0]const u8) u32 {
    const s = ir orelse return 0;
    const p = str orelse return 0;
    return lookup(s, p);
}

/// uint32_t intern_str(interner *ir, const char *str)
pub export fn intern_str(ir: ?*Interner, str: ?[*:0]const u8) u32 {
    const s = ir orelse return 0;
    const p = str orelse return 0;

    // Lookup half never mutates / never rebuilds: uses the fwd walk when the
    // cache is current, else the rev[] scan.
    const found = lookup(s, p);
    if (found != 0) return found;

    const slen = cstrLen(p);

    s.dirty = 1;
    const key_len: usize = slen + 1 + 4;
    if (key_len > MAX_WORD_LEN) return 0; // too long

    const id = s.next_id;
    s.next_id +%= 1;

    // Grow reverse array if needed.
    if (id > s.rev_cap) {
        var new_cap = s.rev_cap *% 2;
        if (new_cap < id) new_cap = id *% 2;
        const nm = c.realloc(@ptrCast(s.rev.?), @as(usize, new_cap) * @sizeOf(?[*:0]u8)) orelse return 0;
        const new_rev: [*]?[*:0]u8 = @ptrCast(@alignCast(nm));
        @memset(new_rev[s.rev_cap..new_cap], null);
        s.rev = new_rev;
        s.rev_cap = new_cap;
    }

    // Store string in reverse array.
    const dup = strdup(p);
    s.rev.?[id - 1] = dup;
    if (dup == null) return 0;

    // Grow the forward-DAFSA cache instead of freeing it when the handle is
    // MUTABLE (fwd_ro==0) and CURRENT (fwd_gen == id covers ids 1..id-1).
    // On any add failure (OOM) drop the DAFSA so lookups use the
    // always-correct rev[] scan — never keep a partially-grown cache
    // claiming to be current.
    if (s.fwd != null and s.fwd_ro == 0 and s.fwd_gen == id) {
        if (c.malloc(key_len)) |km| {
            const k: [*]u8 = @ptrCast(km);
            fillKey(k, p, slen, id);
            if (dafsa_add_n(s.fwd.?, k, key_len) < 0) {
                c.free(km);
                dafsa_free(s.fwd.?);
                s.fwd = null;
            } else {
                c.free(km);
                s.fwd_gen = s.next_id; // stay current
            }
        } else {
            dafsa_free(s.fwd.?);
            s.fwd = null;
        }
    } else {
        // Invalidate the stale/read-only forward-DAFSA cache.
        if (s.fwd) |d| {
            dafsa_free(d);
            s.fwd = null;
        }
    }

    return id;
}

/// const char *intern_str_of(interner *ir, uint32_t sym_id)
pub export fn intern_str_of(ir: ?*Interner, sym_id: u32) ?[*:0]const u8 {
    const s = ir orelse return null;
    if (sym_id == 0 or sym_id >= s.next_id) return null;
    if (sym_id > s.rev_cap) return null;
    return s.rev.?[sym_id - 1];
}

// ─── Accessors ───────────────────────────────────────────────────────────

/// const dafsa *intern_fwd(interner *ir)
pub export fn intern_fwd(ir: ?*Interner) ?*const dafsa {
    const s = ir orelse return null;
    if (ensureFwd(s, false) != 0) return null; // OOM
    return s.fwd;
}

/// const dafsa *intern_fwd_mutable(interner *ir)
pub export fn intern_fwd_mutable(ir: ?*Interner) ?*const dafsa {
    const s = ir orelse return null;
    if (ensureFwd(s, true) != 0) return null; // OOM
    return s.fwd;
}

// ─── Persistence ─────────────────────────────────────────────────────────

/// int intern_save(interner *ir, const char *fwd_path, const char *rev_path)
/// Atomic save of symbols.array ONLY: streaming tmp+fsync+rename+dir-fsync.
/// The forward DAFSA is a pure derived cache and is NOT persisted; fwd_path
/// is kept in the signature for caller compatibility but ignored.
pub export fn intern_save(ir: ?*Interner, fwd_path: ?[*:0]const u8, rev_path: ?[*:0]const u8) c_int {
    const s = ir orelse return -1;
    if (fwd_path == null or rev_path == null) return -1;
    const rev_path_p = rev_path.?;

    var tmp: [8192:0]u8 = undefined;
    tmpNameOf(&tmp, rev_path_p);
    const f = c.fopen(&tmp, "w") orelse return -1;

    var i: u32 = 1;
    while (i < s.next_id) : (i += 1) {
        // Escape '\\' and '\n' so embedded newlines round-trip as one
        // physical line (see intern_load).  '\n' becomes the two chars
        // '\\' 'n' (0x5C 0x6E).
        if (s.rev.?[i - 1]) |str| {
            var j: usize = 0;
            while (str[j] != 0) : (j += 1) {
                var ch: c_int = str[j];
                if (ch == '\\' or ch == '\n') {
                    if (fputc('\\', f) == -1) { // EOF
                        _ = c.fclose(f);
                        _ = c.unlink(&tmp);
                        return -1;
                    }
                    ch = if (ch == '\\') '\\' else 'n';
                }
                if (fputc(ch, f) == -1) {
                    _ = c.fclose(f);
                    _ = c.unlink(&tmp);
                    return -1;
                }
            }
        }
        if (fputc('\n', f) == -1) {
            _ = c.fclose(f);
            _ = c.unlink(&tmp);
            return -1;
        }
    }

    if (fflush(f) != 0) {
        _ = c.fclose(f);
        _ = c.unlink(&tmp);
        return -1;
    }
    if (c.fsync(fileno(f)) != 0) {
        _ = c.fclose(f);
        _ = c.unlink(&tmp);
        return -1;
    }
    if (c.fclose(f) != 0) {
        _ = c.unlink(&tmp);
        return -1;
    }

    if (c.rename(&tmp, rev_path_p) != 0) {
        _ = c.unlink(&tmp);
        return -1;
    }
    if (util.fsync_dir_of_path(rev_path_p) != 0) return -1;

    s.dirty = 0;
    return 0;
}

/// interner *intern_load(const char *fwd_path, const char *rev_path)
/// Lazy forward DAFSA: ir->fwd stays NULL; only the reverse array is parsed.
/// Returns an empty interner if the rev file doesn't exist; NULL on OOM.
pub export fn intern_load(fwd_path: ?[*:0]const u8, rev_path: ?[*:0]const u8) ?*Interner {
    const mem = c.calloc(1, @sizeOf(Interner)) orelse return null;
    const ir: *Interner = @ptrCast(@alignCast(mem));
    ir.* = .{
        .fwd = null, // lazy: parsed on first need via ensureFwd
        .rev = null,
        .rev_cap = INTERN_REV_INIT_CAP,
        .next_id = 1,
        .dirty = 0,
        .fwd_path = null,
        .fwd_ro = 0,
        .fwd_gen = 0,
    };

    if (fwd_path) |fp| {
        ir.fwd_path = strdup(fp);
        if (ir.fwd_path == null) {
            c.free(mem);
            return null;
        }
    }

    const rm = c.calloc(ir.rev_cap, @sizeOf(?[*:0]u8)) orelse {
        dafsa_free(ir.fwd);
        c.free(mem);
        return null;
    };
    ir.rev = @ptrCast(@alignCast(rm));

    const f = c.fopen(rev_path.?, "r") orelse return ir; // no file: start empty

    var line: ?[*]u8 = null;
    var linecap: usize = 0;
    while (true) {
        const ll = getline(&line, &linecap, f);
        if (ll <= 0) break;
        var len: usize = @intCast(ll);

        // strip trailing newline
        if (len > 0 and line.?[len - 1] == '\n') {
            len -= 1;
            line.?[len] = 0;
        }
        if (len > 0 and line.?[len - 1] == '\r') {
            len -= 1;
            line.?[len] = 0;
        }

        // Decode escapes in place: '\\' '\\' -> 0x5C, '\\' 'n' -> 0x0A,
        // else verbatim (lone trailing backslash stays a backslash).
        const buf = line.?;
        var in_i: usize = 0;
        var out_i: usize = 0;
        while (buf[in_i] != 0) {
            if (buf[in_i] == '\\') {
                in_i += 1;
                if (buf[in_i] == 'n') {
                    buf[out_i] = '\n';
                    out_i += 1;
                    in_i += 1;
                } else if (buf[in_i] == '\\') {
                    buf[out_i] = '\\';
                    out_i += 1;
                    in_i += 1;
                } else {
                    // lone trailing backslash, or unknown escape
                    buf[out_i] = '\\';
                    out_i += 1;
                }
            } else {
                buf[out_i] = buf[in_i];
                out_i += 1;
                in_i += 1;
            }
        }
        buf[out_i] = 0;

        // Grow rev array if needed.
        if (ir.next_id > ir.rev_cap) {
            var new_cap = ir.rev_cap *% 2;
            if (new_cap < ir.next_id) new_cap = ir.next_id *% 2;
            const nm = c.realloc(@ptrCast(ir.rev.?), @as(usize, new_cap) * @sizeOf(?[*:0]u8)) orelse {
                c.free(@ptrCast(line));
                _ = c.fclose(f);
                intern_free(ir);
                return null;
            };
            const new_rev: [*]?[*:0]u8 = @ptrCast(@alignCast(nm));
            @memset(new_rev[ir.rev_cap..new_cap], null);
            ir.rev = new_rev;
            ir.rev_cap = new_cap;
        }

        const dup = strdup(@ptrCast(buf));
        ir.rev.?[ir.next_id - 1] = dup;
        if (dup == null) {
            c.free(@ptrCast(line));
            _ = c.fclose(f);
            intern_free(ir);
            return null;
        }
        ir.next_id += 1;
    }
    c.free(@ptrCast(line));
    _ = c.fclose(f);

    return ir;
}

// ─── Dirty tracking (M7) ─────────────────────────────────────────────────

/// int intern_is_dirty(interner *ir)
pub export fn intern_is_dirty(ir: ?*Interner) c_int {
    const s = ir orelse return 0;
    return if (s.dirty != 0) 1 else 0;
}

/// void intern_clear_dirty(interner *ir)
pub export fn intern_clear_dirty(ir: ?*Interner) void {
    if (ir) |s| s.dirty = 0;
}

// ─── Tests ───────────────────────────────────────────────────────────────

test "intern create/str/str_find/str_of roundtrip" {
    const ir = intern_create() orelse return error.OutOfMemory;
    defer intern_free(ir);

    try std.testing.expectEqual(@as(c_int, 0), intern_is_dirty(ir));

    const a = intern_str(ir, "hello");
    try std.testing.expectEqual(@as(u32, 1), a);
    const b = intern_str(ir, "world");
    try std.testing.expectEqual(@as(u32, 2), b);
    // Idempotent: same string -> same id.
    try std.testing.expectEqual(a, intern_str(ir, "hello"));
    try std.testing.expectEqual(@as(u32, 2), intern_str(ir, "world"));
    // New symbol marks dirty.
    try std.testing.expectEqual(@as(c_int, 1), intern_is_dirty(ir));

    try std.testing.expectEqual(b, intern_str_find(ir, "world"));
    try std.testing.expectEqual(@as(u32, 0), intern_str_find(ir, "absent"));

    try std.testing.expectEqualStrings("hello", std.mem.span(intern_str_of(ir, a).?));
    try std.testing.expectEqualStrings("world", std.mem.span(intern_str_of(ir, b).?));
    try std.testing.expect(intern_str_of(ir, 0) == null);
    try std.testing.expect(intern_str_of(ir, 3) == null); // >= next_id

    intern_clear_dirty(ir);
    try std.testing.expectEqual(@as(c_int, 0), intern_is_dirty(ir));
    try std.testing.expectEqual(@as(c_int, 0), intern_is_dirty(null));
}

test "lazy fwd rebuild + grow-in-place vs invalidate" {
    const ir = intern_create() orelse return error.OutOfMemory;
    defer intern_free(ir);

    // Lazy: fwd materializes on first accessor; lookups via the DAFSA walk.
    const a = intern_str(ir, "alpha");
    try std.testing.expect(intern_fwd(ir) != null);
    try std.testing.expectEqual(a, intern_str_find(ir, "alpha"));
    try std.testing.expectEqual(@as(u32, 0), intern_str_find(ir, "omega"));

    // intern_fwd_mutable rebuilds a MUTABLE+current handle; intern_str then
    // grows it in place (fwd_gen tracks next_id, so the DAFSA walk answers).
    try std.testing.expect(intern_fwd_mutable(ir) != null);
    const b = intern_str(ir, "beta");
    try std.testing.expectEqual(@as(u32, 2), b);
    try std.testing.expectEqual(b, intern_str_find(ir, "beta"));
    try std.testing.expectEqualStrings("alpha", std.mem.span(intern_str_of(ir, 1).?));
    try std.testing.expectEqualStrings("beta", std.mem.span(intern_str_of(ir, 2).?));

    // A read-only handle (intern_fwd) is invalidated by the next intern_str
    // (fwd freed, lookups fall back to the rev[] scan — same answers).
    try std.testing.expect(intern_fwd(ir) != null);
    const g = intern_str(ir, "gamma");
    try std.testing.expectEqual(@as(u32, 3), g);
    try std.testing.expectEqual(g, intern_str_find(ir, "gamma"));
    try std.testing.expectEqual(a, intern_str_find(ir, "alpha"));
}

test "save/load roundtrip with escape chars" {
    const path = "/tmp/datalog_zig_u3_symbols_test.array";
    defer _ = c.unlink(path);

    const ir = intern_load("unused.dafsa", "/tmp/datalog_zig_u3_nonexistent.array");
    try std.testing.expect(ir != null); // no file -> empty interner
    try std.testing.expectEqual(@as(u32, 0), intern_str_find(ir, "anything"));

    _ = intern_str(ir, "plain");
    _ = intern_str(ir, "back\\slash");
    _ = intern_str(ir, "line1\nline2");
    _ = intern_str(ir, ""); // empty symbol
    try std.testing.expectEqual(@as(c_int, 1), intern_is_dirty(ir));

    // fwd_path is ignored: only symbols.array is written.
    try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", path));
    try std.testing.expectEqual(@as(c_int, 0), intern_is_dirty(ir));

    // Byte-exact escaped text format (embedded \n as backslash-n, \ as \\).
    const fd = c.open(path, .{ .ACCMODE = .RDONLY });
    try std.testing.expect(fd >= 0);
    var buf: [256]u8 = undefined;
    const n: usize = @intCast(c.read(fd, &buf, buf.len));
    _ = c.close(fd);
    try std.testing.expectEqualStrings("plain\nback\\\\slash\nline1\\nline2\n\n", buf[0..n]);
    // tmp file must be gone (renamed onto path).
    try std.testing.expect(c.unlink(path ++ ".tmp") != 0);

    // Reload: escaped strings round-trip, ids restart at 1 in file order.
    const ir2 = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir2);
    try std.testing.expectEqual(@as(c_int, 0), intern_is_dirty(ir2));
    try std.testing.expectEqualStrings("plain", std.mem.span(intern_str_of(ir2, 1).?));
    try std.testing.expectEqualStrings("back\\slash", std.mem.span(intern_str_of(ir2, 2).?));
    try std.testing.expectEqualStrings("line1\nline2", std.mem.span(intern_str_of(ir2, 3).?));
    try std.testing.expectEqualStrings("", std.mem.span(intern_str_of(ir2, 4).?));
    try std.testing.expectEqual(@as(u32, 3), intern_str_find(ir2, "line1\nline2"));
    // Fwd DAFSA stays lazy after load but answers once materialized.
    try std.testing.expect(intern_fwd(ir2) != null);
    try std.testing.expectEqual(@as(u32, 2), intern_str_find(ir2, "back\\slash"));

    intern_free(ir);
}

test "MAX_WORD_LEN reject (key = str \\0 id_u32BE > 65536)" {
    const ir = intern_create() orelse return error.OutOfMemory;
    defer intern_free(ir);

    const too_long = "x" ** 65532; // key_len = 65532 + 1 + 4 = 65537 > 65536
    try std.testing.expectEqual(@as(u32, 0), intern_str(ir, too_long));
    try std.testing.expectEqual(@as(u32, 0), intern_str_find(ir, too_long));
    try std.testing.expect(intern_str_of(ir, 1) == null);

    const ok_long = "x" ** 65531; // key_len = 65536: exactly at the cap, accepted
    const id = intern_str(ir, ok_long);
    try std.testing.expectEqual(@as(u32, 1), id);
    try std.testing.expectEqual(@as(u32, 1), intern_str_find(ir, ok_long));
}

test "rev array grows past initial capacity (doubling)" {
    const ir = intern_create() orelse return error.OutOfMemory;
    defer intern_free(ir);

    var namebuf: [16]u8 = undefined;
    var i: u32 = 1;
    while (i <= 300) : (i += 1) { // crosses the 256 initial capacity
        const name = std.fmt.bufPrintZ(&namebuf, "sym{d}", .{i}) catch unreachable;
        const id = intern_str(ir, name);
        try std.testing.expectEqual(i, id);
    }
    try std.testing.expectEqualStrings("sym1", std.mem.span(intern_str_of(ir, 1).?));
    try std.testing.expectEqualStrings("sym256", std.mem.span(intern_str_of(ir, 256).?));
    try std.testing.expectEqualStrings("sym300", std.mem.span(intern_str_of(ir, 300).?));
    try std.testing.expect(intern_str_of(ir, 301) == null);
    // Lookup works across the grown array (rev[] scan path after rebuild).
    try std.testing.expectEqual(@as(u32, 257), intern_str_find(ir, "sym257"));
}
