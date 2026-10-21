//! relation.zig — port of src/relation.c (per-relation DAFSA with fixed-width
//! u32BE column encoding + per-relation WAL).
//!
//! Key encoding (the linchpin, architecture §2.3):
//!   | col0 u32BE | col1 u32BE | ... | col_{a-1} u32BE | \0 |
//!   Total: 4*a + 1 bytes, NO inter-column separators.
//!
//! Strangler-hybrid ABI: `struct relation` is OPAQUE in relation.h, so the
//! implementation is a native Zig struct; every non-static C function is an
//! `export fn` with the exact C name/signature/return semantics.  The dafsa
//! engine stays C in the hybrid .so (vendor/dafsa/*.c) and is addressed via
//! @cImport("dafsa_internal.h") — this port REACHES INTO dafsa internals
//! (struct dafsa states/initial/subtree_valid, State/Edge, trans_find) exactly
//! like the C oracle does; it does NOT reimplement them.  regex_dfa_walk and
//! symset_contains come from the ported zig/src/regexwalk.zig (U6) — imported
//! directly.  crc32_compute comes from dafsa_internal.h.
//!
//! Oracle: src/relation.c (never modified).

const std = @import("std");
const c = std.c;
const tupleset = @import("tupleset.zig");

// The dafsa engine is now Zig; dafsa_c.zig hand-declares its C-layout
// structs (struct dafsa/State/Edge/dafsa_wal) and exported ABI
// (dafsa_*/trans_find/dafsa_wal_*/rank/select/crc32) in place of the removed
// @cImport("dafsa_internal.h").
const dc = @import("dafsa_c.zig");

// libc strdup/strrchr/strndup helpers the old translate-c pulled in via
// dafsa_internal.h's transitive <string.h> include.
extern "c" fn strdup(s: [*c]const u8) [*c]u8;

// regexwalk.zig (U6): regex_dfa, sym_set, regex_dfa_walk, symset_contains.
const regexwalk = @import("regexwalk.zig");

const MAX_ARITY: usize = 8;
const MAX_KEY_LEN: usize = MAX_ARITY * 4 + 1; // 33

/// Overlay materialization threshold: adds are accumulated in the hot-set
/// (sorted-array) overlay and folded into the DAFSA at consolidation
/// (publish/compact).  Below this count the overlay IS cheaper than a
/// DAFSA rebuild even at consolidation time; above it we materialize
/// eagerly so a single huge batch does not pay sorted-array insert on a
/// set that is about to be rebuilt anyway.  NOT a correctness knob.
const REL_OVERLAY_FLUSH_AT: usize = 1 << 20;

/// struct relation — opaque to C; native Zig layout.
///
/// selfreg-dl-storage (C19): the agent-level store is APPEND-ONLY — a wrong
/// fact is corrected by SUPERSESSION (re-assert with a newer version;
/// latest-wins at read time via the set semantics of the key), never by
/// deletion.  `ov` is the hot-write overlay: a sorted-array tupleset of
/// facts added since the last consolidation.  Reads see `d` UNION `ov`
/// (pure set union — no masking, no resurrect risk, because there are no
/// tombstones).  `ov_dirty` counts unconsolidated adds.
pub const Relation = struct {
    d: [*c]dc.dafsa, // VIEW = base ∪ derived; all reads enumerate this
    base: [*c]dc.dafsa, // BASE = durable EDB facts; base == d for EDB-only
    arity: u8,
    wal: [*c]dc.dafsa_wal, // per-relation WAL handle, or NULL
    wal_path: [*c]u8, // path to WAL file (owned, strdup'd)
    dirty: c_int, // 1 if mutated since open (rel_is_dirty)
    // ─── selfreg-dl-storage overlay (beyond C's sizeof(struct relation)) ───
    ov: ?*tupleset.tuple_set = null, // hot-write overlay (append-only union)
    ov_dirty: u64 = 0, // adds since last consolidation (decay/sizing signal)
    ov_epoch: u32 = 0, // access epoch of the newest overlay fact (C20)
    // Monotonic content revision (publish gate): bumped on every mutation
    // that changes d ∪ ov or base.  `dirty` cannot serve as a publish gate
    // because it is never cleared (rel_compact leaves it set), so it says
    // "mutated at least once this process", not "changed since the last
    // snapshot".  rev==0 means "never mutated" (calloc-initialized).
    rev: u64 = 0,
    // Monotonic bytes appended to this relation's WAL since open (selfreg
    // periodic snapshot).  Never reset by rel_compact (which truncates the
    // WAL FILE but not this counter), so it measures total append traffic
    // independent of compaction — the signal for the snapshot-budget trigger.
    wal_appended_bytes: u64 = 0,
    // ─── U-homog (S2): recorded per-column value kinds ─────────────────────
    // kind[c] is the value space column c stores, RECORDED when values enter
    // the store (never inferred from content — the shared interner makes a
    // stored u32's space unobservable): 0=unknown (empty column, permissive),
    // 1=int (raw u32), 2=sym (interned id).  A column that would hold BOTH
    // is rejected LOUDLY at the insert site, so 3=mixed never gets recorded
    // (3 stays reserved for the compile-time cache join).
    //
    // Lives in the Relation tail (not dl_db's) on purpose: every eval clone
    // (rules_ro/wfs) ALIASES the relation pointers, so kinds ride
    // along into the clones that check them; a dl_db-tail field would be
    // invisible to wfs.zig's dl.DlDb clone.
    col_kind: [MAX_ARITY]u8 = @splat(0),
    // ─── read state (lazyrel + zero-copy view) ───────────────────────────────
    // `state` says how d/base are held.  It is an explicit enum, not an
    // implicit "d == NULL means not loaded", because TWO invariants ride on
    // it:
    //
    //  1. LIVE vs VERSION.  A .view handle is ALWAYS a LIVE view: it is
    //     opened by rel_open_* against the live store directory and by
    //     nothing else (relViewOpen is private to this file, and no exported
    //     function here takes a path).  VERSION reads never see a Relation at
    //     all — snapshot.zig opens a snapshot directory's file through its
    //     own vcache (view_open_cached) and walks it with view_prefix/
    //     view_rank/view_select.  So the two view species cannot be confused:
    //     the version species never reaches this struct.
    //  2. WAL.  A relation whose WAL holds committed records is NEVER
    //     .deferred or .view — recovery replays records into an in-memory
    //     DAFSA, so those open .owned (see rel_open_*).  relEnsureReadable
    //     re-checks it, so a WAL with appends can never be silently read
    //     through a plain view that would miss them.
    //
    //   .owned    — d/base are in-memory dc.dafsa (writable; also the state
    //               every mutation promotes to).
    //   .deferred — nothing read yet; on_disk_dafsa/on_disk_base name the
    //               files.  Entered by rel_open_* only when the WAL has no
    //               committed records.
    //   .view     — d/base are read through zero-copy mmap views (view_d /
    //               view_base), never parsed.  Read-only use; a write (or a
    //               read the view cannot serve) PROMOTES to .owned first.
    state: ReadState = .owned,
    view_d: ?*anyopaque = null, // dafsa_view handle for d (.view only)
    view_base: ?*anyopaque = null, // dafsa_view for base; NULL => base aliases d
    // On-disk paths, owned.  Live in .deferred (where to load FROM) and in
    // .view (where to promote from); freed on promotion to .owned.  Non-NULL
    // on_disk_base iff the relation is IDB (split base), which keeps
    // rel_is_idb exact without loading.
    on_disk_dafsa: [*c]u8 = null,
    on_disk_base: [*c]u8 = null,
};

/// See Relation.state.
pub const ReadState = enum(u8) {
    owned = 0,
    deferred = 1,
    view = 2,
};

/// Bump the content revision (every mutation site that sets dirty=1 calls
/// this too; open-time WAL replay deliberately does not — a fresh process
/// starts at rev 0 and its first publish must serialize everything).
pub export fn rel_touch(rel: ?*Relation) void {
    const r = rel orelse return;
    r.rev +%= 1;
}

// ─── rel_load cost split instrumentation (DL_REL_TIMING) ────────────────────
//
// MEASURED split of what rel_load pays for one on-disk relation, so the
// zero-copy-view decision rests on numbers rather than on the assumption that
// the parse is the term.  Zero overhead when unset (one cached getenv).
//
// Each component is timed on the SAME file in one process:
//   read_ms = open + read the whole file into a scratch chunk loop (the I/O
//             component; includes first-touch page-cache fill),
//   load_ms = dafsa_load with the file already warm (the parse + index
//             component: states[]/trans[] materialization, incoming-edge
//             rebuild, register rebuild),
//   view_ms = dafsa_view_open (mmap + header/CRC validation + state_off walk),
//   view_crc_ms = the CRC32 share of view_ms, measured separately over the
//             same bytes (the v4 trailer validates the whole file).
// parse/index = load_ms minus the read component: the read pass above is
// discarded, so what dafsa_load pays beyond the bytes IS the parse.
extern "c" fn getenv(name: [*c]const u8) ?[*:0]const u8;

const RelTimespec = extern struct {
    sec: isize,
    nsec: isize,
};
extern fn clock_gettime(clk_id: c_int, tp: *RelTimespec) c_int;
const REL_CLOCK_MONOTONIC: c_int = 1;

var rel_timing_state: i8 = -1; // -1 unknown, 0 off, 1 on

fn relTimingOn() bool {
    if (rel_timing_state < 0) {
        const v = getenv("DL_REL_TIMING");
        rel_timing_state = if (v != null and v.?[0] != 0) 1 else 0;
    }
    return rel_timing_state == 1;
}

fn relNowNs() u64 {
    var ts: RelTimespec = undefined;
    _ = clock_gettime(REL_CLOCK_MONOTONIC, &ts);
    return @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
}

fn relTimingMsg(comptime fmt: []const u8, args: anytype) void {
    var buf: [1024]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch buf[0..0];
    _ = c.write(2, msg.ptr, msg.len);
}

/// Read the whole file and discard it: the pure I/O component of a load.
fn relReadPass(path: [*c]const u8) struct { ns: u64, bytes: u64 } {
    const t0 = relNowNs();
    var total: u64 = 0;
    const fd = c.open(@ptrCast(path), .{ .ACCMODE = .RDONLY });
    if (fd >= 0) {
        var buf: [65536]u8 = undefined;
        while (true) {
            const n = c.read(fd, &buf, buf.len);
            if (n <= 0) break;
            total += @intCast(n);
        }
        _ = c.close(fd);
    }
    return .{ .ns = relNowNs() - t0, .bytes = total };
}

/// Print the split for one relation path.  Called only when DL_REL_TIMING is
/// set, immediately before the real load (so the real load is what the
/// measurement warm-up leaves behind, never a second copy of the relation).
fn relTimingSplit(path: [*c]const u8) void {
    const io = relReadPass(path);

    const t1 = relNowNs();
    const d = dc.dafsa_load(path);
    const load_ns = relNowNs() - t1;
    if (d != null) dc.dafsa_free(d);

    const t2 = relNowNs();
    const v = dc.dafsa_view_open(path);
    const view_ns = relNowNs() - t2;
    var n_states: u32 = 0;
    if (v != null) {
        const vv: [*c]const dc.dafsa_view = @ptrCast(@alignCast(v.?));
        n_states = vv.*.n_states;
        dc.dafsa_view_close(@ptrCast(v));
    }

    // CRC component of the view open: dafsa_view_open validates a v4 trailer
    // over the whole file, so this is the floor the view pays for its bytes.
    var crc_ns: u64 = 0;
    var crc_bytes: usize = 0;
    var crc_val: u32 = 0;
    if (io.bytes > 0) {
        const buf = c.malloc(@intCast(io.bytes));
        if (buf != null) {
            const b: [*]u8 = @ptrCast(buf);
            const fd = c.open(@ptrCast(path), .{ .ACCMODE = .RDONLY });
            if (fd >= 0) {
                var got: usize = 0;
                while (got < io.bytes) {
                    const n = c.read(fd, b + got, @intCast(io.bytes - got));
                    if (n <= 0) break;
                    got += @intCast(n);
                }
                _ = c.close(fd);
                crc_bytes = got;
                const t3 = relNowNs();
                // The value is REPORTED, not discarded: at ReleaseFast an
                // unused extern call's loop is optimized away and the CRC
                // timing reads 0.00 ms (observed), which would understate the
                // view open's real cost.
                crc_val = dc.crc32_compute(b, got);
                crc_ns = relNowNs() - t3;
            }
            c.free(buf);
        }
    }

    relTimingMsg("REL_TIMING {s} bytes={d} n_states={d} read_ms={d:.2} load_ms={d:.2} view_ms={d:.2} view_crc_ms={d:.2} crc_bytes={d} crc={x}\n", .{
        std.mem.span(@as([*:0]const u8, @ptrCast(path))),
        io.bytes,
        n_states,
        @as(f64, @floatFromInt(io.ns)) / 1e6,
        @as(f64, @floatFromInt(load_ns)) / 1e6,
        @as(f64, @floatFromInt(view_ns)) / 1e6,
        @as(f64, @floatFromInt(crc_ns)) / 1e6,
        crc_bytes,
        crc_val,
    });
}

// ─── Lazy on-disk load (lazyrel) + zero-copy view reads ─────────────────────
//
// The engine's per-invocation cost profile (MEASURED on the agent store):
// dl_open eagerly dafsa_load'd EVERY relation's on-disk DAFSA, so a command
// touching ONE relation paid for all of them — store B (one 20 MiB
// __postings__.dafsa) opened in 0.350 s where the same command on store A
// (everything but postings) took 0.031 s.  lazyrel deferred the read to the
// first access; zerocopy-view then made that first access cheap: it is a
// mmap'd dafsa_view, not a parse.
//
// MEASURED (DL_REL_TIMING, 20,714,186-byte __postings__.dafsa, 616,258
// states, warm page cache): read 2.0-3.2 ms, dafsa_load 255-266 ms (so the
// parse/index rebuild is ~253-263 ms of it — THE term), dafsa_view_open
// 73-87 ms (of which 41 ms is the file-wide CRC32 the v4 format validates).
// Hence: read through a view, and never parse unless something writes.
//
// A relation is still loaded EAGERLY (owned) when its WAL holds committed
// records, because recovery at open must replay into the in-memory DAFSA.

/// Bring the relation to .owned (materialize from disk if it is .deferred or
/// .view).  This is the gate for every WRITE, for WAL replay, and for the
/// reads a view cannot serve (pointer escapes, regex walk, stats, save).
/// No-op (true) when already owned; false only on load/OOM failure.
///
/// Promotion is NOT a mutation: the materialized DAFSA has exactly the content
/// the view had, so `dirty` and `rev` are left untouched (a read-only process
/// that promotes still publishes nothing).
fn relEnsureOwned(r: *Relation) bool {
    if (r.state == .owned) return true;

    if (relTimingOn()) relTimingSplit(r.on_disk_dafsa);

    // Drop any live view first: the DAFSA below is the source of truth from
    // here on, and the mapping must not outlive the state that explains it.
    relViewClose(r);

    r.d = dc.dafsa_load(r.on_disk_dafsa);
    if (r.d == null) {
        r.d = dc.dafsa_create();
        if (r.d == null) return false;
    }
    if (r.on_disk_base != null) {
        r.base = dc.dafsa_load(r.on_disk_base);
        if (r.base == null) {
            r.base = dc.dafsa_create();
            if (r.base == null) {
                dc.dafsa_free(r.d);
                r.d = null;
                return false;
            }
        }
    } else {
        r.base = r.d; // EDB-only: base aliases view
    }

    r.state = .owned;
    if (r.on_disk_dafsa) |p| c.free(@ptrCast(p));
    r.on_disk_dafsa = null;
    if (r.on_disk_base) |p| c.free(@ptrCast(p));
    r.on_disk_base = null;
    return true;
}

/// Read gate: make the relation readable, preferring the zero-copy VIEW.
/// .owned and .view are already readable; .deferred opens the view, falling
/// back to a full load when the file cannot be viewed (missing/empty file,
/// pre-v3 format, CRC mismatch) — the read then answers exactly as before.
///
/// WAL safety (see Relation.state): a relation with committed WAL records is
/// never viewed.  rel_open_* already opens those .owned, so this is the
/// belt-and-braces check that keeps the invariant true even if a caller ever
/// appends to the WAL of a .view relation.
fn relEnsureReadable(r: *Relation) bool {
    if (r.state != .deferred) return true;
    if (r.wal != null and r.wal.*.size > 16) return relEnsureOwned(r);
    // Unconsolidated overlay facts are not in the file: a view would miss
    // them, so materialize instead (rel_overlay_add promotes anyway; this is
    // the guard that makes the union contract independent of call order).
    if (!viewOverlayClean(r)) return relEnsureOwned(r);
    // Instrumentation hook: report what a PARSE would have cost here versus
    // what the view costs (the same split relEnsureOwned prints).
    if (relTimingOn()) relTimingSplit(r.on_disk_dafsa);
    if (relViewOpen(r)) return true;
    return relEnsureOwned(r);
}

/// Open the live zero-copy view(s).  PRIVATE ON PURPOSE: this is the only
/// place a Relation enters .view, and every caller is a read path reached from
/// rel_open_* on the live store directory — so a .view handle is always a LIVE
/// view, never a snapshot's (see Relation.state, invariant 1).
fn relViewOpen(r: *Relation) bool {
    std.debug.assert(r.state == .deferred);
    // Invariant 2 (see Relation.state): a plain view over a WAL that holds
    // records would silently miss those appends.
    std.debug.assert(r.wal == null or r.wal.*.size <= 16);
    const v = dc.dafsa_view_open(r.on_disk_dafsa) orelse return false;
    if (r.on_disk_base != null) {
        const vb = dc.dafsa_view_open(r.on_disk_base) orelse {
            dc.dafsa_view_close(v);
            return false;
        };
        r.view_base = vb;
    }
    r.view_d = v;
    r.state = .view;
    return true;
}

/// Close the live view(s), if any (promotion, teardown).
fn relViewClose(r: *Relation) void {
    if (r.view_base) |vb| {
        dc.dafsa_view_close(vb);
        r.view_base = null;
    }
    if (r.view_d) |v| {
        dc.dafsa_view_close(v);
        r.view_d = null;
    }
}

/// The view handle a READ should use: view_d, or view_base for a base read
/// when the relation is EDB-only (where base aliases the view).
inline fn viewOf(r: *const Relation, base: bool) [*c]const dc.dafsa_view {
    std.debug.assert(r.state == .view);
    const h = if (base) (r.view_base orelse r.view_d) else r.view_d;
    // Checked in every build mode: a .view state without a handle would
    // otherwise be a silent null deref (UB under ReleaseFast).
    const hh = h orelse @panic("relation in .view state without a view handle");
    return @ptrCast(@alignCast(hh));
}

/// A view read must see the SAME set an owned read would: d ∪ overlay.  In
/// .view state the overlay is provably empty (rel_overlay_add promotes first,
/// and a fresh process starts with no overlay), so the union is d alone.
inline fn viewOverlayClean(r: *const Relation) bool {
    return r.ov == null or r.ov.?.count == 0;
}

// ─── view read primitives (mirror the dafsa_* ones above) ──────────────────

fn viewExactD(r: *const Relation, base: bool, cols: ?[*]const u32) c_int {
    if (cols == null) return 0;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, r.arity);
    return dc.dafsa_view_lookup_n(@ptrCast(viewOf(r, base)), &key, key_len);
}

const ViewPrefixCtx = struct {
    leading: ?[*]const u32,
    k: u8,
    arity: u8,
    cb: RelEnumCb,
    user: ?*anyopaque,
    count: c_long,
};

/// view_enum_dfs callback: reassemble the full tuple and forward it.  Same
/// contract as prefixDfs's final-state body (the payload is the remaining
/// columns + the trailing \0, which readColsBe does not read).
fn viewPrefixCb(payload: [*c]const u8, payload_len: usize, user: ?*anyopaque) callconv(.c) c_int {
    _ = payload_len;
    const ctx: *ViewPrefixCtx = @ptrCast(@alignCast(user orelse return 1));
    const n_rem: u8 = ctx.arity - ctx.k;
    var full_tuple: [MAX_ARITY]u32 = undefined;
    var i: u8 = 0;
    while (i < ctx.k) : (i += 1) full_tuple[i] = ctx.leading.?[i];
    if (n_rem > 0) readColsBe(full_tuple[ctx.k..].ptr, payload, n_rem);
    ctx.count +%= 1;
    return ctx.cb.?(&full_tuple, ctx.arity, ctx.user);
}

/// Zero-copy rel_prefix: walk the k*4 prefix bytes through view_trans_find,
/// then DFS from the endpoint with view_enum_dfs.  Deliberately NOT
/// dafsa_view_prefix_enum (the W\0 trap: that function requires a 0x00 edge
/// before the DFS) — exactly the shape snapshot.zig's view_prefix uses.
fn viewPrefixD(r: *const Relation, base: bool, leading: ?[*]const u32, k: u8, cb: RelEnumCb, user: ?*anyopaque) c_long {
    if (cb == null) return -1;
    if (k > r.arity) return -1;
    if (k > 0 and leading == null) return -1;

    const v = viewOf(r, base);
    var current: c_uint = v.*.initial;
    var i: u8 = 0;
    while (i < k) : (i += 1) {
        const val = leading.?[i];
        const col_be = [4]u8{
            @truncate(val >> 24),
            @truncate(val >> 16),
            @truncate(val >> 8),
            @truncate(val),
        };
        var b: usize = 0;
        while (b < 4) : (b += 1) {
            var target: c_uint = undefined;
            if (dc.view_trans_find(v, current, col_be[b], &target) != 0) return 0;
            current = target;
        }
    }

    var ctx = ViewPrefixCtx{
        .leading = leading,
        .k = k,
        .arity = r.arity,
        .cb = cb,
        .user = user,
        .count = 0,
    };
    var buf: [MAX_KEY_LEN]u8 = undefined;
    _ = dc.view_enum_dfs(v, current, &buf, 0, viewPrefixCb, &ctx, &ctx.count);
    return ctx.count;
}

/// Final-STATE count (what dafsa_stats reports as n_final), read straight off
/// the view's bitmap: dafsa_view_open already validated it against the header.
fn viewFinalCount(v: [*c]const dc.dafsa_view) u64 {
    const bytes = (@as(usize, v.*.n_states) + 8) / 8;
    var n: u64 = 0;
    var i: usize = 0;
    while (i < bytes) : (i += 1) n += @popCount(v.*.final_bits[i]);
    return n;
}

/// int rel_is_loaded(const relation *rel)
/// 1 if the relation is OWNED (its DAFSA is materialized in memory, or it is
/// in-memory-only), 0 otherwise.  A .view relation is readable but NOT loaded:
/// that distinction is the whole point of the view, so it is what this
/// predicate reports.  Use rel_read_state for the three-way answer.
pub export fn rel_is_loaded(rel: ?*const Relation) c_int {
    const r = rel orelse return 0;
    return if (r.state == .owned) 1 else 0;
}

/// uint8_t rel_read_state(const relation *rel) — 0=owned, 1=deferred,
/// 2=view (ReadState).  The zerocopy test surface.
pub export fn rel_read_state(rel: ?*const Relation) u8 {
    const r = rel orelse return @intFromEnum(ReadState.owned);
    return @intFromEnum(r.state);
}

/// Current content revision (publish-gate read side).
pub export fn rel_rev(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    return r.rev;
}

/// Monotonic WAL-append byte count since open (snapshot-budget signal).
/// Survives rel_compact's ftruncate (which resets the WAL FILE size) so the
/// snapshot trigger sees total append traffic, not the current file length.
pub export fn rel_wal_appended(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    return r.wal_appended_bytes;
}

// ─── U-homog (S2): recorded per-column kinds ───────────────────────────────

/// Column kind values (see Relation.col_kind).
pub const KIND_UNKNOWN: u8 = 0;
pub const KIND_INT: u8 = 1;
pub const KIND_SYM: u8 = 2;

/// uint8_t rel_col_kind(const relation *rel, uint8_t col)
/// The RECORDED kind of column `col` (0=unknown, 1=int, 2=sym).  Content is
/// NEVER inferred here — see the col_kind field comment.
pub export fn rel_col_kind(rel: ?*const Relation, col: u8) u8 {
    const r = rel orelse return KIND_UNKNOWN;
    if (col >= MAX_ARITY) return KIND_UNKNOWN;
    return r.col_kind[col];
}

/// void rel_col_kind_set(relation *rel, uint8_t col, uint8_t kind)
/// Directly set the RECORDED kind of column `col`.  Used ONLY by the G1
/// populate-after-load rollback to restore col_kind to its pre-load state
/// (kind 0 / UNKNOWN) after a rejected recheck — NOT a general mutator.
pub export fn rel_col_kind_set(rel: ?*Relation, col: u8, kind: u8) void {
    const r = rel orelse return;
    if (col >= MAX_ARITY) return;
    r.col_kind[col] = kind;
}

/// int rel_kind_note(relation *rel, uint8_t col, uint8_t kind)
/// Record/fold a kind into column `col`: unknown folds away, a matching kind
/// is idempotent, and a CONFLICTING kind is refused (-1) WITHOUT mutating —
/// the caller turns that into the loud insert-time rejection.  kind 3 is not
/// accepted (mixed is a reject, not a state).
pub export fn rel_kind_note(rel: ?*Relation, col: u8, kind: u8) c_int {
    const r = rel orelse return -1;
    if (col >= MAX_ARITY or kind == 0 or kind > KIND_SYM) return -1;
    const cur = r.col_kind[col];
    if (cur == KIND_UNKNOWN) {
        r.col_kind[col] = kind;
        return 0;
    }
    return if (cur == kind) 0 else -1;
}

/// typedef int (*rel_enum_cb)(const uint32_t *cols, uint8_t arity, void *user)
pub const RelEnumCb = ?*const fn (cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int;

/// regexwalk.h types, re-exported for vrelation.zig (single source of truth so
/// the two modules share the exact same struct identity).
pub const regex_dfa = regexwalk.regex_dfa;
pub const sym_set = regexwalk.sym_set;

// ─── Key encoding helpers ─────────────────────────────────────────────────

/// trans_arr_c from dafsa_internal.h, reimplemented locally: the translate-c
/// output for the static-inline accessor fails to compile under Zig 0.16
/// (@ptrCast discarding const).  Byte-for-byte identical logic:
///   s->trans_heap ? s->trans_heap->edges : s->trans
fn transArrC(s: [*c]const dc.State) [*c]const dc.Edge {
    const heap = s.*.trans_heap;
    if (heap != null) {
        return @ptrCast(@alignCast(&heap.*._edges));
    }
    return @ptrCast(&s.*.trans);
}

/// Write arity u32 columns as big-endian into buf (4*arity bytes).
/// Does NOT write the trailing \0 — caller must add it.
fn writeColsBe(buf: [*]u8, cols: [*]const u32, arity: u8) void {
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        const v = cols[i];
        buf[4 * @as(usize, i)] = @truncate(v >> 24);
        buf[4 * @as(usize, i) + 1] = @truncate(v >> 16);
        buf[4 * @as(usize, i) + 2] = @truncate(v >> 8);
        buf[4 * @as(usize, i) + 3] = @truncate(v);
    }
}

/// Read arity u32 columns from big-endian buf.
fn readColsBe(cols: [*]u32, buf: [*]const u8, arity: u8) void {
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        cols[i] = (@as(u32, buf[4 * @as(usize, i)]) << 24) |
            (@as(u32, buf[4 * @as(usize, i) + 1]) << 16) |
            (@as(u32, buf[4 * @as(usize, i) + 2]) << 8) |
            (@as(u32, buf[4 * @as(usize, i) + 3]));
    }
}

/// Build the full key (4*arity + 1 bytes) in buf.  buf must be >= MAX_KEY_LEN.
fn encodeKey(buf: [*]u8, cols: [*]const u32, arity: u8) usize {
    writeColsBe(buf, cols, arity);
    buf[4 * @as(usize, arity)] = 0x00; // trailing guard
    return 4 * @as(usize, arity) + 1;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

/// relation *rel_create(uint8_t arity)
pub export fn rel_create(arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);

    rel.d = dc.dafsa_create();
    if (rel.d == null) {
        c.free(mem);
        return null;
    }

    rel.base = rel.d; // EDB-only: base aliases view
    rel.arity = arity;
    return rel;
}

/// relation *rel_open(const char *path, uint8_t arity)
/// No WAL — nothing to recover — so the on-disk DAFSA is ALWAYS lazy here:
/// the read is deferred to the first touch (relEnsureReadable / relEnsureOwned).
pub export fn rel_open(path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);

    rel.state = .deferred;
    rel.on_disk_dafsa = strdup(path);
    if (rel.on_disk_dafsa == null) {
        c.free(mem);
        return null;
    }

    rel.arity = arity;
    return rel;
}

// ─── Hot-write overlay (selfreg-dl-storage, C19 append-only) ───────────────

/// Lazily create the overlay tupleset.  Returns null on OOM.
fn ovEnsure(rel: *Relation) ?*tupleset.tuple_set {
    if (rel.ov == null) {
        const m = c.malloc(@sizeOf(tupleset.tuple_set)) orelse return null;
        const ts: *tupleset.tuple_set = @ptrCast(@alignCast(m));
        if (tupleset.ts_init(ts, rel.arity) != 0) {
            c.free(m);
            return null;
        }
        rel.ov = ts;
    }
    return rel.ov;
}

/// Append a fact to the overlay WITHOUT touching the DAFSA (O(1) amortized
/// hash insert).  Returns 1 added / 0 duplicate (already in DAFSA or
/// overlay) / -1 error.  This is the hot-write path: the fact's durability
/// record is the caller's WAL append (fsync'd at the next cycle boundary —
/// selfreg-batched-fsync); reads see it through rel_exact/rel_prefix
/// union; consolidation (rel_overlay_flush) folds it into the DAFSA.
pub export fn rel_overlay_add(rel: ?*Relation, cols: [*c]const u32) c_int {
    const r = rel orelse return -1;
    if (cols == null) return -1;
    // Load before the duplicate check: the overlay's dedup is against the
    // DAFSA view, so an unloaded relation must be read first (lazyrel).
    if (!relEnsureOwned(r)) return -1;

    // duplicate against the DAFSA view: overlay adds nothing
    if (relExactD(r, r.d, cols) != 0) return 0;

    const ov = ovEnsure(r) orelse return -1;
    const rc = tupleset.ts_add(ov, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        r.ov_dirty += 1;
        r.dirty = 1; // close-time compaction contract (same as rel_add_base)
        r.rev +%= 1;
    }
    return rc;
}

/// Record the access epoch on the overlay (C20 decay bookkeeping; epoch
/// granularity, never a per-access write).
pub export fn rel_overlay_touch(rel: ?*Relation, epoch: u32) void {
    const r = rel orelse return;
    if (epoch > r.ov_epoch) r.ov_epoch = epoch;
}

/// Current overlay occupancy (facts pending consolidation).
pub export fn rel_overlay_count(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    const ov = r.ov orelse return 0;
    return @intCast(ov.count);
}

/// Incrementally insert one fact into a DAFSA (4*arity+1-byte key).  The
/// engine is the Carrasco & Forcada incremental construction: dafsa_add_n
/// maintains minimality in place at O(key) — the same per-fact path rel_add
/// (above) and WAL replay (below) already drive on live DAFSAs, so a DAFSA
/// built by dafsa_build_sorted and one grown by dafsa_add_n are the same
/// object.  Returns 1 added / 0 already present / -1 error.
fn ovFoldInto(d: [*c]dc.dafsa, key: [*]const u8, key_len: usize) c_int {
    return dc.dafsa_add_n(d, key, key_len);
}

/// Fold the overlay into the DAFSA and drop it (consolidation step).
/// The overlay is a PURE SET UNION over d (C19 append-only), so folding is
/// dafsa_add_n per overlay fact — O(overlay), in place, minimality kept —
/// instead of the enumerate d ∪ ov + ts_sort + dafsa_build_sorted full
/// rebuild, which paid O(store) on EVERY consolidation regardless of delta
/// size.  On error the DAFSAs are left with a PREFIX of the overlay folded
/// in: the union read is unaffected (every fact is still in d ∪ ov, no
/// masking), the fold is idempotent (already-present keys return 0), and
/// `ov` is only cleared after the fold fully succeeds so a retry re-folds
/// exactly the remaining facts.  Returns 0 ok / -1 error.
pub export fn rel_overlay_flush(rel: ?*Relation) c_int {
    const r = rel orelse return -1;
    const ov = r.ov orelse return 0;
    if (ov.count == 0) {
        tupleset.ts_reset(ov);
        return 0;
    }
    // Folding reads AND writes the DAFSA view/base: load first (lazyrel).
    if (!relEnsureOwned(r)) return -1;

    var i: c_long = 0;
    var rc: c_int = 0;
    while (i < ov.count) : (i += 1) {
        var key: [MAX_KEY_LEN]u8 = undefined;
        const key_len = encodeKey(&key, ov.data.? + @as(usize, @intCast(i)) * r.arity, r.arity);
        rc = ovFoldInto(r.d, &key, key_len);
        if (rc < 0) break;
        // Overlay facts are EDB premises — they must survive a view reset —
        // so a split (IDB) relation folds them into the BASE too.  The view
        // already contains them; the base fold failing leaves d complete and
        // the overlay intact (retry below at the next consolidation).
        if (r.base != r.d and ovFoldInto(r.base, &key, key_len) < 0) {
            rc = -1;
            break;
        }
    }

    if (rc < 0) return -1; // partial fold: overlay kept, reads still complete

    r.dirty = 1;
    r.rev +%= 1;
    tupleset.ts_reset(ov); // capacity kept; hash cleared
    r.ov_dirty = 0;
    return 0;
}

/// Read-side consolidation gate (selfreg-dl-storage C18): enumeration and
/// order-statistics reads walk the DAFSA, so they must see d ∪ ov.  Rather
/// than merging two unsorted sources per read, a read through a non-empty
/// overlay folds it into the DAFSA once — the same consolidation step
/// publish performs, pulled forward to the first reader.  No-op when the
/// overlay is empty or already materialized; a flush failure leaves the
/// relation unchanged (readers then see the pre-overlay state, never a
/// torn one).
fn ovConsolidateForRead(r: *const Relation) void {
    const ov = r.ov orelse return;
    if (ov.count == 0) return;
    _ = rel_overlay_flush(@constCast(r));
}

/// Free the overlay (relation teardown).  Non-consolidated facts MUST have
/// been flushed (or are still recorded in the WAL — synced at the cycle
/// boundary — by the caller).
fn ovFree(rel: *Relation) void {
    if (rel.ov) |ov| {
        tupleset.ts_free(ov);
        c.free(ov);
        rel.ov = null;
    }
    rel.ov_dirty = 0;
}


/// int rel_save(const relation *rel, const char *path)
pub export fn rel_save(rel: ?*const Relation, path: [*c]const u8) c_int {
    const r = rel orelse return -1;
    if (path == null) return -1;
    if (!relEnsureOwned(@constCast(r))) return -1;
    return dc.dafsa_save(r.d, path);
}

/// int rel_save_base(const relation *rel, const char *path)
pub export fn rel_save_base(rel: ?*const Relation, path: [*c]const u8) c_int {
    const r = rel orelse return -1;
    if (path == null) return -1;
    if (!relEnsureOwned(@constCast(r))) return -1;
    return dc.dafsa_save(r.base, path);
}

/// void rel_free(relation *rel)
pub export fn rel_free(rel: ?*Relation) void {
    const r = rel orelse return;
    ovFree(r);
    if (r.state == .owned) {
        if (r.base != r.d) dc.dafsa_free(r.base);
        dc.dafsa_free(r.d);
    } else {
        relViewClose(r); // .view: unmap; .deferred: nothing mapped
    }
    if (r.on_disk_dafsa != null) c.free(@ptrCast(r.on_disk_dafsa));
    if (r.on_disk_base != null) c.free(@ptrCast(r.on_disk_base));
    if (r.wal != null) dc.dafsa_wal_close(r.wal);
    if (r.wal_path != null) c.free(r.wal_path);
    c.free(@ptrCast(r));
}

/// uint8_t rel_arity(const relation *rel)
pub export fn rel_arity(rel: ?*const Relation) u8 {
    const r = rel orelse return 0;
    return r.arity;
}

/// uint64_t rel_count(const relation *rel)
/// Word count = DAFSA words + overlay tuples (C19 union; no double count —
/// rel_overlay_add rejects DAFSA duplicates).
pub export fn rel_count(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    if (!relEnsureReadable(@constCast(r))) return 0;
    if (r.state == .view) {
        std.debug.assert(viewOverlayClean(r));
        var vn: u64 = viewFinalCount(viewOf(r, false));
        if (r.ov) |ov| vn +%= @as(u64, @intCast(ov.count));
        return vn;
    }
    var n: u64 = 0;
    if (r.d != null) {
        var st: dc.dafsa_stats_out = std.mem.zeroes(dc.dafsa_stats_out);
        dc.dafsa_stats(r.d, &st);
        n = @as(u64, st.n_final);
    }
    if (r.ov) |ov| n +%= @as(u64, @intCast(ov.count));
    return n;
}

/// const dafsa *rel_dafsa(const relation *rel)
/// Read-side consolidation gate: iterator opens (dl_iter_open,
/// dl_iter_open_live -> OP_RANGE, merge-join) borrow rel->d and DFS it, so
/// the overlay must be folded in before the pointer escapes.
/// The escaped pointer must be a real struct dafsa, so a .view relation is
/// PROMOTED here (the one read class the zero-copy view cannot serve).
pub export fn rel_dafsa(rel: ?*const Relation) [*c]const dc.dafsa {
    const r = rel orelse return null;
    if (!relEnsureOwned(@constCast(r))) return null;
    ovConsolidateForRead(r);
    return r.d;
}

// ─── Order statistics (Tier-2) ────────────────────────────────────────────

/// uint64_t rel_rank(const relation *rel, const uint32_t *cols)
pub export fn rel_rank(rel: ?*const Relation, cols: ?[*]const u32) u64 {
    const r = rel orelse return std.math.maxInt(u64);
    if (cols == null) return std.math.maxInt(u64);
    if (!relEnsureReadable(@constCast(r))) return std.math.maxInt(u64);
    if (r.state == .view) {
        std.debug.assert(viewOverlayClean(r));
        var vkey: [MAX_KEY_LEN]u8 = undefined;
        const vlen = encodeKey(&vkey, cols.?, r.arity);
        return dc.dafsa_view_rank_n(viewOf(r, false), &vkey, vlen);
    }
    if (r.d == null) return std.math.maxInt(u64); // OOM during the lazy load
    ovConsolidateForRead(r);
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, r.arity);
    return dc.dafsa_rank_n(r.d, &key, key_len);
}

/// int rel_select(const relation *rel, uint64_t k, uint32_t *cols_out)
pub export fn rel_select(rel: ?*const Relation, k: u64, cols_out: ?[*]u32) c_int {
    const r = rel orelse return -1;
    if (cols_out == null) return -1;
    if (!relEnsureReadable(@constCast(r))) return -1;
    if (r.state == .view) {
        std.debug.assert(viewOverlayClean(r));
        var vkey: [MAX_KEY_LEN]u8 = undefined;
        const vlen = dc.dafsa_view_select_n(viewOf(r, false), k, &vkey, MAX_KEY_LEN);
        if (vlen < 0) return -1;
        readColsBe(cols_out.?, &vkey, r.arity);
        return 0;
    }
    if (r.d == null) return -1; // OOM during the lazy load
    ovConsolidateForRead(r);
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = dc.dafsa_select_n(r.d, k, &key, MAX_KEY_LEN);
    if (key_len < 0) return -1;
    readColsBe(cols_out.?, &key, r.arity);
    return 0;
}

/// uint64_t rel_range_count(const relation *rel, const uint32_t *lo,
///                          const uint32_t *hi)
pub export fn rel_range_count(rel: ?*const Relation, lo: ?[*]const u32, hi: ?[*]const u32) u64 {
    const r = rel orelse return std.math.maxInt(u64);
    if (lo == null or hi == null) return std.math.maxInt(u64);
    if (!relEnsureReadable(@constCast(r))) return std.math.maxInt(u64);
    if (r.state == .view) {
        std.debug.assert(viewOverlayClean(r));
        var vlo: [MAX_KEY_LEN]u8 = undefined;
        var vhi: [MAX_KEY_LEN]u8 = undefined;
        const vlo_len = encodeKey(&vlo, lo.?, r.arity);
        const vhi_len = encodeKey(&vhi, hi.?, r.arity);
        return dc.dafsa_view_range_count_n(viewOf(r, false), &vlo, vlo_len, &vhi, vhi_len);
    }
    if (r.d == null) return std.math.maxInt(u64); // OOM during the lazy load
    ovConsolidateForRead(r);
    var lo_key: [MAX_KEY_LEN]u8 = undefined;
    var hi_key: [MAX_KEY_LEN]u8 = undefined;
    const lo_len = encodeKey(&lo_key, lo.?, r.arity);
    const hi_len = encodeKey(&hi_key, hi.?, r.arity);
    return dc.dafsa_range_count_n(r.d, &lo_key, lo_len, &hi_key, hi_len);
}

/// uint64_t rel_count_subtree(const relation *rel)
/// Word count via subtree counts + overlay occupancy (C19 union).
pub export fn rel_count_subtree(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    if (!relEnsureReadable(@constCast(r))) return 0;
    if (r.state == .view) {
        std.debug.assert(viewOverlayClean(r));
        // Subtree counts are computed on demand into a fresh array (the view
        // is immutable); the caller owns nothing, so free it here.
        var counts: [*c]u64 = null;
        var vn: u64 = dc.dafsa_view_subtree_counts(viewOf(r, false), &counts);
        c.free(@ptrCast(counts));
        if (r.ov) |ov| vn +%= @as(u64, @intCast(ov.count));
        return vn;
    }
    var n: u64 = 0;
    if (r.d != null) {
        n = dc.dafsa_ensure_subtree(r.d);
        if (r.d.*.subtree_valid == 0) n = 0; // OOM during build: degrade to 0
    }
    if (r.ov) |ov| n +%= @as(u64, @intCast(ov.count));
    return n;
}

// ─── Prefix-bound order statistics (Tier-2 follow-up #1) ──────────────────

/// int rel_prefix_state(const relation *rel, const uint32_t *leading,
///                      uint8_t k)
/// Walk the k*4-byte prefix of `leading` (u32BE each) from the root via
/// trans_find to the DAFSA state representing that bound.  Returns the state
/// index, or -1 if the prefix is not a prefix of any key.
pub export fn rel_prefix_state(rel: ?*const Relation, leading: ?[*]const u32, k: u8) c_int {
    const r = rel orelse return -1;
    if (r.arity == 0) return -1;
    // A bound read continues from a DAFSA STATE id through dafsa_rank_from /
    // dafsa_select_from / dafsa_range_count_from, and the view exports no
    // from-state order statistics — so this family PROMOTES.  (The state ids
    // are the same numbering in both, but the from-state walks are not.)
    if (!relEnsureOwned(@constCast(r))) return -1;
    if (r.d == null) return -1; // OOM during the lazy load
    ovConsolidateForRead(r);
    var current: c_uint = r.d.*.initial;
    var i: u8 = 0;
    while (i < k) : (i += 1) {
        const v = leading.?[i];
        var col_be: [4]u8 = undefined;
        col_be[0] = @truncate(v >> 24);
        col_be[1] = @truncate(v >> 16);
        col_be[2] = @truncate(v >> 8);
        col_be[3] = @truncate(v);
        var b: u8 = 0;
        while (b < 4) : (b += 1) {
            const tr = dc.trans_find(r.d.*.states + current, col_be[b]);
            if (tr < 0) return -1; // bound prefix not present in any key
            current = (transArrC(r.d.*.states + current) + @as(usize, @intCast(tr))).*.target;
        }
    }
    return @intCast(current);
}

/// uint64_t rel_rank_bound(const relation *rel, const uint32_t *leading,
///                         uint8_t k, const uint32_t *cols)
pub export fn rel_rank_bound(rel: ?*const Relation, leading: ?[*]const u32, k: u8, cols: ?[*]const u32) u64 {
    const r = rel orelse return std.math.maxInt(u64);
    if (cols == null) return std.math.maxInt(u64);
    if (!relEnsureOwned(@constCast(r))) return std.math.maxInt(u64);
    if (r.d == null) return std.math.maxInt(u64); // OOM during the lazy load
    if (k > r.arity) return std.math.maxInt(u64);
    if (k > 0 and leading == null) return std.math.maxInt(u64);
    const s = rel_prefix_state(r, leading, k);
    if (s < 0) return 0; // bound absent: rank 0 within the (empty) bound
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.? + k, @intCast(r.arity - k));
    return dc.dafsa_rank_from(r.d, @intCast(s), &key, key_len);
}

/// int rel_select_bound(const relation *rel, const uint32_t *leading,
///                      uint8_t k, uint64_t idx, uint32_t *cols_out)
pub export fn rel_select_bound(rel: ?*const Relation, leading: ?[*]const u32, k: u8, idx: u64, cols_out: ?[*]u32) c_int {
    const r = rel orelse return -1;
    if (cols_out == null) return -1;
    if (!relEnsureOwned(@constCast(r))) return -1;
    if (r.d == null) return -1; // OOM during the lazy load
    if (k > r.arity) return -1;
    if (k > 0 and leading == null) return -1;
    const s = rel_prefix_state(r, leading, k);
    if (s < 0) return -1; // bound absent
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = dc.dafsa_select_from(r.d, @intCast(s), idx, &key, MAX_KEY_LEN);
    if (key_len < 0) return -1;
    if (k > 0) {
        @memcpy(cols_out.?[0..k], leading.?[0..k]);
    }
    readColsBe(cols_out.? + k, &key, @intCast(r.arity - k));
    return 0;
}

/// uint64_t rel_range_count_bound(const relation *rel, const uint32_t *leading,
///                                uint8_t k, const uint32_t *lo,
///                                const uint32_t *hi)
pub export fn rel_range_count_bound(rel: ?*const Relation, leading: ?[*]const u32, k: u8, lo: ?[*]const u32, hi: ?[*]const u32) u64 {
    const r = rel orelse return std.math.maxInt(u64);
    if (lo == null or hi == null) return std.math.maxInt(u64);
    if (!relEnsureOwned(@constCast(r))) return std.math.maxInt(u64);
    if (r.d == null) return std.math.maxInt(u64); // OOM during the lazy load
    if (k > r.arity) return std.math.maxInt(u64);
    if (k > 0 and leading == null) return std.math.maxInt(u64);
    const s = rel_prefix_state(r, leading, k);
    if (s < 0) return 0; // bound absent
    var lo_key: [MAX_KEY_LEN]u8 = undefined;
    var hi_key: [MAX_KEY_LEN]u8 = undefined;
    const lo_len = encodeKey(&lo_key, lo.? + k, @intCast(r.arity - k));
    const hi_len = encodeKey(&hi_key, hi.? + k, @intCast(r.arity - k));
    return dc.dafsa_range_count_from(r.d, @intCast(s), &lo_key, lo_len, &hi_key, hi_len);
}

// ─── Fact operations ──────────────────────────────────────────────────────

fn relAddD(rel: *Relation, d: [*c]dc.dafsa, cols: ?[*]const u32) c_int {
    if (d == null or cols == null) return -1;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, rel.arity);
    return dc.dafsa_add_n(d, &key, key_len);
}

/// int rel_add(relation *rel, const uint32_t *cols)
pub export fn rel_add(rel: ?*Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return -1;
    if (!relEnsureOwned(r)) return -1;
    const rc = relAddD(r, r.d, cols);
    if (rc > 0) {
        r.dirty = 1;
        r.rev +%= 1;
    }
    return rc;
}

/// int rel_add_base(relation *rel, const uint32_t *cols)
pub export fn rel_add_base(rel: ?*Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return -1;
    if (!relEnsureOwned(r)) return -1;
    const rc = relAddD(r, r.base, cols);
    if (rc > 0) {
        r.dirty = 1;
        r.rev +%= 1;
    }
    return rc;
}

fn relExactD(rel: *const Relation, d: [*c]const dc.dafsa, cols: ?[*]const u32) c_int {
    if (d == null or cols == null) return 0;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, rel.arity);
    return dc.dafsa_lookup_n(d, &key, key_len);
}

/// int rel_exact(const relation *rel, const uint32_t *cols)
/// VIEW = DAFSA ∪ overlay (C19: append-only set union).
pub export fn rel_exact(rel: ?*const Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return 0;
    if (!relEnsureReadable(@constCast(r))) return 0;
    if (r.state == .view) {
        if (viewExactD(r, false, cols) != 0) return 1;
    } else if (relExactD(r, r.d, cols) != 0) return 1;
    if (r.ov) |ov| {
        if (ov.count > 0 and tupleset.ts_contains(ov, cols) != 0) return 1;
    }
    return 0;
}

/// int rel_exact_base(const relation *rel, const uint32_t *cols)
pub export fn rel_exact_base(rel: ?*const Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return 0;
    if (!relEnsureReadable(@constCast(r))) return 0;
    if (r.state == .view) return viewExactD(r, true, cols);
    return relExactD(r, r.base, cols);
}

fn relDeleteD(rel: *Relation, d: [*c]dc.dafsa, cols: ?[*]const u32) c_int {
    if (d == null or cols == null) return -1;
    var key: [MAX_KEY_LEN]u8 = undefined;
    const key_len = encodeKey(&key, cols.?, rel.arity);
    return dc.dafsa_delete_n(d, &key, key_len);
}

/// int rel_delete(relation *rel, const uint32_t *cols)
pub export fn rel_delete(rel: ?*Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return -1;
    if (!relEnsureOwned(r)) return -1;
    const rc = relDeleteD(r, r.d, cols);
    if (rc > 0) {
        r.dirty = 1;
        r.rev +%= 1;
    }
    return rc;
}

/// int rel_delete_base(relation *rel, const uint32_t *cols)
pub export fn rel_delete_base(rel: ?*Relation, cols: ?[*]const u32) c_int {
    const r = rel orelse return -1;
    if (!relEnsureOwned(r)) return -1;
    const rc = relDeleteD(r, r.base, cols);
    if (rc > 0) {
        r.dirty = 1;
        r.rev +%= 1;
    }
    return rc;
}

// ─── Prefix enumeration ───────────────────────────────────────────────────

/// Context passed through the DFS recursion
const PrefixCtx = struct {
    leading: ?[*]const u32, // bound leading column values
    k: u8, // how many leading columns are bound
    arity: u8, // total arity of the relation
    cb: RelEnumCb, // user callback
    user: ?*anyopaque,
    count: c_long, // running count
};

/// Recursive DFS from `state`, accumulating bytes into `buf`.
/// At each final state, reconstruct the full tuple and call the user cb.
/// Returns non-zero to stop early (propagated from cb).
fn prefixDfs(d: [*c]const dc.dafsa, state: c_uint, buf: [*]u8, depth: usize, ctx: *PrefixCtx) c_int {
    const s = d.*.states + state;
    if (s.*.is_final != 0) {
        // Payload is `depth` bytes: the remaining columns + trailing \0.
        const n_rem: u8 = ctx.arity - ctx.k;
        var full_tuple: [MAX_ARITY]u32 = undefined;

        // Copy leading columns
        var i: u8 = 0;
        while (i < ctx.k) : (i += 1) {
            full_tuple[i] = ctx.leading.?[i];
        }

        // Parse remaining columns from buf (skip trailing \0)
        if (n_rem > 0 and depth >= @as(usize, n_rem) * 4) {
            readColsBe(full_tuple[ctx.k..].ptr, buf, n_rem);
        }

        ctx.count +%= 1;
        if (ctx.cb.?(&full_tuple, ctx.arity, ctx.user) != 0)
            return 1;
    }

    if (depth >= MAX_KEY_LEN) return 0;

    var j: c_uint = 0;
    while (j < s.*.ntrans) : (j += 1) {
        const e = transArrC(s) + j;
        buf[depth] = e.*.sym;
        if (prefixDfs(d, e.*.target, buf, depth + 1, ctx) != 0)
            return 1;
    }
    return 0;
}

fn relPrefixD(rel: *const Relation, d: [*c]const dc.dafsa, leading: ?[*]const u32, k: u8, cb: RelEnumCb, user: ?*anyopaque) c_long {
    if (d == null or cb == null) return -1;
    if (k > rel.arity) return -1;
    if (k > 0 and leading == null) return -1;

    // Walk the k*4 prefix bytes
    var current: c_uint = d.*.initial;
    var i: u8 = 0;
    while (i < k) : (i += 1) {
        const v = leading.?[i];
        var col_be: [4]u8 = undefined;
        col_be[0] = @truncate(v >> 24);
        col_be[1] = @truncate(v >> 16);
        col_be[2] = @truncate(v >> 8);
        col_be[3] = @truncate(v);
        var b: u8 = 0;
        while (b < 4) : (b += 1) {
            const tr = dc.trans_find(d.*.states + current, col_be[b]);
            if (tr < 0) return 0; // prefix not found
            current = (transArrC(d.*.states + current) + @as(usize, @intCast(tr))).*.target;
        }
    }

    // Set up context and DFS from current state
    var ctx = PrefixCtx{
        .leading = leading,
        .k = k,
        .arity = rel.arity,
        .cb = cb,
        .user = user,
        .count = 0,
    };
    var buf: [MAX_KEY_LEN]u8 = undefined;

    _ = prefixDfs(d, current, &buf, 0, &ctx);
    return ctx.count;
}

/// Overlay prefix scan: emit overlay tuples matching the k-column leading
/// bound through the same cb contract as relPrefixD.  The tupleset is
/// unsorted between flushes, so this filters by linear scan — acceptable
/// because the overlay is bounded (flushed at consolidation) and point
/// reads (rel_exact) take the O(1) hash path.
const OvScanCtx = struct {
    leading: ?[*]const u32,
    k: u8,
    arity: u8,
    cb: RelEnumCb,
    user: ?*anyopaque,
    count: c_long,
};

fn ovScanCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ctx: *OvScanCtx = @ptrCast(@alignCast(user orelse return 0));
    const p = cols.?;
    var i: u8 = 0;
    while (i < ctx.k) : (i += 1) {
        if (p[i] != ctx.leading.?[i]) return 0;
    }
    ctx.count += 1;
    return ctx.cb.?(p, ctx.arity, ctx.user);
}

fn ovPrefixScan(r: *const Relation, ov: *tupleset.tuple_set, leading: ?[*]const u32, k: u8, cb: RelEnumCb, user: ?*anyopaque) c_long {
    if (ov.count == 0) return 0;
    var ctx = OvScanCtx{
        .leading = leading,
        .k = k,
        .arity = r.arity,
        .cb = cb,
        .user = user,
        .count = 0,
    };
    var i: c_long = 0;
    while (i < ov.count) : (i += 1) {
        _ = ovScanCb(ov.data.? + @as(usize, @intCast(i)) * r.arity, r.arity, &ctx);
    }
    return ctx.count;
}

/// long rel_prefix(const relation *rel, const uint32_t *leading, uint8_t k,
///                 rel_enum_cb cb, void *user)
/// VIEW = DAFSA ∪ overlay (C19).  NOTE: emission order is DAFSA (lex-sorted)
/// then overlay (insertion order); consumers needing global sort must sort.
pub export fn rel_prefix(rel: ?*const Relation, leading: ?[*]const u32, k: u8, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    if (!relEnsureReadable(@constCast(r))) return -1;
    if (r.state == .view) {
        // The overlay is provably empty in .view state, so the union is the
        // view's own enumeration: same tuples, same lex order as the owned
        // DFS (both walk the same minimal DAFSA's transitions in order).
        std.debug.assert(viewOverlayClean(r));
        return viewPrefixD(r, false, leading, k, cb, user);
    }
    ovConsolidateForRead(r);
    const n = relPrefixD(r, r.d, leading, k, cb, user);
    if (n < 0) return -1;
    var total: c_long = n;
    if (r.ov) |ov| {
        const m = ovPrefixScan(r, ov, leading, k, cb, user);
        if (m < 0) return -1;
        total += m;
    }
    return total;
}

/// long rel_prefix_base(const relation *rel, const uint32_t *leading, uint8_t k,
///                      rel_enum_cb cb, void *user)
pub export fn rel_prefix_base(rel: ?*const Relation, leading: ?[*]const u32, k: u8, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    if (!relEnsureReadable(@constCast(r))) return -1;
    if (r.state == .view) return viewPrefixD(r, true, leading, k, cb, user);
    return relPrefixD(r, r.base, leading, k, cb, user);
}

// ─── Leading-column range enumeration ─────────────────────────────────────

/// int rel_has_col0(const relation *rel, uint32_t x)
pub export fn rel_has_col0(rel: ?*const Relation, x: u32) c_int {
    const r = rel orelse return 0;
    if (!relEnsureReadable(@constCast(r))) return 0;
    if (r.state == .view) {
        // Same walk as rel_prefix_state's, without needing a state id back:
        // "some tuple has col0 == x" is exactly "the 4-byte prefix exists".
        std.debug.assert(viewOverlayClean(r));
        const v = viewOf(r, false);
        var current: c_uint = v.*.initial;
        const col_be = [4]u8{
            @truncate(x >> 24),
            @truncate(x >> 16),
            @truncate(x >> 8),
            @truncate(x),
        };
        for (col_be) |b| {
            var target: c_uint = undefined;
            if (dc.view_trans_find(v, current, b, &target) != 0) return 0;
            current = target;
        }
        return 1;
    }
    const xp: [*]const u32 = @ptrCast(&x);
    return if (rel_prefix_state(rel, xp, 1) >= 0) 1 else 0;
}

/// long rel_range_each(const relation *rel, uint32_t lo, uint32_t hi,
///                     rel_enum_cb cb, void *user)
pub export fn rel_range_each(rel: ?*const Relation, lo: u32, hi: u32, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    if (cb == null) return -1;
    // PROMOTES: this is a SCAN (rank + one select per row), and the view's
    // order statistics rebuild the whole subtree-count table per call — fine
    // for a point read, quadratic over a range.
    if (!relEnsureOwned(@constCast(r))) return -1;
    if (r.d == null) return -1; // OOM during the lazy load
    if (hi <= lo) return 0;

    var lo_key: [MAX_ARITY]u32 = std.mem.zeroes([MAX_ARITY]u32);
    var hi_key: [MAX_ARITY]u32 = std.mem.zeroes([MAX_ARITY]u32);
    lo_key[0] = lo;
    hi_key[0] = hi;

    const r0 = rel_rank(r, &lo_key);
    const r1 = rel_rank(r, &hi_key);
    if (r0 == std.math.maxInt(u64) or r1 == std.math.maxInt(u64)) return -1;
    if (r1 <= r0) return 0;

    var count: u64 = 0;
    var k: u64 = r0;
    while (k < r1) : (k += 1) {
        var cols: [MAX_ARITY]u32 = undefined;
        if (rel_select(r, k, &cols) != 0) return -1;
        count +%= 1;
        if (cb.?(&cols, r.arity, user) != 0)
            return @intCast(count); // early stop requested
    }
    return @intCast(count);
}

// ─── Bulk build from tuple set ────────────────────────────────────────────

/// int ts_sink_cb(const uint32_t *cols, uint8_t arity, void *user)
pub export fn ts_sink_cb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) c_int {
    _ = arity;
    const ts: *tupleset.tuple_set = @ptrCast(@alignCast(user orelse return -1));
    return if (tupleset.ts_add(ts, cols) < 0) -1 else 0;
}

/// Build a fresh minimal DAFSA from a SORTED, DEDUPLICATED tuple_set.
/// Returns NULL on error/OOM.
fn dafsaBuildFromTs(ts: *const tupleset.tuple_set, arity: u8) [*c]dc.dafsa {
    const n = ts.count;
    if (n == 0)
        return dc.dafsa_create();

    const keys_mem = c.calloc(@as(usize, @intCast(n)), @sizeOf([*c]u8));
    const lens_mem = c.calloc(@as(usize, @intCast(n)), @sizeOf(usize));
    if (keys_mem == null or lens_mem == null) {
        if (keys_mem) |k| c.free(k);
        if (lens_mem) |l| c.free(l);
        return null;
    }
    const keys: [*c][*c]u8 = @ptrCast(@alignCast(keys_mem.?));
    const lens: [*c]usize = @ptrCast(@alignCast(lens_mem.?));

    var i: c_long = 0;
    while (i < n) : (i += 1) {
        const cols = ts.data.? + @as(usize, @intCast(i)) * @as(usize, ts.arity);
        const km = c.malloc(MAX_KEY_LEN);
        if (km == null) break;
        const key: [*]u8 = @ptrCast(km);
        keys[@intCast(i)] = @ptrCast(key);
        lens[@intCast(i)] = encodeKey(key, cols, arity);
    }

    var new_d: [*c]dc.dafsa = null;
    if (i == n) {
        new_d = dc.dafsa_build_sorted(@ptrCast(keys), lens, @as(usize, @intCast(n)));
    }

    // Free every key (calloc'd NULLs for the unfilled tail are free(NULL)).
    var j: c_long = 0;
    while (j < n) : (j += 1) {
        if (keys[@intCast(j)] != null) c.free(keys[@intCast(j)]);
    }
    c.free(keys_mem.?);
    c.free(lens_mem.?);
    return new_d;
}

/// int rel_build_from_tupleset(relation *rel, const struct tuple_set *ts)
pub export fn rel_build_from_tupleset(rel: ?*Relation, ts: ?*const tupleset.tuple_set) c_int {
    const r = rel orelse return -1;
    const t = ts orelse return -1;
    if (t.arity != r.arity) return -1;
    // The view is REPLACED wholesale: the old view's on-disk content must be
    // resident for the caller's recompute-and-merge contract to hold (and the
    // old DAFSA must be freed) — load first (lazyrel).
    if (!relEnsureOwned(r)) return -1;

    const new_d = dafsaBuildFromTs(t, r.arity);
    if (new_d == null) return -1;

    const aliased = (r.base == r.d);
    dc.dafsa_free(r.d);
    r.d = new_d;
    if (aliased) r.base = r.d; // keep base aliased for EDB rels
    r.dirty = 1;
    r.rev +%= 1;
    return 0;
}

/// int rel_add_tupleset(relation *rel, const struct tuple_set *ts)
/// Insert-only incremental fold: add every tuple of `ts` into the VIEW DAFSA
/// via dafsa_add_n instead of rebuilding.  Tuples already present are no-ops
/// (dafsa_add_n returns 0); `ts` may be unsorted.  Callers must guarantee the
/// resulting view is exactly the intended one — this NEVER removes a fact, so
/// it cannot replace rel_build_from_tupleset where the new content is a
/// from-scratch recomputation that may shrink the relation.
/// Returns 0 ok / -1 error.
pub export fn rel_add_tupleset(rel: ?*Relation, ts: ?*const tupleset.tuple_set) c_int {
    const r = rel orelse return -1;
    const t = ts orelse return -1;
    if (t.arity != r.arity) return -1;
    if (t.count == 0) return 0;
    if (!relEnsureOwned(r)) return -1;

    // Insert in SORTED order: the cost of a single dafsa_add_n is dominated
    // by re-registering the divergent suffix path, so adding in lex order
    // (maximum prefix sharing between consecutive inserts) is up to an order
    // of magnitude cheaper than an adversarial order on a large DAFSA — the
    // batch then behaves like an incremental Daciuk construction while each
    // individual add still merges through the live equivalence registry
    // (minimality is preserved exactly as for arbitrary-order adds).
    //
    // A caller whose batch is ALREADY sorted (the fixpoint's per-rule commit
    // sorts its candidate set before handing it over) skips the defensive
    // copy: a tuple_set is unique by construction (ts_add rejects a tuple it
    // already holds), so sorted+unique is exactly what the loops below need.
    var scratch: tupleset.tuple_set = undefined;
    var src: *const tupleset.tuple_set = t;
    if (!tsSorted(t)) {
        if (tupleset.ts_init(&scratch, t.arity) != 0) return -1;
        var j: c_long = 0;
        while (j < t.count) : (j += 1) {
            if (tupleset.ts_add(&scratch, t.data.? + @as(usize, @intCast(j)) * @as(usize, t.arity)) < 0) {
                tupleset.ts_free(&scratch);
                return -1;
            }
        }
        tupleset.ts_sort(&scratch);
        src = &scratch;
    }

    // EMPTY VIEW: one bulk build instead of N incremental adds.  The result is
    // the same set — a minimal DAFSA is canonical, and dafsa_save's contract
    // is that the bytes are identical to adding the same keys one by one
    // (dafsa.h:27-29) — but dafsa_build_sorted is ~3x cheaper per key than N
    // dafsa_add_n, which pays the register/confluence maintenance per word
    // (MEASURED on a 200-word 1-column relation: 124ns vs 408ns per key).
    //
    // This is the fixpoint's hot term: a head view that the driver resets for
    // a from-scratch re-derivation is empty, so the round's whole batch lands
    // here (MEASURED, win/move chain n=400: 34.3ms of the 62ms query is
    // per-tuple dafsa_add_n, and 400 of the 400 rounds reset the view).
    if (viewOverlayClean(r) and relViewDafsaEmpty(r)) {
        const new_d = dafsaBuildFromTs(src, r.arity);
        if (new_d == null) {
            if (src == &scratch) tupleset.ts_free(&scratch);
            return -1;
        }
        const aliased = (r.base == r.d);
        dc.dafsa_free(r.d);
        r.d = new_d;
        if (aliased) r.base = r.d; // EDB-only: base aliases the view
        r.dirty = 1;
        // One bump per fact, as the per-tuple loop below does — rev is a
        // monotonic change gate (publish), and an empty view means every
        // tuple of the batch is new.
        r.rev +%= @intCast(src.count);
        if (src == &scratch) tupleset.ts_free(&scratch);
        return 0;
    }

    var i: c_long = 0;
    while (i < src.count) : (i += 1) {
        const rc = relAddD(r, r.d, src.data.? + @as(usize, @intCast(i)) * @as(usize, src.arity));
        if (rc < 0) {
            if (src == &scratch) tupleset.ts_free(&scratch);
            return -1;
        }
        if (rc > 0) {
            r.dirty = 1;
            r.rev +%= 1;
        }
    }
    if (src == &scratch) tupleset.ts_free(&scratch);
    return 0;
}

/// Is the tuple_set's array in lexicographic order?  O(n) over the strided
/// rows — cheaper than the copy+ts_sort it lets rel_add_tupleset skip.
fn tsSorted(ts: *const tupleset.tuple_set) bool {
    const ar: usize = ts.arity;
    var i: c_long = 1;
    while (i < ts.count) : (i += 1) {
        const prev = ts.data.? + @as(usize, @intCast(i - 1)) * ar;
        const cur = ts.data.? + @as(usize, @intCast(i)) * ar;
        var k: usize = 0;
        while (k < ar and prev[k] == cur[k]) k += 1;
        if (k < ar and cur[k] < prev[k]) return false;
    }
    return true;
}

/// Early-stopping sink: one tuple answers "does this DAFSA hold any word".
fn firstTupleProbe(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = cols;
    _ = arity;
    _ = user;
    return 1;
}

/// Is the relation's VIEW DAFSA word-empty?  One DFS descent to the first
/// final state — O(depth), NOT an O(states) stats walk: rel_add_tupleset asks
/// this per bulk commit, and the view it asks about can be large.
fn relViewDafsaEmpty(r: *const Relation) bool {
    return relPrefixD(r, r.d, null, 0, firstTupleProbe, null) == 0;
}

/// int rel_build_base_from_tupleset(relation *rel, const struct tuple_set *ts)
pub export fn rel_build_base_from_tupleset(rel: ?*Relation, ts: ?*const tupleset.tuple_set) c_int {
    const r = rel orelse return -1;
    const t = ts orelse return -1;
    if (t.arity != r.arity) return -1;
    // The BASE is replaced wholesale (same contract as the view rebuild
    // above): the old base must be resident and freed, so load first.
    if (!relEnsureOwned(r)) return -1;

    const new_b = dafsaBuildFromTs(t, r.arity);
    if (new_b == null) return -1;

    const aliased = (r.base == r.d);
    dc.dafsa_free(r.base);
    r.base = new_b;
    if (aliased) r.d = r.base; // keep view aliased for EDB rels
    r.dirty = 1;
    r.rev +%= 1;
    return 0;
}

/// Copy the facts of `src` into a fresh minimal DAFSA (via enumeration +
/// bulk build).
fn dafsaCopyFrom(rel: *const Relation, src: [*c]const dc.dafsa) [*c]dc.dafsa {
    if (src == null) return null;
    var ts: tupleset.tuple_set = undefined;
    if (tupleset.ts_init(&ts, rel.arity) != 0) return null;
    if (relPrefixD(rel, src, null, 0, ts_sink_cb, &ts) < 0) {
        tupleset.ts_free(&ts);
        return null;
    }
    tupleset.ts_sort(&ts);
    const out = dafsaBuildFromTs(&ts, rel.arity);
    tupleset.ts_free(&ts);
    return out;
}

// ─── Base/view partition (IVM Slice 0) ────────────────────────────────────

/// int rel_is_idb(const relation *rel)
pub export fn rel_is_idb(rel: ?*const Relation) c_int {
    const r = rel orelse return 0;
    // Unloaded (.deferred) or view-backed relation: base==d==NULL would read
    // as EDB.  The split is recorded in on_disk_base / view_base (non-NULL iff
    // IDB), so answer WITHOUT loading.
    switch (r.state) {
        .deferred => return if (r.on_disk_base != null) 1 else 0,
        .view => return if (r.view_base != null) 1 else 0,
        .owned => return if (r.base != r.d) 1 else 0,
    }
}

/// int rel_is_dirty(const relation *rel)
pub export fn rel_is_dirty(rel: ?*const Relation) c_int {
    const r = rel orelse return 0;
    return if (r.dirty != 0) 1 else 0;
}

/// int rel_reset_view(relation *rel)
pub export fn rel_reset_view(rel: ?*Relation) c_int {
    const r = rel orelse return -1;
    if (!relEnsureOwned(r)) return -1;

    if (r.base == r.d) {
        // First evaluation as a derived relation: SPLIT base from view.
        const nb = dafsaCopyFrom(r, r.d);
        if (nb == null) return -1;
        r.base = nb;
        r.dirty = 1; // view is about to be re-derived by the VM
        r.rev +%= 1;
        return 0;
    }

    // Already split: drop stale derived facts — view = copy of base.
    {
        const nv = dafsaCopyFrom(r, r.base);
        if (nv == null) return -1;
        dc.dafsa_free(r.d);
        r.d = nv;
    }
    r.dirty = 1;
    r.rev +%= 1; // view reset for re-derivation
    return 0;
}

// ─── WAL operations (M7) ──────────────────────────────────────────────────

/// WAL replay callback: apply ADD or DEL to the in-memory DAFSA.
const WalReplayCtx = struct {
    rel: ?*Relation,
    ok: c_int, // becomes -1 on first error
};

fn walReplayCb(op: u8, key: [*c]const u8, key_len: u32, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *WalReplayCtx = @ptrCast(@alignCast(user orelse return 0));

    if (ctx.ok != 0) return 0; // already failed, skip

    if (op == dc.DAFSA_WAL_OP_ADD) {
        const rc = dc.dafsa_add_n(ctx.rel.?.base, key, key_len);
        if (rc < 0) ctx.ok = -1;
    } else if (op == dc.DAFSA_WAL_OP_DEL) {
        const rc = dc.dafsa_delete_n(ctx.rel.?.base, key, key_len);
        if (rc < 0) ctx.ok = -1;
    }
    return 0;
}

/// int rel_wal_replay_into(relation *rel)
pub export fn rel_wal_replay_into(rel: ?*Relation) c_int {
    const r = rel orelse return -1;
    if (r.wal == null) return -1;
    // Replay writes into BASE: it must be resident — load first (lazyrel).
    if (!relEnsureOwned(r)) return -1;

    var ctx = WalReplayCtx{ .rel = r, .ok = 0 };

    if (dc.dafsa_wal_replay(r.wal, walReplayCb, &ctx) != 0)
        return -1;

    return ctx.ok;
}

// ─── WAL durability contract (selfreg-batched-fsync) ─────────────────────
//
// Per-relation WAL appends are NOT fsync'd per fact.  Durability is at
// CYCLE granularity (the same buffered-then-one-sync shape as the txn
// path, and the same conclusion the access-epoch decay block already drew
// for per-read writes): an append writes the record into the WAL file
// (O_APPEND, kernel page cache) and returns; the record becomes durable
// only when rel_wal_sync is called at a cycle boundary — dl_publish_snapshot
// (consolidation/publish) and dl_close (clean shutdown, which also compacts
// every dirty relation).  On an UNclean stop (SIGKILL / power loss) between
// boundaries the current cycle's WAL records are lost: a bounded, stated
// exposure.  The WAL is the durability record; the DAFSA snapshot is
// read-optimization, so a crash loses exactly the writes since the last
// sync point and recovers everything before it via rel_wal_replay_into.
//
// C9 split: only DECISIONS/OBSERVATIONS (EDB facts) reach this WAL — every
// rel_wal_append_* caller is an EDB write (dl_add_fact, dl_delete_fact,
// dl_cas_revision).  DERIVED (IDB) facts are recomputable and are never
// WAL-appended (they are materialized into the DAFSA at publish and
// re-derived), so batching the sync cannot drop the unrecoverable class.

/// int rel_wal_append_add(relation *rel, const unsigned char *key, uint32_t key_len)
/// Append an ADD record WITHOUT fsync (durability is at cycle boundaries —
/// see rel_wal_sync).
pub export fn rel_wal_append_add(rel: ?*Relation, key: [*c]const u8, key_len: u32) c_int {
    const r = rel orelse return -1;
    if (r.wal == null or key == null) return -1;
    // A WAL record is a WRITE: it PROMOTES a view/deferred relation first, so
    // a .view relation can never have appends the view does not show (the
    // record is in the file, not in the mmap'd snapshot).
    if (!relEnsureOwned(r)) return -1;
    const before = dc.dafsa_wal_size(r.wal);
    if (dc.dafsa_wal_append_add(r.wal, key, key_len) != 0) return -1;
    r.wal_appended_bytes +%= dc.dafsa_wal_size(r.wal) - before;
    return 0;
}

/// int rel_wal_append_del(relation *rel, const unsigned char *key, uint32_t key_len)
/// Append a DEL record WITHOUT fsync (durability is at cycle boundaries —
/// see rel_wal_sync).
pub export fn rel_wal_append_del(rel: ?*Relation, key: [*c]const u8, key_len: u32) c_int {
    const r = rel orelse return -1;
    if (r.wal == null or key == null) return -1;
    // See rel_wal_append_add: an append is a write, so it promotes first.
    if (!relEnsureOwned(r)) return -1;
    const before = dc.dafsa_wal_size(r.wal);
    if (dc.dafsa_wal_append_del(r.wal, key, key_len) != 0) return -1;
    r.wal_appended_bytes +%= dc.dafsa_wal_size(r.wal) - before;
    return 0;
}

/// int rel_wal_sync(relation *rel)
/// Cycle-boundary durability barrier: fsync this relation's WAL so every
/// record appended since the previous barrier is durable on disk.
pub export fn rel_wal_sync(rel: ?*Relation) c_int {
    const r = rel orelse return -1;
    if (r.wal == null) return -1;
    return dc.dafsa_wal_sync(r.wal);
}

/// fsync helper: fsync the directory containing a path.
/// NOTE: this replicates relation.c's own fsync_parent_dir (takes a path,
/// fsyncs its containing directory) — distinct from util.fsync_dir_of_path.
fn fsyncParentDir(path: [*c]const u8) c_int {
    // Find the last '/' (strrchr).  -1/none or index 0 => the dir is "/".
    var last_slash: isize = -1;
    var i: isize = 0;
    while (path[@intCast(i)] != 0) : (i += 1) {
        if (path[@intCast(i)] == '/') last_slash = i;
    }
    var ret: c_int = -1;
    if (last_slash <= 0) {
        const fd = c.open("/", .{ .ACCMODE = .RDONLY, .DIRECTORY = true });
        if (fd >= 0) {
            ret = c.fsync(fd);
            _ = c.close(fd);
        }
        return ret;
    }

    const dir_len: usize = @intCast(last_slash);
    const mem = c.malloc(dir_len + 1) orelse return -1;
    const buf: [*]u8 = @ptrCast(mem);
    @memcpy(buf[0..dir_len], @as([*]const u8, @ptrCast(path))[0..dir_len]);
    buf[dir_len] = 0;
    defer c.free(mem);
    const fd = c.open(@ptrCast(buf), .{ .ACCMODE = .RDONLY, .DIRECTORY = true });
    if (fd >= 0) {
        ret = c.fsync(fd);
        _ = c.close(fd);
    }
    return ret;
}

/// int rel_compact(relation *rel, const char *dafsa_path)
pub export fn rel_compact(rel: ?*Relation, dafsa_path: [*c]const u8) c_int {
    const r = rel orelse return -1;
    if (r.wal == null or dafsa_path == null) return -1;
    // Compaction saves BASE: it must be resident — load first (lazyrel).
    // (Every dirty path loads first, so this is normally a no-op; it keeps
    // compaction correct even for a caller that dirtied nothing.)
    if (!relEnsureOwned(r)) return -1;

    // Fold the hot-write overlay into the DAFSA BEFORE truncating the WAL:
    // un-flushed overlay facts are durable only through the WAL, so a
    // compact-without-flush would silently destroy them (selfreg-dl-storage
    // C18/C19).  On flush error the WAL is left intact, so nothing is lost.
    if (rel_overlay_flush(r) != 0) return -1;

    // 1. Save the BASE DAFSA atomically.
    if (dc.dafsa_save(r.base, dafsa_path) != 0) return -1;

    // 2. ftruncate WAL to 16 bytes (header-only) and fsync.
    if (c.ftruncate(r.wal.*.fd, 16) != 0) return -1;
    if (c.fsync(r.wal.*.fd) != 0) return -1;
    r.wal.*.size = 16;

    // 3. fsync the directory containing the WAL.
    if (fsyncParentDir(r.wal_path) != 0) return -1;

    return 0;
}

/// uint64_t rel_wal_size(const relation *rel)
pub export fn rel_wal_size(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    if (r.wal == null) return 0;
    return dc.dafsa_wal_size(r.wal);
}

/// uint64_t rel_dafsa_size(const relation *rel)
pub export fn rel_dafsa_size(rel: ?*const Relation) u64 {
    const r = rel orelse return 0;
    // Size estimate walks BASE's states: it must be resident — load first
    // (lazyrel).  (Callers reach this only for relations they wrote to, which
    // already loaded; the gate keeps the answer exact for any other caller.)
    if (!relEnsureOwned(@constCast(r))) return 0;
    if (r.base == null) return 0; // OOM during the lazy load
    var st: dc.dafsa_stats_out = std.mem.zeroes(dc.dafsa_stats_out);
    dc.dafsa_stats(r.base, &st);
    // Rough byte estimate based on state/transition counts (25% compaction
    // threshold).
    const est = @as(u64, st.n_states_reachable) * 64 + @as(u64, st.n_trans) * 8;
    return est;
}

// ─── Open with WAL ────────────────────────────────────────────────────────

/// relation *rel_open_writable(const char *dafsa_path, const char *wal_path,
///                             uint8_t arity)
/// WAL opens EAGERLY (its tail repair is part of open), but the DAFSA read
/// is deferred unless the WAL holds committed records — recovery must
/// replay into the in-memory DAFSA and compact, which needs it loaded.
pub export fn rel_open_writable(dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);
    rel.arity = arity;

    // Open WAL (rw) — auto-repairs torn tail
    rel.wal = dc.dafsa_wal_open_rw(wal_path);
    if (rel.wal == null) {
        c.free(mem);
        return null;
    }

    rel.wal_path = strdup(wal_path);
    if (rel.wal_path == null) {
        dc.dafsa_wal_close(rel.wal);
        c.free(mem);
        return null;
    }

    if (rel.wal.*.size > 16) {
        // Committed records: recover NOW (replay + compact), loading the
        // DAFSA eagerly — the lazyrel deferral never delays recovery.
        rel.d = dc.dafsa_load(dafsa_path);
        if (rel.d == null) rel.d = dc.dafsa_create();
        if (rel.d == null) {
            rel_free(rel);
            return null;
        }
        rel.base = rel.d; // EDB-only: base aliases view

        if (rel_wal_replay_into(rel) != 0) {
            rel_free(rel);
            return null;
        }
        // Compact immediately: save DAFSA + truncate WAL
        if (rel_compact(rel, dafsa_path) != 0) {
            rel_free(rel);
            return null;
        }
        return rel;
    }

    // Header-only WAL: defer the DAFSA read to the first touch (lazyrel).
    rel.state = .deferred;
    rel.on_disk_dafsa = strdup(dafsa_path);
    if (rel.on_disk_dafsa == null) {
        rel_free(rel);
        return null;
    }
    return rel;
}

/// relation *rel_open_writable_idb(const char *base_path, const char *dafsa_path,
///                                 const char *wal_path, uint8_t arity)
/// Same lazyrel contract as rel_open_writable, for a SPLIT (IDB) relation:
/// eager only when the WAL holds committed records, lazy otherwise.
pub export fn rel_open_writable_idb(base_path: [*c]const u8, dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);
    rel.arity = arity;

    // Open WAL (rw) — auto-repairs torn tail
    rel.wal = dc.dafsa_wal_open_rw(wal_path);
    if (rel.wal == null) {
        c.free(mem);
        return null;
    }

    rel.wal_path = strdup(wal_path);
    if (rel.wal_path == null) {
        dc.dafsa_wal_close(rel.wal);
        c.free(mem);
        return null;
    }

    if (rel.wal.*.size > 16) {
        // Committed records: recover NOW (replay + compact), loading base
        // and view DAFSAs eagerly — the lazyrel deferral never delays
        // recovery.
        rel.base = dc.dafsa_load(base_path);
        if (rel.base == null) rel.base = dc.dafsa_create();
        if (rel.base == null) {
            rel_free(rel);
            return null;
        }
        rel.d = dc.dafsa_load(dafsa_path);
        if (rel.d == null) rel.d = dc.dafsa_create();
        if (rel.d == null) {
            dc.dafsa_free(rel.base);
            rel.base = null;
            rel_free(rel);
            return null;
        }

        // Replay WAL into BASE and compact immediately.
        if (rel_wal_replay_into(rel) != 0) {
            rel_free(rel);
            return null;
        }
        if (rel_compact(rel, base_path) != 0) {
            rel_free(rel);
            return null;
        }
        return rel;
    }

    // Header-only WAL: defer both DAFSA reads to the first touch (lazyrel).
    // on_disk_base != NULL marks the relation IDB for rel_is_idb unloaded.
    rel.state = .deferred;
    rel.on_disk_dafsa = strdup(dafsa_path);
    rel.on_disk_base = strdup(base_path);
    if (rel.on_disk_dafsa == null or rel.on_disk_base == null) {
        rel_free(rel);
        return null;
    }
    return rel;
}

/// relation *rel_open_readonly(const char *dafsa_path, const char *wal_path,
///                             uint8_t arity)
/// Same lazyrel contract as rel_open_writable; RO never compacts, so the
/// eager branch only replays (no dafsa_save / WAL truncate).
pub export fn rel_open_readonly(dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);
    rel.arity = arity;

    if (wal_path != null) {
        rel.wal = dc.dafsa_wal_open_ro(wal_path);
        if (rel.wal != null and rel.wal.*.size > 16) {
            // Committed records: recover NOW, loading the DAFSA eagerly.
            rel.d = dc.dafsa_load(dafsa_path);
            if (rel.d == null) rel.d = dc.dafsa_create();
            if (rel.d == null) {
                rel_free(rel);
                return null;
            }
            rel.base = rel.d; // EDB-only: base aliases view
            if (rel_wal_replay_into(rel) != 0) {
                rel_free(rel);
                return null;
            }
            return rel;
        }
        // wal_path stays NULL: only rel_compact reads it and RO never compacts.
    }

    // No WAL records to recover: defer the DAFSA read to the first touch.
    rel.state = .deferred;
    rel.on_disk_dafsa = strdup(dafsa_path);
    if (rel.on_disk_dafsa == null) {
        rel_free(rel);
        return null;
    }
    return rel;
}

/// relation *rel_open_readonly_idb(const char *base_path, const char *dafsa_path,
///                                 const char *wal_path, uint8_t arity)
pub export fn rel_open_readonly_idb(base_path: [*c]const u8, dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*Relation {
    if (arity == 0 or arity > MAX_ARITY) return null;

    const mem = c.calloc(1, @sizeOf(Relation)) orelse return null;
    const rel: *Relation = @ptrCast(@alignCast(mem));
    rel.* = std.mem.zeroes(Relation);
    rel.arity = arity;

    if (wal_path != null) {
        rel.wal = dc.dafsa_wal_open_ro(wal_path);
        if (rel.wal != null and rel.wal.*.size > 16) {
            // Committed records: recover NOW, loading base and view eagerly.
            rel.base = dc.dafsa_load(base_path);
            if (rel.base == null) rel.base = dc.dafsa_create();
            if (rel.base == null) {
                rel_free(rel);
                return null;
            }
            rel.d = dc.dafsa_load(dafsa_path);
            if (rel.d == null) rel.d = dc.dafsa_create();
            if (rel.d == null) {
                dc.dafsa_free(rel.base);
                rel.base = null;
                rel_free(rel);
                return null;
            }
            if (rel_wal_replay_into(rel) != 0) {
                rel_free(rel);
                return null;
            }
            return rel;
        }
    }

    // No WAL records to recover: defer both DAFSA reads to the first touch.
    // on_disk_base != NULL marks the relation IDB for rel_is_idb unloaded.
    rel.state = .deferred;
    rel.on_disk_dafsa = strdup(dafsa_path);
    rel.on_disk_base = strdup(base_path);
    if (rel.on_disk_dafsa == null or rel.on_disk_base == null) {
        rel_free(rel);
        return null;
    }
    return rel;
}

// ─── Regex pattern walk ───────────────────────────────────────────────────

const PatCtx = struct {
    arity: u8,
    cb: RelEnumCb,
    user: ?*anyopaque,
    count: c_long,
};

fn patCb(key_bytes: [*c]const u8, key_len: usize, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *PatCtx = @ptrCast(@alignCast(user orelse return 0));
    var cols: [MAX_ARITY]u32 = undefined;

    // Key is 4*arity+1 bytes. Decode columns from buf.
    if (key_len != @as(usize, ctx.arity) * 4 + 1) return 0;

    var i: u8 = 0;
    while (i < ctx.arity) : (i += 1) {
        cols[i] = (@as(u32, key_bytes[4 * @as(usize, i)]) << 24) |
            (@as(u32, key_bytes[4 * @as(usize, i) + 1]) << 16) |
            (@as(u32, key_bytes[4 * @as(usize, i) + 2]) << 8) |
            (@as(u32, key_bytes[4 * @as(usize, i) + 3]));
    }

    ctx.count +%= 1;
    return ctx.cb.?(&cols, ctx.arity, ctx.user);
}

/// long rel_pattern(const relation *rel, const struct regex_dfa *dfa,
///                  rel_enum_cb cb, void *user)
pub export fn rel_pattern(rel: ?*const Relation, dfa: [*c]const regexwalk.regex_dfa, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    if (dfa == null or cb == null) return -1;
    // PROMOTES: regex_dfa_walk reads struct dafsa states/edges directly.
    if (!relEnsureOwned(@constCast(r))) return -1;

    ovConsolidateForRead(r);
    var ctx = PatCtx{
        .arity = r.arity,
        .cb = cb,
        .user = user,
        .count = 0,
    };

    // r.d is a [*c]dc.dafsa (this module's @cImport); regexwalk's dc is a
    // separate @cImport, so cast the opaque struct pointer across.
    const n = regexwalk.regex_dfa_walk(@ptrCast(r.d), dfa, patCb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

// ─── Filter callback ──────────────────────────────────────────────────────

const FilterCtx = struct {
    col: u8,
    set: [*c]const regexwalk.sym_set,
    cb: RelEnumCb,
    user: ?*anyopaque,
    count: c_long,
};

fn filterCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *FilterCtx = @ptrCast(@alignCast(user orelse return 0));
    const cols_p = cols orelse return 0;
    if (ctx.col < arity and regexwalk.symset_contains(ctx.set, cols_p[ctx.col]) != 0) {
        ctx.count +%= 1;
        return ctx.cb.?(cols, arity, ctx.user);
    }
    return 0;
}

/// long rel_filter_col(const relation *rel, uint8_t col, const struct sym_set *set,
///                     rel_enum_cb cb, void *user)
pub export fn rel_filter_col(rel: ?*const Relation, col: u8, set: [*c]const regexwalk.sym_set, cb: RelEnumCb, user: ?*anyopaque) c_long {
    const r = rel orelse return -1;
    if (set == null or cb == null) return -1;
    if (col >= r.arity) return 0; // out of range: no matches

    ovConsolidateForRead(r);
    var ctx = FilterCtx{
        .col = col,
        .set = set,
        .cb = cb,
        .user = user,
        .count = 0,
    };

    // Enumerate all tuples and filter by column membership
    const n = rel_prefix(r, null, 0, filterCb, &ctx);
    if (n < 0) return -1;
    return ctx.count;
}

// ─── Tests ────────────────────────────────────────────────────────────────

// ─── zero-copy view tests ─────────────────────────────────────────────────

const ZcCtx = struct {
    rows: [64][8]u32 = undefined,
    n: usize = 0,
    fn cb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
        const self: *ZcCtx = @ptrCast(@alignCast(user.?));
        const p = cols.?;
        var i: u8 = 0;
        while (i < arity) : (i += 1) self.rows[self.n][i] = p[i];
        self.n += 1;
        return 0;
    }
};

test "zerocopy: a saved relation reads IDENTICALLY through a view and owned" {
    // The differential guard for the whole feature: every read primitive must
    // return exactly what the parsed (owned) relation returns.  A view and an
    // owned handle over the SAME on-disk bytes are compared value by value.
    // rel_save commits atomically (tmp+rename), so a leftover file from a
    // previous run is replaced, never appended to.
    const path = "/tmp/datalog_zig_zerocopy_diff.dafsa";

    const owned = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(owned);
    const rows = [_][2]u32{ .{ 1, 2 }, .{ 1, 3 }, .{ 2, 4 }, .{ 9, 9 }, .{ 5, 1 } };
    for (rows) |row| try std.testing.expectEqual(@as(c_int, 1), rel_add(owned, &row));
    try std.testing.expectEqual(@as(c_int, 0), rel_save(owned, path));

    const viewed = rel_open(path, 2) orelse return error.OutOfMemory;
    defer rel_free(viewed);
    // A first read must land in .view, not .owned — otherwise the comparison
    // below is vacuous (it would compare owned against owned).
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(viewed, &rows[0]));
    try std.testing.expectEqual(@as(u8, @intFromEnum(ReadState.view)), rel_read_state(viewed));
    try std.testing.expectEqual(@as(c_int, 0), rel_is_loaded(viewed));

    // point lookups: present, absent, prefix-present-but-tuple-absent
    for (rows) |row| {
        try std.testing.expectEqual(@as(c_int, 1), rel_exact(viewed, &row));
        try std.testing.expectEqual(@as(c_int, 1), rel_exact_base(viewed, &row));
    }
    try std.testing.expectEqual(@as(c_int, 0), rel_exact(viewed, &.{ 1, 4 }));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact(viewed, &.{ 7, 7 }));

    // enumeration: k=0 (all) and every 1-column prefix, tuple for tuple
    var ctx_o = ZcCtx{};
    var ctx_v = ZcCtx{};
    try std.testing.expectEqual(
        rel_prefix(owned, null, 0, ZcCtx.cb, &ctx_o),
        rel_prefix(viewed, null, 0, ZcCtx.cb, &ctx_v),
    );
    try std.testing.expectEqual(ctx_o.n, ctx_v.n);
    var i: usize = 0;
    while (i < ctx_o.n) : (i += 1) try std.testing.expectEqualSlices(u32, ctx_o.rows[i][0..2], ctx_v.rows[i][0..2]);

    for ([_]u32{ 1, 2, 5, 9, 77 }) |c0| {
        var po = ZcCtx{};
        var pv = ZcCtx{};
        const no = rel_prefix(owned, @as([*]const u32, @ptrCast(&c0)), 1, ZcCtx.cb, &po);
        const nv = rel_prefix(viewed, @as([*]const u32, @ptrCast(&c0)), 1, ZcCtx.cb, &pv);
        try std.testing.expectEqual(no, nv);
        try std.testing.expectEqual(po.n, pv.n);
        i = 0;
        while (i < po.n) : (i += 1) try std.testing.expectEqualSlices(u32, po.rows[i][0..2], pv.rows[i][0..2]);
        try std.testing.expectEqual(rel_has_col0(owned, c0), rel_has_col0(viewed, c0));
    }

    // order statistics
    const probes = [_][2]u32{ .{ 1, 2 }, .{ 1, 3 }, .{ 2, 0 }, .{ 5, 1 }, .{ 9, 9 }, .{ 0, 0 }, .{ 99, 99 } };
    for (probes) |p| try std.testing.expectEqual(rel_rank(owned, &p), rel_rank(viewed, &p));
    var k: u64 = 0;
    while (k < 6) : (k += 1) {
        var a: [2]u32 = undefined;
        var b: [2]u32 = undefined;
        const ra = rel_select(owned, k, &a);
        const rb = rel_select(viewed, k, &b);
        try std.testing.expectEqual(ra, rb);
        if (ra == 0) try std.testing.expectEqualSlices(u32, a[0..2], b[0..2]);
    }
    try std.testing.expectEqual(
        rel_range_count(owned, &.{ 1, 0 }, &.{ 3, 0 }),
        rel_range_count(viewed, &.{ 1, 0 }, &.{ 3, 0 }),
    );
    try std.testing.expectEqual(rel_count(owned), rel_count(viewed));
    try std.testing.expectEqual(rel_count_subtree(owned), rel_count_subtree(viewed));

    // PROMOTION: a write through the view-backed handle must materialize the
    // SAME content and then behave exactly like an owned relation.
    try std.testing.expectEqual(@as(c_int, 1), rel_add(viewed, &.{ 3, 3 }));
    try std.testing.expectEqual(@as(u8, @intFromEnum(ReadState.owned)), rel_read_state(viewed));
    try std.testing.expectEqual(@as(c_int, 1), rel_is_loaded(viewed)); // promoted == loaded
    try std.testing.expectEqual(@as(c_int, 1), rel_add(owned, &.{ 3, 3 }));
    try std.testing.expectEqual(rel_count_subtree(owned), rel_count_subtree(viewed));
    try std.testing.expectEqual(rel_rank(owned, &.{ 9, 9 }), rel_rank(viewed, &.{ 9, 9 }));
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(viewed, &rows[0])); // old facts survived
}

test "key encode/decode roundtrip" {
    var buf: [MAX_KEY_LEN]u8 = undefined;
    const cols = [_]u32{ 0xDEADBEEF, 0x00000001, 0xFFFFFFFF, 0x01020304 };
    const len = encodeKey(&buf, &cols, 4);
    try std.testing.expectEqual(@as(usize, 17), len);
    try std.testing.expectEqual(@as(u8, 0x00), buf[16]);
    var out: [4]u32 = undefined;
    readColsBe(&out, &buf, 4);
    try std.testing.expectEqualSlices(u32, &cols, &out);
    // Byte-exact big-endian layout.
    try std.testing.expectEqual(@as(u8, 0xDE), buf[0]);
    try std.testing.expectEqual(@as(u8, 0xAD), buf[1]);
    try std.testing.expectEqual(@as(u8, 0xBE), buf[2]);
    try std.testing.expectEqual(@as(u8, 0xEF), buf[3]);
}

test "rel_create/add/exact/prefix/rank/select/range_count roundtrip" {
    const r = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(r);

    const facts = [_]u32{ 1, 2, 1, 3, 2, 1, 5, 5 };
    var i: usize = 0;
    while (i < 4) : (i += 1) {
        try std.testing.expectEqual(@as(c_int, 1), rel_add(r, facts[i * 2 ..][0..2].ptr));
    }
    try std.testing.expectEqual(@as(c_int, 0), rel_add(r, facts[0..2].ptr)); // dup
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(r, facts[2..4].ptr));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact(r, &.{ 9, 9 }));
    // rel_count = dafsa n_final = merged final STATES (identical right
    // languages collapse), not words.  count_subtree is the word count.
    try std.testing.expectEqual(@as(u64, 1), rel_count(r));
    try std.testing.expectEqual(@as(u64, 4), rel_count_subtree(r));

    // lex order (u32BE): (1,2) (1,3) (2,1) (5,5)
    try std.testing.expectEqual(@as(u64, 0), rel_rank(r, &.{ 1, 2 }));
    try std.testing.expectEqual(@as(u64, 2), rel_rank(r, &.{ 2, 0 }));
    try std.testing.expectEqual(@as(u64, 4), rel_rank(r, &.{ 9, 9 }));

    var out: [2]u32 = undefined;
    try std.testing.expectEqual(@as(c_int, 0), rel_select(r, 2, &out));
    try std.testing.expectEqualSlices(u32, &.{ 2, 1 }, &out);
    try std.testing.expectEqual(@as(c_int, -1), rel_select(r, 4, &out));

    // key-lexicographic range: (1,2),(1,3),(2,1) fall in [(1,0)..(2,2)].
    try std.testing.expectEqual(@as(u64, 3), rel_range_count(r, &.{ 1, 0 }, &.{ 2, 2 }));

    // prefix walk
    var count: c_long = 0;
    try std.testing.expectEqual(@as(c_long, 4), rel_prefix(r, null, 0, countCb, &count));
    try std.testing.expectEqual(@as(c_long, 4), count);
    count = 0;
    try std.testing.expectEqual(@as(c_long, 2), rel_prefix(r, &.{1}, 1, countCb, &count));
    try std.testing.expectEqual(@as(c_long, 2), count);
    count = 0;
    try std.testing.expectEqual(@as(c_long, 1), rel_prefix(r, &.{ 2, 1 }, 2, countCb, &count));
    try std.testing.expectEqual(@as(c_long, 1), count);
    count = 0;
    try std.testing.expectEqual(@as(c_long, 0), rel_prefix(r, &.{ 7 }, 1, countCb, &count));

    // prefix-bound order stats
    try std.testing.expectEqual(@as(u64, 1), rel_rank_bound(r, &.{1}, 1, &.{ 1, 3 }));
    var b2: [2]u32 = undefined;
    try std.testing.expectEqual(@as(c_int, 0), rel_select_bound(r, &.{1}, 1, 1, &b2));
    try std.testing.expectEqualSlices(u32, &.{ 1, 3 }, &b2);
    // bound range: within the prefix-{1} subtree, tail in [0..0] — none of
    // (1,2),(1,3) qualifies (oracle-verified: 0).
    try std.testing.expectEqual(@as(u64, 0), rel_range_count_bound(r, &.{1}, 1, &.{ 1, 0 }, &.{ 2, 0 }));

    // delete
    try std.testing.expectEqual(@as(c_int, 1), rel_delete(r, facts[0..2].ptr));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact(r, facts[0..2].ptr));
    // (1,3),(2,1),(5,5) remain; their final states merge to one.
    try std.testing.expectEqual(@as(u64, 1), rel_count(r));
}

fn countCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = cols;
    _ = arity;
    const counter: *c_long = @ptrCast(@alignCast(user orelse return 0));
    counter.* +%= 1;
    return 0;
}

test "rel_add_tupleset incremental fold matches bulk rebuild" {
    const r1 = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(r1);
    const r2 = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(r2);

    // Split base from view on both (IDB shape).
    try std.testing.expectEqual(@as(c_int, 0), rel_reset_view(r1));
    try std.testing.expectEqual(@as(c_int, 0), rel_reset_view(r2));

    var ts: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), tupleset.ts_init(&ts, 2));
    defer tupleset.ts_free(&ts);
    // Unsorted, with internal duplicates — exercises the no-sort, no-delete
    // contract.
    const rows = [_][2]u32{ .{ 5, 5 }, .{ 1, 2 }, .{ 2, 1 }, .{ 1, 3 } };
    for (rows) |row| {
        try std.testing.expectEqual(@as(c_int, 1), tupleset.ts_add(&ts, &row));
    }

    // Incremental fold takes the ts AS-IS (unsorted — its contract needs no
    // order); the bulk rebuild gets the sorted ts that dafsa_build_sorted
    // requires.  Both DAFSAs must accept the same set.
    try std.testing.expectEqual(@as(c_int, 0), rel_add_tupleset(r1, &ts));
    tupleset.ts_sort(&ts);
    try std.testing.expectEqual(@as(c_int, 0), rel_build_from_tupleset(r2, &ts));

    for (rows) |row| {
        try std.testing.expectEqual(@as(c_int, 1), rel_exact(r1, &row));
        try std.testing.expectEqual(@as(c_int, 1), rel_exact(r2, &row));
    }
    // Order-statistics view identical to the bulk build's.
    try std.testing.expectEqual(rel_rank(r2, &.{ 1, 3 }), rel_rank(r1, &.{ 1, 3 }));
    var ord1: [4][2]u32 = undefined;
    var ord2: [4][2]u32 = undefined;
    var k: u64 = 0;
    while (k < 4) : (k += 1) {
        var a: [2]u32 = undefined;
        var b: [2]u32 = undefined;
        try std.testing.expectEqual(@as(c_int, 0), rel_select(r1, k, &a));
        try std.testing.expectEqual(@as(c_int, 0), rel_select(r2, k, &b));
        ord1[@intCast(k)] = a;
        ord2[@intCast(k)] = b;
    }
    try std.testing.expectEqualSlices([2]u32, &ord1, &ord2);

    // Empty tupleset: no-op, still ok.
    var empty: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), tupleset.ts_init(&empty, 2));
    defer tupleset.ts_free(&empty);
    try std.testing.expectEqual(@as(c_int, 0), rel_add_tupleset(r1, &empty));

    // Arity mismatch rejected.
    var bad: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), tupleset.ts_init(&bad, 3));
    defer tupleset.ts_free(&bad);
    try std.testing.expectEqual(@as(c_int, -1), rel_add_tupleset(r1, &bad));

    // Null ts rejected.
    try std.testing.expectEqual(@as(c_int, -1), rel_add_tupleset(r1, null));
}

test "rel_reset_view split + build_from_tupleset" {
    const r = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(r);

    // EDB: base == view
    try std.testing.expectEqual(@as(c_int, 0), rel_is_idb(r));

    // First reset SPLITs base from view.
    try std.testing.expectEqual(@as(c_int, 0), rel_reset_view(r));
    try std.testing.expectEqual(@as(c_int, 1), rel_is_idb(r));

    // Add a derived fact to the view only.
    try std.testing.expectEqual(@as(c_int, 1), rel_add(r, &.{ 9, 9 }));
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(r, &.{ 9, 9 }));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact_base(r, &.{ 9, 9 }));

    // Reset again: view = copy of base, dropping derived facts.
    try std.testing.expectEqual(@as(c_int, 0), rel_reset_view(r));
    try std.testing.expectEqual(@as(c_int, 0), rel_exact(r, &.{ 9, 9 }));
    try std.testing.expectEqual(@as(c_int, 1), rel_is_idb(r));
}

test "rel_add_tupleset bulk-builds an empty view, folds into a populated one" {
    const r = rel_create(1) orelse return error.OutOfMemory;
    defer rel_free(r);
    // IDB shape: base and view both empty (what the fixpoint's per-round
    // rel_reset_view leaves behind — the batch then hits the bulk build).
    try std.testing.expectEqual(@as(c_int, 0), rel_reset_view(r));

    // SORTED batch (the fixpoint's commit sorts it): takes the no-copy path.
    // Duplicates inside the batch collapse — a rule deriving the same head
    // tuple from two body bindings must insert it once.
    var ts: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), tupleset.ts_init(&ts, 1));
    defer tupleset.ts_free(&ts);
    const rows = [_]u32{ 3, 1, 2 };
    for (rows) |v| try std.testing.expectEqual(@as(c_int, 1), tupleset.ts_add(&ts, &.{v}));
    try std.testing.expectEqual(@as(c_int, 0), tupleset.ts_add(&ts, &.{2})); // duplicate
    tupleset.ts_sort(&ts);

    const rev0 = rel_rev(r);
    try std.testing.expectEqual(@as(c_int, 0), rel_add_tupleset(r, &ts));
    // NOTE: rel_count is the final-STATE count (final states merge), not the
    // word count — count words through rel_prefix.
    var nw: c_long = 0;
    try std.testing.expectEqual(@as(c_long, 3), rel_prefix(r, null, 0, countCb, &nw));
    try std.testing.expectEqual(@as(c_long, 3), nw);
    // One rev bump per inserted fact, exactly as the per-tuple loop does.
    try std.testing.expectEqual(@as(u64, 3), rel_rev(r) - rev0);
    for (rows) |v| try std.testing.expectEqual(@as(c_int, 1), rel_exact(r, &.{v}));

    // Populated view + UNSORTED batch: the defensive copy path.  One tuple is
    // already present (a no-op insert), one is new.
    var ts2: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), tupleset.ts_init(&ts2, 1));
    defer tupleset.ts_free(&ts2);
    try std.testing.expectEqual(@as(c_int, 1), tupleset.ts_add(&ts2, &.{5}));
    try std.testing.expectEqual(@as(c_int, 1), tupleset.ts_add(&ts2, &.{2}));

    const rev1 = rel_rev(r);
    try std.testing.expectEqual(@as(c_int, 0), rel_add_tupleset(r, &ts2));
    nw = 0;
    try std.testing.expectEqual(@as(c_long, 4), rel_prefix(r, null, 0, countCb, &nw));
    try std.testing.expectEqual(@as(u64, 1), rel_rev(r) - rev1); // only {5} was new
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(r, &.{5}));
}

test "rel_build_from_tupleset bulk build" {
    const r = rel_create(2) orelse return error.OutOfMemory;
    defer rel_free(r);

    var ts: tupleset.tuple_set = undefined;
    try std.testing.expectEqual(@as(c_int, 0), tupleset.ts_init(&ts, 2));
    defer tupleset.ts_free(&ts);

    _ = tupleset.ts_add(&ts, &.{ 3, 1 });
    _ = tupleset.ts_add(&ts, &.{ 1, 2 });
    _ = tupleset.ts_add(&ts, &.{ 2, 2 });
    tupleset.ts_sort(&ts);

    try std.testing.expectEqual(@as(c_int, 0), rel_build_from_tupleset(r, &ts));
    // Merged final states (see roundtrip test above), not word count.
    try std.testing.expectEqual(@as(u64, 1), rel_count(r));
    try std.testing.expectEqual(@as(c_int, 1), rel_exact(r, &.{ 3, 1 }));
    try std.testing.expectEqual(@as(c_int, 0), rel_is_idb(r)); // still aliased
}
