//! intern.zig — port of src/intern.c (string ↔ u32 symbol-id interner).
//!
//! Forward map: DAFSA keyed  utf8_bytes \0 sym_id_u32BE  — a pure DERIVED
//! cache of the reverse array (the source of truth); it is never persisted
//! (intern_save writes only symbols.array) and rebuilt lazily from rev[].
//! Reverse map: growable array of char* (rev[sym_id-1]) — intern_load points
//! the slots straight into one whole-file buffer sized from fstatx and read in
//! one pass (zero per-symbol allocation, no growth copies), intern_str appends
//! strdup'd ones past that.
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
//!
//! THE SAVE IS INCREMENTAL for the same reason: a save whose destination file
//! is still the one this interner last made durable only has to APPEND the
//! canonical lines for the ids added since (one write + one fsync, O(k)) —
//! see appendSymbols.  MEASURED on a copy of the agent store (174k symbols /
//! 45 MB array): a new-string fact went 50.0/52.1/49.5 ms -> 0.14/0.06/0.07 ms
//! (three in a row: 151.5 ms -> 0.27 ms).  Everything that cannot be proven
//! appendable falls back to the whole-file tmp+fsync+rename.

const std = @import("std");
const c = std.c;
const linux = std.os.linux;
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
extern "c" fn fork() c_int;

/// Longest path the incremental-save bookkeeping keys on; a longer one simply
/// takes the whole-file path (every path buffer in the engine is 4096).
const MAX_SAVED_PATH: usize = 4096;

/// One "this file is the durable symbols.array for ids [1, next_id)" record.
///
/// The identity is the FILE's (device, inode, size, mtime, ctime), keyed by
/// the path it was written to.  Size alone would accept a same-length
/// divergence (the hazard the publish stamp exists for: a restored/copied/
/// tampered root array) and append onto bytes this interner never wrote;
/// mtime/ctime catch an in-place rewrite even at an unchanged size, and the
/// path key keeps a recycled inode from being mistaken for our file.
const SavedPrefix = struct {
    path: [MAX_SAVED_PATH]u8, // NUL-terminated; path[0] == 0 -> empty slot
    next_id: u32, // ids [1, next_id) are durable in the file
    bytes: u64, // file size at that point
    dev: u64, // major << 32 | minor
    ino: u64,
    mtim_sec: i64,
    mtim_nsec: i64,
    ctim_sec: i64,
    ctim_nsec: i64,
    used: u64, // LRU stamp
};

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
    // Durable-prefix records for the incremental save (two slots: the db-root
    // array and the last snapshot-staging array are both live in a publishing
    // process, and each must keep its own append state).
    saved: [2]SavedPrefix,
    saved_seq: u64,
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
///
/// WHAT IS FED is literally writeSymbolsTo's output (the same function the
/// save writes with), through one of the sinks below — so the stamp cannot
/// drift from the bytes a save produces.
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
};

/// Hash sink for the CATCH-UP pass: stages the canonical bytes in one buffer
/// and feeds the hash a whole chunk at a time.  MEASURED on the agent store's
/// 45 MB array: one update per symbol/escape/newline (~700k calls) made the
/// first publish of every process cost ~48 ms; 64 KiB batches make the same
/// content hash ~29 ms (the rest is the escape scan and the staging copy, both
/// memory-bandwidth-bound).
const HashSink = struct {
    h: *SymHash,
    buf: [64 * 1024]u8 = undefined,
    len: usize = 0,

    fn flush(self: *HashSink) bool {
        if (self.len != 0) self.h.feed(self.buf[0..self.len]);
        self.len = 0;
        return true;
    }

    fn put(self: *HashSink, bytes: []const u8) bool {
        var rest = bytes;
        while (rest.len != 0) {
            if (self.len == self.buf.len and !self.flush()) return false;
            const k = @min(self.buf.len - self.len, rest.len);
            @memcpy(self.buf[self.len .. self.len + k], rest[0..k]);
            self.len += k;
            rest = rest[k..];
        }
        return true;
    }
};

/// Hash sink for ONE slot (the intern_str append path): feeds directly, since
/// a single symbol is far smaller than a staging buffer.
const DirectSink = struct {
    h: *SymHash,

    fn flush(self: *DirectSink) bool {
        _ = self;
        return true;
    }

    fn put(self: *DirectSink, bytes: []const u8) bool {
        self.h.feed(bytes);
        return true;
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
        .saved = .{ std.mem.zeroes(SavedPrefix), std.mem.zeroes(SavedPrefix) },
        .saved_seq = 0,
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
    // failed strdup leaves a HOLE that serializes as a bare newline, which is
    // exactly what writeSymbolsTo emits for a null slot.
    if (s.sym_hashed == id - 1) {
        var ds = DirectSink{ .h = &s.sym_hash };
        _ = writeSymbolsTo(s, &ds, id, id + 1);
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
/// pay a second pass over a 44 MB array it may never publish from — MEASURED:
/// fusing it into the load costs every open ~5 ms, and it has to be thrown
/// away and redone whenever a line needs escaping), so the first call here
/// feeds every slot loaded since — O(one pass over the array, once per
/// process) — and later calls are O(1) until the next append, which intern_str
/// feeds inline.  A publish therefore pays ~29 ms of hashing instead of ~150 ms
/// of per-byte rewriting, and nothing else pays anything.  Wyhash.final()
/// shallow-copies its state, so stamping is idempotent.
pub fn symStamp(ir: ?*anyopaque) SymStamp {
    if (ir == null) return .{ .count = 0, .h0 = 0, .h1 = 0 };
    const s: *Interner = @ptrCast(@alignCast(ir.?));
    // Catch up over slots appended since the last stamp (in load ORDER: the
    // canonical byte stream is a concatenation, so order is not optional),
    // through the SAME writer a save uses and in whole-buffer batches.
    if (s.sym_hashed + 1 < s.next_id) {
        var hs = HashSink{ .h = &s.sym_hash };
        _ = writeSymbolsTo(s, &hs, s.sym_hashed + 1, s.next_id);
        _ = hs.flush();
        s.sym_hashed = s.next_id - 1;
    }
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

/// Growable in-memory sink for the incremental append's canonical bytes.  The
/// chunk is O(k) — the ids added since the last save — so it is built whole
/// and written with ONE write(2) after the file is locked and re-verified.
const MemSink = struct {
    buf: ?*anyopaque = null,
    cap: usize = 0,
    len: usize = 0,

    fn deinit(self: *MemSink) void {
        if (self.buf) |b| c.free(b);
        self.buf = null;
        self.cap = 0;
        self.len = 0;
    }

    fn flush(self: *MemSink) bool {
        _ = self;
        return true; // nothing is buffered behind the buffer
    }

    fn put(self: *MemSink, bytes: []const u8) bool {
        if (self.len + bytes.len > self.cap) {
            var ncap: usize = if (self.cap == 0) 4096 else self.cap;
            while (ncap < self.len + bytes.len) ncap *= 2;
            const nm = c.realloc(self.buf, ncap) orelse return false;
            self.buf = nm;
            self.cap = ncap;
        }
        const out: [*]u8 = @ptrCast(self.buf.?);
        @memcpy(out[self.len .. self.len + bytes.len], bytes);
        self.len += bytes.len;
        return true;
    }
};

/// Write rev[] slots [from, to) (1-based ids) in canonical symbols.array form
/// to `sink`: each symbol with '\\' -> "\\\\" and '\n' -> "\\n" (so an embedded
/// newline round-trips as one physical line, see intern_load), then one '\n'
/// per symbol.  A null slot (an id consumed by a strdup failure) is its bare
/// '\n'.  Returns false on a failed write.
fn writeSymbolsTo(s: *Interner, sink: anytype, from: u32, to: u32) bool {
    var i: u32 = from;
    while (i < to) : (i += 1) {
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
                if (at > off and !sink.put(str[off..at])) return false;
                if (at == slen) break;
                const esc = [2]u8{ '\\', if (ch == '\\') '\\' else 'n' };
                if (!sink.put(&esc)) return false;
                off = at + 1;
            }
        }
        if (!sink.put("\n")) return false;
    }
    return sink.flush();
}

// ─── intern_load timing probe (DL_INTERN_TIMING) ─────────────────────────
//
// The load is the floor under every CLI invocation, so where its milliseconds
// go is a measurement, not an assumption: the bulk-read copy, the split pass,
// the escape-decode share of it and rev[]'s growth are timed separately on the
// SAME file in one process.  Zero overhead when unset (one cached getenv).
// Uses the Timespec/clock_gettime declarations further down this file.
var intern_timing_state: i8 = -1; // -1 unknown, 0 off, 1 on

fn internTimingOn() bool {
    if (intern_timing_state < 0) {
        const v = c.getenv("DL_INTERN_TIMING");
        intern_timing_state = if (v != null and v.?[0] != 0) 1 else 0;
    }
    return intern_timing_state == 1;
}

fn internNowNs() u64 {
    var ts: Timespec = undefined;
    _ = clock_gettime(CLOCK_MONOTONIC, &ts);
    return @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
}

fn internTimingMsg(comptime fmt: []const u8, args: anytype) void {
    var buf: [512]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch buf[0..0];
    _ = c.write(2, msg.ptr, msg.len);
}

// ─── The split/decode pass ───────────────────────────────────────────────

/// The escape decoder: copies the clean runs between backslashes whole.  MEASURED
/// on the agent store's array: the escaped lines are 4,547 of 177,576 but hold
/// 12.8 MB (avg 2.8 kB each, one backslash per ~145 bytes), so the byte loop
/// spends its time on branch-per-byte over bytes that need no change at all.
///
/// Exactness: the byte loop stopped at the first embedded NUL, and no decode
/// step can PRODUCE one (it emits 0x5C, 0x0A, or a copied non-NUL byte) while
/// the write index never overtakes the read index — so the first NUL in the
/// input bounds the decoded line exactly, and the prefix is decoded the same
/// way without a per-byte NUL test.
fn decodeRuns(buf: [*]u8, pos: usize, seg_len_in: usize) void {
    var seg_len = seg_len_in;
    const p = buf + pos;
    if (memchr(p, 0, seg_len)) |q| seg_len = @intFromPtr(q) - @intFromPtr(p);
    var in_i: usize = 0;
    var out_i: usize = 0;
    while (in_i < seg_len) {
        const run = if (memchr(p + in_i, '\\', seg_len - in_i)) |q|
            @intFromPtr(q) - @intFromPtr(p) - in_i
        else
            seg_len - in_i;
        if (run != 0) {
            std.mem.copyForwards(u8, p[out_i .. out_i + run], p[in_i .. in_i + run]);
            in_i += run;
            out_i += run;
        }
        if (in_i >= seg_len) break;
        in_i += 1; // the '\\' itself
        if (in_i < seg_len and p[in_i] == 'n') {
            p[out_i] = '\n';
            out_i += 1;
            in_i += 1;
        } else if (in_i < seg_len and p[in_i] == '\\') {
            p[out_i] = '\\';
            out_i += 1;
            in_i += 1;
        } else {
            // lone trailing backslash, or unknown escape
            p[out_i] = '\\';
            out_i += 1;
        }
    }
    p[out_i] = 0;
}

/// The split/decode pass over the load buffer: one rev[] slot per physical
/// line, in file order, each line NUL-terminated in place and (when it carries
/// a backslash) escape-decoded in place.
const Splitter = struct {
    ir: *Interner,
    buf: [*]u8,
    len: usize = 0,
    pos: usize = 0,
    esc_n: u64 = 0,
    t_start: u64 = 0,
    t_esc: u64 = 0,
    t_grow: u64 = 0,
    timing: bool = false,

    /// Consume every complete line: split on '\n' up to `to`.
    fn run(self: *Splitter, to: usize) void {
        self.len = to;
        const base = @intFromPtr(self.buf);
        while (self.pos < to) {
            const p = memchr(self.buf + self.pos, '\n', to - self.pos) orelse return;
            const end = @intFromPtr(p) - base; // exclusive: the '\n' itself
            self.one(self.pos, end);
            self.pos = end + 1;
        }
    }

    /// Consume the trailing line of a file that does not end at a line
    /// boundary (accepted, as getline did).
    fn finish(self: *Splitter) void {
        if (self.pos < self.len) {
            self.one(self.pos, self.len);
            self.pos = self.len;
        }
    }

    /// One physical line, [pos, end) — exactly the symbol getline+strdup
    /// produced at that index, in the same order.
    fn one(self: *Splitter, pos: usize, end: usize) void {
        const buf = self.buf;
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
            self.esc_n += 1;
            const te = if (self.timing) internNowNs() else 0;
            decodeRuns(buf, pos, seg_len);
            if (self.timing) self.t_esc += internNowNs() - te;
        }

        // Grow rev array if needed.
        const ir = self.ir;
        if (ir.next_id > ir.rev_cap) {
            const tg = if (self.timing) internNowNs() else 0;
            var new_cap = ir.rev_cap *% 2;
            if (new_cap < ir.next_id) new_cap = ir.next_id *% 2;
            const nm = c.realloc(@ptrCast(ir.rev.?), @as(usize, new_cap) * @sizeOf(?[*:0]u8)) orelse {
                // No rev[] slot was handed out for this line, so leaving the
                // interner as it stands is what the old in-loop OOM did too.
                return;
            };
            const new_rev: [*]?[*:0]u8 = @ptrCast(@alignCast(nm));
            @memset(new_rev[ir.rev_cap..new_cap], null);
            ir.rev = new_rev;
            ir.rev_cap = new_cap;
            if (self.timing) self.t_grow += internNowNs() - tg;
        }

        // Zero-copy: the slot points at the NUL-terminated line in the buffer.
        ir.rev.?[ir.next_id - 1] = @ptrCast(buf + pos);
        ir.buf_owned_n = ir.next_id;
        ir.next_id += 1;
    }

    /// Bookkeeping once every line has been consumed: the appendable-prefix
    /// record, the timing probe, and the interner itself.
    fn done(self: *Splitter, len: usize, ends_nl: bool, path: [*:0]const u8, t_read: u64) *Interner {
        const ir = self.ir;
        // The file holds exactly the ids just parsed, so a later save can
        // append to it — unless it does not end at a line boundary, in which
        // case the parse consumed a PARTIAL line and appending would join the
        // next symbol onto it.
        if (ends_nl) rememberSavedAt(ir, path, len);
        if (self.timing) {
            const total = internNowNs() - self.t_start;
            const split = total - t_read; // everything after the read loop
            internTimingMsg(
                "intern_load: bytes={d} syms={d} esc={d} read_ms={d}.{d:0>3} split_ms={d}.{d:0>3} esc_ms={d}.{d:0>3} grow_ms={d}.{d:0>3} rev_cap={d}\n",
                .{ len, ir.next_id - 1, self.esc_n, t_read / 1_000_000, (t_read / 1000) % 1000, split / 1_000_000, (split / 1000) % 1000, self.t_esc / 1_000_000, (self.t_esc / 1000) % 1000, self.t_grow / 1_000_000, (self.t_grow / 1000) % 1000, ir.rev_cap },
            );
        }
        return ir;
    }
};

// ─── Incremental save: the durable-prefix records ────────────────────────
//
// A save is O(k) when the destination file is still the exact file this
// interner last made durable: symbols.array is append-only, so the only bytes
// that are missing are the canonical lines for ids [saved.next_id, next_id).
// The identity check is what makes that sound — see SavedPrefix — and it is
// O(1) (one statx).

fn statxOfPath(path: [*:0]const u8, out: *linux.Statx) bool {
    return c.statx(c.AT.FDCWD, path, 0, linux.STATX.BASIC_STATS, out) == 0;
}

fn statxOfFd(fd: c_int, out: *linux.Statx) bool {
    return c.statx(fd, "", c.AT.EMPTY_PATH, linux.STATX.BASIC_STATS, out) == 0;
}

fn devOf(stx: *const linux.Statx) u64 {
    return (@as(u64, stx.dev_major) << 32) | @as(u64, stx.dev_minor);
}

/// Is `stx` the file `slot` recorded — same device, inode, size and both
/// timestamps?  Anything that rewrote the file (a tmp+rename from another
/// engine, a restore, an in-place edit) moves at least one of them.
fn isRecordedFile(slot: *const SavedPrefix, stx: *const linux.Statx) bool {
    return stx.ino == slot.ino and devOf(stx) == slot.dev and
        stx.size == slot.bytes and
        stx.mtime.sec == slot.mtim_sec and @as(i64, stx.mtime.nsec) == slot.mtim_nsec and
        stx.ctime.sec == slot.ctim_sec and @as(i64, stx.ctime.nsec) == slot.ctim_nsec;
}

fn savedSlot(s: *Interner, path: [*:0]const u8) ?*SavedPrefix {
    const plen = cstrLen(path);
    if (plen >= MAX_SAVED_PATH) return null;
    for (&s.saved) |*slot| {
        if (slot.path[0] == 0) continue;
        if (std.mem.eql(u8, std.mem.sliceTo(&slot.path, 0), path[0..plen])) return slot;
    }
    return null;
}

/// Record that `path` now holds the canonical bytes for ids [1, next_id).
/// `expect_bytes`, when given, is the size this interner itself just wrote or
/// read: a file of any other size is not ours to append to, so nothing is
/// recorded (and the next save rewrites in full).
fn rememberSavedAt(s: *Interner, path: [*:0]const u8, expect_bytes: ?u64) void {
    const plen = cstrLen(path);
    if (plen >= MAX_SAVED_PATH) return;
    var stx: linux.Statx = undefined;
    if (!statxOfPath(path, &stx)) return;
    if (expect_bytes) |want| {
        if (stx.size != want) return;
    }
    // Reuse this path's slot, else a free one, else the least recently used:
    // the db-root array and the current snapshot-staging array are both live.
    var target: ?*SavedPrefix = null;
    var lru: *SavedPrefix = &s.saved[0];
    for (&s.saved) |*cand| {
        if (cand.path[0] == 0) {
            if (target == null) target = cand; // a free slot beats evicting one
            continue;
        }
        if (std.mem.eql(u8, std.mem.sliceTo(&cand.path, 0), path[0..plen])) {
            target = cand;
            break;
        }
        if (cand.used < lru.used) lru = cand;
    }
    const slot = target orelse lru;
    @memset(&slot.path, 0);
    @memcpy(slot.path[0..plen], path[0..plen]);
    slot.next_id = s.next_id;
    slot.bytes = stx.size;
    slot.dev = devOf(&stx);
    slot.ino = stx.ino;
    slot.mtim_sec = stx.mtime.sec;
    slot.mtim_nsec = @intCast(stx.mtime.nsec);
    slot.ctim_sec = stx.ctime.sec;
    slot.ctim_nsec = @intCast(stx.ctime.nsec);
    s.saved_seq +%= 1;
    slot.used = s.saved_seq;
}

/// Append the ids added since the last save of `path`, in place, and fsync
/// them.  Returns true when the file is durable and complete up to next_id;
/// false means "not provably appendable — write it whole" (the caller then
/// does exactly that, which is also what repairs a torn file).
///
/// DURABILITY (M7): the fsync happens before this returns 0, so the interner
/// is durable before the WAL record that references the new ids is appended —
/// the same ordering the whole-file save provided.
///
/// ATOMICITY: the whole-file path commits by rename, so a crash leaves either
/// the old file or the new one.  An append cannot be atomic that way, and it
/// does not need to be: a crash mid-append leaves a TRUNCATED FINAL LINE, which
/// intern_load accepts (it takes the last line without a trailing '\n' as a
/// symbol — the getline behaviour it replaced).  The id it lands on was
/// allocated but never referenced by any WAL record (the WAL append follows a
/// successful save), so the residue is an unreferenced symbol: readable,
/// monotone in the ids that matter, and CANONICALIZED by the next save, which
/// refuses to append to a file that does not end at a line boundary.  A reader
/// never sees the torn state at all: intern_load takes a shared flock on the
/// array for the length of its read and the append holds it exclusively.
fn appendSymbols(s: *Interner, path: [*:0]const u8) bool {
    const slot = savedSlot(s, path) orelse return false;
    if (slot.next_id > s.next_id) { // cannot happen (append-only); distrust it
        slot.path[0] = 0;
        return false;
    }
    var stx: linux.Statx = undefined;
    if (!statxOfPath(path, &stx) or !isRecordedFile(slot, &stx)) {
        slot.path[0] = 0;
        return false;
    }
    if (slot.next_id == s.next_id) return true; // nothing new to make durable

    var sink = MemSink{};
    defer sink.deinit();
    if (!writeSymbolsTo(s, &sink, slot.next_id, s.next_id)) return false;

    // O_APPEND and NO O_CREAT: the file must be the one we just verified.
    const fd = c.open(path, .{ .ACCMODE = .WRONLY, .APPEND = true });
    if (fd < 0) {
        slot.path[0] = 0;
        return false;
    }
    if (c.flock(fd, c.LOCK.EX) != 0) {
        _ = c.close(fd);
        slot.path[0] = 0;
        return false;
    }
    // Re-verify under the lock: the stat above raced with the open.
    if (!statxOfFd(fd, &stx) or !isRecordedFile(slot, &stx)) {
        _ = c.close(fd);
        slot.path[0] = 0;
        return false;
    }
    const out: [*]const u8 = @ptrCast(sink.buf.?);
    var off: usize = 0;
    while (off < sink.len) {
        const n = c.write(fd, out + off, sink.len - off);
        if (n <= 0) break;
        off += @intCast(n);
    }
    const ok = off == sink.len and c.fsync(fd) == 0;
    if (!ok) {
        // Roll the file back to the last complete line so the torn tail does
        // not outlive this call; the caller's full rewrite repairs it anyway.
        _ = c.ftruncate(fd, @intCast(slot.bytes));
        _ = c.fsync(fd);
        slot.path[0] = 0;
    }
    _ = c.close(fd); // the fsync above is the durability point
    if (!ok) return false;
    rememberSavedAt(s, path, null);
    return true;
}

/// int intern_save(interner *ir, const char *fwd_path, const char *rev_path)
/// Save of symbols.array ONLY.  Two shapes:
///   - INCREMENTAL (appendSymbols): append the canonical lines of the ids
///     added since the last save of this exact file — one write + one fsync,
///     O(k).  This is the shape every per-fact write takes (dl_add_fact and
///     friends save the interner before appending the WAL record).
///   - WHOLE FILE: streaming tmp+fsync+rename+dir-fsync, as before.  Taken on
///     the first save of a path, after any rewrite of the file by something
///     else, and for a file that does not end at a line boundary.
/// The forward DAFSA is a pure derived cache and is NOT persisted; fwd_path
/// is kept in the signature for caller compatibility but ignored.
pub export fn intern_save(ir: ?*Interner, fwd_path: ?[*:0]const u8, rev_path: ?[*:0]const u8) c_int {
    const s = ir orelse return -1;
    if (fwd_path == null or rev_path == null) return -1;
    const rev_path_p = rev_path.?;

    if (appendSymbols(s, rev_path_p)) {
        s.dirty = 0;
        return 0;
    }

    var tmp: [8192:0]u8 = undefined;
    tmpNameOf(&tmp, rev_path_p);
    const f = c.fopen(&tmp, "w") orelse return -1;

    const bufp = c.malloc(SAVE_CHUNK) orelse {
        _ = c.fclose(f);
        _ = c.unlink(&tmp);
        return -1;
    };
    var sink = SaveSink{ .f = f, .buf = @ptrCast(bufp) };
    const wrote = writeSymbolsTo(s, &sink, 1, s.next_id);
    c.free(bufp);
    if (!wrote) {
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

    // The renamed file is now the durable prefix for every live id, so the
    // next save of this path can append instead of rewriting.
    rememberSavedAt(s, rev_path_p, null);
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
        .saved = .{ std.mem.zeroes(SavedPrefix), std.mem.zeroes(SavedPrefix) },
        .saved_seq = 0,
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

    const timing = internTimingOn();
    var t_read: u64 = 0;
    var t_start: u64 = 0;
    if (timing) t_start = internNowNs();

    // SHARED lock for the length of the read: a save that APPENDS to this file
    // holds it exclusively, so a reader can never observe a half-appended
    // array (the whole-file path commits by rename and needs no lock).  The
    // close below releases it.
    _ = c.flock(fileno(f), c.LOCK.SH);

    // Read the array into ONE buffer of EXACTLY its size, then split/decode it
    // in place.  MEASURED on the agent store's 49 MB / 177,576-symbol array,
    // where the read copy is the largest single term of a CLI invocation:
    // sizing the buffer from fstatx up front (one malloc, one fread, no
    // doubling) is 4.7 ms cheaper than growing it by doubling while reading —
    // the growth path re-reads/re-copies up to a whole extra array and leaves
    // ~15 MB of over-allocation to fault in.
    //
    // The size is taken from fstatx on the fd this process holds the SHARED
    // lock on, which is what makes it trustworthy: an appender needs that lock
    // exclusively, and a whole-file save replaces the inode (tmp+rename)
    // without touching the one this fd refers to.  rev[] then points INTO this
    // buffer, so it must never be realloc'd — which is exactly what pre-sizing
    // guarantees (nothing is appended to it after the split starts).
    //
    // Without a usable size (no fstatx, or an empty/non-regular file) the
    // buffer grows while it is read, as it did before.
    var st = Splitter{ .ir = ir, .buf = undefined, .timing = timing };
    if (timing) st.t_start = t_start;
    var last_byte: u8 = 0; // the file's raw final byte, before the split overwrites it
    var stx: linux.Statx = undefined;
    const pre: usize = if (statxOfFd(fileno(f), &stx)) @intCast(stx.size) else 0;

    if (pre > 0) {
        const bufm = c.malloc(pre + 1) orelse {
            _ = c.fclose(f);
            intern_free(ir);
            return null;
        };
        st.buf = @ptrCast(bufm);
        // Retained for intern_free to release ONCE: every rev[] slot below
        // buf_owned_n points into it and must not be freed individually.
        ir.load_buf = st.buf;
        // fread loops internally until the count is met or EOF, so one call
        // fills the whole buffer.
        const got = c.fread(st.buf, 1, pre, f);
        if (timing) t_read = internNowNs() - t_start;
        _ = c.fclose(f);
        if (got == 0) {
            // Empty file (or an unreadable one): empty interner, no buffer
            // retained.  A 0-byte file IS a valid appendable prefix.
            c.free(bufm);
            rememberSavedAt(ir, rev_path.?, 0);
            return ir;
        }
        last_byte = st.buf[got - 1];
        st.run(got);
        st.finish();
        return st.done(got, last_byte == '\n', rev_path.?, t_read);
    }

    // Grow-while-reading fallback: fstatx gave no usable size.
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
    // Does the file end at a LINE BOUNDARY?  Read before the split below
    // overwrites the final '\n' with a NUL terminator.  A file that does not
    // (a torn append — the one crash shape this module can leave behind) is
    // still parsed (the last line is a symbol, as getline accepted it) but is
    // NOT recorded as an appendable prefix: the next save rewrites it whole,
    // which both canonicalizes it and prevents appending onto a partial line.
    const ends_nl = len > 0 and buf[len - 1] == '\n';
    _ = c.fclose(f);
    if (timing) t_read = internNowNs() - t_start;

    if (len == 0) {
        // Empty file (or an unopenable/unreadable one): empty interner, no
        // buffer retained.  A 0-byte file IS a valid appendable prefix.
        c.free(bufm);
        rememberSavedAt(ir, rev_path.?, 0);
        return ir;
    }

    st.buf = buf;
    ir.load_buf = buf; // retained for intern_free, as above
    st.run(len);
    st.finish();
    return st.done(len, ends_nl, rev_path.?, t_read);
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

test "escape decode: run-copy answers the byte-at-a-time state machine exactly" {
    // The reference: the decoder this module used before it was rewritten to
    // copy the clean runs between backslashes whole.  Every shape it has to
    // answer is in here ('\\' 'n' -> LF, '\\' '\\' -> backslash, an unknown
    // escape or a lone trailing backslash kept verbatim, a stop at the first
    // embedded NUL), plus a seeded random corpus over exactly those bytes.
    const Reference = struct {
        fn decode(buf: [*]u8, pos: usize, seg_len: usize) void {
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
    };

    // Offset the corpus inside the buffer: the caller hands the decoder a
    // pointer into the middle of the load buffer, never index 0.
    const off = 7;
    var ref: [128]u8 = undefined;
    var fast: [128]u8 = undefined;
    var prng = std.Random.DefaultPrng.init(0x5eed_1775);
    const rand = prng.random();
    const alphabet = [_]u8{ 'a', 'z', ' ', 'q', '\\', 'n', 0x00, '\r', '\n' };

    var round: usize = 0;
    while (round < 20_000) : (round += 1) {
        const n = rand.intRangeAtMost(usize, 0, 96);
        var i: usize = 0;
        while (i < n) : (i += 1) {
            fast[off + i] = alphabet[rand.intRangeAtMost(usize, 0, alphabet.len - 1)];
        }
        // Tail garbage after the segment must survive identically too.
        i = 0;
        while (i < 24) : (i += 1) {
            const g = rand.int(u32);
            fast[off + n + i] = @truncate(g);
        }
        @memcpy(ref[0 .. off + n + 24], fast[0 .. off + n + 24]);
        Reference.decode(&ref, off, n);
        decodeRuns(&fast, off, n);
        try std.testing.expectEqualSlices(u8, ref[0 .. off + n + 24], fast[0 .. off + n + 24]);
    }

    // And the named shapes, spelled out so a corpus change cannot lose them.
    const cases = [_]struct { in: []const u8, out: []const u8 }{
        .{ .in = "", .out = "" },
        .{ .in = "plain", .out = "plain" },
        .{ .in = "a\\nb", .out = "a\nb" },
        .{ .in = "a\\\\b", .out = "a\\b" },
        .{ .in = "a\\qb", .out = "a\\qb" },
        .{ .in = "trailing\\", .out = "trailing\\" },
        .{ .in = "\\", .out = "\\" },
        .{ .in = "\\n", .out = "\n" },
        .{ .in = "\\\\n", .out = "\\n" },
        .{ .in = "nul\\n\x00tail", .out = "nul\n" },
        .{ .in = "\x00leading", .out = "" },
        .{ .in = "\\\\\\n", .out = "\\\n" },
    };
    for (cases) |cs| {
        @memcpy(fast[off .. off + cs.in.len], cs.in);
        decodeRuns(&fast, off, cs.in.len);
        try std.testing.expectEqualStrings(cs.out, std.mem.sliceTo(fast[off .. off + cs.in.len + 1], 0));
    }
}

test "bulk load retains its buffer for intern_free (no leak of the array)" {
    // intern_free frees the load buffer ONCE and the slots past buf_owned_n
    // individually.  If intern_load stops recording the buffer, every slot
    // below buf_owned_n becomes unfreeable and the whole array leaks — silently,
    // since nothing else reads the field.  This pins the invariant.
    const path = "/tmp/datalog_zig_u3_loadbuf.array";
    defer _ = c.unlink(path);
    {
        const wf = c.fopen(path, "w") orelse return error.TestUnexpectedResult;
        defer _ = c.fclose(wf);
        try std.testing.expectEqual(@as(usize, 11), c.fwrite("one\ntwo\n3rd", 1, 11, wf));
    }
    const ir = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir);
    try std.testing.expect(ir.load_buf != null);
    try std.testing.expectEqual(@as(u32, 3), ir.buf_owned_n);
    try std.testing.expectEqual(@as(u32, 4), ir.next_id);
    // Every loaded slot points INTO that buffer.
    const lo = @intFromPtr(ir.load_buf.?);
    var i: u32 = 0;
    while (i < ir.buf_owned_n) : (i += 1) {
        const p = @intFromPtr(ir.rev.?[i].?);
        try std.testing.expect(p >= lo and p < lo + 11);
    }
    try std.testing.expectEqualStrings("one", std.mem.span(ir.rev.?[0].?));
    try std.testing.expectEqualStrings("3rd", std.mem.span(ir.rev.?[2].?));
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

// ─── Incremental save: the append path ───────────────────────────────────

const Timespec = extern struct { sec: isize, nsec: isize };
const CLOCK_MONOTONIC: c_int = 1;
extern "c" fn clock_gettime(clk_id: c_int, tp: *Timespec) c_int;

fn testNowNs() u64 {
    var ts: Timespec = undefined;
    _ = clock_gettime(CLOCK_MONOTONIC, &ts);
    return @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
}

/// (device, inode) of `path`, or null when it cannot be stat'd.
fn testIno(path: [*:0]const u8) ?u64 {
    var stx: linux.Statx = undefined;
    if (!statxOfPath(path, &stx)) return null;
    return stx.ino;
}

fn testSize(path: [*:0]const u8) ?u64 {
    var stx: linux.Statx = undefined;
    if (!statxOfPath(path, &stx)) return null;
    return stx.size;
}

/// Read a whole file into `out`; returns the byte count read.
fn testRead(path: [*:0]const u8, out: []u8) usize {
    const f = c.fopen(path, "r") orelse return 0;
    defer _ = c.fclose(f);
    return c.fread(out.ptr, 1, out.len, f);
}

/// Write `bytes` to `path`, replacing it.
fn testWrite(path: [*:0]const u8, bytes: []const u8) bool {
    const f = c.fopen(path, "w") orelse return false;
    const n = c.fwrite(bytes.ptr, 1, bytes.len, f);
    return c.fclose(f) == 0 and n == bytes.len;
}

test "incremental save: k new symbols cost an append, not a whole-file rewrite" {
    const path = "/tmp/datalog_zig_u3_incr_append.array";
    const full = "/tmp/datalog_zig_u3_incr_append_full.array";
    defer _ = c.unlink(path);
    defer _ = c.unlink(full);
    defer _ = c.unlink(full ++ ".tmp");

    // A vocabulary big enough for the difference to be visible, written
    // directly: interning 60k symbols one at a time is a linear rev[] scan
    // each (O(n^2)).
    {
        const f = c.fopen(path, "w") orelse return error.TestUnexpectedResult;
        var namebuf: [64]u8 = undefined;
        var i: u32 = 0;
        while (i < 60000) : (i += 1) {
            const line = std.fmt.bufPrint(&namebuf, "seed_symbol_{d}_padding_padding_padding\n", .{i}) catch unreachable;
            try std.testing.expectEqual(line.len, c.fwrite(line.ptr, 1, line.len, f));
        }
        try std.testing.expectEqual(@as(c_int, 0), c.fclose(f));
    }
    const size0 = testSize(path).?;
    try std.testing.expect(size0 > 2_000_000);

    const ir = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir);
    try std.testing.expectEqual(@as(u32, 60000), liveSymBound(ir) - 1);

    // Reference cost: the WHOLE file, written to a different path (a full save
    // of the same vocabulary — what every new-string add used to pay).
    var t_full = testNowNs();
    try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", full));
    t_full = testNowNs() - t_full;
    try std.testing.expectEqual(size0, testSize(full).?); // same bytes

    // Three new symbols, each saved on its own (the dl_add_fact shape).
    const ino0 = testIno(path).?;
    var t_app: u64 = 0;
    var k: u32 = 0;
    while (k < 3) : (k += 1) {
        var nbuf: [64]u8 = undefined;
        const s = std.fmt.bufPrintZ(&nbuf, "appended_symbol_{d}", .{k}) catch unreachable;
        try std.testing.expectEqual(@as(u32, 60001 + k), intern_str(ir, s.ptr));
        const t0 = testNowNs();
        try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", path));
        t_app += testNowNs() - t0;
        // An APPEND keeps the inode; a whole-file save renames a new one in.
        try std.testing.expectEqual(ino0, testIno(path).?);
    }
    // The file grew by exactly the three new lines, and no more.
    const grown = "appended_symbol_0\nappended_symbol_1\nappended_symbol_2\n";
    try std.testing.expectEqual(size0 + grown.len, testSize(path).?);
    // THE CLAIM: three saves together cost less than ONE whole-file save (they
    // used to cost three of them).  Deliberately a loose bound — the point is
    // the O(k)/O(n) shape, and the inode assertion above is the exact proof.
    try std.testing.expect(t_app < t_full);

    // What was appended is what a reload sees, in order, after the seed.
    const ir2 = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir2);
    try std.testing.expectEqual(@as(u32, 60003), liveSymBound(ir2) - 1);
    try std.testing.expectEqualStrings("seed_symbol_0_padding_padding_padding", std.mem.span(intern_str_of(ir2, 1).?));
    try std.testing.expectEqualStrings("seed_symbol_59999_padding_padding_padding", std.mem.span(intern_str_of(ir2, 60000).?));
    try std.testing.expectEqualStrings("appended_symbol_2", std.mem.span(intern_str_of(ir2, 60003).?));
    try std.testing.expectEqual(@as(u32, 60002), intern_str_find(ir2, "appended_symbol_1"));
    // The stamp of the appended file equals the stamp of the same content
    // written whole: the append writes exactly the canonical bytes.
    const s_app = symStamp(ir);
    const s_full = symStamp(ir2);
    try std.testing.expectEqual(s_app.count, s_full.count);
    try std.testing.expectEqual(s_app.h0, s_full.h0);
    try std.testing.expectEqual(s_app.h1, s_full.h1);
}

test "incremental save: appended ids are durable across an _exit without close" {
    const path = "/tmp/datalog_zig_u3_incr_crash.array";
    defer _ = c.unlink(path);

    {
        const ir = intern_load(null, path) orelse return error.OutOfMemory;
        _ = intern_str(ir, "before_a");
        _ = intern_str(ir, "before_b");
        try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", path));
        intern_free(ir);
    }
    try std.testing.expectEqualStrings("before_a\nbefore_b\n", blk: {
        var b: [64]u8 = undefined;
        const n = testRead(path, &b);
        break :blk b[0..n];
    });
    const ino0 = testIno(path).?;

    const pid = fork();
    try std.testing.expect(pid >= 0);
    if (pid == 0) {
        // CHILD: load, intern two more, save (APPEND), then _exit with no
        // close and no intern_free — the crash-equivalent of a process that
        // dies right after a save.
        const ir = intern_load(null, path) orelse c._exit(11);
        _ = intern_str(ir, "crash_c");
        _ = intern_str(ir, "crash_d");
        if (intern_save(ir, "/tmp/ignored.dafsa", path) != 0) c._exit(12);
        c._exit(0);
    }
    var status: c_int = -1;
    try std.testing.expectEqual(pid, c.waitpid(pid, &status, 0));
    try std.testing.expectEqual(@as(c_int, 0), status);
    try std.testing.expectEqual(ino0, testIno(path).?); // it was an APPEND

    // A fresh process (this one) reads the appended strings: the save's fsync
    // is the durability point, not the close.
    const ir2 = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir2);
    try std.testing.expectEqual(@as(u32, 4), liveSymBound(ir2) - 1);
    try std.testing.expectEqual(@as(u32, 3), intern_str_find(ir2, "crash_c"));
    try std.testing.expectEqual(@as(u32, 4), intern_str_find(ir2, "crash_d"));
    try std.testing.expectEqualStrings("crash_d", std.mem.span(intern_str_of(ir2, 4).?));
}

test "incremental save: a torn tail is not appended to — the next save rewrites it" {
    const path = "/tmp/datalog_zig_u3_incr_torn.array";
    defer _ = c.unlink(path);
    defer _ = c.unlink(path ++ ".tmp");

    // A file whose final line was cut mid-append: no trailing '\n'.
    try std.testing.expect(testWrite(path, "alpha\nbeta"));
    const ino0 = testIno(path).?;

    const ir = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir);
    // The parser ACCEPTS the truncated final line (the getline behaviour it
    // replaced), so the file is readable — that is the recovery guarantee.
    try std.testing.expectEqualStrings("alpha", std.mem.span(intern_str_of(ir, 1).?));
    try std.testing.expectEqualStrings("beta", std.mem.span(intern_str_of(ir, 2).?));

    // But it is NOT an appendable prefix: appending would join "gamma" onto
    // "beta".  The save rewrites the file whole (a NEW inode) and canonicalizes
    // it.
    _ = intern_str(ir, "gamma");
    try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", path));
    try std.testing.expect(testIno(path).? != ino0);
    {
        var b: [64]u8 = undefined;
        const n = testRead(path, &b);
        try std.testing.expectEqualStrings("alpha\nbeta\ngamma\n", b[0..n]);
    }

    // ... and THAT file is appendable, so the next save is O(k) again.
    const ino1 = testIno(path).?;
    _ = intern_str(ir, "delta");
    try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", path));
    try std.testing.expectEqual(ino1, testIno(path).?);
    {
        var b: [64]u8 = undefined;
        const n = testRead(path, &b);
        try std.testing.expectEqualStrings("alpha\nbeta\ngamma\ndelta\n", b[0..n]);
    }
}

test "incremental save: a rewritten or diverged file is never appended to" {
    const path = "/tmp/datalog_zig_u3_incr_diverge.array";
    defer _ = c.unlink(path);
    defer _ = c.unlink(path ++ ".tmp");

    try std.testing.expect(testWrite(path, "red\nblue\n"));
    const ir = intern_load(null, path) orelse return error.OutOfMemory;
    defer intern_free(ir);

    // Same SIZE, different bytes, rewritten under the live interner — the
    // divergence shape the publish stamp exists for.  Appending would leave a
    // file whose prefix disagrees with rev[], so the save must rewrite.
    try std.testing.expect(testWrite(path, "blue\nred\n"));
    const ino0 = testIno(path).?;
    _ = intern_str(ir, "green");
    try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", path));
    try std.testing.expect(testIno(path).? != ino0);
    {
        var b: [64]u8 = undefined;
        const n = testRead(path, &b);
        try std.testing.expectEqualStrings("red\nblue\ngreen\n", b[0..n]);
    }
    // An engine-rewritten (tmp+rename) file is a NEW inode, so the append state
    // is dropped with it: the next save rewrites rather than appending to an
    // inode this interner never wrote.
    try std.testing.expect(savedSlot(ir, path) != null); // the rewrite re-armed it
    _ = intern_str(ir, "yellow");
    const ino1 = testIno(path).?;
    try std.testing.expectEqual(@as(c_int, 0), intern_save(ir, "/tmp/ignored.dafsa", path));
    try std.testing.expectEqual(ino1, testIno(path).?); // appended onto OUR file
    {
        var b: [64]u8 = undefined;
        const n = testRead(path, &b);
        try std.testing.expectEqualStrings("red\nblue\ngreen\nyellow\n", b[0..n]);
    }
}
