//! dafsa_c.zig — hand-declared C ABI surface of the (Zig) DAFSA engine.
//!
//! The vendored dafsa engine is now 100% Zig (vendor/dafsa/zig/src/*.zig,
//! exported via abi.zig).  Its C-internal header (dafsa_internal.h) is gone,
//! so datalog modules can no longer @cImport it.  This module is the typed
//! stand-in for that removed header: it hand-declares (callconv(.c)) the
//! engine's C-layout structs (struct dafsa / struct dafsa_view / State /
//! Edge / TransHeap — byte-identical to abi.zig's CFacade/CViewFacade and
//! internal.zig's State/Edge) and every exported ABI function the ported
//! modules call, plus the exported WAL/crc32 helpers.
//!
//! Layouts are copied 1:1 from the engine's real types (abi.zig CFacade at
//! vendor/dafsa/zig/src/abi.zig:96-130, internal.zig State/Edge at
//! internal.zig:30-71, wal.zig Wal at wal.zig:41-44).  Pointer fields use the
//! `[*c]` C-pointer spelling translate-c used, so null-checks / pointer
//! arithmetic / `.field` derefs in the ported code compile unchanged.
//!
//! The actual symbols are exported by abi.zig; these `extern "c" fn`s link
//! against them inside the same .so (and, for the unit-test binary, through
//! the dafsa_abi import tests.zig wires up).

const std = @import("std");
const c = std.c;

// ─── struct dafsa_wal (dafsa_internal.h:119 / wal.zig Wal) ────────────────
// extern so field order is ABI-exact (fd@0, size@8) — rel_compact reads
// ->fd / ->size straight off the handle dafsa_wal_open_* returns.
pub const dafsa_wal = extern struct {
    fd: c_int,
    size: u64,
};

// ─── State / Edge / TransHeap (dafsa_internal.h:42-74 / internal.zig) ─────
pub const Edge = extern struct {
    sym: u8,
    target: u32,
};

pub const Inode = extern struct {
    parent: u32,
    sym: u8,
    next: u32,
};

// `edges[]` flexible-array header.  Engine stores edges at base+@sizeOf —
// `_edges` (a [0]Edge member) lands at the same offset 4, matching the
// translate-c shape the ported code already reads.
pub const TransHeap = extern struct {
    cap: u32,
    _edges: [0]Edge,
};

pub const State = extern struct {
    refcount: u32, // @0
    is_final: u8, // @4
    _pad0: [3]u8, // @5
    ntrans: u32, // @8
    in_head: u32, // @12
    sig: u64, // @16
    trans_heap: [*c]TransHeap, // @24 (NULL => ≤4 edges inline)
    trans: [4]Edge, // @32
};

comptime {
    if (@sizeOf(State) != 64) @compileError("dafsa_c State must be 64B (engine internal.zig)");
}

// ─── struct dafsa (dafsa_internal.h:76-115 / abi.zig CFacade) ─────────────
// Only the datalog engine reads specific fields (states/initial/subtree_valid);
// every field is declared so offsets (and thus the fields that matter) are
// exact.  `impl` (past C's sizeof) is omitted — datalog never reads it.
pub const dafsa = extern struct {
    nstates: u32,
    initial: u32,
    states: [*c]State,
    states_cap: usize,
    inodes: [*c]Inode,
    inodes_cap: usize,
    inodes_used: u32,
    reg_keys: [*c]u64,
    reg_vals: [*c]u32,
    reg_cap: usize,
    reg_used: usize,
    reg_probes: u64,
    spath: [*c]u32,
    schars: [*c]u8,
    sparents: [*c]u32,
    scratch_cap: usize,
    free_head: u32,
    subtree: [*c]u64,
    subtree_cap: usize,
    subtree_valid: c_int,
};

// ─── struct dafsa_view (dafsa_internal.h:141-152 / abi.zig CViewFacade) ───
pub const dafsa_view = extern struct {
    map: [*c]u8,
    map_len: usize,
    n_states: u32,
    initial: u32,
    final_bits: [*c]const u8,
    csr: [*c]const u8,
    state_off: [*c]u64,
    ov: ?*anyopaque,
};

// dafsa_stats_out (dafsa.h / abi.zig CStatsOut).
pub const dafsa_stats_out = extern struct {
    n_states_total: u32,
    n_states_reachable: u32,
    n_final: u32,
    n_trans: u32,
    register_probes: u64,
};

// ─── C callback types (dafsa.h:55 / dafsa_wal_replay_cb) ──────────────────
pub const DafsaEnumCb = ?*const fn (payload: [*c]const u8, len: usize, user: ?*anyopaque) callconv(.c) c_int;
pub const DafsaWalReplayCb = ?*const fn (op: u8, key: [*c]const u8, key_len: u32, user: ?*anyopaque) callconv(.c) c_int;

// ─── Tunable constants (dafsa_internal.h / internal.zig) ──────────────────
pub const MAX_WORD_LEN: usize = 65536;
pub const DAFSA_WAL_OP_ADD: u8 = 1;
pub const DAFSA_WAL_OP_DEL: u8 = 2;

// ─── Lifecycle / bulk build / core ───────────────────────────────────────
pub extern "c" fn dafsa_create() ?*dafsa;
pub extern "c" fn dafsa_free(d: ?*dafsa) void;
pub extern "c" fn dafsa_load(path: [*c]const u8) ?*dafsa;
pub extern "c" fn dafsa_save(d: ?*const dafsa, path: [*c]const u8) c_int;
pub extern "c" fn dafsa_build_sorted(keys: [*c]const [*c]const u8, lens: [*c]const usize, nkeys: usize) ?*dafsa;
pub extern "c" fn dafsa_stats(d: ?*const dafsa, out: [*c]dafsa_stats_out) void;
pub extern "c" fn dafsa_add_n(d: ?*dafsa, key: [*c]const u8, len: usize) c_int;
pub extern "c" fn dafsa_lookup_n(d: ?*const dafsa, key: [*c]const u8, len: usize) c_int;
pub extern "c" fn dafsa_delete_n(d: ?*dafsa, key: [*c]const u8, len: usize) c_int;

// ─── Order statistics (dafsa_internal.h:218-235) ──────────────────────────
pub extern "c" fn dafsa_ensure_subtree(d: ?*dafsa) u64;
pub extern "c" fn dafsa_rank_n(d: ?*dafsa, key: [*c]const u8, len: usize) u64;
pub extern "c" fn dafsa_rank_from(d: ?*dafsa, s: u32, key: [*c]const u8, len: usize) u64;
pub extern "c" fn dafsa_select_n(d: ?*dafsa, k: u64, key_out: [*c]u8, key_cap: usize) c_int;
pub extern "c" fn dafsa_select_from(d: ?*dafsa, s: u32, k: u64, key_out: [*c]u8, key_cap: usize) c_int;
pub extern "c" fn dafsa_range_count_n(d: ?*dafsa, lo: [*c]const u8, lo_len: usize, hi: [*c]const u8, hi_len: usize) u64;
pub extern "c" fn dafsa_range_count_from(d: ?*dafsa, s: u32, lo: [*c]const u8, lo_len: usize, hi: [*c]const u8, hi_len: usize) u64;

// ─── State transition helper (dafsa_internal.h:178) ───────────────────────
pub extern "c" fn trans_find(s: ?*const State, c: u8) c_int;

// ─── Per-relation / WAL (dafsa_internal.h:119, dafsa_wal.c) ───────────────
pub extern "c" fn dafsa_wal_open(path: [*c]const u8) ?*dafsa_wal;
pub extern "c" fn dafsa_wal_open_rw(path: [*c]const u8) ?*dafsa_wal;
pub extern "c" fn dafsa_wal_open_ro(path: [*c]const u8) ?*dafsa_wal;
pub extern "c" fn dafsa_wal_close(w: ?*anyopaque) void;
pub extern "c" fn dafsa_wal_sync(w: ?*anyopaque) c_int;
pub extern "c" fn dafsa_wal_size(w: ?*const anyopaque) u64;
pub extern "c" fn dafsa_wal_append_add(w: ?*anyopaque, key: [*c]const u8, key_len: u32) c_int;
pub extern "c" fn dafsa_wal_append_del(w: ?*anyopaque, key: [*c]const u8, key_len: u32) c_int;
pub extern "c" fn dafsa_wal_replay(w: ?*anyopaque, cb: DafsaWalReplayCb, user: ?*anyopaque) c_int;

// ─── Zero-copy view (dafsa.h:62-68 / dafsa_view.c) ────────────────────────
pub extern "c" fn dafsa_view_open(path: [*c]const u8) ?*dafsa_view;
pub extern "c" fn dafsa_view_close(v: ?*anyopaque) void;
pub extern "c" fn dafsa_view_lookup_n(v: ?*const anyopaque, key: [*c]const u8, len: usize) c_int;
pub extern "c" fn view_trans_find(v: ?*const dafsa_view, s: u32, sym: u8, target_out: [*c]u32) c_int;
pub extern "c" fn view_edge_next(v: ?*const dafsa_view, s: u32, cursor: *[*c]const u8, sym_out: [*c]u8, target_out: [*c]u32) c_int;
pub extern "c" fn view_enum_dfs(v: ?*const dafsa_view, state: u32, buf: [*c]u8, depth: usize, cb: DafsaEnumCb, user: ?*anyopaque, count: [*c]c_long) c_int;

// ─── View order statistics (dafsa_internal.h:257-265) ─────────────────────
pub extern "c" fn dafsa_view_subtree_counts(v: ?*const dafsa_view, counts_out: [*c]?[*]u64) u64;
pub extern "c" fn dafsa_view_rank_n(v: ?*const dafsa_view, key: [*c]const u8, len: usize) u64;
pub extern "c" fn dafsa_view_select_n(v: ?*const dafsa_view, k: u64, key_out: [*c]u8, key_cap: usize) c_int;
pub extern "c" fn dafsa_view_range_count_n(v: ?*const dafsa_view, lo: [*c]const u8, lo_len: usize, hi: [*c]const u8, hi_len: usize) u64;

// ─── crc32 helper (dafsa_internal.h:242) ──────────────────────────────────
pub extern "c" fn crc32_compute(data: [*c]const u8, len: usize) u32;

// Force the externs to be referenced (they resolve against the engine's
// abi.zig exports at link time even when a module only uses the types).
comptime {
    _ = c;
    _ = std;
}
