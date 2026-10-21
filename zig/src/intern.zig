//! intern.zig — port of src/intern.c (string ↔ u32 symbol-id interner).
//!
//! Forward map: DAFSA keyed  utf8_bytes \0 sym_id_u32BE  — a pure DERIVED
//! cache of the reverse array (the source of truth); it is never persisted
//! (intern_save writes only symbols.array) and rebuilt lazily from rev[].
//! Reverse map: growable array of char* (rev[sym_id-1]) — intern_load points
//! the slots straight into one whole-file buffer (zero per-symbol
//! allocation), intern_str appends strdup'd ones past that.
//! Sym ids are 1-based; 0 = not-found sentinel.
//!
//! WHO ACTUALLY WANTS THE FORWARD DAFSA (as of the ensureFwd-cliff unit): no
//! READ path.  `dl pattern`, the `~` / `~~` rule atoms and version-pattern
//! queries match names with a linear scan over rev[] driven by the compiled
//! regex DFA (regexwalk.zig symbolsScanTable), which MEASURED ~20-40 ms at
//! 166k symbols (including a 166k-name strlen pass) against ensureFwd's
//! 6-9 µs/symbol.  The remaining callers are
//!   - intern_fwd (dl.zig publishDerivedDafsa): the DL_PUB_DERIVED=1 opt-in
//!     serializes the forward DAFSA into a snapshot — a derived artifact
//!     nothing in the engine reads, kept for an external consumer;
//!   - intern_fwd_mutable (index.zig): the full-text index rebuild interns
//!     thousands of tokens and grows the DAFSA IN PLACE instead of paying a
//!     rev[] scan per token (index.zig:994).
//! Both are off the read path, so neither can put ensureFwd in front of a
//! query.  Cutting the index-rebuild one would need a string→id hash index
//! (the scan it replaces is O(symbols) per interned token).
//!
//! Strangler-hybrid ABI: `struct interner` is OPAQUE to C (intern.h only
//! forward-declares it), so the implementation is a native Zig struct; every
//! non-static C function is an `export fn` with the exact C name, signature
//! and return semantics.  The dafsa engine stays C in the hybrid .so
//! (vendor/dafsa/*.c) and is addressed through raw extern bindings.  All
//! allocation and file I/O goes through raw libc (calloc/realloc/free/strdup,
//! FILE*, fread/fwrite) so syscall/partial-write/error semantics match the
//! C oracle byte-for-byte.
//!
//! Oracle: src/intern.c (never modified).  NULL ir/str inputs crash in C and
//! are left unchecked here for the same reason.
//!
//! APPEND-ONLY (the publish gate's premise, MEASURED by reading this module):
//! rev[] is written in exactly two places — the append at rev[id-1] for a
//! NEWLY allocated id in intern_str, and the sequential refill at rev[i-1]
//! while intern_load parses a symbols.array — plus the memset of the freshly
//! realloc'd TAIL (indices >= rev_cap, always above every live id).  No path
//! rewrites or frees an existing entry except intern_free (whole-interner
//! destroy).  So the serialized array is a function of (count, content) only,
//! which is what SymHash below tracks incrementally.

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
extern "c" fn memchr(s: ?*const anyopaque, ch: c_int, n: usize) ?*const anyopaque;
extern "c" fn strdup(s: [*:0]const u8) ?[*:0]u8;
extern "c" fn strcmp(a: [*:0]const u8, b: [*:0]const u8) c_int;
extern "c" fn fflush(stream: *c.FILE) c_int;
extern "c" fn fileno(stream: *c.FILE) c_int;

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
    sym_hash: SymHash, // running hash of symbols.array's canonical bytes
    sym_hashed: u32, // rev[] slots already fed to sym_hash (lazy catch-up)
    // Whole-file buffer intern_load read symbols.array into; rev[0..
    // buf_owned_n) point into it (zero per-symbol allocation), the slots
    // past buf_owned_n are individually strdup'd/freed (the intern_str
    // append path).  intern_free frees the buffer ONCE instead of the
    // per-slot free the strdup'd slots get.
    load_buf: ?[*]u8,
    buf_owned_n: u32,
};

/// The content stamp a publish compares to decide whether a snapshot's
/// symbols.array can be inherited (dl.zig inheritSymbolsArray): the number of
/// symbols and a 128-bit hash of the canonical encoding intern_save writes.
/// count is derived (next_id - 1) and is what the array file's line count is;
/// the hash distinguishes two interners of equal count but different content —
/// which is the case a bare count cannot see (the live rev[] may come from a
/// db-root symbols.array that diverged from the previous SNAPSHOT's).
pub const SymStamp = struct {
    count: u32,
    h0: u64,
    h1: u64,
};

/// Streaming hash of the canonical symbols.array encoding (intern_save's byte
/// stream: text with '\\'->"\\\\" and '\n'->"\\n", then a '\n' per symbol).
/// Two independently seeded 64-bit Wyhashes = a 128-bit stamp.
///
/// LAZY and INCREMENTAL: the hash covers rev[] slots [0, sym_hashed) only.
/// intern_str feeds each APPEND for free when the hash is caught up (the array
/// is append-only, so earlier bytes never change) and symStamp catches up the
/// rest in one sequential pass.  A read-only process therefore never hashes,
/// and a publishing one hashes the pending prefix at most once, OUTSIDE its
/// write path — MEASURED: hashing interleaved with intern_save's per-byte write
/// loop cost ~37 ms on a 44 MB array vs ~25 ms for the sequential catch-up, so
/// the write path deliberately does not fuse it.
const SymHash = struct {
    a: std.hash.Wyhash,
    b: std.hash.Wyhash,

    const seed_b: u64 = 0x9e3779b97f4a7c15;

    fn init() SymHash {
        return .{ .a = std.hash.Wyhash.init(0), .b = std.hash.Wyhash.init(seed_b) };
    }

    fn feed(self: *SymHash, bytes: []const u8) void {
        self.a.update(bytes);
        self.b.update(bytes);
    }

    /// Feed one rev[] slot in canonical encoding.  A null slot (an id consumed
    /// by a strdup failure) encodes exactly as intern_save writes it: a bare
    /// newline.
    fn addSlot(self: *SymHash, s: ?[*:0]const u8) void {
        const p = s orelse {
            self.feed("\n");
            return;
        };
        var start: usize = 0;
        var i: usize = 0;
        while (p[i] != 0) : (i += 1) {
            if (p[i] == '\\' or p[i] == '\n') {
                if (i > start) self.feed(p[start..i]);
                self.feed(if (p[i] == '\\') "\\\\" else "\\n");
                start = i + 1;
            }
        }
        if (i > start) self.feed(p[start..i]);
        self.feed("\n");
    }
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
        .sym_hash = SymHash.init(),
        .sym_hashed = 0,
        .load_buf = null,
        .buf_owned_n = 0,
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
    // The load buffer backs rev[0..buf_owned_n) — free it once, and free the
    // remaining slots (strdup'd by intern_str) individually, as before.
    if (s.load_buf) |b| c.free(@ptrCast(b));
    if (s.rev) |rev| {
        var i: u32 = s.buf_owned_n;
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
    // Feed the content stamp the slot just appended — but ONLY when the hash is
    // caught up to this slot; otherwise the pending slots (unhashed since a
    // load) are fed in order by symStamp's catch-up, this one included.  A
    // failed strdup leaves a HOLE that serializes as a bare newline (addSlot).
    if (s.sym_hashed == id - 1) {
        s.sym_hash.addSlot(if (dup == null) null else p);
        s.sym_hashed = id;
    }
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

/// Zig-internal (NOT a C export — abi_audit stays clean): the number of
/// interned symbols, i.e. the exclusive upper bound of live sym ids.
/// compiler.zig's kind inference uses it to tell a stored u32 that IS a
/// live sym id from a raw int that merely happens to be small.  Takes the
/// opaque dl_internal interner type (same layout as this module's
/// Interner, which is what intern.h leaves opaque).
pub fn liveSymBound(ir: ?*const anyopaque) u32 {
    if (ir == null) return 0;
    const s: *const Interner = @ptrCast(@alignCast(ir.?));
    return s.next_id;
}

/// Zig-internal (NOT a C export): the interner's content stamp — symbol count
/// plus the running 128-bit hash of symbols.array's canonical bytes.  The
/// publish gate (dl.zig inheritSymbolsArray) compares it with the stamp the
/// previous snapshot recorded to decide whether that snapshot's array is
/// already byte-identical to what intern_save would write now — replacing an
/// unconditional 44 MB write + read-back.
///
/// The hash is LAZY: intern_load hashes nothing (a read-only process must not
/// pay a second pass over a 44 MB array it may never publish from), so the
/// first call here feeds every slot loaded since — O(one pass over the array,
/// once per process) — and later calls are O(1) until the next append.  A
/// publish therefore pays ~20 ms of hashing instead of ~150 ms of per-byte
/// rewriting, and nothing else pays anything.  Wyhash.final() shallow-copies
/// its state, so stamping is idempotent.
pub fn symStamp(ir: ?*anyopaque) SymStamp {
    if (ir == null) return .{ .count = 0, .h0 = 0, .h1 = 0 };
    const s: *Interner = @ptrCast(@alignCast(ir.?));
    // Catch up over slots appended since the last stamp (in load ORDER: the
    // canonical byte stream is a concatenation, so order is not optional).
    var i = s.sym_hashed;
    while (i + 1 < s.next_id) : (i += 1) {
        const slot: ?[*:0]const u8 = if (i < s.rev_cap) s.rev.?[i] else null;
        s.sym_hash.addSlot(slot);
    }
    s.sym_hashed = i;
    var a = s.sym_hash.a;
    var b = s.sym_hash.b;
    return .{ .count = s.next_id -% 1, .h0 = a.final(), .h1 = b.final() };
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

/// Bytes accumulated before one fwrite.  MEASURED: the previous loop called
/// fputc once per byte (~44 M calls on the 44 MB array) and was ~126-150 ms of
/// a ~260 ms vocabulary-changing publish; the same stream in 1 MiB chunks is
/// ~44 fwrite calls.
const SAVE_CHUNK: usize = 1 << 20;

/// Buffered sink for symbols.array's canonical byte stream.  Emits the exact
/// bytes the per-byte fputc loop emitted — escaping is decided by the caller —
/// and reports failure the way fputc==-1 did: any chunk that does not transfer
/// whole (ENOSPC, EIO) fails, and intern_save then fcloses + unlinks the tmp
/// file and returns -1.
const SaveSink = struct {
    f: *c.FILE,
    buf: [*]u8,
    len: usize = 0,

    fn flush(self: *SaveSink) bool {
        if (self.len == 0) return true;
        const n = c.fwrite(self.buf, 1, self.len, self.f);
        const ok = n == self.len;
        self.len = 0;
        return ok;
    }

    /// Append bytes, flushing whenever the chunk fills.  A run longer than the
    /// chunk (a symbol may be up to MAX_WORD_LEN bytes) is written across
    /// several chunks rather than needing a buffer that fits all of it.
    fn put(self: *SaveSink, bytes: []const u8) bool {
        var rest = bytes;
        while (rest.len != 0) {
            if (self.len == SAVE_CHUNK and !self.flush()) return false;
            const k = @min(SAVE_CHUNK - self.len, rest.len);
            @memcpy(self.buf[self.len .. self.len + k], rest[0..k]);
            self.len += k;
            rest = rest[k..];
        }
        return true;
    }
};

/// Write the whole reverse array in canonical symbols.array form: each symbol
/// with '\\' -> "\\\\" and '\n' -> "\\n" (so an embedded newline round-trips as
/// one physical line, see intern_load), then one '\n' per symbol.  A null slot
/// (an id consumed by a strdup failure) is its bare '\n'.  Returns 0, or -1 on
/// a failed write.
fn writeSymbols(s: *Interner, f: *c.FILE, buf: [*]u8) c_int {
    var sink = SaveSink{ .f = f, .buf = buf };
    var i: u32 = 1;
    while (i < s.next_id) : (i += 1) {
        if (s.rev.?[i - 1]) |str| {
            // Run scan with libc memchr (vectorized) instead of a
            // byte-at-a-time loop: the scalar scan was MEASURED at ~10 ms of
            // the 44 MB array's write (51 -> 41 ms for the whole save).
            const slen = cstrLen(str);
            var off: usize = 0;
            while (off < slen) {
                var at = slen; // next byte that needs an escape
                var ch: u8 = 0;
                if (memchr(str + off, '\\', slen - off)) |p| {
                    at = @intFromPtr(p) - @intFromPtr(str);
                    ch = '\\';
                }
                if (memchr(str + off, '\n', slen - off)) |p| {
                    const k = @intFromPtr(p) - @intFromPtr(str);
                    if (k < at) {
                        at = k;
                        ch = '\n';
                    }
                }
                if (at > off and !sink.put(str[off..at])) return -1;
                if (at == slen) break;
                const esc = [2]u8{ '\\', if (ch == '\\') '\\' else 'n' };
                if (!sink.put(&esc)) return -1;
                off = at + 1;
            }
        }
        if (!sink.put("\n")) return -1;
    }
    return if (sink.flush()) 0 else -1;
}

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

    const bufp = c.malloc(SAVE_CHUNK) orelse {
        _ = c.fclose(f);
        _ = c.unlink(&tmp);
        return -1;
    };
    const wrote = writeSymbols(s, f, @ptrCast(bufp));
    c.free(bufp);
    if (wrote != 0) {
        _ = c.fclose(f);
        _ = c.unlink(&tmp);
        return -1;
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
/// Bulk read: one fread'd buffer split/decoded in place, rev[] pointing into
/// it (see the body comment for the measurement that retired getline).
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
        .sym_hash = SymHash.init(),
        .sym_hashed = 0,
        .load_buf = null,
        .buf_owned_n = 0,
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

    // Bulk read the whole array into one buffer (fread on large requests
    // bypasses stdio's buffer for direct read()s), then split/decode in
    // place.  MEASURED on a 171,933-symbol / 44 MB array: the old loop —
    // one getline per symbol, a byte-at-a-time escape decode and one
    // strdup per symbol — cost ~100 ms of a CLI invocation's open; this
    // shape answers the same ids/bytes from ~two vectorized memchr scans.
    // The +1 is the sentinel byte a final unterminated line's NUL
    // terminator lands in.
    var cap: usize = 1 << 16;
    var len: usize = 0;
    var bufm: ?*anyopaque = c.malloc(cap + 1) orelse {
        _ = c.fclose(f);
        intern_free(ir);
        return null;
    };
    var buf: [*]u8 = @ptrCast(bufm.?);
    read: while (true) {
        const want = cap - len;
        const n = c.fread(buf + len, 1, want, f);
        len += n;
        if (n < want) break :read; // EOF (a read error stops the load the way getline's did)
        cap *= 2;
        bufm = c.realloc(bufm, cap + 1) orelse {
            c.free(bufm);
            _ = c.fclose(f);
            intern_free(ir);
            return null;
        };
        buf = @ptrCast(bufm.?);
    }
    _ = c.fclose(f);

    if (len == 0) {
        // Empty file (or an unopenable/unreadable one): empty interner, no
        // buffer retained.
        c.free(bufm);
        return ir;
    }
    ir.load_buf = buf;

    // Split on '\n' and decode in place, one rev[] slot per physical line —
    // exactly the symbols getline+strdup produced, in the same order.
    var pos: usize = 0;
    while (pos < len) {
        const base = @intFromPtr(buf);
        const end = if (memchr(buf + pos, '\n', len - pos)) |p|
            @intFromPtr(p) - base // exclusive: the '\n' itself
        else
            len; // final line without a trailing '\n' (accepted, as getline did)

        var seg_len = end - pos;
        // One trailing '\r' (the CRLF the old strip-after-getline removed).
        if (seg_len > 0 and buf[end - 1] == '\r') seg_len -= 1;

        // Decode escapes in place, the old loop's exact state machine:
        // '\\' '\\' -> 0x5C, '\\' 'n' -> 0x0A, else verbatim (a lone trailing
        // backslash stays a backslash), stopping at an embedded NUL exactly
        // where the old strdup did.  A line with no '\\' (the overwhelming
        // case — memchr, vectorized) needs no copy at all: the bytes are
        // already the answer, just terminate.
        if (memchr(buf + pos, '\\', seg_len) == null) {
            buf[pos + seg_len] = 0;
        } else {
            var in_i: usize = 0;
            var out_i: usize = 0;
            while (in_i < seg_len and buf[pos + in_i] != 0) {
                if (buf[pos + in_i] == '\\') {
                    in_i += 1;
                    if (in_i < seg_len and buf[pos + in_i] == 'n') {
                        buf[pos + out_i] = '\n';
                        out_i += 1;
                        in_i += 1;
                    } else if (in_i < seg_len and buf[pos + in_i] == '\\') {
                        buf[pos + out_i] = '\\';
                        out_i += 1;
                        in_i += 1;
                    } else {
                        // lone trailing backslash, or unknown escape
                        buf[pos + out_i] = '\\';
                        out_i += 1;
                    }
                } else {
                    buf[pos + out_i] = buf[pos + in_i];
                    out_i += 1;
                    in_i += 1;
                }
            }
            buf[pos + out_i] = 0;
        }

        // Grow rev array if needed.
        if (ir.next_id > ir.rev_cap) {
            var new_cap = ir.rev_cap *% 2;
            if (new_cap < ir.next_id) new_cap = ir.next_id *% 2;
            const nm = c.realloc(@ptrCast(ir.rev.?), @as(usize, new_cap) * @sizeOf(?[*:0]u8)) orelse {
                intern_free(ir);
                return null;
            };
            const new_rev: [*]?[*:0]u8 = @ptrCast(@alignCast(nm));
            @memset(new_rev[ir.rev_cap..new_cap], null);
            ir.rev = new_rev;
            ir.rev_cap = new_cap;
        }

        // Zero-copy: the slot points at the NUL-terminated line in load_buf.
        ir.rev.?[ir.next_id - 1] = @ptrCast(buf + pos);
        ir.buf_owned_n = ir.next_id;
        ir.next_id += 1;
        pos = end + 1;
    }

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

test "bulk load: CRLF, embedded NUL, truncated final line, empty file" {
    const path = "/tmp/datalog_zig_u3_bulk.array";
    defer _ = c.unlink(path);
    {
        const wf = c.fopen(path, "w") orelse return error.TestUnexpectedResult;
        defer _ = c.fclose(wf);
        // CRLF: exactly one trailing '\r' stripped, as the old strip-after-getline did.
        try std.testing.expectEqual(@as(usize, 6), c.fwrite("crlf\r\n", 1, 6, wf));
        // Embedded NUL: the decode (and every reader) stops where strdup did.
        try std.testing.expectEqual(@as(usize, 4), c.fwrite("a\x00b\n", 1, 4, wf));
        // Escaped newline + unknown escape (\z keeps the backslash verbatim).
        try std.testing.expectEqual(@as(usize, 7), c.fwrite("x\\ny\\z\n", 1, 7, wf));
        // Final line WITHOUT a trailing '\n' — accepted, as getline accepted it.
        try std.testing.expectEqual(@as(usize, 5), c.fwrite("tail.", 1, 5, wf));
    }
    const ir = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir);
    try std.testing.expectEqualStrings("crlf", std.mem.span(intern_str_of(ir, 1).?));
    try std.testing.expectEqualStrings("a", std.mem.span(intern_str_of(ir, 2).?));
    try std.testing.expectEqualStrings("x\ny\\z", std.mem.span(intern_str_of(ir, 3).?));
    try std.testing.expectEqualStrings("tail.", std.mem.span(intern_str_of(ir, 4).?));
    try std.testing.expect(intern_str_of(ir, 5) == null); // 4 symbols exactly
    try std.testing.expectEqual(@as(u32, 3), intern_str_find(ir, "x\ny\\z"));

    // A 0-byte file answers the same empty interner a missing one does.
    const empty = "/tmp/datalog_zig_u3_empty.array";
    defer _ = c.unlink(empty);
    {
        const wf = c.fopen(empty, "w") orelse return error.TestUnexpectedResult;
        _ = c.fclose(wf);
    }
    const eir = intern_load(null, empty) orelse return error.OutOfMemory;
    defer intern_free(eir);
    try std.testing.expectEqual(@as(u32, 0), intern_str_find(eir, "anything"));
    try std.testing.expect(intern_str_of(eir, 1) == null);
}

test "bulk load grows rev[] past initial capacity (zero-copy slots + append)" {
    const path = "/tmp/datalog_zig_u3_bulkgrow.array";
    defer _ = c.unlink(path);
    {
        const wf = c.fopen(path, "w") orelse return error.TestUnexpectedResult;
        defer _ = c.fclose(wf);
        var namebuf: [32]u8 = undefined;
        var i: u32 = 1;
        while (i <= 600) : (i += 1) { // crosses the 256 initial rev_cap ON LOAD
            const name = std.fmt.bufPrint(&namebuf, "s{d}\n", .{i}) catch unreachable;
            try std.testing.expectEqual(name.len, c.fwrite(name.ptr, 1, name.len, wf));
        }
    }
    const ir = intern_load(null, path) orelse return error.OutOfMemory;
    // intern_free must free the load-backed slots via the buffer (a wrong
    // buf_owned_n would free an interior pointer — a loud allocator abort,
    // not a silent leak), so exercise it both WITHOUT and WITH an append.
    try std.testing.expectEqualStrings("s1", std.mem.span(intern_str_of(ir, 1).?));
    try std.testing.expectEqualStrings("s256", std.mem.span(intern_str_of(ir, 256).?));
    try std.testing.expectEqualStrings("s600", std.mem.span(intern_str_of(ir, 600).?));
    try std.testing.expect(intern_str_of(ir, 601) == null);
    try std.testing.expectEqual(@as(u32, 300), intern_str_find(ir, "s300"));
    try std.testing.expectEqual(@as(u32, 601), intern_str(ir, "late"));
    try std.testing.expectEqual(@as(u32, 600), intern_str_find(ir, "s600"));
    intern_free(ir);
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

test "content stamp: append-only hash survives a save/load round trip" {
    const path = "/tmp/datalog_zig_u3_stamp.array";
    defer _ = c.unlink(path);

    const ir = intern_create() orelse return error.OutOfMemory;
    defer intern_free(ir);

    // Empty interner: count 0, a fixed (non-zero) hash of the empty stream.
    const s0 = symStamp(ir);
    try std.testing.expectEqual(@as(u32, 0), s0.count);

    _ = intern_str(ir, "plain");
    _ = intern_str(ir, "back\\slash"); // exercises the '\\'->"\\\\" escape
    _ = intern_str(ir, "line1\nline2"); // exercises the '\n'->"\\n" escape
    _ = intern_str(ir, "");
    const s1 = symStamp(ir);
    try std.testing.expectEqual(@as(u32, 4), s1.count);
    try std.testing.expect(s1.h0 != s0.h0 or s1.h1 != s0.h1);

    // Idempotent re-intern of an existing symbol must NOT move the stamp (no
    // append happened, so no new bytes).
    _ = intern_str(ir, "plain");
    const s1b = symStamp(ir);
    try std.testing.expectEqual(s1.count, s1b.count);
    try std.testing.expectEqual(s1.h0, s1b.h0);
    try std.testing.expectEqual(s1.h1, s1b.h1);

    try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", path));

    // THE INVARIANT THE PUBLISH GATE RESTS ON: an interner rebuilt from the
    // serialized array stamps IDENTICALLY — so a fresh process that loaded the
    // db's symbols.array and published without interning anything computes the
    // same stamp the previous publisher recorded.
    const ir2 = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir2);
    const s2 = symStamp(ir2);
    try std.testing.expectEqual(s1.count, s2.count);
    try std.testing.expectEqual(s1.h0, s2.h0);
    try std.testing.expectEqual(s1.h1, s2.h1);

    // An append moves the stamp (count and hash), which is what makes the gate
    // fall back to the full write on a vocabulary change.
    _ = intern_str(ir2, "later");
    const s3 = symStamp(ir2);
    try std.testing.expectEqual(@as(u32, 5), s3.count);
    try std.testing.expect(s3.h0 != s2.h0 or s3.h1 != s2.h1);

    // A DIFFERENT vocabulary of the SAME count stamps differently (the case a
    // bare count cannot distinguish — my reason for hashing, not counting).
    const ir3 = intern_create() orelse return error.OutOfMemory;
    defer intern_free(ir3);
    _ = intern_str(ir3, "plain");
    _ = intern_str(ir3, "back\\slash");
    _ = intern_str(ir3, "line1\nline2");
    _ = intern_str(ir3, "changed");
    const s4 = symStamp(ir3);
    try std.testing.expectEqual(s1.count, s4.count);
    try std.testing.expect(s4.h0 != s1.h0 or s4.h1 != s1.h1);

    // ... and a GROWING prefix never collides with the shorter one.
    try std.testing.expect(s3.h0 != s1.h0 or s3.h1 != s1.h1);

    // A lone trailing backslash in the FILE decodes to a backslash whose
    // canonical re-encoding differs from the input bytes: the stamp is over the
    // canonical form (what a save writes), so a later save+load of the decoded
    // content is stamp-stable from that point on.
    {
        const odd = "/tmp/datalog_zig_u3_stamp_odd.array";
        defer _ = c.unlink(odd);
        // "a\x" written NON-canonically as a lone backslash + 'x'.
        {
            const wf = c.fopen(odd, "w") orelse return error.TestUnexpectedResult;
            try std.testing.expectEqual(@as(usize, 4), c.fwrite("a\\x\n", 1, 4, wf));
            try std.testing.expectEqual(@as(c_int, 0), c.fclose(wf));
        }
        const odd_ir = intern_load(null, odd) orelse return error.OutOfMemory;
        defer intern_free(odd_ir);
        try std.testing.expectEqualStrings("a\\x", std.mem.span(intern_str_of(odd_ir, 1).?));
        const st_a = symStamp(odd_ir);
        try std.testing.expectEqual(@as(c_int, 0), intern_save(odd_ir, "/tmp/ignored.dafsa", odd));
        const odd_ir2 = intern_load(null, odd) orelse return error.OutOfMemory;
        defer intern_free(odd_ir2);
        const st_b = symStamp(odd_ir2);
        try std.testing.expectEqual(st_a.count, st_b.count);
        try std.testing.expectEqual(st_a.h0, st_b.h0);
        try std.testing.expectEqual(st_a.h1, st_b.h1);
    }
}
