//! dl.zig — port of src/dl.c (the dl_db integration layer / public API glue).
//!
//! This is the facade that ties every ported module together: lifecycle
//! (dl_open/open2/open_ro/close), relation declare (fixed + variadic), CSV
//! fact loading, incremental add/delete with per-relation WAL, the CAS
//! revision API + transaction buffer, interner/term-store access, the query
//! family (lookup/prefix/rank/select/range_count + _perm/_bound/count), rule
//! loading + compilation, snapshot publish (with the IVM/DRed/aggregate
//! incremental-maintenance dispatch cascade), permutation-index management,
//! fault hooks, graph traversal, node observations and the regex pattern
//! query.
//!
//! Strangler-hybrid ABI: `struct dl_db` is OPAQUE in dl.h but its
//! AUTHORITATIVE layout lives in src/dl_internal.h (still C, still
//! @cImport'd by the retained dl_cli.c / analyze.c).  dl.zig owns the
//! AUTHORITATIVE Zig definition as a native extern struct that MUST stay
//! byte-for-byte identical to that header — comptime-gated against
//! @cImport("dl_internal.h")'s translate-c layout via @sizeOf/@offsetOf.
//! The still-C consumers and every ported module (vm.zig/permindex.zig/
//! snapshot.zig/iter.zig/compiler.zig, each with its OWN
//! @cImport of dl_internal.h) all dereference the SAME byte layout, so this
//! one mirror is the single point of truth for the migration.
//!
//! Every ported module is reached through @import for its pub types and
//! pub-export functions, or through raw `extern "c" fn` bindings where the
//! callee's parameter type is a module-private @cImport of dl_db (each
//! translate-c produces a distinct type for the same C struct).
//!
//! Oracle: src/dl.c (never modified).  All allocation/file I/O goes through
//! raw libc like the C oracle.
const std = @import("std");
const builtin = @import("builtin");
const c = std.c;

const relation = @import("relation.zig");
const vrelation = @import("vrelation.zig");
const txnwal = @import("txnwal.zig");
const tupleset = @import("tupleset.zig");
const parser = @import("parser.zig");
const compiler = @import("compiler.zig");
const regexwalk = @import("regexwalk.zig");
const schema_mod = @import("schema.zig");
const wfs = @import("wfs.zig");
const intern_mod = @import("intern.zig");

// dl.zig OWNS the authoritative Zig layouts of dl_db/rel_entry/txn/txn_op/
// view_cache_slot/perm_index_entry (DlDb/RelEntry/Txn/TxnOp/ViewCacheSlot/
// PermIndexEntry below) and the dl.h/db_* public entry points.  The C-shaped
// mirror in dl_abi.zig exists ONLY for the out-of-library executables
// (dl_cli.zig) and the abi_witness comptime oracle — the in-library
// modules name these owner types and call these functions directly.

// dafsa_internal.h: struct dafsa/dafsa_view + dafsa_save/dafsa_view_open/
// dafsa_view_close.  The engine is now Zig; dafsa_c.zig hand-declares the
// C-layout types + extern ABI in place of the removed header.
const dc = @import("dafsa_c.zig");

// POSIX filesystem/lock primitives (stat/lstat/fstatat/mkdir/open/fcntl/
// flock/opendir/readdir/dirfd/rename/unlink/rmdir) with _GNU_SOURCE so
// glibc exposes the full declarations (stat/lstat/fstatat/dirfd/S_ISDIR).
//
// TARGET-GATED struct stat (the Zig 0.16 translate-c blocker): musl's
// sys/stat.h does not survive translation (struct timespec demotes to
// opaque), so on musl the KERNEL <asm/stat.h> is used instead — its layout
// is byte-for-byte identical to the libc struct on x86_64 (MEASURED; the
// comptime guard below pins it at every compile).  On glibc the plain
// sys/stat.h is kept: glibc headers already define `struct stat`, and
// including asm/stat.h too would be a redefinition error.  The comptime
// guard asserts BOTH paths give the same layout, so the musl and gnu
// builds are the same ABI by construction.  S_IFMT/S_ISDIR come from
// sys/stat.h on gnu and linux/stat.h on musl (asm/stat.h has only the
// struct, not the macros).
const musl_ilp32 = builtin.target.abi.isMusl() and @sizeOf(usize) == 4;

const posix = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    if (builtin.target.abi.isMusl()) {
        if (musl_ilp32) {
            @cInclude("linux/stat.h"); // S_IFMT/S_ISDIR macros; the struct is Stat below
        } else {
            @cInclude("asm/stat.h");
            @cInclude("linux/stat.h");
        }
    } else {
        @cInclude("sys/stat.h");
    }
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
    @cInclude("dirent.h");
    @cInclude("stdio.h");
});

// THE TARGET-CORRECT struct stat.
//
// x86_64: the kernel <asm/stat.h> struct is byte-identical to libc's, so it is
// used as a drop-in (only the field names differ).
//
// i386: it is NOT a drop-in.  <asm/stat.h> there declares the OLD 16-bit-field
// kernel struct (sizeof 68), while musl's libc fills a 144-byte struct — a
// buffer overflow plus silently wrong modes/sizes.  Neither can musl's own
// <sys/stat.h> be used: Zig 0.16's translate-c turns its `struct timespec`
// (an unnamed bitfield) into `opaque {}` and takes struct stat with it.  So the
// layout is declared here, MEASURED with `zig cc -target x86-linux-musl` against
// the target's own <sys/stat.h>: sizeof 144; dev 0 ino 88 nlink 20 mode 16 uid 24
// gid 28 rdev 32 size 44 blksize 52 blocks 56, __st_*tim32 64/72/80, atim 96
// mtim 112 ctim 128 (each 16 bytes, tv_nsec at +8).
const Stat = if (musl_ilp32) StatI386Musl else posix.struct_stat;

const StatTimespec = extern struct { tv_sec: i64, tv_nsec: c_long, _pad: c_long };

pub const StatI386Musl = extern struct {
    st_dev: u64, // @0
    __st_dev_padding: c_int, // @8
    __st_ino_truncated: c_long, // @12
    st_mode: c_uint, // @16
    st_nlink: c_uint, // @20
    st_uid: c_uint, // @24
    st_gid: c_uint, // @28
    st_rdev: u64, // @32
    __st_rdev_padding: c_int, // @40
    st_size: i64, // @44
    st_blksize: c_long, // @52
    st_blocks: i64, // @56
    __st_atim32: [2]c_long, // @64
    __st_mtim32: [2]c_long, // @72
    __st_ctim32: [2]c_long, // @80
    st_ino: u64, // @88
    st_atim: StatTimespec, // @96
    st_mtim: StatTimespec, // @112
    st_ctim: StatTimespec, // @128
};

// STAT LAYOUT GUARD: the struct passed to stat/lstat/fstatat MUST be the struct
// libc fills — a mismatch is a silent-wrong-result bug (wrong modes/sizes), not
// a compile error — so pin sizeof and every offset read below at comptime.
comptime {
    const S = Stat;
    const want: usize = if (musl_ilp32) 144 else 144;
    if (@sizeOf(S) != want)
        @compileError("struct stat layout drift on target '" ++ @tagName(builtin.target.cpu.arch) ++
            "': sizeof mismatch — the 144-byte layout here is MEASURED only for x86_64-musl (kernel <asm/stat.h>) and i386-musl (the StatI386Musl declaration); another 32-bit musl target needs its own measured branch");
    const expect = if (musl_ilp32) .{
        .{ "st_dev", 0 }, .{ "st_mode", 16 }, .{ "st_size", 44 }, .{ "st_ino", 88 },
    } else .{
        .{ "st_dev", 0 }, .{ "st_ino", 8 }, .{ "st_nlink", 16 }, .{ "st_mode", 24 },
        .{ "st_uid", 28 }, .{ "st_gid", 32 }, .{ "st_rdev", 40 }, .{ "st_size", 48 },
        .{ "st_blksize", 56 }, .{ "st_blocks", 64 },
    };
    for (expect) |e| {
        if (@offsetOf(S, e[0]) != e[1])
            @compileError("struct stat layout drift: " ++ e[0] ++ " offset mismatch");
    }
}

// libc stat-family calls + mkdir over `Stat`.  i386 musl __REDIRs the whole
// family to the time64 symbols (sys/stat.h: stat->__stat_time64,
// lstat->__lstat_time64, fstatat->__fstatat_time64); a bare `extern fn fstatat`
// binds the OLD-time compat entry point, which was MEASURED to fill only 96 of
// the 144 bytes (the 64-bit-time tail untouched) — no error, garbage mtimes.
// Bind the same symbol a C compile would.
const stat_syms = struct {
    extern fn stat(path: [*c]const u8, buf: *Stat) c_int;
    extern fn lstat(path: [*c]const u8, buf: *Stat) c_int;
    extern fn fstatat(dirfd: c_int, path: [*c]const u8, buf: *Stat, flags: c_int) c_int;
};
const stat_syms_time64 = struct {
    extern fn __stat_time64(path: [*c]const u8, buf: *Stat) c_int;
    extern fn __lstat_time64(path: [*c]const u8, buf: *Stat) c_int;
    extern fn __fstatat_time64(dirfd: c_int, path: [*c]const u8, buf: *Stat, flags: c_int) c_int;
};
const stat = if (musl_ilp32) &stat_syms_time64.__stat_time64 else &stat_syms.stat;
const lstat = if (musl_ilp32) &stat_syms_time64.__lstat_time64 else &stat_syms.lstat;
const fstatat = if (musl_ilp32) &stat_syms_time64.__fstatat_time64 else &stat_syms.fstatat;
extern fn mkdir(path: [*c]const u8, mode: posix.mode_t) c_int;

// ─── libc decls not in std.c (precedent: vm.zig / snapshot.zig) ────────────
extern "c" fn strdup(s: [*c]const u8) ?[*:0]u8;
extern "c" fn strcmp(a: [*c]const u8, b: [*c]const u8) c_int;
extern "c" fn strlen(s: [*c]const u8) usize;
extern "c" fn strncmp(a: [*c]const u8, b: [*c]const u8, n: usize) c_int;
extern "c" fn strstr(haystack: [*c]const u8, needle: [*c]const u8) [*c]u8;
extern "c" fn strtoul(nptr: [*c]const u8, endptr: [*c][*c]u8, base: c_int) c_ulong;
extern "c" fn getenv(name: [*c]const u8) ?[*:0]const u8;
extern "c" fn snprintf(buf: [*c]u8, size: usize, fmt: [*c]const u8, ...) c_int;
extern "c" fn fprintf(stream: *c.FILE, fmt: [*c]const u8, ...) c_int;
extern "c" fn getline(lineptr: *?[*]u8, n: *usize, stream: *c.FILE) isize;
extern "c" fn fputs(s: [*:0]const u8, stream: *c.FILE) c_int;
extern "c" fn nanosleep(req: *const Timespec, rem: ?*Timespec) c_int;

// ─── Constants (mirror src/dl_internal.h / dl.h / compiler.h / txnwal.h) ───

pub const MAX_RELS: usize = 64;
const MAX_VAR_ARITY: u8 = 8;
const MAX_PERMS: usize = 64;
pub const DL_VIEW_CACHE_SZ: usize = 8;

pub const RELK_FIXED: u8 = 0;
pub const RELK_VARIADIC: u8 = 1;

const TXN_ADD: c_int = 1;
const TXN_DEL: c_int = 2;
const TXN_CAS: c_int = 3;

const DL_E_LOCKED: c_int = 1;
const DL_E_CONFLICT: c_int = 2;

const TXNWAL_OP_ADD: u8 = 1;
const TXNWAL_OP_DEL: u8 = 2;

const DL_FPOINT_AFTER_REL_SAVE: c_int = 0;
const DL_FPOINT_AFTER_RENAME: c_int = 1;
const DL_FPOINT_TXN_BEFORE_MARKER: c_int = 2;
const DL_FPOINT_DERIVED_SAVE: c_int = 3; // opt-in derived symbols.dafsa, before the write

// compiler.h opcodes consulted by db_has_list_builtin / db_has_range_builtin.
const OP_LIST_CONS: u8 = 20;
const OP_LIST_CAR: u8 = 21;
const OP_LIST_CDR: u8 = 22;
const OP_LIST_APPEND: u8 = 23;
const OP_LIST_MEMBER: u8 = 24;
const OP_RANGE: u8 = 25;

// ─── Callback types (mirror dl.h typedefs) ─────────────────────────────────

/// dl_tuple_cb == rel_enum_cb — identical C signature, shared type identity
/// with relation.zig so rel_prefix/vrel_prefix calls need no cast.
const DlTupleCb = relation.RelEnumCb;
pub const DlRelationCb = ?*const fn (name: [*c]const u8, arity: u8, idb: c_int, user: ?*anyopaque) callconv(.c) c_int;
const DlTraverseCb = ?*const fn (node_sym: u32, depth: u8, user: ?*anyopaque) callconv(.c) c_int;
const DlStrCb = ?*const fn (s: [*c]const u8, user: ?*anyopaque) callconv(.c) c_int;
const DlFuzzyCb = ?*const fn (sym_id: u32, name: [*c]const u8, name_len: usize, dist: u32, user: ?*anyopaque) callconv(.c) c_int;
const FaultHook = ?*const fn (fp: c_int, user: ?*anyopaque) callconv(.c) c_int;

// ─── Public C-ABI structs (src/dl_internal.h, byte-for-byte) ───────────────

/// typedef struct { char *name; uint8_t kind; uint8_t arity;
///                   relation *rel; vrelation *vrel; } rel_entry
pub const RelEntry = extern struct {
    name: ?[*:0]u8,
    kind: u8, // RELK_FIXED or RELK_VARIADIC
    arity: u8, // RELK_FIXED: the fixed arity; RELK_VARIADIC: 0
    rel: ?*relation.Relation, // RELK_FIXED only
    vrel: ?*vrelation.Vrelation, // RELK_VARIADIC only
};

/// typedef struct txn_op — TXN_ADD/DEL/CAS buffered operation.
pub const TxnOp = extern struct {
    kind: c_int, // TXN_ADD / TXN_DEL / TXN_CAS
    rel_id: c_int,
    arity: u8,
    cols: [8]u32,
    entity_sym: u32, // TXN_CAS: interned entity symbol
    expected: u32,
    next: u32,
};

/// typedef struct txn — dynamically grown op buffer.
pub const Txn = extern struct {
    ops: ?[*]TxnOp,
    nops: usize,
    cap: usize,
    // U-homog: rollback snapshot of every relation's col_kind[0..arity) at
    // begin-time.  dl_txn_add_fact records kinds at BUFFER time (rows apply
    // at commit), so WITHOUT this a rolled-back txn leaves its recorded
    // kind behind (MEASURED: txn add int 77, rollback, then a sym CSV load
    // is rejected forever).  Rollback restores the snapshot; the commit
    // success path never reads it.  Set of (rel,arity) pairs actually
    // snapshotted by txn_add_fact (MAX_RELS mirrors dl_internal).
    ksnap_rels: [MAX_RELS]c_int = [_]c_int{-1} ** MAX_RELS,
    ksnap_arities: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS,
    ksnap_kinds: [8 * MAX_RELS]u8 = [_]u8{0} ** (8 * MAX_RELS),
    ksnap_n: usize = 0,
    // B2 (review): begin-time meta_dirty.  A buffer-time kindNoteRaw sets
    // meta_dirty=1 AND ensureRevRel (dl_txn_cas) flushes rels.txt eagerly
    // via dl_declare_relation, so by the time commit fails the poisoned
    // kind may already be ON DISK.  Abort restores the kinds in memory,
    // rewrites rels.txt when any kind actually changed, and restores this
    // begin-time flag (an unconditional clear would drop a LEGITIMATE
    // pre-txn unflushed kind's pending flush).
    meta_dirty_at_begin: c_int = 0,
};

/// view_cache_slot (snapshot.h) — rel_name[64] + view* + used + version.
pub const ViewCacheSlot = extern struct {
    rel_name: [64]u8,
    view: ?*anyopaque, // dafsa_view*
    used: c_int,
    // The snapshot version the view was opened from.  A view maps ONE
    // version's bytes, so a slot is only reusable for the same
    // (rel_name, version) — keying on the name alone hands a v2 read v1's data.
    version: u32,
};

/// perm_index_entry (permindex.h).
pub const PermIndexEntry = extern struct {
    rel_id: c_int,
    arity: u8,
    perm: [8]u8,
    pidx_rel: ?*relation.Relation,
    dirty: c_int,
};

/// The authoritative dl_db — MUST stay byte-identical to src/dl_internal.h.
pub const DlDb = extern struct {
    dir: ?[*:0]u8,
    ir: ?*anyopaque, // interner*
    terms: ?*anyopaque, // termstore*
    rels: [MAX_RELS]RelEntry,
    nrels: usize,
    lock_fd: c_int,
    read_only: c_int,
    meta_dirty: c_int,
    rev_rel_id: c_int,
    txn: ?*Txn,
    schema: ?*const schema_mod.dl_schema,
    crules: ?[*]?*compiler.compiled_rule,
    n_crules: c_int,
    fixpoint_dirty: c_int,
    snap_version: u32,
    snapshot_retain: c_uint,
    vcache: [DL_VIEW_CACHE_SZ]ViewCacheSlot,
    fault_hook: FaultHook,
    fault_user: ?*anyopaque,
    perms: [MAX_PERMS]PermIndexEntry,
    n_perms: c_int,
    ast_rules: ?[*]?*parser.rule,
    n_ast_rules: c_int,
    full_reeval_pending: c_int,
    delta_pending: [MAX_RELS]?*tupleset.tuple_set,
    del_pending: [MAX_RELS]?*tupleset.tuple_set,
    // ─── selfreg-dl-storage (beyond C's sizeof(struct dl_db)) ─────────────
    compact_checks: u64 = 0, // add counter for throttled compaction checks
    access_epoch: u32 = 0, // C20: current consolidation epoch (cycle count)
    // ─── publish gate (beyond C's sizeof) ─────────────────────────────────
    // saved_rev[i] is the relation.revs content revision already serialized
    // into the last published snapshot.  SAVED_REV_NONE (max u64) means
    // "no snapshot from this process has serialized it yet" — forces a full
    // save on the first publish after open.  It is a PER-PROCESS watermark
    // (re-armed on every open), so it only covers relations whose content
    // lives in memory; the cross-process half of the gate is
    // relation.rel_content_file (an unmutated relation is .deferred/.view and
    // is hardlinked from its live file — see relSaveGated).
    saved_rev: [MAX_RELS]u64 = @splat(SAVED_REV_NONE),
    saved_rev_var: [MAX_RELS][MAX_VAR_ARITY + 1]u64 = @splat(@splat(SAVED_REV_NONE)),
    saved_rev_perm: [MAX_PERMS]u64 = @splat(SAVED_REV_NONE),
    // Identity half of the perm gate: a perm index is FREED and re-created
    // on rebuild (permindex_build), so its rev counter restarts at 0 and a
    // rev-only watermark could alias a stale value.  The saved pointer
    // detects the restart; rev alone suffices for fixed relations and
    // variadic variants (created once per process, never re-created).
    saved_perm_rel: [MAX_PERMS]?*anyopaque = @splat(null),
    // ─── selfreg periodic snapshot (beyond C's sizeof) ────────────────────
    // snapshot_budget_bytes: WAL-append byte budget for dl_consolidate's
    // auto-materialization (0 = never auto-snapshot; set via
    // dl_set_snapshot_budget).  wal_appended_at_snapshot is the sum of
    // rel_wal_appended() across relations at the last materialization; the
    // trigger fires when sum(rel_wal_appended) - watermark >= budget.
    snapshot_budget_bytes: u64 = 0,
    wal_appended_at_snapshot: u64 = 0,
    // ─── VERSION pattern queries on a symbol-DFA-less snapshot (beyond C) ──
    // The snapshot's OWN interner, rebuilt from its symbols.array on first use
    // and reused while snap_version is unchanged (freed by dl_close, replaced
    // when snap_version moves).  Never ASSUMED to be the live interner: sym ids
    // are reused across a publish, so the live map is not the snapshot's map.
    // It IS the live interner when the snapshot's manifest stamp proves the two
    // vocabularies identical (snapshotInterner) — then snap_sym_shared is set
    // and this handle must NOT be freed.
    snap_sym_ir: ?*anyopaque = null,
    snap_sym_version: u32 = 0,
    snap_sym_shared: bool = false,
};

/// Marker for "not yet serialized by any publish of this process".
const SAVED_REV_NONE: u64 = std.math.maxInt(u64);

// The owner-vs-mirror layout assertions live in abi_witness.zig (imported only
// from tests.zig so the single @cImport("dl_internal.h") stays out of the
// lib/exe module graphs).

// ─── Ported-module extern bindings (callees whose db type is a private ────
// ─── @cImport'd dl_db, or whose module-private types are opaque) ────────────

// intern.zig (Interner is module-private) — bind with anyopaque handles.
extern "c" fn intern_free(ir: ?*anyopaque) void;
extern "c" fn intern_str_find(ir: ?*anyopaque, str: [*c]const u8) u32;
extern "c" fn intern_str(ir: ?*anyopaque, str: [*c]const u8) u32;
extern "c" fn intern_str_of(ir: ?*anyopaque, sym_id: u32) ?[*:0]const u8;
extern "c" fn intern_fwd(ir: ?*anyopaque) ?*const anyopaque; // const dafsa*
extern "c" fn intern_fwd_mutable(ir: ?*anyopaque) ?*const anyopaque;
extern "c" fn intern_save(ir: ?*anyopaque, fwd_path: [*c]const u8, rev_path: [*c]const u8) c_int;
extern "c" fn intern_load(fwd_path: [*c]const u8, rev_path: [*c]const u8) ?*anyopaque;
extern "c" fn intern_is_dirty(ir: ?*anyopaque) c_int;

// termstore.zig (Termstore is module-private).
extern "c" fn term_free(t: ?*anyopaque) void;
extern "c" fn term_is_list(t: ?*anyopaque, v: u32) c_int;
extern "c" fn term_car(t: ?*anyopaque, h: u32) u32;
extern "c" fn term_cdr(t: ?*anyopaque, h: u32) u32;
extern "c" fn term_cons(t: ?*anyopaque, head: u32, tail: u32) u32;
extern "c" fn term_append(t: ?*anyopaque, a: u32, b: u32) u32;
extern "c" fn term_is_dirty(t: ?*anyopaque) c_int;
extern "c" fn term_save(t: ?*anyopaque, path: [*c]const u8) c_int;
extern "c" fn term_load(path: [*c]const u8) ?*anyopaque;

// parser.zig (export fn, not pub — parser is opaque; rule is pub via import).
extern "c" fn parse_create(source: [*c]const u8) ?*anyopaque;
extern "c" fn parse_rules(p: ?*anyopaque, n_rules: ?*c_int) ?[*]?*parser.rule;
extern "c" fn parse_free(p: ?*anyopaque) void;
extern "c" fn rule_free(r: ?*parser.rule) void;
extern "c" fn expr_free(e: ?*parser.expr) void;
extern "c" fn expr_clone(e: ?*const parser.expr) ?*parser.expr;

// compiler.zig — compile_rules is its `pub export fn` (takes ?*DlDb).
extern "c" fn compile_rules(db: ?*DlDb, rules: ?[*]?*parser.rule, n_rules: c_int, out_rules: ?*?[*]?*compiler.compiled_rule, out_n: ?*c_int) c_int;

// typecheck.zig.
extern "c" fn dl_typecheck_rules(schm: ?*const schema_mod.dl_schema, rules: ?*anyopaque, n_rules: c_int, srcname: ?[*:0]const u8, errbuf: ?[*]u8, errcap: usize) c_int;

// vm.zig.
extern "c" fn vm_execute(db: ?*DlDb, rules: ?[*]?*compiler.compiled_rule, n_rules: c_int) c_int;
extern "c" fn vm_execute_ivm(db: ?*DlDb) c_int;
extern "c" fn vm_ivm_eligible(db: ?*DlDb) c_int;
extern "c" fn vm_has_recursive(db: ?*DlDb) c_int;
extern "c" fn vm_clear_deltas(db: ?*DlDb) void;
extern "c" fn vm_propagate_deltas(db: ?*DlDb) c_int;
extern "c" fn vm_dred_eligible(db: ?*DlDb) c_int;
extern "c" fn vm_clear_deletes(db: ?*DlDb) void;
extern "c" fn vm_dred_delete(db: ?*DlDb) c_int;
extern "c" fn vm_agg_eligible(db: ?*DlDb) c_int;
extern "c" fn vm_agg_maintain(db: ?*DlDb) c_int;

// snapshot.zig.
extern "c" fn snapshot_read_current(db_dir: [*c]const u8) u32;
extern "c" fn vcache_invalidate(vcache: [*c]ViewCacheSlot) void;
extern "c" fn view_open_cached(vcache: [*c]ViewCacheSlot, rel_name: [*c]const u8, version: u32, sdir: [*c]const u8) ?*anyopaque;
extern "c" fn vcache_drop_version(vcache: [*c]ViewCacheSlot, version: u32) void;
extern "c" fn manifest_find_rel_ex(sdir: [*c]const u8, rel_name: [*c]const u8, arity_out: [*c]u8, variadic_out: [*c]c_int) c_int;
extern "c" fn manifest_find_variants(sdir: [*c]const u8, rel_name: [*c]const u8, present: [*]u8) void;
extern "c" fn manifest_enumerate_rels(sdir: [*c]const u8, cb: DlRelationCb, user: ?*anyopaque) c_long;
extern "c" fn snapshot_query_scan(db_dir: [*c]const u8, snap_version: c_uint, vcache: [*c]ViewCacheSlot, goal_rel: [*c]const u8, leading: ?[*]const u32, k: u8, cb: DlTupleCb, user: ?*anyopaque) c_long;
extern "c" fn view_filter_col(view_handle: ?*anyopaque, arity: u8, col: u8, set: ?*const regexwalk.sym_set, cb: DlTupleCb, user: ?*anyopaque) c_long;
extern "c" fn view_rank(view_handle: ?*anyopaque, arity: u8, cols: ?[*]const u32) u64;
extern "c" fn view_select(view_handle: ?*anyopaque, arity: u8, k: u64, cols_out: ?[*]u32) c_int;
extern "c" fn view_range_count(view_handle: ?*anyopaque, arity: u8, lo: ?[*]const u32, hi: ?[*]const u32) u64;
extern "c" fn view_count(view_handle: ?*anyopaque) u64;

// permindex.zig.
extern "c" fn permindex_build(db: ?*DlDb, rel_id: c_int, perm_id: c_int) c_int;
extern "c" fn permindex_build_dirty(db: ?*DlDb) c_int;
extern "c" fn permindex_mark_dirty(db: ?*DlDb, rel_id: c_int) void;
extern "c" fn permindex_free_all(db: ?*DlDb) void;

// regexwalk.zig — symbol walkers take regexwalk.zig's private dafsa types.
extern "c" fn symbols_dfa_walk(d: ?*const anyopaque, dfa: ?*const regexwalk.regex_dfa, cb: regexwalk.SymWalkCb, user: ?*anyopaque) c_long;

// util.zig.
extern "c" fn atomic_write_str(path: [*c]const u8, content: [*c]const u8) c_int;
extern "c" fn fsync_dir_path(dirpath: [*c]const u8) c_int;

// ─── Small C-string helpers ────────────────────────────────────────────────

fn strEq(a: [*c]const u8, b: [*c]const u8) bool {
    var i: usize = 0;
    while (a[i] != 0 and a[i] == b[i]) : (i += 1) {}
    return a[i] == b[i];
}

fn strLen(s: [*c]const u8) usize {
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {}
    return i;
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
/// first non-digit, e.g. the ':' in a rels.txt "3:edb" field).
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

/// Write an error message to stderr BYTE-IDENTICALLY to the C oracle's
/// fprintf(stderr, ...) (stderr is unbuffered => one raw fd-2 write).
fn dlErr(comptime fmt: []const u8, args: anytype) void {
    var buf: [2048]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch buf[0..0];
    _ = c.write(2, msg.ptr, msg.len);
}

/// DL_E_FRAMES (src/dl.h): the vm_ layer's frame-limit overflow code, which
/// the rule-evaluation family here surfaces VERBATIM where the contract says
/// "-1 on error" (dl_compile, dl_query, dl_query_rules_ro, dl_publish_snapshot,
/// dl_consolidate).  A literal copy because dl.zig hand-declares the dl.h
/// surface (no cImport); abi_witness.zig pins it to the header at test time.
const DL_E_FRAMES: c_int = -2;

// ─── Internal helpers ──────────────────────────────────────────────────────

fn findRel(db: *const DlDb, name: [*c]const u8) c_int {
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (strEq(db.rels[i].name, name)) return @intCast(i);
    }
    return -1;
}

/// Resolve a FIXED-arity relation by name — the read path's own lookup, exposed
/// to in-engine readers that need relation-level metadata (e.g. the content
/// revision) rather than a row enumeration.
/// Null when the name is absent, or names a VARIADIC relation (the caller's
/// contract is one flat tuple set).  In-engine only: not part of the C ABI.
pub fn relFixedByName(db: ?*const DlDb, rel_name: [*c]const u8) ?*relation.Relation {
    const d = db orelse return null;
    if (rel_name == null) return null;
    const idx = findRel(d, rel_name);
    if (idx < 0) return null;
    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC) return null;
    return d.rels[@intCast(idx)].rel;
}

/// Build a file path: db->dir/name.suffix (malloc'd, caller frees).
fn makePath(db: *const DlDb, name: [*c]const u8, suffix: [*c]const u8) ?[*:0]u8 {
    const dlen = strLen(db.dir.?);
    const nlen = strLen(name);
    const slen = strLen(suffix);
    const path = c.malloc(dlen + 1 + nlen + slen + 1) orelse return null;
    const p: [*]u8 = @ptrCast(path);
    @memcpy(p[0..dlen], db.dir.?[0..dlen]);
    p[dlen] = '/';
    @memcpy(p[dlen + 1 .. dlen + 1 + nlen], name[0..nlen]);
    @memcpy(p[dlen + 1 + nlen .. dlen + 1 + nlen + slen], suffix[0..slen]);
    p[dlen + 1 + nlen + slen] = 0;
    return @ptrCast(p);
}

/// Build a VARIADIC VARIANT file path: db->dir/name.<a><suffix>.
fn makeVpath(db: *const DlDb, name: [*c]const u8, a: u8, suffix: [*c]const u8) ?[*:0]u8 {
    const dlen = strLen(db.dir.?);
    const nlen = strLen(name);
    const slen = strLen(suffix);
    const path = c.malloc(dlen + 1 + nlen + 2 + slen + 1) orelse return null;
    const p: [*]u8 = @ptrCast(path);
    @memcpy(p[0..dlen], db.dir.?[0..dlen]);
    p[dlen] = '/';
    @memcpy(p[dlen + 1 .. dlen + 1 + nlen], name[0..nlen]);
    p[dlen + 1 + nlen] = '.';
    p[dlen + 2 + nlen] = @intCast('0' + (a % 10));
    @memcpy(p[dlen + 3 + nlen .. dlen + 3 + nlen + slen], suffix[0..slen]);
    p[dlen + 3 + nlen + slen] = 0;
    return @ptrCast(p);
}

// ─── v2 variable arity: rel_entry dispatch helpers ─────────────────────────

pub export fn db_entry_is_variadic(e: ?*const RelEntry) c_int {
    if (e != null and e.?.kind == RELK_VARIADIC) return 1;
    return 0;
}

pub export fn db_has_variadic(db: ?*const DlDb) c_int {
    const d = db orelse return 0;
    var i: usize = 0;
    while (i < d.nrels) : (i += 1) {
        if (d.rels[i].kind == RELK_VARIADIC) return 1;
    }
    return 0;
}

pub export fn db_has_list_builtin(db: ?*const DlDb) c_int {
    const d = db orelse return 0;
    var i: c_int = 0;
    while (i < d.n_crules) : (i += 1) {
        const cr = d.crules.?[@intCast(i)].?;
        var k: c_int = 0;
        while (k < cr.n_instrs) : (k += 1) {
            const op: u8 = cr.instrs.?[@intCast(k)].op;
            if (op == OP_LIST_CONS or op == OP_LIST_CAR or op == OP_LIST_CDR or
                op == OP_LIST_APPEND or op == OP_LIST_MEMBER) return 1;
        }
    }
    return 0;
}

pub export fn db_has_range_builtin(db: ?*const DlDb) c_int {
    const d = db orelse return 0;
    var i: c_int = 0;
    while (i < d.n_crules) : (i += 1) {
        const cr = d.crules.?[@intCast(i)].?;
        var k: c_int = 0;
        while (k < cr.n_instrs) : (k += 1) {
            if (cr.instrs.?[@intCast(k)].op == OP_RANGE) return 1;
        }
    }
    return 0;
}

pub export fn db_entry_variant_ro(e: ?*const RelEntry, arity: u8) ?*relation.Relation {
    const ee = e orelse return null;
    if (ee.kind == RELK_VARIADIC)
        return vrelation.vrel_variant_or_null(ee.vrel, arity); // absent = empty
    return if (ee.arity == arity) ee.rel else null;
}

pub export fn db_entry_variant_rw(e: ?*RelEntry, arity: u8) ?*relation.Relation {
    const ee = e orelse return null;
    if (ee.kind == RELK_VARIADIC)
        return vrelation.vrel_variant(ee.vrel, arity); // get-or-create
    return if (ee.arity == arity) ee.rel else null;
}

pub export fn db_rel_at_arity_ro(db: ?*const DlDb, rel_id: c_int, arity: u8) ?*relation.Relation {
    const d = db orelse return null;
    if (rel_id < 0 or @as(usize, @intCast(rel_id)) >= d.nrels) return null;
    return db_entry_variant_ro(&d.rels[@intCast(rel_id)], arity);
}

pub export fn db_rel_at_arity_rw(db: ?*DlDb, rel_id: c_int, arity: u8) ?*relation.Relation {
    const d = db orelse return null;
    if (rel_id < 0 or @as(usize, @intCast(rel_id)) >= d.nrels) return null;
    return db_entry_variant_rw(&d.rels[@intCast(rel_id)], arity);
}

/// Does any on-disk file exist for variant a of `name`?  (declare-time scan)
fn variantFilesExist(db: *const DlDb, name: [*c]const u8, a: u8) bool {
    const suffixes = [_][*c]const u8{ ".dafsa", ".wal", ".base.dafsa" };
    var st: Stat = undefined;
    var i: usize = 0;
    while (i < 3) : (i += 1) {
        const p = makeVpath(db, name, a, suffixes[i]);
        const exists = if (p) |pp| stat(pp, &st) == 0 else false;
        if (p) |pp| c.free(@ptrCast(pp));
        if (exists) return true;
    }
    return false;
}

/// A variadic variant's rule-head-ness is derived from FILE EXISTENCE.
fn variantIsIdbOnDisk(db: *const DlDb, name: [*c]const u8, a: u8) bool {
    var st: Stat = undefined;
    const p = makeVpath(db, name, a, ".base.dafsa");
    const exists = if (p) |pp| stat(pp, &st) == 0 else false;
    if (p) |pp| c.free(@ptrCast(pp));
    return exists;
}

/// Get-or-open (durable, WAL-backed) variant a of a variadic entry.
fn variadicOpenVariant(db: *DlDb, e: *RelEntry, a: u8) ?*relation.Relation {
    if (e.kind != RELK_VARIADIC) return null;
    if (a == 0 or a > MAX_VAR_ARITY) return null;
    if (db.dir == null) return vrelation.vrel_variant(e.vrel, a); // eval clone

    const existing = vrelation.vrel_variant_or_null(e.vrel, a);
    if (existing != null) return existing;

    const dafsa = makeVpath(db, e.name, a, ".dafsa");
    const wal = makeVpath(db, e.name, a, ".wal");
    const is_idb = variantIsIdbOnDisk(db, e.name, a);
    const base = if (is_idb) makeVpath(db, e.name, a, ".base.dafsa") else null;

    if (dafsa == null or wal == null or (is_idb and base == null)) {
        if (dafsa) |x| c.free(@ptrCast(x));
        if (wal) |x| c.free(@ptrCast(x));
        if (base) |x| c.free(@ptrCast(x));
        return null;
    }

    const r = if (is_idb)
        (if (db.read_only != 0)
            relation.rel_open_readonly_idb(base.?, dafsa.?, wal.?, a)
        else
            relation.rel_open_writable_idb(base.?, dafsa.?, wal.?, a))
    else
        (if (db.read_only != 0)
            relation.rel_open_readonly(dafsa.?, wal.?, a)
        else
            relation.rel_open_writable(dafsa.?, wal.?, a));

    c.free(@ptrCast(dafsa.?));
    c.free(@ptrCast(wal.?));
    if (base) |x| c.free(@ptrCast(x));

    if (r == null) return null;
    if (vrelation.vrel_attach(e.vrel, a, r) != 0) {
        relation.rel_free(r);
        return null;
    }
    return r;
}

pub export fn dl_ensure_variant(db: ?*DlDb, rel_id: c_int, arity: u8) ?*relation.Relation {
    const d = db orelse return null;
    if (rel_id < 0 or @as(usize, @intCast(rel_id)) >= d.nrels) return null;
    if (d.rels[@intCast(rel_id)].kind != RELK_VARIADIC)
        return db_entry_variant_ro(&d.rels[@intCast(rel_id)], arity);
    return variadicOpenVariant(d, &d.rels[@intCast(rel_id)], arity);
}

// ─── Concurrency: the two lock scopes on <dir>/LOCK ────────────────────────
//
// <dir>/LOCK carries TWO INDEPENDENT fcntl BYTE-RANGE locks.  They are
// separate ranges because the two roles they arbitrate are different, and a
// single whole-file lock cannot express both: an F_WRLCK on a range blocks
// every F_RDLCK on that same range — POSIX advisory locking is
// exclusive-vs-shared in BOTH directions (MEASURED; it is why a live writer
// used to exclude every reader).
//
//   byte 0 (LOCK_EXCL_BYTE) — WRITER EXCLUSIVITY, held by a read-write
//       handle for its whole lifetime.  Readers never touch it, so a live
//       writer no longer excludes them.  A second dl_open/dl_open2 in
//       another process QUEUES for it (see "Acquisition modes" below).
//
//   byte 1 (LOCK_WINDOW_BYTE) — MUTATION WINDOW.  The writer takes it
//       exclusively around every on-disk state REPLACEMENT (WAL truncation
//       in rel_compact, the snapshot materialization/publish sweep,
//       rels.txt, the dl_close save sweep).  A reader takes it SHARED for
//       the whole of its open — the metadata read plus the eager
//       per-relation WAL replay — and releases it before the handle is
//       returned.  That is the only window in which the two must not
//       overlap: rel_compact FTRUNCATES the WAL, and a replay that mmap'd
//       the pre-truncate size would SIGBUS or parse two generations mixed.
//
// NOT in the window, because it is safe by construction:
//   * WAL APPENDS are O_APPEND writes of self-framing records
//     (op / len / key / CRC32).  A reader replaying concurrently sees only
//     whole records; a torn tail fails the CRC and replay STOPS there (never
//     mis-parsed), and an append cannot invalidate a mapping already held.
//   * DAFSA / rels.txt / symbols / terms saves commit as
//     write-tmp + fsync + RENAME, so a lazyrel loader opens either the
//     complete old or the complete new inode — never a partial file.
// Taking the window per fact-add would buy nothing and cost two fcntl
// round-trips per write.
//
// SUPPORTED MODEL: MANY READERS AND ONE WRITER, LIVE AT THE SAME TIME.
// A reader sees the writer's committed state as of its own open (the
// durability contract's cycle boundary — dl.h "Durability"), never a torn
// record; cross-process read-your-own-writes needs a re-open.  Writers stay
// serialized: exactly one per database directory.
//
// ─── Acquisition modes (WHICH PRIMITIVE, AND WHY) ──────────────────────────
//
// MEASURED (Phase-0 M1b, a fresh process per call, K concurrent slots): with
// the writer-exclusive byte 0 acquired FAIL-FAST, writers RACE in user space
// and the loser STARVES — at K=4 only 31% of opens succeeded first try, K=16
// ~8%, with p95 open waits of 1.08/3.35/7.78 s and maxima 3.14/9.12/11.1 s
// while throughput stayed at ~1.0x sequential.  The lock cost LATENCY through
// unfairness, not capacity: whoever retried fastest won, and an honest
// backoff starved.  Retry counts were BIMODAL (attempt 1, or >10).
//
// So byte 0 BLOCKS BY DEFAULT: F_SETLKW, i.e. the waiter SLEEPS on the
// kernel's per-inode POSIX-lock wait queue and is woken when the holder
// releases, instead of re-racing in user space between backoff sleeps.  An
// UNBOUNDED wait is safe here for two reasons:
//   * fcntl locks are released automatically when the holding process dies
//     (close, exit, SIGKILL alike — the kernel drops every lock of the
//     exiting file table), so a crashed writer cannot leave a stale lock
//     behind and no waiter can be wedged by one;
//   * the hold is SHORT: the byte-0 hold is the handle's lifetime, but what
//     other writers actually queue behind is the handle's work — publish
//     ~90 ms worst case (7de1c0c/110b5f9: hardlinked unchanged relations +
//     append-instead-of-rewrite interning), a new-string fact 0.05 ms.
// FAIL-FAST remains available for callers that want to be told "busy" rather
// than queued: DL_LOCK_FAIL_FAST=1 selects F_SETLK and an immediate
// DL_E_LOCKED (the CLI's read-only commands and the suites that assert the
// refusal contract use it).
//
// MEASURED AFTER the change (tests/fairlock_m1b.py, the same M1b shape: a
// fresh process per batch, 120 batches, K slots staggered, seeded 100k-fact
// store so a batch is ~11 ms of work, loadavg ~4 on a 16-core 3950X), before
// = the 110b5f9 binary, after = this default:
//   K=8  first-try 3.3% -> 100%, opens refused 96.7% -> 0%,
//        open wait p95/max 209/384 ms -> 102/109 ms, retries >20: 65 -> 0;
//   K=16 first-try 3.3% -> 100%, opens refused 96.7% -> 0%,
//        open wait p95/max 359/752 ms -> 356/502 ms, retries >20: 89 -> 0.
//   wall vs sequential unchanged (0.90-0.93x both arms): the lock never cost
//   capacity, only latency through unfairness — which is what went away.
//
// A READER's shared window (byte 1) is BOUNDED, never indefinite: it uses
// F_SETLK + a short jittered sleep until a deadline (DL_LOCK_READ_WAIT_MS,
// default 2000) and then fails with DL_E_LOCKED.  A reader queued behind the
// writer's short mutation window is fine, but a reader must not hang forever
// behind a writer that is alive-but-slow (F_SETLKW has no timeout, which is
// why the bounded wait is a retry loop rather than that primitive).
// The WRITER's own mutation window stays F_SETLKW: its only possible blocker
// is another process's open-time WAL replay (a reader's shared hold), and a
// writer that abandoned the window would let rel_compact FTRUNCATE a WAL
// under a live reader's mapping — a bounded wait would have to give up the
// guarantee, so it stays unbounded.
//
// No lock cycle exists between the two bytes: a writer takes byte 1 only
// while holding byte 0, and a reader takes byte 1 only and never byte 0, so
// a waiting writer can never be the thing a reader waits for while that
// reader waits for the writer.
const LOCK_EXCL_BYTE: posix.off_t = 0;
const LOCK_WINDOW_BYTE: posix.off_t = 1;

extern "c" fn __errno_location() *c_int;
const LOCK_EINTR: c_int = 4; // Linux glibc EINTR

/// One fcntl byte-range lock operation on `start` (length 1).  `wait`
/// selects F_SETLKW (block on the kernel's per-inode queue) over F_SETLK
/// (fail fast).  Returns 0 on success, -1 on error.
fn lockRange(fd: c_int, ty: c_short, start: posix.off_t, wait: bool) c_int {
    var fl: posix.struct_flock = undefined;
    fl.l_type = ty;
    fl.l_whence = posix.SEEK_SET;
    fl.l_start = start;
    fl.l_len = 1;
    fl.l_pid = 0;
    const cmd: c_int = if (wait) posix.F_SETLKW else posix.F_SETLK;
    while (true) {
        if (posix.fcntl(fd, cmd, &fl) == 0) return 0;
        if (__errno_location().* == LOCK_EINTR) continue;
        return -1;
    }
}

/// BOUNDED acquisition: F_SETLK, and on EAGAIN a short JITTERED sleep before
/// the next attempt, until `deadline_ms` elapses.  Returns 0 on success, -1
/// on deadline expiry or a real error.  Never a bare spin: the sleep is what
/// keeps a queued reader off the CPU while the writer finishes (F_SETLKW has
/// no timeout, so this loop — not that primitive — is how a bounded wait is
/// expressed).  The jitter de-synchronises several readers retrying at once;
/// it is derived from the clock + the attempt count, so no RNG is pulled into
/// the library.
fn lockRangeBounded(fd: c_int, ty: c_short, start: posix.off_t, deadline_ms: u64) c_int {
    const deadline = nowNs() + deadline_ms * 1_000_000;
    var attempt: u64 = 0;
    while (true) {
        if (lockRange(fd, ty, start, false) == 0) return 0;
        if (nowNs() >= deadline) return -1;
        attempt += 1;
        // 0.25 ms .. 1.25 ms, jittered.
        const sleep_ns: u64 = 250_000 + (nowNs() +% attempt *% 7919) % 1_000_000;
        var req = Timespec{ .sec = @intCast(sleep_ns / 1_000_000_000), .nsec = @intCast(sleep_ns % 1_000_000_000) };
        while (nanosleep(&req, &req) != 0) {
            if (__errno_location().* != LOCK_EINTR) return -1;
        }
    }
}

/// DL_LOCK_FAIL_FAST=1: acquire the writer-exclusive byte with F_SETLK and
/// refuse at once with DL_E_LOCKED instead of queueing.  Read PER OPEN (not
/// cached like the publish/timing env vars): the C oracle and the CLI flip it
/// within one process, and an open is never on a hot path.
fn lockFailFast() bool {
    const v = getenv("DL_LOCK_FAIL_FAST");
    return v != null and v.?[0] != 0 and !strEq(v.?, "0");
}

/// Default deadline (ms) for a reader's shared-window acquisition.
const LOCK_READ_WAIT_MS_DEFAULT: u64 = 2000;

/// DL_LOCK_READ_WAIT_MS: how long a READER may wait for the shared mutation
/// window before giving up with DL_E_LOCKED.  Bounded on purpose — a reader
/// must never hang forever behind a writer that is alive but slow.
fn lockReadWaitMs() u64 {
    const v = getenv("DL_LOCK_READ_WAIT_MS");
    if (v != null and v.?[0] != 0) {
        const ms = strtoul(v, null, 10);
        if (ms > 0) return ms;
    }
    return LOCK_READ_WAIT_MS_DEFAULT;
}

// Mutation-window state, per thread.  fcntl locks are per (process, file)
// and the engine is single-threaded per db (no internal mutex — see dl.h),
// so tracking the window by the LOCK fd held in this thread is enough to
// make a nested window (dl_close -> writeRelsTxt) a no-op instead of an
// early unlock of the outer one.
threadlocal var mut_window_fd: c_int = -1;
threadlocal var mut_window_depth: c_int = 0;

/// Enter the writer's mutation window: EXCLUSIVE on LOCK_WINDOW_BYTE,
/// BLOCKING (F_SETLKW) — the only holder that can be in the way is another
/// process's open-time replay, which is bounded by its WAL size.  Unbounded
/// on purpose: a bounded wait would have to give up and then let the mutation
/// run WITHOUT the window, and that window is what stops rel_compact
/// FTRUNCATING a WAL under a live reader's mapping (see the lock-scope
/// block).  A lock error is not fatal to the mutation (the write path's
/// convention: durability failures are surfaced by the fsync/rename that
/// follows, not by the lock); the depth accounting stays balanced either way.
fn mutWindowBegin(db: *DlDb) void {
    if (db.read_only != 0 or db.lock_fd < 0) return;
    if (mut_window_fd == db.lock_fd) {
        mut_window_depth += 1;
        return;
    }
    if (lockRange(db.lock_fd, posix.F_WRLCK, LOCK_WINDOW_BYTE, true) == 0) {
        mut_window_fd = db.lock_fd;
        mut_window_depth = 1;
    }
}

fn mutWindowEnd(db: *DlDb) void {
    if (db.read_only != 0 or db.lock_fd < 0) return;
    if (mut_window_fd != db.lock_fd or mut_window_depth <= 0) return;
    mut_window_depth -= 1;
    if (mut_window_depth > 0) return;
    _ = lockRange(db.lock_fd, posix.F_UNLCK, LOCK_WINDOW_BYTE, false);
    mut_window_fd = -1;
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────

pub export fn dl_open(dir: ?[*:0]const u8) ?*DlDb {
    return dl_open2(dir, null);
}

/// RO existence probe: 1 if <dir>/<name> exists, 0 if it does not, -1 if
/// the path cannot be formed (over-long dir).
fn roProbeExists(dir: [*c]const u8, name: [*c]const u8) c_int {
    var path: [4096:0]u8 = undefined;
    var st: Stat = undefined;
    const n = snprintf(&path, 4096, "%s/%s", dir, name);
    if (n < 0 or n >= 4096) return -1;
    return if (stat(&path, &st) == 0) 1 else 0;
}

/// Shared body of dl_open2 (read-write) and dl_open_ro.
fn dlOpenCommon(dir: ?[*:0]const u8, err_out: ?*c_int, ro: c_int) ?*DlDb {
    if (dir == null) {
        if (err_out) |e| e.* = -1;
        return null;
    }
    const dirn: [*c]const u8 = dir.?;

    if (ro != 0) {
        var st: Stat = undefined;
        if (stat(dirn, &st) != 0 or !posix.S_ISDIR(st.st_mode)) {
            if (err_out) |e| e.* = -1;
            return null;
        }
        var have_meta = roProbeExists(dirn, "rels.txt");
        if (have_meta == 0) have_meta = roProbeExists(dirn, "symbols.dafsa");
        if (have_meta != 1) {
            if (err_out) |e| e.* = -1;
            return null;
        }
    } else {
        _ = mkdir(dirn, @as(posix.mode_t, 0o755));
    }

    var lock_path: [4096:0]u8 = undefined;
    _ = snprintf(&lock_path, 4096, "%s/LOCK", dirn);
    const lfd = if (ro != 0)
        posix.open(&lock_path, posix.O_RDONLY)
    else
        posix.open(&lock_path, posix.O_CREAT | posix.O_RDWR, @as(posix.mode_t, 0o644));
    if (lfd < 0) {
        if (err_out) |e| e.* = -1;
        return null;
    }

    // Lock scopes (see "Concurrency: the two lock scopes on <dir>/LOCK"):
    //  * writer: EXCLUSIVE on byte 0 for the handle's whole lifetime —
    //    BLOCKING (F_SETLKW) by default, so a second writer QUEUES on the
    //    kernel's lock queue instead of racing and starving; DL_LOCK_FAIL_FAST=1
    //    selects F_SETLK and an immediate DL_E_LOCKED instead.  Readers never
    //    take this byte;
    //  * reader: SHARED on byte 1, held only across this open (released
    //    before the handle is returned) so it cannot overlap a
    //    WAL-truncating mutation.  BOUNDED wait (F_SETLK + jittered sleep,
    //    DL_LOCK_READ_WAIT_MS default 2000) so a reader waits out at most a
    //    mutation window but can never hang forever behind a slow writer.
    var reader_window: bool = false;
    if (ro != 0) {
        if (lockRangeBounded(lfd, posix.F_RDLCK, LOCK_WINDOW_BYTE, lockReadWaitMs()) != 0) {
            _ = posix.close(lfd);
            if (err_out) |e| e.* = DL_E_LOCKED;
            return null;
        }
        reader_window = true;
    } else if (lockRange(lfd, posix.F_WRLCK, LOCK_EXCL_BYTE, !lockFailFast()) != 0) {
        _ = posix.close(lfd);
        if (err_out) |e| e.* = DL_E_LOCKED;
        return null;
    }
    // Every early return below must drop the reader's shared window; the
    // lock fd itself stays open for the handle's lifetime (dl_close closes
    // it) — closing it is NOT what releases this lock on the success path.
    // On a FAILURE path the body closes lfd first, which drops the process's
    // locks on the file anyway; the deferred unlock then returns EBADF, which
    // is ignored (no fd is allocated between that close and the return).
    defer if (reader_window) {
        _ = lockRange(lfd, posix.F_UNLCK, LOCK_WINDOW_BYTE, false);
    };

    const mem = c.calloc(1, @sizeOf(DlDb)) orelse {
        _ = posix.close(lfd);
        if (err_out) |e| e.* = -1;
        return null;
    };
    const db: *DlDb = @ptrCast(@alignCast(mem));
    db.* = std.mem.zeroes(DlDb);
    db.lock_fd = lfd;
    db.read_only = ro;
    db.rev_rel_id = -1;

    db.dir = strdup(dirn);
    if (db.dir == null) {
        _ = posix.close(lfd);
        c.free(mem);
        if (err_out) |e| e.* = -1;
        return null;
    }

    // M4: initial snapshot state
    db.fixpoint_dirty = 0;
    db.snap_version = 0;
    db.snapshot_retain = 0;
    // Periodic snapshot: default budget from env (64 MiB); 0 would mean
    // "never auto-snapshot" — the default keeps dl_consolidate periodic.
    db.snapshot_budget_bytes = snapshotBudgetBytes();
    // Publish gate: mem.zeroes above clears the default-field sentinels;
    // re-arm them so an OWNED relation (mutated, or WAL-replayed at open —
    // which does not bump rev) is serialized on the first publish of the
    // process rather than hardlinked from a previous snapshot.  UNMUTATED
    // relations do not need this: they are .deferred/.view and the gate links
    // their live file (see relSaveGated), so a fresh process no longer
    // re-serializes the whole store.
    @memset(&db.saved_rev, SAVED_REV_NONE);
    for (&db.saved_rev_var) |*row| @memset(row, SAVED_REV_NONE);
    @memset(&db.saved_rev_perm, SAVED_REV_NONE);
    @memset(std.mem.asBytes(&db.vcache), 0);
    db.fault_hook = null;
    db.fault_user = null;

    // M8: retained rule AST (empty until dl_load_rules)
    db.ast_rules = null;
    db.n_ast_rules = 0;

    // Load or create interner
    {
        const fwd_path = makePath(db, "symbols", ".dafsa");
        const rev_path = makePath(db, "symbols", ".array");
        if (fwd_path == null or rev_path == null) {
            if (fwd_path) |x| c.free(@ptrCast(x));
            if (rev_path) |x| c.free(@ptrCast(x));
            _ = posix.close(lfd);
            c.free(@ptrCast(db.dir.?));
            c.free(mem);
            if (err_out) |e| e.* = -1;
            return null;
        }
        db.ir = intern_load(fwd_path.?, rev_path.?);
        c.free(@ptrCast(fwd_path.?));
        c.free(@ptrCast(rev_path.?));
        if (db.ir == null) {
            _ = posix.close(lfd);
            c.free(@ptrCast(db.dir.?));
            c.free(mem);
            if (err_out) |e| e.* = -1;
            return null;
        }
    }

    // v2-lists: load the list term store.
    {
        const tpath = makePath(db, "terms", ".bin");
        if (tpath == null) {
            intern_free(db.ir);
            _ = posix.close(lfd);
            c.free(@ptrCast(db.dir.?));
            c.free(mem);
            if (err_out) |e| e.* = -1;
            return null;
        }
        db.terms = term_load(tpath.?);
        c.free(@ptrCast(tpath.?));
        if (db.terms == null) {
            intern_free(db.ir);
            _ = posix.close(lfd);
            c.free(@ptrCast(db.dir.?));
            c.free(mem);
            if (err_out) |e| e.* = -1;
            return null;
        }
    }

    // Load existing relations from metadata file (whole file into a temp
    // array BEFORE declaring, so declaring never truncates mid-read).
    const rels_path = makePath(db, "rels", ".txt");
    if (rels_path != null) {
        const rf = c.fopen(rels_path.?, "r");
        if (rf != null) {
            const rff = rf.?;
            var line: ?[*]u8 = null;
            var cap: usize = 0;
            var rel_names: [MAX_RELS][256]u8 = undefined;
            var rel_arities: [MAX_RELS]u8 = undefined;
            var rel_idb: [MAX_RELS]u8 = undefined;
            var rel_kinds: [MAX_RELS][8]u8 = @splat(@splat(0)); // U-homog S2
            var n_meta: usize = 0;

            var len: isize = getline(&line, &cap, rff);
            while (len > 0 and n_meta < MAX_RELS) : (len = getline(&line, &cap, rff)) {
                var l: usize = @intCast(len);
                const buf = line.?;
                if (l > 0 and buf[l - 1] == '\n') {
                    l -= 1;
                    buf[l] = 0;
                }
                if (l > 0 and buf[l - 1] == '\r') {
                    l -= 1;
                    buf[l] = 0;
                }

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
                const rest: [*c]const u8 = @ptrCast(buf + ci + 1);
                const arity = atoiC(rest);
                const is_variadic = rest[0] == '*';
                var is_idb: u8 = 0;
                var colon2: ?usize = null;
                {
                    var j: usize = 0;
                    while (rest[j] != 0) : (j += 1) {
                        if (rest[j] == ':') {
                            colon2 = j;
                            break;
                        }
                    }
                }
                if (colon2) |c2| {
                    // the token ends at the NEXT ':' (the U-homog S2 kinds
                    // field follows) or at the line end.
                    const tok = rest + c2 + 1;
                    var te: usize = 0;
                    while (tok[te] != 0 and tok[te] != ':') te += 1;
                    is_idb = if (te == 3 and strncmp(tok, "idb", 3) == 0) 1 else 0;
                }
                if (buf[0] != 0 and (is_variadic or (arity >= 1 and arity <= 8))) {
                    _ = snprintf(&rel_names[n_meta], 256, "%s", buf);
                    rel_arities[n_meta] = if (is_variadic) 0 else @intCast(arity);
                    rel_idb[n_meta] = is_idb;
                    // parse the kinds field (chars after the 2nd ':')
                    if (colon2) |c2| {
                        var kstr = rest + c2 + 1;
                        // skip the edb/idb token to the next ':' if present
                        while (kstr[0] != 0 and kstr[0] != ':') kstr += 1;
                        if (kstr[0] == ':') {
                            kstr += 1;
                            const ar = if (is_variadic) 0 else arity;
                            var kk: usize = 0;
                            while (kk < ar and kk < 8) : (kk += 1) {
                                const ch = kstr[kk];
                                rel_kinds[n_meta][kk] = switch (ch) {
                                    'n' => relation.KIND_INT,
                                    't' => relation.KIND_SYM,
                                    'm' => 3, // tolerated on read; never written
                                    else => 0,
                                };
                            }
                        }
                    }
                    n_meta += 1;
                }
            }
            if (line) |lp| c.free(@ptrCast(lp));
            _ = c.fclose(rff);

            var mi: usize = 0;
            while (mi < n_meta) : (mi += 1) {
                _ = dlDeclareRelationKind(db, @ptrCast(&rel_names[mi]), rel_arities[mi], rel_idb[mi], 1);
                // U-homog S2: restore the recorded kinds (after the declare
                // created the Relation object).
                if (rel_arities[mi] > 0) {
                    const kri = findRel(db, @ptrCast(&rel_names[mi]));
                    if (kri >= 0) {
                        if (db.rels[@intCast(kri)].rel) |rel| {
                            var kk: usize = 0;
                            while (kk < rel_arities[mi] and kk < 8) : (kk += 1) {
                                if (rel_kinds[mi][kk] != 0 and rel_kinds[mi][kk] <= relation.KIND_SYM)
                                    _ = relation.rel_kind_note(rel, @intCast(kk), rel_kinds[mi][kk]);
                            }
                        }
                    }
                }
            }
        }
        c.free(@ptrCast(rels_path.?));
    }

    // CAS Slice 2: replay any committed txn WAL prefix into the base relations.
    replayTxnWal(db);

    // M4: detect existing snapshot and fixpoint state
    db.snap_version = snapshot_read_current(db.dir.?);
    db.fixpoint_dirty = if (db.n_crules > 0) 1 else 0;

    if (err_out) |e| e.* = 0;
    return db;
}

pub export fn dl_open2(dir: ?[*:0]const u8, err_out: ?*c_int) ?*DlDb {
    return dlOpenCommon(dir, err_out, 0);
}

pub export fn dl_open_ro(dir: ?[*:0]const u8, err_out: ?*c_int) ?*DlDb {
    return dlOpenCommon(dir, err_out, 1);
}

pub export fn dl_close(db: ?*DlDb) void {
    const d = db orelse return;

    // CAS Slice 2: an open transaction is never committed implicitly.
    // B4 (review): undo the buffer-time kind recordings first (same as a
    // rollback/commit-failure — the buffered rows never applied), then free.
    if (d.txn) |t| {
        txnKindRestore(d);
        if (t.ops) |ops| c.free(@ptrCast(ops));
        c.free(@ptrCast(t));
        d.txn = null;
    }

    // M4: close all cached snapshot views
    vcache_invalidate(&d.vcache);

    // M6: free permutation indices
    permindex_free_all(d);

    // IVM Slice 1/3: free any pending delta/delete tuple_sets
    vm_clear_deltas(d);
    vm_clear_deletes(d);

    // Save interner (skipped entirely on read-only handles).
    //
    // Clean shutdown IS a cycle boundary and the whole save sweep below
    // (interner, term store, WAL fsync, per-relation compaction with its WAL
    // truncation, rels.txt) replaces on-disk state, so it runs inside ONE
    // mutation window: a reader that opens while this process is closing
    // waits for the sweep instead of replaying a half-truncated WAL set.
    mutWindowBegin(d);
    {
        const fwd_path = makePath(d, "symbols", ".dafsa");
        const rev_path = makePath(d, "symbols", ".array");
        if (fwd_path != null and rev_path != null and d.read_only == 0 and
            intern_is_dirty(d.ir) != 0)
            _ = intern_save(d.ir, fwd_path.?, rev_path.?);
        if (fwd_path) |x| c.free(@ptrCast(x));
        if (rev_path) |x| c.free(@ptrCast(x));
    }

    // v2-lists: save the term store AFTER the interner and BEFORE relations.
    {
        const tpath = makePath(d, "terms", ".bin");
        if (tpath != null) {
            if (d.read_only == 0 and term_is_dirty(d.terms) != 0)
                _ = term_save(d.terms, tpath.?);
            c.free(@ptrCast(tpath.?));
        }
    }

    // selfreg-batched-fsync: clean shutdown IS a cycle boundary.  fsync every
    // per-relation WAL BEFORE the compact loop, so that even if compaction
    // fails (dafsa_save error) the appended records are still durable and
    // recoverable at the next open.  (rel_compact's own ftruncate+fsync would
    // otherwise discard the un-synced tail; this barrier closes that gap.)
    if (d.read_only == 0)
        syncAllWals(d);

    // M7/IVM: compact each relation's BASE (save + truncate WAL).  v2: a
    // VARIADIC relation does the same PER VARIANT under <name>.<a>.dafsa.
    var i: usize = 0;
    while (i < d.nrels) : (i += 1) {
        if (d.rels[i].kind == RELK_VARIADIC) {
            var a: u8 = 1;
            while (a <= MAX_VAR_ARITY) : (a += 1) {
                const vr = vrelation.vrel_variant_or_null(d.rels[i].vrel, a);
                if (vr == null) continue;
                if (d.read_only != 0 or relation.rel_is_dirty(vr) == 0) continue;
                if (relation.rel_is_idb(vr) != 0) {
                    const path = makeVpath(d, d.rels[i].name, a, ".base.dafsa");
                    if (path != null) {
                        _ = relation.rel_compact(vr, path.?);
                        c.free(@ptrCast(path.?));
                    }
                    const path2 = makeVpath(d, d.rels[i].name, a, ".dafsa");
                    if (path2 != null) {
                        _ = relation.rel_save(vr, path2.?);
                        c.free(@ptrCast(path2.?));
                    }
                } else {
                    const path = makeVpath(d, d.rels[i].name, a, ".dafsa");
                    if (path != null) {
                        _ = relation.rel_compact(vr, path.?);
                        c.free(@ptrCast(path.?));
                    }
                }
            }
        } else if (d.read_only != 0 or relation.rel_is_dirty(d.rels[i].rel) == 0) {
            continue;
        } else if (relation.rel_is_idb(d.rels[i].rel) != 0) {
            const path = makePath(d, d.rels[i].name, ".base.dafsa");
            if (path != null) {
                _ = relation.rel_compact(d.rels[i].rel, path.?);
                c.free(@ptrCast(path.?));
            }
            const path2 = makePath(d, d.rels[i].name, ".dafsa");
            if (path2 != null) {
                _ = relation.rel_save(d.rels[i].rel, path2.?);
                c.free(@ptrCast(path2.?));
            }
        } else {
            const path = makePath(d, d.rels[i].name, ".dafsa");
            if (path != null) {
                _ = relation.rel_compact(d.rels[i].rel, path.?);
                c.free(@ptrCast(path.?));
            }
        }
    }

    // Save relation metadata (name:arity:edb|idb).
    if (d.read_only == 0 and d.meta_dirty != 0)
        writeRelsTxt(d);

    // End of the save sweep: the mutation window is released HERE, not by a
    // defer — everything below frees `d` and closes its lock fd.
    mutWindowEnd(d);

    // Free relations and their names
    i = 0;
    while (i < d.nrels) : (i += 1) {
        if (d.rels[i].kind == RELK_VARIADIC)
            vrelation.vrel_free(d.rels[i].vrel)
        else
            relation.rel_free(d.rels[i].rel);
        if (d.rels[i].name) |n| c.free(@ptrCast(n));
    }

    // Free compiled rules
    if (d.crules) |crs| {
        i = 0;
        while (i < @as(usize, @intCast(d.n_crules))) : (i += 1)
            compiler.compiled_rule_free(crs[i]);
        c.free(@ptrCast(crs));
    }

    // M8: free retained rule AST
    if (d.ast_rules) |ars| {
        i = 0;
        while (i < @as(usize, @intCast(d.n_ast_rules))) : (i += 1)
            rule_free(ars[i]);
        c.free(@ptrCast(ars));
    }

    // VERSION pattern queries may hold a private interner built from a
    // snapshot's symbols.array (snapshotSymSet); dl_close owns it — unless it
    // is the live interner borrowed under a proven-equal stamp (snapshotInterner),
    // which the next line frees.
    if (d.snap_sym_ir) |sir| {
        if (!d.snap_sym_shared) intern_free(sir);
    }

    intern_free(d.ir);
    term_free(d.terms);
    if (d.dir) |dd| c.free(@ptrCast(dd));

    // M7: release fcntl lock LAST (lock released when fd closes)
    if (d.lock_fd >= 0) _ = posix.close(d.lock_fd);

    c.free(@ptrCast(d));
}

// ─── Schema / metadata ─────────────────────────────────────────────────────

/// M6: check if a name matches the reserved __PI<hex>__ suffix pattern.
fn isReservedPiName(name: [*c]const u8) bool {
    var p = strstr(name, "__PI") orelse return false;
    p += 4; // skip __PI
    var hex_digits: usize = 0;
    while ((p[0] >= '0' and p[0] <= '9') or (p[0] >= 'a' and p[0] <= 'f')) {
        hex_digits += 1;
        p += 1;
    }
    if (hex_digits == 0) return false;
    if (strncmp(p, "__", 2) != 0) return false;
    return true;
}

/// Persist relation metadata (name:arity:edb|idb per line), atomically.
/// U-homog S2: a 4th field ":<kinds>" is appended when any column kind is
/// recorded — one char per column, u/n/t (unknown/int/sym; mixed is never
/// stored).  Omitted entirely when all kinds are unknown, so pre-S2 stores
/// keep their exact bytes (and old engines keep reading new ones: their
/// parser stops at the 2nd ':').
fn writeRelsTxt(db: *DlDb) void {
    mutWindowBegin(db);
    defer mutWindowEnd(db);
    const rels_path = makePath(db, "rels", ".txt") orelse return;

    var total: usize = 1; // trailing NUL
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        total += strLen(db.rels[i].name) + 16;
        total += 10; // U-homog S2: ":nnnnnnnn" kinds suffix
    }

    const buf_mem = c.malloc(total) orelse {
        c.free(@ptrCast(rels_path));
        return;
    };
    const buf: [*]u8 = @ptrCast(buf_mem);

    var p: usize = 0;
    i = 0;
    while (i < db.nrels) : (i += 1) {
        var n: c_int = undefined;
        if (db.rels[i].kind == RELK_VARIADIC) {
            n = snprintf(buf + p, total - p, "%s:*:%s\n", db.rels[i].name, if (vrelation.vrel_any_idb(db.rels[i].vrel) != 0) "idb" else "edb");
        } else {
            n = snprintf(buf + p, total - p, "%s:%d:%s", db.rels[i].name, @as(c_int, relation.rel_arity(db.rels[i].rel)), if (relation.rel_is_idb(db.rels[i].rel) != 0) "idb" else "edb");
            if (n < 0) {
                c.free(buf_mem);
                c.free(@ptrCast(rels_path));
                return;
            }
            p += @intCast(n);
            // U-homog S2: append ":<kinds>" only when at least one column
            // kind is recorded (all-unknown lines stay byte-identical).
            const ar = relation.rel_arity(db.rels[i].rel);
            var any = false;
            var kc: u8 = 0;
            while (kc < ar) : (kc += 1) {
                if (relation.rel_col_kind(db.rels[i].rel, kc) != 0) any = true;
            }
            if (any) {
                var kbuf: [9]u8 = undefined;
                kc = 0;
                while (kc < ar) : (kc += 1) {
                    kbuf[kc] = switch (relation.rel_col_kind(db.rels[i].rel, kc)) {
                        relation.KIND_INT => 'n',
                        relation.KIND_SYM => 't',
                        else => 'u',
                    };
                }
                n = snprintf(buf + p, total - p, ":%.*s\n", @as(c_int, ar), @as([*c]const u8, &kbuf));
            } else {
                n = snprintf(buf + p, total - p, "\n");
            }
        }
        if (n < 0) {
            c.free(buf_mem);
            c.free(@ptrCast(rels_path));
            return;
        }
        p += @intCast(n);
    }

    _ = atomic_write_str(rels_path, @ptrCast(buf));
    c.free(buf_mem);
    c.free(@ptrCast(rels_path));
}

/// Does `name` look like a variadic-variant file stem "<x>.<d>" (d in 1..8)?
fn isVariantStem(name: [*c]const u8, stem: [*]u8) bool {
    const len = strLen(name);
    if (len < 3 or len > 254) return false;
    if (name[len - 2] != '.') return false;
    if (name[len - 1] < '1' or name[len - 1] > '8') return false;
    @memcpy(stem[0 .. len - 2], name[0 .. len - 2]);
    stem[len - 2] = 0;
    return true;
}

/// Declare a relation, optionally as a rule head (is_idb).  arity == 0
/// declares a VARIADIC relation.
fn dlDeclareRelationKind(db: *DlDb, name: [*c]const u8, arity: u8, is_idb: c_int, from_meta: c_int) c_int {
    if (name == null) return -1;
    if (arity > 8) return -1; // 0 = variadic (v2)

    // CAS: the "rev" relation is system-reserved with FIXED arity 2.
    if (strEq(name, "rev") and arity != 2) {
        dlErr("error: relation name 'rev' is reserved (arity must be 2)\n", .{});
        return -1;
    }

    // M8: scoped-eval clones have dir==NULL.
    if (db.dir == null) {
        dlErr("error: dl_declare_relation('{s}') on a filesystem-less eval clone — transform must pre-declare all head relations\n", .{name});
        return -1;
    }

    // M6: reject reserved permutation index names
    if (isReservedPiName(name)) {
        dlErr("error: relation name '{s}' uses reserved __PI<hex>__ suffix\n", .{name});
        return -1;
    }

    // v2: keep variadic variant files from aliasing other relations' files.
    if (arity == 0) {
        var i: usize = 0;
        while (i < db.nrels) : (i += 1) {
            if (db.rels[i].kind != RELK_VARIADIC and db.rels[i].name.?[0] != 0) {
                var stem: [256]u8 = undefined;
                if (isVariantStem(db.rels[i].name, &stem) and strEq(@ptrCast(&stem), name)) {
                    dlErr("error: variadic relation '{s}' collides with variant files of '{s}'\n", .{ name, db.rels[i].name.? });
                    return -1;
                }
            }
        }
    } else {
        var stem: [256]u8 = undefined;
        if (isVariantStem(name, &stem)) {
            const sidx = findRel(db, @ptrCast(&stem));
            if (sidx >= 0 and db.rels[@intCast(sidx)].kind == RELK_VARIADIC) {
                dlErr("error: relation name '{s}' collides with variant files of variadic '{s}'\n", .{ name, @as([*c]const u8, @ptrCast(&stem)) });
                return -1;
            }
        }
    }

    // Check if already declared
    const idx = findRel(db, name);
    if (idx >= 0) {
        if (arity == 0) {
            if (db.rels[@intCast(idx)].kind == RELK_VARIADIC) return 0;
            return -1; // exists as fixed: kind mismatch
        }
        if (db.rels[@intCast(idx)].kind == RELK_VARIADIC) return -1; // kind mismatch
        if (relation.rel_arity(db.rels[@intCast(idx)].rel) == arity) return 0;
        return -1; // arity mismatch
    }

    if (db.nrels >= MAX_RELS) return -1;

    var rel: ?*relation.Relation = null;
    var vrel: ?*vrelation.Vrelation = null;

    if (arity == 0) {
        var tmp: RelEntry = std.mem.zeroes(RelEntry);
        vrel = vrelation.vrel_create() orelse return -1;
        tmp.name = @constCast(name);
        tmp.kind = RELK_VARIADIC;
        tmp.vrel = vrel;
        var a: u8 = 1;
        while (a <= MAX_VAR_ARITY) : (a += 1) {
            if (!variantFilesExist(db, name, a)) continue;
            if (variadicOpenVariant(db, &tmp, a) == null) {
                vrelation.vrel_free(vrel);
                return -1;
            }
        }
    } else {
        const path = makePath(db, name, ".dafsa");
        const wal_path = makePath(db, name, ".wal");
        const base_path = if (is_idb != 0) makePath(db, name, ".base.dafsa") else null;
        if (path == null or wal_path == null or (is_idb != 0 and base_path == null)) {
            if (path) |x| c.free(@ptrCast(x));
            if (wal_path) |x| c.free(@ptrCast(x));
            if (base_path) |x| c.free(@ptrCast(x));
            return -1;
        }

        rel = if (is_idb != 0)
            (if (db.read_only != 0)
                relation.rel_open_readonly_idb(base_path.?, path.?, wal_path.?, arity)
            else
                relation.rel_open_writable_idb(base_path.?, path.?, wal_path.?, arity))
        else
            (if (db.read_only != 0)
                relation.rel_open_readonly(path.?, wal_path.?, arity)
            else
                relation.rel_open_writable(path.?, wal_path.?, arity));
        c.free(@ptrCast(path.?));
        c.free(@ptrCast(wal_path.?));
        if (base_path) |x| c.free(@ptrCast(x));
        if (rel == null) return -1;
    }

    // Register
    const ridx = db.nrels;
    db.nrels += 1;
    db.rels[ridx].name = strdup(name);
    db.rels[ridx].kind = if (arity == 0) RELK_VARIADIC else RELK_FIXED;
    db.rels[ridx].arity = arity;
    db.rels[ridx].rel = if (arity == 0) null else rel;
    db.rels[ridx].vrel = if (arity == 0) vrel else null;
    if (db.rels[ridx].name == null or
        (if (arity == 0) db.rels[ridx].vrel == null else db.rels[ridx].rel == null))
    {
        if (arity == 0) vrelation.vrel_free(db.rels[ridx].vrel) else relation.rel_free(db.rels[ridx].rel);
        if (db.rels[ridx].name) |n| c.free(@ptrCast(n));
        db.nrels -= 1;
        return -1;
    }

    // M7: persist relation metadata immediately (read-only handles never
    // rewrite; from_meta declarations are already recorded).
    if (from_meta == 0) {
        db.meta_dirty = 1;
        if (db.read_only == 0)
            writeRelsTxt(db);
    }

    return 0;
}

pub export fn dl_declare_relation(db: ?*DlDb, name: [*c]const u8, arity: u8) c_int {
    if (db != null and db.?.read_only != 0) return -1; // write API on RO handle
    return dlDeclareRelationKind(db orelse return -1, name, arity, 0, 0);
}

pub export fn dl_declare_relation_variadic(db: ?*DlDb, name: [*c]const u8) c_int {
    if (db != null and db.?.read_only != 0) return -1;
    return dlDeclareRelationKind(db orelse return -1, name, 0, 0, 0);
}

// ─── CSV parser ────────────────────────────────────────────────────────────

/// Parse a single CSV row into an array of strings.  Modifies `line` in place
/// (NUL-terminates each field).  Returns number of fields, or -1 on error.
fn csvSplit(line: [*c]u8, fields: [*c][*c]u8, max_fields: c_int) c_int {
    var n: c_int = 0;
    var p: [*c]u8 = line;

    while (p[0] != 0 and n < max_fields) {
        while (p[0] == ' ' or p[0] == '\t') p += 1;

        if (p[0] == 0 or p[0] == '\n' or p[0] == '\r') break;

        if (p[0] == '"') {
            p += 1; // skip opening quote
            fields[@intCast(n)] = p;
            n += 1;
            while (p[0] != 0 and p[0] != '"') p += 1;
            if (p[0] == '"') {
                p[0] = 0; // terminate
                p += 1;
            }
            while (p[0] == ' ' or p[0] == '\t') p += 1;
            if (p[0] == ',') p += 1;
        } else {
            fields[@intCast(n)] = p;
            n += 1;
            while (p[0] != 0 and p[0] != ',' and p[0] != '\n' and p[0] != '\r') p += 1;
            if (p[0] == ',') {
                p[0] = 0;
                p += 1;
            } else {
                if (p[0] != 0) p[0] = 0;
            }
        }
    }

    return n;
}

/// 1 if `s` is a bare integer (optional minus not supported), else 0.
fn isInteger(s: [*c]const u8) c_int {
    if (s == null or s[0] == 0) return 0;
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {
        if (s[i] < '0' or s[i] > '9') return 0;
    }
    return 1;
}

// ─── Fact loading ──────────────────────────────────────────────────────────

/// v2: variadic bulk load (variable-width rows routed per-arity).
fn dlLoadFactsVariadic(db: *DlDb, idx: usize, rel_name: [*c]const u8, csv_path: [*c]const u8) c_int {
    const f = c.fopen(csv_path, "r") orelse return -1;
    var line: ?[*]u8 = null;
    var linecap: usize = 0;
    const e = &db.rels[idx];
    var ts: [MAX_VAR_ARITY + 1]tupleset.tuple_set = undefined;
    var have: [MAX_VAR_ARITY + 1]u8 = undefined;
    var loaded: c_long = 0;
    var n_new: c_long = 0;
    var a: c_int = 0;
    var rc: c_int = -1;

    @memset(std.mem.asBytes(&ts), 0);
    @memset(&have, 0);

    var linelen: isize = getline(&line, &linecap, f);
    while (linelen > 0) : (linelen = getline(&line, &linecap, f)) {
        var fields: [9][*c]u8 = undefined;
        var nf: c_int = undefined;
        var cols: [8]u32 = undefined;
        var i: c_int = 0;

        var l: usize = @intCast(linelen);
        const lb = line.?;
        if (l > 0 and lb[l - 1] == '\n') {
            l -= 1;
            lb[l] = 0;
        }
        if (l > 0 and lb[l - 1] == '\r') {
            l -= 1;
            lb[l] = 0;
        }
        if (l == 0) continue;

        nf = csvSplit(lb, &fields, 9);
        if (nf < 1 or nf > 8) continue; // empty or >8 fields: skip

        while (i < nf) : (i += 1) {
            if (isInteger(fields[@intCast(i)]) != 0) {
                const val = strtoul(fields[@intCast(i)], null, 10);
                if (val > 0xFFFFFFFF) break; // overflow: skip row
                cols[@intCast(i)] = @truncate(val);
            } else {
                const sym = intern_str(db.ir, fields[@intCast(i)]);
                if (sym == 0) {
                    var j: c_int = 0;
                    while (j < i) : (j += 1) {}
                    if (line) |lp| c.free(@ptrCast(lp));
                    _ = c.fclose(f);
                    var aa: c_int = 1;
                    while (aa <= MAX_VAR_ARITY) : (aa += 1) {
                        if (have[@intCast(aa)] != 0) tupleset.ts_free(&ts[@intCast(aa)]);
                    }
                    return rc;
                }
                cols[@intCast(i)] = sym;
            }
        }
        if (i < nf) continue; // row marked for skip

        if (have[@intCast(nf)] == 0) {
            const vr = vrelation.vrel_variant_or_null(e.vrel, @intCast(nf));
            if (tupleset.ts_init(&ts[@intCast(nf)], @intCast(nf)) != 0) break;
            have[@intCast(nf)] = 1;
            if (vr != null and relation.rel_prefix_base(vr, null, 0, relation.ts_sink_cb, &ts[@intCast(nf)]) < 0) break;
        }
        {
            const trc = tupleset.ts_add(&ts[@intCast(nf)], &cols);
            if (trc < 0) break;
            if (trc == 1) n_new += 1;
        }
    }

    // M7 invariant: interner durable BEFORE any relation DAFSA.
    {
        const fwd_path = makePath(db, "symbols", ".dafsa");
        const rev_path = makePath(db, "symbols", ".array");
        if (fwd_path != null and rev_path != null) {
            if (intern_save(db.ir, fwd_path.?, rev_path.?) != 0) {
                if (fwd_path) |x| c.free(@ptrCast(x));
                if (rev_path) |x| c.free(@ptrCast(x));
                if (line) |lp| c.free(@ptrCast(lp));
                _ = c.fclose(f);
                var aa: c_int = 1;
                while (aa <= MAX_VAR_ARITY) : (aa += 1) {
                    if (have[@intCast(aa)] != 0) tupleset.ts_free(&ts[@intCast(aa)]);
                }
                return rc;
            }
        }
        if (fwd_path) |x| c.free(@ptrCast(x));
        if (rev_path) |x| c.free(@ptrCast(x));
    }

    // v2-lists: term store durable before any relation DAFSA.
    {
        const tpath = makePath(db, "terms", ".bin");
        if (tpath != null) {
            if (term_save(db.terms, tpath.?) != 0) {
                c.free(@ptrCast(tpath.?));
                if (line) |lp| c.free(@ptrCast(lp));
                _ = c.fclose(f);
                var aa: c_int = 1;
                while (aa <= MAX_VAR_ARITY) : (aa += 1) {
                    if (have[@intCast(aa)] != 0) tupleset.ts_free(&ts[@intCast(aa)]);
                }
                return rc;
            }
            c.free(@ptrCast(tpath.?));
        }
    }

    a = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        var path: ?[*:0]u8 = undefined;
        if (have[@intCast(a)] == 0) continue;

        const vr = variadicOpenVariant(db, e, @intCast(a));
        if (vr == null) break;

        tupleset.ts_sort(&ts[@intCast(a)]);
        if (relation.rel_build_base_from_tupleset(vr, &ts[@intCast(a)]) != 0) break;
        loaded += ts[@intCast(a)].count;

        path = if (relation.rel_is_idb(vr) != 0)
            makeVpath(db, rel_name, @intCast(a), ".base.dafsa")
        else
            makeVpath(db, rel_name, @intCast(a), ".dafsa");
        if (path != null) {
            _ = relation.rel_save_base(vr, path.?);
            c.free(@ptrCast(path.?));
        }
    }

    rc = @intCast(loaded);

    if (n_new > 0) {
        // v2 gating: variadic is outside every incremental class.
        db.full_reeval_pending = 1;
        db.fixpoint_dirty = 1;
        permindex_mark_dirty(db, @intCast(idx));
    }

    if (line) |lp| c.free(@ptrCast(lp));
    _ = c.fclose(f);
    a = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        if (have[@intCast(a)] != 0) tupleset.ts_free(&ts[@intCast(a)]);
    }
    return rc;
}

/// IVM Slice 1: is rel_id a rule head (and therefore a derived view)?
fn ivmRelIsHead(db: *const DlDb, rel_id: c_int) bool {
    var i: c_int = 0;
    while (i < db.n_crules) : (i += 1) {
        if (@as(c_int, db.crules.?[@intCast(i)].?.head_rel_id) == rel_id) return true;
    }
    return false;
}

/// IVM Slice 1: append a +delta for rel_id to db->delta_pending (lazy-init).
fn ivmCaptureDelta(db: *DlDb, rel_id: c_int, cols: [*c]const u32, arity: u8) c_int {
    var ts_p = db.delta_pending[@intCast(rel_id)];
    if (ts_p == null) {
        const m = c.malloc(@sizeOf(tupleset.tuple_set)) orelse return -1;
        ts_p = @ptrCast(@alignCast(m));
        if (tupleset.ts_init(ts_p, arity) != 0) {
            c.free(m);
            return -1;
        }
        db.delta_pending[@intCast(rel_id)] = ts_p;
    }
    const rc = tupleset.ts_add(ts_p, cols);
    return if (rc < 0) -1 else 0;
}

/// IVM Slice 3: append a -delta for rel_id to db->del_pending (lazy-init).
fn ivmCaptureDelete(db: *DlDb, rel_id: c_int, cols: [*c]const u32, arity: u8) c_int {
    var ts_p = db.del_pending[@intCast(rel_id)];
    if (ts_p == null) {
        const m = c.malloc(@sizeOf(tupleset.tuple_set)) orelse return -1;
        ts_p = @ptrCast(@alignCast(m));
        if (tupleset.ts_init(ts_p, arity) != 0) {
            c.free(m);
            return -1;
        }
        db.del_pending[@intCast(rel_id)] = ts_p;
    }
    const rc = tupleset.ts_add(ts_p, cols);
    return if (rc < 0) -1 else 0;
}

pub export fn dl_load_facts(db: ?*DlDb, rel_name: [*c]const u8, csv_path: [*c]const u8) c_int {
    const d = db orelse return -1;
    if (rel_name == null or csv_path == null) return -1;
    if (d.read_only != 0) return -1; // write API on RO handle
    if (d.txn != null) return -1; // bulk load during a txn would break atomicity

    const idx = findRel(d, rel_name);
    if (idx < 0) return -1;

    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC)
        return dlLoadFactsVariadic(d, @intCast(idx), rel_name, csv_path);

    const arity = relation.rel_arity(d.rels[@intCast(idx)].rel);

    const f = c.fopen(csv_path, "r") orelse return -1;

    var ts: tupleset.tuple_set = undefined;
    var delta: tupleset.tuple_set = undefined;
    if (tupleset.ts_init(&ts, arity) != 0) {
        _ = c.fclose(f);
        return -1;
    }
    if (tupleset.ts_init(&delta, arity) != 0) {
        tupleset.ts_free(&ts);
        _ = c.fclose(f);
        return -1;
    }

    // Union the relation's pre-existing BASE into ts.
    if (relation.rel_prefix_base(d.rels[@intCast(idx)].rel, null, 0, relation.ts_sink_cb, &ts) < 0) {
        tupleset.ts_free(&ts);
        tupleset.ts_free(&delta);
        _ = c.fclose(f);
        return -1;
    }

    var line: ?[*]u8 = null;
    var linecap: usize = 0;
    var loaded: c_int = 0;
    var delta_failed: c_int = 0;
    var n_new: c_int = 0;
    var lineno: c_int = 0;

    // U-homog (G1): capture pre-load state for the rollback.  A fact-load is
    // NOT atomic by ordering (unlike SH2's pre-append recheck): the kind is
    // recorded per-cell DURING the parse, before the base is rebuilt.  EVERY
    // failure return in the parse loop below (the mixed-conflict rejects, the
    // intern/ts_add OOMs) AND the post-parse recheck (kindRecheckAfterLoad)
    // must undo the kind mutations so a failed load leaves no partial kind.
    // Track which columns of THIS relation transitioned 0 -> definite (their
    // pre-load value was 0, so restore = clear) and the pre-load meta_dirty;
    // loadKindRollback() applies the restore at every failure site.
    const load_rel = d.rels[@intCast(idx)].rel;
    const meta_dirty_before: c_int = d.meta_dirty;
    var new_cols: [8]bool = .{false} ** 8;

    var linelen: isize = getline(&line, &linecap, f);
    while (linelen > 0) : (linelen = getline(&line, &linecap, f)) {
        var fields: [8][*c]u8 = undefined;
        var nf: c_int = undefined;
        var cols: [8]u32 = undefined;
        var i: c_int = 0;

        var l: usize = @intCast(linelen);
        const lb = line.?;
        if (l > 0 and lb[l - 1] == '\n') {
            l -= 1;
            lb[l] = 0;
        }
        if (l > 0 and lb[l - 1] == '\r') {
            l -= 1;
            lb[l] = 0;
        }
        if (l == 0) continue;
        lineno += 1;

        nf = csvSplit(lb, &fields, @intCast(arity));
        if (nf != @as(c_int, arity)) continue; // wrong field count: skip

        while (i < nf) : (i += 1) {
            if (isInteger(fields[@intCast(i)]) != 0) {
                const val = strtoul(fields[@intCast(i)], null, 10);
                if (val > 0xFFFFFFFF) {
                    cols[0] = 0; // force skip
                    break;
                }
                cols[@intCast(i)] = @truncate(val);
                // U-homog S2: record the kind exactly where it is known.
                const kn = kindNoteDefinite(d, @intCast(idx), @intCast(i), relation.KIND_INT);
                if (kn < 0) {
                    dlErr("error: {s}:{d}: mixed int/symbol values in column {d} of {s}\n", .{ csv_path, lineno, i + 1, rel_name });
                    loadKindRollback(d, load_rel, &new_cols, arity, meta_dirty_before);
                    tupleset.ts_free(&ts);
                    tupleset.ts_free(&delta);
                    _ = c.fclose(f);
                    if (line) |lp| c.free(@ptrCast(lp));
                    return -1;
                }
                if (kn == 1) new_cols[@intCast(i)] = true;
            } else {
                const sym = intern_str(d.ir, fields[@intCast(i)]);
                if (sym == 0) {
                    loadKindRollback(d, load_rel, &new_cols, arity, meta_dirty_before);
                    tupleset.ts_free(&ts);
                    tupleset.ts_free(&delta);
                    _ = c.fclose(f);
                    if (line) |lp| c.free(@ptrCast(lp));
                    return -1;
                }
                cols[@intCast(i)] = sym;
                const kn = kindNoteDefinite(d, @intCast(idx), @intCast(i), relation.KIND_SYM);
                if (kn < 0) {
                    dlErr("error: {s}:{d}: mixed int/symbol values in column {d} of {s}\n", .{ csv_path, lineno, i + 1, rel_name });
                    loadKindRollback(d, load_rel, &new_cols, arity, meta_dirty_before);
                    tupleset.ts_free(&ts);
                    tupleset.ts_free(&delta);
                    _ = c.fclose(f);
                    if (line) |lp| c.free(@ptrCast(lp));
                    return -1;
                }
                if (kn == 1) new_cols[@intCast(i)] = true;
            }
        }

        {
            const rc = tupleset.ts_add(&ts, &cols);
            if (rc < 0) {
                loadKindRollback(d, load_rel, &new_cols, arity, meta_dirty_before);
                tupleset.ts_free(&ts);
                tupleset.ts_free(&delta);
                _ = c.fclose(f);
                if (line) |lp| c.free(@ptrCast(lp));
                return -1;
            }
            if (rc == 1) {
                n_new += 1;
                if (delta_failed == 0 and tupleset.ts_add(&delta, &cols) < 0)
                    delta_failed = 1; // OOM: fall back to full re-eval
            }
        }
    }

    if (line) |lp| c.free(@ptrCast(lp));
    _ = c.fclose(f);

    // U-homog (G1): populate-after-load recheck.  If this load NEWLY
    // established a column kind on a db that already holds compiled rules,
    // re-run the kind check over the resident AST BEFORE the base is rebuilt
    // or anything is flushed to disk.  A reject here means a rule that
    // referenced the now-kind column as UNKNOWN now conflicts; fail the LOAD
    // loudly.  Run before rel_build_base / intern_save / rel_save_base so a
    // rejected load leaves the base, interner-on-disk and DAFSA untouched —
    // only the transient col_kind mutation needs rolling back (below).
    if (anyNewCol(&new_cols, arity)) {
        if (kindRecheckAfterLoad(d) != 0) {
            // Rollback (RK-G1-rollback): restore the recorded col_kind and
            // meta_dirty to their pre-load state so a failed load leaves no
            // partial kind.  The facts live only in the transient ts
            // (discarded); the base was never rebuilt.
            loadKindRollback(d, load_rel, &new_cols, arity, meta_dirty_before);
            tupleset.ts_free(&ts);
            tupleset.ts_free(&delta);
            return -1;
        }
    }

    tupleset.ts_sort(&ts);
    if (relation.rel_build_base_from_tupleset(d.rels[@intCast(idx)].rel, &ts) != 0) {
        tupleset.ts_free(&ts);
        tupleset.ts_free(&delta);
        return -1;
    }

    loaded = @intCast(ts.count);
    tupleset.ts_free(&ts);

    // M7: save interner BEFORE relation save.
    {
        const fwd_path = makePath(d, "symbols", ".dafsa");
        const rev_path = makePath(d, "symbols", ".array");
        if (fwd_path != null and rev_path != null) {
            if (intern_save(d.ir, fwd_path.?, rev_path.?) != 0) {
                if (fwd_path) |x| c.free(@ptrCast(x));
                if (rev_path) |x| c.free(@ptrCast(x));
                tupleset.ts_free(&delta);
                return -1;
            }
        }
        if (fwd_path) |x| c.free(@ptrCast(x));
        if (rev_path) |x| c.free(@ptrCast(x));
    }

    // v2-lists: term store durable before the relation base DAFSA.
    {
        const tpath = makePath(d, "terms", ".bin");
        if (tpath != null) {
            if (term_save(d.terms, tpath.?) != 0) {
                c.free(@ptrCast(tpath.?));
                tupleset.ts_free(&delta);
                return -1;
            }
            c.free(@ptrCast(tpath.?));
        }
    }

    // Auto-save the relation's BASE DAFSA after load (bulk load bypasses WAL).
    {
        const path = if (relation.rel_is_idb(d.rels[@intCast(idx)].rel) != 0)
            makePath(d, rel_name, ".base.dafsa")
        else
            makePath(d, rel_name, ".dafsa");
        if (path != null) {
            _ = relation.rel_save_base(d.rels[@intCast(idx)].rel, path.?);
            c.free(@ptrCast(path.?));
        }
    }

    // IVM Slice 5: bulk load is a BATCHED DELTA.
    if (n_new > 0) {
        if (delta_failed != 0 or ivmRelIsHead(d, idx)) {
            d.full_reeval_pending = 1;
        } else {
            var di: c_long = 0;
            while (di < delta.count) : (di += 1) {
                if (ivmCaptureDelta(d, idx, delta.data.? + @as(usize, @intCast(di)) * arity, arity) != 0) {
                    d.full_reeval_pending = 1;
                    break;
                }
            }
        }
        tupleset.ts_free(&delta);

        d.fixpoint_dirty = 1;

        // M6: mark permutation indices of this relation dirty
        var pi: c_int = 0;
        while (pi < d.n_perms) : (pi += 1) {
            if (d.perms[@intCast(pi)].rel_id == idx)
                d.perms[@intCast(pi)].dirty = 1;
        }
    } else {
        tupleset.ts_free(&delta);
    }

    return loaded;
}

// ─── Incremental fact API (M7) ─────────────────────────────────────────────

fn encodeFactKey(key: [*c]u8, key_len: *usize, cols: [*c]const u32, arity: u8) c_int {
    var i: u8 = 0;
    if (arity == 0 or arity > 8) return -1;
    while (i < arity) : (i += 1) {
        const v = cols[i];
        key[4 * @as(usize, i)] = @truncate(v >> 24);
        key[4 * @as(usize, i) + 1] = @truncate(v >> 16);
        key[4 * @as(usize, i) + 2] = @truncate(v >> 8);
        key[4 * @as(usize, i) + 3] = @truncate(v);
    }
    key[4 * @as(usize, arity)] = 0x00;
    key_len.* = 4 * @as(usize, arity) + 1;
    return 0;
}

/// v2: variadic add — WAL-backed variant for `arity`.
fn dlAddFactVariadic(db: *DlDb, idx: usize, rel_name: [*c]const u8, cols: [*c]const u32, arity: u8) c_int {
    _ = rel_name; // retained for signature parity; the variant rel carries paths
    var key: [33]u8 = undefined;
    var key_len: usize = undefined;

    if (arity == 0 or arity > 8) return -1;

    const vr = variadicOpenVariant(db, &db.rels[idx], arity) orelse return -1;

    if (encodeFactKey(&key, &key_len, cols, arity) != 0) return -1;

    // 1. Interner-before-WAL invariant.
    if (intern_is_dirty(db.ir) != 0) {
        const fwd_path = makePath(db, "symbols", ".dafsa");
        const rev_path = makePath(db, "symbols", ".array");
        if (fwd_path == null or rev_path == null) {
            if (fwd_path) |x| c.free(@ptrCast(x));
            if (rev_path) |x| c.free(@ptrCast(x));
            return -1;
        }
        if (intern_save(db.ir, fwd_path.?, rev_path.?) != 0) {
            c.free(@ptrCast(fwd_path.?));
            c.free(@ptrCast(rev_path.?));
            return -1;
        }
        c.free(@ptrCast(fwd_path.?));
        c.free(@ptrCast(rev_path.?));
    }

    // v2-lists: term store durable before a WAL referencing a list handle.
    if (term_is_dirty(db.terms) != 0) {
        const tpath = makePath(db, "terms", ".bin") orelse return -1;
        if (term_save(db.terms, tpath) != 0) {
            c.free(@ptrCast(tpath));
            return -1;
        }
        c.free(@ptrCast(tpath));
    }

    // 2. Duplicate check against the variant VIEW (base ∪ overlay).
    if (relation.rel_exact(vr, cols) != 0)
        return 0;

    // 3. WAL-append ADD (no fsync — durable at the next cycle
    //    boundary, see dl_publish_snapshot/dl_close).
    if (relation.rel_wal_append_add(vr, &key, @intCast(key_len)) != 0)
        return -1;

    // 4. In-memory add to the variant OVERLAY (hot-write; DAFSA untouched
    //    until consolidation).
    const rc = relation.rel_overlay_add(vr, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(db, @intCast(idx));
        db.full_reeval_pending = 1;
    }

    db.fixpoint_dirty = 1;
    return 1; // added
}

/// How many adds between compaction-threshold evaluations.  The threshold
/// check itself (rel_dafsa_size -> dafsa_stats) is a fresh O(states) BFS —
/// measured 6.2ms at 100k states, 34.7ms at 800k — so it MUST NOT run per
/// add (it made dl_add_fact 2500x slower than the DAFSA insert itself).
/// 64 adds amortizes it to O(states/64) while still compacting promptly.
const COMPACT_CHECK_EVERY: u64 = 64;

// ─── U-homog (S2): column-kind recording at the insert sites ──────────────

/// Record a column kind on a fixed relation (CSV site — the cell's STRING
/// form makes the kind definite).  Returns -1 on a CONFLICT with the
/// recorded kind (the caller rejects the load loudly); 1 when this call
/// NEWLY established a kind (a 0 -> definite transition, the only case that
/// can make a previously-UNKNOWN column conflict with a resident rule — G1
/// re-checks only on this return); 0 when the kind was already recorded
/// (idempotent, no new establishment).  Bumps meta_dirty when the recording
/// makes rels.txt stale (a kinds field it does not yet carry), so the kinds
/// persist across the CLI's separate processes.
fn kindNoteDefinite(db: *DlDb, idx: usize, col: usize, kind: u8) c_int {
    const rel = db.rels[idx].rel orelse return 0;
    const prev = relation.rel_col_kind(rel, @intCast(col));
    if (relation.rel_kind_note(rel, @intCast(col), kind) != 0) return -1;
    if (prev == 0) {
        if (db.read_only == 0) db.meta_dirty = 1;
        return 1; // newly recorded: G1 re-check trigger
    }
    return 0;
}

/// Does the open txn hold a buffered ADD for relation `idx`?  The txn site
/// kind-checks at BUFFER time (rows apply at commit), so an unrecorded
/// column about to receive a buffered row is NOT first-row-empty.
fn relHasPendingTxnAdd(db: *DlDb, idx: usize) bool {
    const t = db.txn orelse return false;
    const ops = t.ops orelse return false;
    var i: usize = 0;
    while (i < t.nops) : (i += 1) {
        if (ops[i].kind == TXN_ADD and ops[i].rel_id == @as(c_int, @intCast(idx))) return true;
    }
    return false;
}

/// Kind-check + record one raw-u32 fact (dl_add_fact / dl_txn_add_fact).
/// The u32 API cannot observe a value's space, but ONE direction is sound:
/// a value that does NOT resolve as a live sym id (ids are dense
/// 1..next_id-1) is DEFINITELY a raw int — REJECT it against a sym-kind
/// column.  A resolving value is ambiguous (a raw int below next_id
/// resolves too, whenever any symbol is interned), so it neither records
/// nor rejects: a loud-but-wrong reject is worse than the gap.
/// RECORDING is narrower still (review SH1): an unrecorded column may
/// already hold AMBIGUOUS (resolving) values that were deliberately never
/// recorded — recording INT when a definite int arrives later would
/// mislabel a column that holds BOTH spaces, blessing the exact collision
/// this check exists to catch.  So INT is recorded only when the fact is
/// the column's FIRST row (relation empty, no buffered txn ADD) — the one
/// case where "first definite" really is "first value".  Returns 1 when
/// this call NEWLY recorded INT (a 0 -> INT transition — the G1 re-check
/// trigger), 0 otherwise, or -1 on the definite conflict.
fn kindNoteRaw(db: *DlDb, idx: usize, cols: [*c]const u32, arity: u8) c_int {
    const rel = db.rels[idx].rel orelse return 0;
    const bound = intern_mod.liveSymBound(db.ir);
    var newly: c_int = 0;
    var i: usize = 0;
    while (i < arity) : (i += 1) {
        const v = cols[i];
        const resolves = v > 0 and v < bound and intern_str_of(db.ir, v) != null;
        if (resolves) continue; // ambiguous: neither records nor rejects
        const prev = relation.rel_col_kind(rel, @intCast(i));
        if (prev == relation.KIND_SYM) {
            dlErr("error: mixed int/symbol values in column {d} of {s} (value {d} is not a live symbol id)\n", .{ i + 1, db.rels[idx].name orelse @as([*c]const u8, "?"), v });
            return -1;
        }
        if (prev == 0) {
            if (relation.rel_count(rel) == 0 and !relHasPendingTxnAdd(db, idx)) {
                if (relation.rel_kind_note(rel, @intCast(i), relation.KIND_INT) != 0) return -1;
                if (db.read_only == 0) db.meta_dirty = 1;
                newly = 1; // newly recorded: G1 re-check trigger
            }
        }
    }
    return newly;
}

// ─── U-homog (G1): populate-after-load recheck ───────────────────────────

/// G1: when a fact-load NEWLY establishes a definite column kind (a 0 ->
/// INT/SYM transition) on a db that ALREADY holds compiled rules
/// (n_ast_rules > 0), re-run the kind check over the resident AST.  A rule
/// that referenced the column as UNKNOWN at compile time (empty column ->
/// permissive) may now conflict with the newly-recorded kind.  This mirrors
/// the SH2 merged re-check dl_load_rules does at :3108 — the same
/// checkAllRuleConstKinds over the resident AST — but fired by a FACT load
/// rather than a rule load.
///
/// Fires ONLY on a real 0 -> definite transition: an idempotent re-load of
/// the SAME kind into an already-kind column does NOT trigger (the
/// recording helpers return "newly recorded" only on that transition), so a
/// relation filled over several loads stays permissive after the first.
/// Facts loaded BEFORE the rules (dlb's order) never trigger: ast_rules is
/// empty at fact-load time.  Cost is O(rules*rounds) per TRIGGERING load
/// only — MEASURED 0.30ms at 200 rules, 0.08ms at 50 — never per query.
///
/// Returns 0 (ok) or -1 (loud load failure: the check emits its diagnostic
/// to the compile error sink; the caller rolls back the kind mutation and
/// fails the LOAD, not a later query).
fn kindRecheckAfterLoad(db: *DlDb) c_int {
    if (db.n_ast_rules <= 0) return 0;
    const ars = db.ast_rules orelse return 0;
    compiler.compile_clear_error();
    return compiler.checkAllRuleConstKinds(db, ars, db.n_ast_rules);
}

/// True if any column in `cols[0..arity)` was newly recorded this load.
fn anyNewCol(cols: *const [8]bool, arity: u8) bool {
    var i: usize = 0;
    while (i < arity) : (i += 1) {
        if (cols[i]) return true;
    }
    return false;
}

/// U-homog (G1): the fact-load rollback.  Restore the RECORDED col_kind of
/// every newly-recorded column to 0 (their pre-load value) and meta_dirty to
/// its pre-load state, so a rejected load leaves no partial kind.  Called at
/// EVERY failure return in dl_load_facts (the mid-CSV mixed-conflict and OOM
/// paths fail with EARLIER rows' kinds already recorded) — the all-or-nothing
/// discipline the G1 recheck path already had.
fn loadKindRollback(db: *DlDb, rel: ?*relation.Relation, cols: *const [8]bool, arity: u8, meta_dirty_before: c_int) void {
    if (rel) |rp| {
        var ci: u8 = 0;
        while (ci < arity) : (ci += 1) {
            if (cols[ci]) relation.rel_col_kind_set(rp, ci, 0);
        }
    }
    db.meta_dirty = meta_dirty_before;
}


/// CAS Slice 2: apply a fact add to the in-memory BASE + IVM delta capture.
/// selfreg-dl-storage: the hot write lands in the OVERLAY (sorted-array
/// tupleset, O(1) amortized) + the WAL (the durability record, fsync'd at
/// cycle boundaries — selfreg-batched-fsync); the DAFSA is only touched
/// at consolidation (dl_publish_snapshot -> rel_overlay_flush).
fn addFactApply(db: *DlDb, idx: c_int, cols: [*c]const u32, arity: u8) c_int {
    const rel = db.rels[@intCast(idx)].rel;
    const rc = relation.rel_overlay_add(rel, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(db, idx);
        if (ivmRelIsHead(db, idx)) {
            db.full_reeval_pending = 1;
        } else if (ivmCaptureDelta(db, idx, cols, arity) != 0) {
            db.full_reeval_pending = 1;
        }
    }

    // Compaction threshold (throttled — see COMPACT_CHECK_EVERY).
    {
        db.compact_checks +%= 1;
        if (db.compact_checks % COMPACT_CHECK_EVERY == 0) {
            maybeCompact(db, idx);
        }
    }

    db.fixpoint_dirty = 1;
    return if (rc == 1) 1 else 0;
}

/// Evaluate + fire the WAL/dafsa compaction threshold for one relation.
/// Shared by the throttled add path and consolidation.
fn maybeCompact(db: *DlDb, idx: c_int) void {
    const rel = db.rels[@intCast(idx)].rel;
    const wal_sz = relation.rel_wal_size(rel);
    const dafsa_sz = relation.rel_dafsa_size(rel);
    if (dafsa_sz > 0 and wal_sz > dafsa_sz / 4) {
        // Overlay facts must be IN the DAFSA before the WAL is truncated,
        // or a crash between truncate and re-open loses them.
        if (relation.rel_overlay_flush(rel) != 0) return;
        // rel_compact ftruncates the WAL: a state replacement, so it runs
        // inside the mutation window (a reader's open-time replay must not
        // see the truncation).
        mutWindowBegin(db);
        defer mutWindowEnd(db);
        const path = if (relation.rel_is_idb(rel) != 0)
            makePath(db, db.rels[@intCast(idx)].name, ".base.dafsa")
        else
            makePath(db, db.rels[@intCast(idx)].name, ".dafsa");
        if (path != null) {
            _ = relation.rel_compact(rel, path.?);
            c.free(@ptrCast(path.?));
        }
    }
}

/// CAS Slice 2: apply a fact delete to the in-memory BASE + IVM -delta.
fn deleteFactApply(db: *DlDb, idx: c_int, cols: [*c]const u32, arity: u8) c_int {
    // A pending overlay fact must be materialized before its DEL (C19:
    // deletes only ever target the engine/recomputable layer).
    if (relation.rel_overlay_flush(db.rels[@intCast(idx)].rel) != 0) return -1;
    const rc = relation.rel_delete_base(db.rels[@intCast(idx)].rel, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(db, idx);
        if (ivmCaptureDelete(db, idx, cols, arity) != 0)
            db.full_reeval_pending = 1;
    }

    db.fixpoint_dirty = 1;
    return if (rc == 1) 1 else 0;
}

pub export fn dl_add_fact(db: ?*DlDb, rel_name: [*c]const u8, cols: [*c]const u32, arity: u8) c_int {
    const d = db orelse return -1;
    if (rel_name == null or cols == null) return -1;
    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1;
    if (strEq(rel_name, "rev")) return -1;

    const idx = findRel(d, rel_name);
    if (idx < 0) return -1;

    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC)
        return dlAddFactVariadic(d, @intCast(idx), rel_name, cols, arity);

    if (arity != relation.rel_arity(d.rels[@intCast(idx)].rel)) return -1;

    // U-homog S2: kind-check + record BEFORE any durable effect (the WAL
    // append below) so a rejected fact leaves no trace.
    const add_rel = d.rels[@intCast(idx)].rel;
    var kk_before: [8]u8 = .{0} ** 8;
    if (add_rel) |rel| {
        var ci: u8 = 0;
        while (ci < arity) : (ci += 1) kk_before[ci] = relation.rel_col_kind(rel, ci);
    }
    const kn = kindNoteRaw(d, @intCast(idx), cols, arity);
    if (kn < 0) {
        // B1 (review): kindNoteRaw walks the columns left-to-right RECORDING
        // INT as it goes, so a conflict on a later column leaves the earlier
        // columns recorded.  Restore the snapshot so a rejected add is
        // all-or-nothing for kinds (the same contract the G1 branch below
        // already enforces for its reject).
        if (add_rel) |rel| {
            var ci: u8 = 0;
            while (ci < arity) : (ci += 1)
                relation.rel_col_kind_set(rel, ci, kk_before[ci]);
        }
        return -1;
    }

    // U-homog (G1): if this add NEWLY established a column kind on a db with
    // resident rules, recheck the resident AST.  On a reject, roll back the
    // kind mutation (the fact has not been written yet — no WAL, no overlay)
    // and fail the add loudly.
    if (kn == 1) {
        if (kindRecheckAfterLoad(d) != 0) {
            if (add_rel) |rel| {
                var ci: u8 = 0;
                while (ci < arity) : (ci += 1)
                    relation.rel_col_kind_set(rel, ci, kk_before[ci]);
            }
            return -1;
        }
    }

    var key: [33]u8 = undefined;
    var key_len: usize = undefined;
    if (encodeFactKey(&key, &key_len, cols, arity) != 0) return -1;

    // 1. Interner-before-WAL invariant.
    if (intern_is_dirty(d.ir) != 0) {
        const fwd_path = makePath(d, "symbols", ".dafsa");
        const rev_path = makePath(d, "symbols", ".array");
        if (fwd_path == null or rev_path == null) {
            if (fwd_path) |x| c.free(@ptrCast(x));
            if (rev_path) |x| c.free(@ptrCast(x));
            return -1;
        }
        if (intern_save(d.ir, fwd_path.?, rev_path.?) != 0) {
            c.free(@ptrCast(fwd_path.?));
            c.free(@ptrCast(rev_path.?));
            return -1;
        }
        c.free(@ptrCast(fwd_path.?));
        c.free(@ptrCast(rev_path.?));
    }

    // v2-lists: term store durable before a WAL referencing a list handle.
    if (term_is_dirty(d.terms) != 0) {
        const tpath = makePath(d, "terms", ".bin") orelse return -1;
        if (term_save(d.terms, tpath) != 0) {
            c.free(@ptrCast(tpath));
            return -1;
        }
        c.free(@ptrCast(tpath));
    }

    // 2. Duplicate check against BASE ∪ overlay (supersession re-asserts
    //    and true duplicates both land here; C19 latest-wins is the set
    //    semantics of the key).
    if (relation.rel_exact(d.rels[@intCast(idx)].rel, cols) != 0)
        return 0;

    // 3. WAL-append ADD (no fsync — durable at the next cycle
    //    boundary, see dl_publish_snapshot/dl_close).
    if (relation.rel_wal_append_add(d.rels[@intCast(idx)].rel, &key, @intCast(key_len)) != 0)
        return -1;

    // 4. In-memory add + IVM capture.
    return addFactApply(d, idx, cols, arity);
}

/// v2: variadic delete.
fn dlDeleteFactVariadic(db: *DlDb, idx: usize, cols: [*c]const u32, arity: u8) c_int {
    var key: [33]u8 = undefined;
    var key_len: usize = undefined;

    if (arity == 0 or arity > 8) return -1;

    const vr = variadicOpenVariant(db, &db.rels[idx], arity) orelse return -1;

    if (encodeFactKey(&key, &key_len, cols, arity) != 0) return -1;

    // 1. Absent check against the variant VIEW; flush overlay so the DEL
    //    can remove the fact from the DAFSA (C19).
    if (relation.rel_exact(vr, cols) == 0)
        return 0;
    if (relation.rel_overlay_flush(vr) != 0)
        return -1;

    // 2. WAL-append DEL (no fsync — durable at the next cycle
    //    boundary, see dl_publish_snapshot/dl_close).
    if (relation.rel_wal_append_del(vr, &key, @intCast(key_len)) != 0)
        return -1;

    // 3. In-memory delete from the variant BASE.
    const rc = relation.rel_delete_base(vr, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(db, @intCast(idx));
        db.full_reeval_pending = 1;
    }

    db.fixpoint_dirty = 1;
    return 1; // deleted
}

pub export fn dl_delete_fact(db: ?*DlDb, rel_name: [*c]const u8, cols: [*c]const u32, arity: u8) c_int {
    const d = db orelse return -1;
    if (rel_name == null or cols == null) return -1;
    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1;
    if (strEq(rel_name, "rev")) return -1;

    const idx = findRel(d, rel_name);
    if (idx < 0) return -1;

    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC)
        return dlDeleteFactVariadic(d, @intCast(idx), cols, arity);

    if (arity != relation.rel_arity(d.rels[@intCast(idx)].rel)) return -1;

    var key: [33]u8 = undefined;
    var key_len: usize = undefined;
    if (encodeFactKey(&key, &key_len, cols, arity) != 0) return -1;

    // 1. Absent check against the VIEW (base ∪ overlay) — a fact still in
    //    the overlay must flush into the DAFSA before a DEL can remove it
    //    (deletes exist only on the recomputable/engine layer, C19).
    if (relation.rel_exact(d.rels[@intCast(idx)].rel, cols) == 0)
        return 0;
    if (relation.rel_overlay_flush(d.rels[@intCast(idx)].rel) != 0)
        return -1;

    // 2. WAL-append DEL (no fsync — durable at the next cycle
    //    boundary, see dl_publish_snapshot/dl_close).
    if (relation.rel_wal_append_del(d.rels[@intCast(idx)].rel, &key, @intCast(key_len)) != 0)
        return -1;

    // 3. In-memory delete + IVM capture.
    return deleteFactApply(d, idx, cols, arity);
}

// ─── CAS revision API ──────────────────────────────────────────────────────

/// Ensure the internal arity-2 "rev" relation exists, caching its index.
fn ensureRevRel(db: *DlDb) c_int {
    if (db.rev_rel_id >= 0)
        return db.rev_rel_id;
    var idx = findRel(db, "rev");
    if (idx < 0) {
        if (dl_declare_relation(db, "rev", 2) != 0)
            return -1;
        idx = findRel(db, "rev");
        if (idx < 0) return -1;
    }
    db.rev_rel_id = idx;
    return idx;
}

/// Sink for the rev relation's single tuple under an entity prefix.
fn revSinkCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const out: *u32 = @ptrCast(@alignCast(user.?));
    out.* = cols.?[1];
    return 1; // stop after the single match
}

/// Read the current revision for entity_sym into *out (0 if no rev row).
fn revGet(db: *DlDb, entity_sym: u32, out: *u32) c_int {
    const rel_id = ensureRevRel(db);
    if (rel_id < 0) return -1;
    const rel = db.rels[@intCast(rel_id)].rel orelse return -1;
    out.* = 0;
    const n = relation.rel_prefix(rel, @ptrCast(&entity_sym), 1, revSinkCb, out);
    if (n < 0) return -1;
    return 0;
}

pub export fn dl_cas_revision(db: ?*DlDb, entity: [*c]const u8, expected: u32, new_value: u32) c_int {
    const d = db orelse return -1;
    if (entity == null) return -1;
    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1;
    if (expected == new_value) return 0; // idempotent no-op

    const rel_id = ensureRevRel(d);
    if (rel_id < 0) return -1;
    const rel = d.rels[@intCast(rel_id)].rel orelse return -1;

    const entity_sym = intern_str(d.ir, entity);
    if (entity_sym == 0) return -1; // OOM

    var cur: u32 = 0;
    if (revGet(d, entity_sym, &cur) != 0) return -1;
    if (cur != expected) return DL_E_CONFLICT;

    // Interner-before-WAL invariant.
    if (intern_is_dirty(d.ir) != 0) {
        const fwd_path = makePath(d, "symbols", ".dafsa");
        const rev_path = makePath(d, "symbols", ".array");
        if (fwd_path == null or rev_path == null) {
            if (fwd_path) |x| c.free(@ptrCast(x));
            if (rev_path) |x| c.free(@ptrCast(x));
            return -1;
        }
        if (intern_save(d.ir, fwd_path.?, rev_path.?) != 0) {
            c.free(@ptrCast(fwd_path.?));
            c.free(@ptrCast(rev_path.?));
            return -1;
        }
        c.free(@ptrCast(fwd_path.?));
        c.free(@ptrCast(rev_path.?));
    }

    var key: [33]u8 = undefined;
    var key_len: usize = undefined;
    var old_cols: [2]u32 = undefined;
    var new_cols: [2]u32 = undefined;

    // 1. DELETE the old row.
    old_cols[0] = entity_sym;
    old_cols[1] = cur;
    if (encodeFactKey(&key, &key_len, &old_cols, 2) != 0) return -1;
    if (relation.rel_wal_append_del(rel, &key, @intCast(key_len)) != 0) return -1;
    var rc = relation.rel_delete_base(rel, &old_cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(d, rel_id);
        if (ivmCaptureDelete(d, rel_id, &old_cols, 2) != 0)
            d.full_reeval_pending = 1;
    }

    // 2. ADD the new row (overlay hot path; supersession via delete+add of
    //    the versioned row — the engine's CAS semantics, not a C19 change).
    new_cols[0] = entity_sym;
    new_cols[1] = new_value;
    if (encodeFactKey(&key, &key_len, &new_cols, 2) != 0) return -1;
    if (relation.rel_wal_append_add(rel, &key, @intCast(key_len)) != 0) return -1;
    rc = relation.rel_overlay_add(rel, &new_cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(d, rel_id);
        if (ivmCaptureDelta(d, rel_id, &new_cols, 2) != 0)
            d.full_reeval_pending = 1;
    }

    // 3. Compaction threshold (throttled with the add path's counter).
    {
        d.compact_checks +%= 1;
        if (d.compact_checks % COMPACT_CHECK_EVERY == 0)
            maybeCompact(d, rel_id);
    }

    d.fixpoint_dirty = 1;
    return 0; // CAS succeeded
}

pub export fn dl_rev_get(db: ?*DlDb, entity: [*c]const u8, out: ?*u32) c_int {
    const d = db orelse return -1;
    if (entity == null or out == null) return -1;
    if (d.read_only != 0) {
        // Read-only lookup: never declare rev, never intern a new sym.
        const rel_id = findRel(d, "rev");
        if (rel_id < 0) {
            out.?.* = 0;
            return 0;
        }
        const rel = d.rels[@intCast(rel_id)].rel orelse return -1;
        const entity_sym = intern_str_find(d.ir, entity);
        if (entity_sym == 0) {
            out.?.* = 0;
            return 0;
        }
        out.?.* = 0;
        const n = relation.rel_prefix(rel, @ptrCast(&entity_sym), 1, revSinkCb, out.?);
        return if (n < 0) -1 else 0;
    }
    if (ensureRevRel(d) < 0) return -1;
    const entity_sym = intern_str(d.ir, entity);
    if (entity_sym == 0) return -1;
    return revGet(d, entity_sym, out.?);
}

// ─── CAS Slice 2: transaction buffer ───────────────────────────────────────

/// Free the transaction buffer and clear db->txn.
fn txnDiscard(db: *DlDb) void {
    const t = db.txn orelse return;
    if (t.ops) |ops| c.free(@ptrCast(ops));
    c.free(@ptrCast(t));
    db.txn = null;
}

/// Append a copy of `op` to the open transaction's buffer.
fn txnBufOp(db: *DlDb, op: *const TxnOp) c_int {
    const t = db.txn.?;
    if (t.nops == t.cap) {
        const ncap: usize = if (t.cap != 0) t.cap * 2 else 8;
        const no = c.realloc(@ptrCast(t.ops), ncap * @sizeOf(TxnOp)) orelse return -1;
        t.ops = @ptrCast(@alignCast(no));
        t.cap = ncap;
    }
    t.ops.?[t.nops] = op.*;
    t.nops += 1;
    return 0;
}

// ─── U-homog: txn kind-rollback snapshot ───────────────────────────────────

/// Snapshot rel's col_kind[0..arity) into the txn ONCE (first add for that
/// relation) so dl_txn_rollback can undo what dl_txn_add_fact recorded at
/// BUFFER time.  O(arity) per distinct relation per txn; bounded by
/// MAX_RELS slots (a 65th distinct relation in one txn just loses its
/// rollback coverage — the pre-txn behavior).
fn txnKindSnap(db: *DlDb, rel_id: c_int, arity: u8) void {
    const t = db.txn orelse return;
    if (t.ksnap_n >= MAX_RELS) return; // snapshot full: no coverage
    const rel = db.rels[@intCast(rel_id)].rel orelse return;
    var i: usize = 0;
    while (i < t.ksnap_n) : (i += 1) {
        if (t.ksnap_rels[i] == rel_id) return; // already snapshotted
    }
    const s = t.ksnap_n;
    t.ksnap_rels[s] = rel_id;
    t.ksnap_arities[s] = arity;
    var ci: u8 = 0;
    while (ci < arity) : (ci += 1)
        t.ksnap_kinds[s * 8 + ci] = relation.rel_col_kind(rel, ci);
    t.ksnap_n += 1;
}

/// Restore every snapshotted relation's col_kind (dl_txn_rollback only —
/// the commit success path never calls this).
fn txnKindRestore(db: *DlDb) void {
    const t = db.txn orelse return;
    var s: usize = 0;
    while (s < t.ksnap_n) : (s += 1) {
        const rel_id = t.ksnap_rels[s];
        if (rel_id < 0) continue;
        if (db.rels[@intCast(rel_id)].rel) |rel| {
            const art = t.ksnap_arities[s];
            var ci: u8 = 0;
            while (ci < art) : (ci += 1)
                relation.rel_col_kind_set(rel, ci, t.ksnap_kinds[s * 8 + ci]);
        }
    }
}

/// Abort an open txn: undo the buffer-time kind recordings (the buffered
/// rows never applied — exactly the rollback condition), then free the
/// buffer.  B2 (review): EVERY failure return of dl_txn_commit goes through
/// here — including the ROUTINE CAS-conflict DL_E_CONFLICT — not just
/// dl_txn_rollback.
///
/// DURABILITY: the buffer-time kindNoteRaw set meta_dirty, and
/// ensureRevRel's eager dl_declare_relation may have already flushed the
/// poisoned kind to rels.txt, so restoring only the in-memory col_kind
/// leaves the poison durable across reopen (MEASURED).  When a snapshotted
/// kind actually differs from its restored value, rewrite rels.txt so the
/// file matches the restored state, then restore the begin-time meta_dirty
/// (unconditionally clearing it would drop a LEGITIMATE pre-txn unflushed
/// kind's pending close-time flush).
fn txnAbort(db: *DlDb) void {
    const kinds_changed = blk: {
        const t = db.txn orelse break :blk false;
        var s: usize = 0;
        while (s < t.ksnap_n) : (s += 1) {
            const rel_id = t.ksnap_rels[s];
            if (rel_id < 0) break;
            if (db.rels[@intCast(rel_id)].rel) |rel| {
                const art = t.ksnap_arities[s];
                var ci: u8 = 0;
                while (ci < art) : (ci += 1) {
                    if (relation.rel_col_kind(rel, ci) != t.ksnap_kinds[s * 8 + ci]) break :blk true;
                }
            }
        }
        break :blk false;
    };
    txnKindRestore(db);
    if (kinds_changed) {
        writeRelsTxt(db);
        db.meta_dirty = db.txn.?.meta_dirty_at_begin;
    }
    txnDiscard(db);
}

pub export fn dl_txn_begin(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1; // reject nested transactions
    const mem = c.calloc(1, @sizeOf(Txn)) orelse return -1;
    d.txn = @ptrCast(@alignCast(mem));
    d.txn.?.meta_dirty_at_begin = d.meta_dirty;
    return 0;
}

pub export fn dl_txn_rollback(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.txn == null) return -1;
    // U-homog: undo the kinds dl_txn_add_fact recorded at BUFFER time (the
    // buffered rows never applied, so their kinds must not survive either).
    txnAbort(d);
    return 0;
}

pub export fn dl_txn_cas(db: ?*DlDb, entity: [*c]const u8, expected: u32, new_value: u32) c_int {
    const d = db orelse return -1;
    if (entity == null) return -1;
    if (d.txn == null) return -1;
    if (ensureRevRel(d) < 0) return -1;
    if (expected == new_value) return 0; // idempotent no-op

    var op = std.mem.zeroes(TxnOp);
    op.kind = TXN_CAS;
    op.entity_sym = intern_str(d.ir, entity);
    if (op.entity_sym == 0) return -1; // OOM
    // Reject a second CAS on an already-buffered entity.
    {
        var j: usize = 0;
        while (j < d.txn.?.nops) : (j += 1) {
            if (d.txn.?.ops.?[j].kind == TXN_CAS and
                d.txn.?.ops.?[j].entity_sym == op.entity_sym)
                return -1;
        }
    }
    op.expected = expected;
    op.next = new_value;
    return txnBufOp(d, &op);
}

pub export fn dl_txn_add_fact(db: ?*DlDb, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int {
    const d = db orelse return -1;
    if (rel == null or cols == null) return -1;
    if (d.txn == null) return -1;
    if (strEq(rel, "rev")) return -1;
    const idx = findRel(d, rel);
    if (idx < 0) return -1;
    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC) return -1;
    if (arity != relation.rel_arity(d.rels[@intCast(idx)].rel)) return -1;

    // U-homog S2: kind-check + record at BUFFER time so a conflicting op
    // never enters the txn.  Snapshot the pre-add col_kind FIRST: the rows
    // apply only at COMMIT, so the txn must be able to undo this recording
    // on rollback (txnKindSnap takes it once per relation).
    txnKindSnap(d, idx, arity);
    const txn_rel = d.rels[@intCast(idx)].rel;
    var kk_before: [8]u8 = .{0} ** 8;
    if (txn_rel) |r| {
        var ci: u8 = 0;
        while (ci < arity) : (ci += 1) kk_before[ci] = relation.rel_col_kind(r, ci);
    }
    const kn = kindNoteRaw(d, @intCast(idx), cols, arity);
    if (kn < 0) {
        // B1 (review): same all-or-nothing restore as dl_add_fact — the
        // left-to-right walk may have recorded earlier columns before the
        // conflicting one rejected the add.
        if (txn_rel) |r| {
            var ci: u8 = 0;
            while (ci < arity) : (ci += 1)
                relation.rel_col_kind_set(r, ci, kk_before[ci]);
        }
        return -1;
    }

    // U-homog (G1): a newly-established kind on a db with resident rules
    // rechecks the AST; on reject, roll back the kind (the op has not been
    // buffered yet) and refuse the add.  kindNoteRaw records INT only on a
    // column's first row; a sym-establishing txn add is ambiguous to the
    // raw API and stays permissive (RK-G1-flag-precision).
    if (kn == 1) {
        if (kindRecheckAfterLoad(d) != 0) {
            if (txn_rel) |r| {
                var ci: u8 = 0;
                while (ci < arity) : (ci += 1)
                    relation.rel_col_kind_set(r, ci, kk_before[ci]);
            }
            return -1;
        }
    }

    var op = std.mem.zeroes(TxnOp);
    op.kind = TXN_ADD;
    op.rel_id = idx;
    op.arity = arity;
    @memcpy(op.cols[0..arity], cols[0..arity]);
    return txnBufOp(d, &op);
}

pub export fn dl_txn_delete_fact(db: ?*DlDb, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int {
    const d = db orelse return -1;
    if (rel == null or cols == null) return -1;
    if (d.txn == null) return -1;
    if (strEq(rel, "rev")) return -1;
    const idx = findRel(d, rel);
    if (idx < 0) return -1;
    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC) return -1;
    if (arity != relation.rel_arity(d.rels[@intCast(idx)].rel)) return -1;

    var op = std.mem.zeroes(TxnOp);
    op.kind = TXN_DEL;
    op.rel_id = idx;
    op.arity = arity;
    @memcpy(op.cols[0..arity], cols[0..arity]);
    return txnBufOp(d, &op);
}

pub export fn dl_txn_commit(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    const t = d.txn orelse return -1;

    // (a) Empty transaction commits trivially.
    if (t.nops == 0) {
        txnDiscard(d);
        return 0;
    }

    // (b) Validate every buffered CAS against the current revision.
    {
        var i: usize = 0;
        while (i < t.nops) : (i += 1) {
            if (t.ops.?[i].kind == TXN_CAS) {
                var cur: u32 = 0;
                if (revGet(d, t.ops.?[i].entity_sym, &cur) != 0) {
                    txnAbort(d);
                    return -1;
                }
                if (cur != t.ops.?[i].expected) {
                    txnAbort(d);
                    return DL_E_CONFLICT;
                }
            }
        }
    }

    // (c) M7 ordering: interned syms / terms durable BEFORE the txn WAL.
    if (intern_is_dirty(d.ir) != 0) {
        const fwd_path = makePath(d, "symbols", ".dafsa");
        const rev_path = makePath(d, "symbols", ".array");
        if (fwd_path == null or rev_path == null) {
            if (fwd_path) |x| c.free(@ptrCast(x));
            if (rev_path) |x| c.free(@ptrCast(x));
            txnAbort(d);
            return -1;
        }
        if (intern_save(d.ir, fwd_path.?, rev_path.?) != 0) {
            c.free(@ptrCast(fwd_path.?));
            c.free(@ptrCast(rev_path.?));
            txnAbort(d);
            return -1;
        }
        c.free(@ptrCast(fwd_path.?));
        c.free(@ptrCast(rev_path.?));
    }
    if (term_is_dirty(d.terms) != 0) {
        const tpath = makePath(d, "terms", ".bin") orelse {
            txnAbort(d);
            return -1;
        };
        if (term_save(d.terms, tpath) != 0) {
            c.free(@ptrCast(tpath));
            txnAbort(d);
            return -1;
        }
        c.free(@ptrCast(tpath));
    }

    // (d) Append one record per buffered op + a COMMIT marker, then fsync.
    {
        const w = txnwal.txnwal_open_rw(d.dir.?) orelse {
            txnAbort(d);
            return -1;
        };
        var i: usize = 0;
        while (i < t.nops) : (i += 1) {
            const op = &t.ops.?[i];
            if (op.kind == TXN_CAS) {
                const del_cols = [2]u32{ op.entity_sym, op.expected };
                const add_cols = [2]u32{ op.entity_sym, op.next };
                var key: [33]u8 = undefined;
                var key_len: usize = undefined;
                _ = encodeFactKey(&key, &key_len, &del_cols, 2);
                if (txnwal.txnwal_append_record(w, "rev", 3, TXNWAL_OP_DEL, &key, @intCast(key_len)) != 0) {
                    txnwal.txnwal_close(w);
                    txnAbort(d);
                    return -1;
                }
                _ = encodeFactKey(&key, &key_len, &add_cols, 2);
                if (txnwal.txnwal_append_record(w, "rev", 3, TXNWAL_OP_ADD, &key, @intCast(key_len)) != 0) {
                    txnwal.txnwal_close(w);
                    txnAbort(d);
                    return -1;
                }
            } else {
                const rname = d.rels[@intCast(op.rel_id)].name;
                var key: [33]u8 = undefined;
                var key_len: usize = undefined;
                const wop: u8 = if (op.kind == TXN_DEL) TXNWAL_OP_DEL else TXNWAL_OP_ADD;
                _ = encodeFactKey(&key, &key_len, &op.cols, op.arity);
                if (txnwal.txnwal_append_record(w, rname, @intCast(strLen(rname)), wop, &key, @intCast(key_len)) != 0) {
                    txnwal.txnwal_close(w);
                    txnAbort(d);
                    return -1;
                }
            }
        }
        if (d.fault_hook) |hook| {
            if (hook(DL_FPOINT_TXN_BEFORE_MARKER, d.fault_user) != 0) {
                txnwal.txnwal_close(w);
                txnAbort(d);
                return -1;
            }
        }
        if (txnwal.txnwal_append_commit(w) != 0) {
            txnwal.txnwal_close(w);
            txnAbort(d);
            return -1;
        }
        if (txnwal.txnwal_sync(w) != 0) {
            txnwal.txnwal_close(w);
            txnAbort(d);
            return -1;
        }
        txnwal.txnwal_close(w);
    }

    // (e) Apply every buffered op in-memory IN BUFFER ORDER.
    {
        var i: usize = 0;
        while (i < t.nops) : (i += 1) {
            const op = &t.ops.?[i];
            if (op.kind == TXN_CAS) {
                const rel_id = ensureRevRel(d);
                const del_cols = [2]u32{ op.entity_sym, op.expected };
                const add_cols = [2]u32{ op.entity_sym, op.next };
                if (rel_id < 0) {
                    txnAbort(d);
                    return -1;
                }
                _ = deleteFactApply(d, rel_id, &del_cols, 2);
                _ = addFactApply(d, rel_id, &add_cols, 2);
            } else if (op.kind == TXN_DEL) {
                _ = deleteFactApply(d, op.rel_id, &op.cols, op.arity);
            } else {
                _ = addFactApply(d, op.rel_id, &op.cols, op.arity);
            }
        }
    }

    // (f) Success: free the buffer.
    txnDiscard(d);
    return 0;
}

// ─── CAS Slice 2: reopen-time txn WAL replay ────────────────────────────────

const TxnReplayCtx = struct {
    db: *DlDb,
    touched: [MAX_RELS]c_int,
    n_touched: c_int,
};

fn txnReplayCb(rel: [*c]const u8, rel_len: u16, op: u8, key: [*c]const u8, key_len: u32, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *TxnReplayCtx = @ptrCast(@alignCast(user.?));
    const db = ctx.db;
    var name: [256]u8 = undefined;
    var cols: [8]u32 = undefined;

    if (rel_len >= name.len) return -1; // name too long
    @memcpy(name[0..rel_len], rel[0..rel_len]);
    name[rel_len] = 0;

    const idx = findRel(db, @ptrCast(&name));
    if (idx < 0) return 0; // stale record: skip
    if (db.rels[@intCast(idx)].kind == RELK_VARIADIC) return 0; // not a txn target

    if (key_len < 5 or key_len > 33 or (key_len - 1) % 4 != 0) return -1;
    const arity: u8 = @intCast((key_len - 1) / 4);
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        cols[i] = (@as(u32, key[4 * @as(usize, i)]) << 24) |
            (@as(u32, key[4 * @as(usize, i) + 1]) << 16) |
            (@as(u32, key[4 * @as(usize, i) + 2]) << 8) |
            (@as(u32, key[4 * @as(usize, i) + 3]));
    }

    if (op == TXNWAL_OP_ADD) {
        if (relation.rel_add_base(db.rels[@intCast(idx)].rel, &cols) < 0) return -1;
    } else if (op == TXNWAL_OP_DEL) {
        if (relation.rel_delete_base(db.rels[@intCast(idx)].rel, &cols) < 0) return -1;
    } else {
        return -1; // bad op in committed prefix
    }

    if (ctx.touched[@intCast(idx)] == 0) {
        ctx.touched[@intCast(idx)] = 1;
        ctx.n_touched += 1;
    }
    return 0;
}

/// Replay the committed prefix of the txn WAL into the base relations.
fn replayTxnWal(db: *DlDb) void {
    if (db.dir == null) return;
    const w = if (db.read_only != 0)
        txnwal.txnwal_open_ro(db.dir.?)
    else
        txnwal.txnwal_open_rw(db.dir.?) orelse return;

    var good_bytes: c_long = 0;
    var ctx = TxnReplayCtx{
        .db = db,
        .touched = [_]c_int{0} ** MAX_RELS,
        .n_touched = 0,
    };

    if (txnwal.txnwal_replay(w, txnReplayCb, &ctx, @ptrCast(&good_bytes)) != 0) {
        // Leave committed records in txn.wal for a later open to retry.
        txnwal.txnwal_close(w);
        return;
    }

    if (db.read_only != 0) {
        txnwal.txnwal_close(w);
        return;
    }

    // Compact each touched relation so the facts are durable in the base DAFSA.
    // WAL truncation is a state replacement: mutation window.
    mutWindowBegin(db);
    defer mutWindowEnd(db);
    var compact_failed: c_int = 0;
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (ctx.touched[i] == 0) continue;
        if (db.rels[i].kind == RELK_VARIADIC) continue;
        const p0 = if (relation.rel_is_idb(db.rels[i].rel) != 0)
            makePath(db, db.rels[i].name, ".base.dafsa")
        else
            makePath(db, db.rels[i].name, ".dafsa");
        const path = p0 orelse {
            compact_failed = 1;
            continue;
        };
        if (relation.rel_compact(db.rels[i].rel, path) != 0) compact_failed = 1;
        c.free(@ptrCast(path));
    }

    if (compact_failed != 0)
        _ = txnwal.txnwal_truncate(w, if (good_bytes > 16) good_bytes else 16)
    else
        _ = txnwal.txnwal_truncate(w, 16);
    txnwal.txnwal_close(w);

    if (ctx.n_touched > 0 and compact_failed == 0)
        db.fixpoint_dirty = 1; // facts changed
}

// ─── Interner access (M7) ───────────────────────────────────────────────────

pub export fn dl_intern_str(db: ?*DlDb, str: [*c]const u8) u32 {
    const d = db orelse return 0;
    if (d.ir == null) return 0;
    if (d.read_only != 0) return 0; // interning mutates: reject on RO
    return intern_str(d.ir, str);
}

pub export fn dl_intern_str_find(db: ?*DlDb, str: [*c]const u8) u32 {
    const d = db orelse return 0;
    if (d.ir == null) return 0;
    return intern_str_find(d.ir, str);
}

/// int dl_intern_str_version(dl_db *db, uint32_t version, const char *str,
///                           uint32_t *sym_out)
///
/// VERSION-side interning: resolve `str` against snapshot `version`'s OWN
/// symbols.array.  LOOKUP-ONLY — it never creates a symbol, never grows the
/// live interner, never marks it dirty, and therefore also works on a
/// read-only handle.
///
/// THE LIVE vs VERSION DISCIPLINE, stated once here and referenced from the
/// callers: a term used on a LIVE read (dl_search / dl_search_top /
/// dl_intern_str) is interned by the LIVE interner; a term used on a VERSION
/// read (dl_search_version / dl_search_top_version / dl_query_bound_version)
/// must be interned by THIS function.  Sym ids are not stable across a
/// publish: the live interner numbers strings from the db-ROOT symbols.array,
/// which can diverge from a snapshot's (a legacy store written before publish
/// persisted the root array; a restored/copied/edited root array).  On such a
/// store the same id names a different string in each map, so an id interned
/// live and matched against a snapshot's __postings__ selects another string's
/// rows.
///
/// Returns 0 on success (*sym_out = that snapshot's id, or 0 when the snapshot
/// has no such string: the not-found sentinel — the term has no postings
/// as-of that version, so queries over it match nothing, and NO id is minted).
/// Returns -1 on NULL db/str/sym_out, version == 0 (the LIVE path — use
/// dl_intern_str), a nonexistent version, or a snapshot whose symbols.array is
/// missing/unreadable: a damaged snapshot is an ERROR, never an empty
/// vocabulary and never a fallback to the live interner.
pub export fn dl_intern_str_version(db: ?*DlDb, version: u32, str: [*c]const u8, sym_out: ?*u32) c_int {
    const d = db orelse return -1;
    const out = sym_out orelse return -1;
    if (str == null or version == 0) return -1;
    const ir = snapshotInterner(d, version) orelse return -1;
    // NON-MUTATING: intern_str_find's lookup never allocates a symbol.  It uses
    // the forward DAFSA only if this cached interner already has one current —
    // which nothing in the read path materializes any more (a VERSION pattern
    // query scans the array now, see snapshotSymSet), so in practice this is
    // always the rev[] scan.  Deliberately NOT intern_fwd(): materializing the
    // DAFSA for a handful of query terms is the cost that made
    // dl_fuzzy_lookup drop it (MEASURED 10-19 s at 166k symbols), while the
    // scan is O(symbols) strcmp per term.
    out.* = intern_str_find(ir, str);
    return 0;
}

pub export fn dl_intern_str_of(db: ?*DlDb, sym_id: u32) ?[*:0]const u8 {
    const d = db orelse return null;
    if (d.ir == null) return null;
    return intern_str_of(d.ir, sym_id);
}

/// const char *dl_intern_str_of_version(dl_db *db, uint32_t version,
///                                      uint32_t sym_id)
///
/// The id → string half of a VERSION read: resolve `sym_id` in snapshot
/// `version`'s OWN symbols.array (snapshotInterner), never in the live
/// interner.  A version read returns THAT snapshot's ids — a row's column, or
/// a sym_set the pattern path built from the snapshot's names — so resolving
/// them through the live map names another string on any store whose root and
/// snapshot arrays diverge (the same restored/copied/edited store
/// dl_intern_str_version exists for, in the mirror direction): MEASURED before
/// this fix, on a store whose root array had `doc1`/`doc2` swapped against its
/// snapshot's, `dl pattern observation '^doc1$'` printed
/// `doc2 red fox jumps over the lazy dog` — the right row under the wrong name,
/// exit 0.
///
/// Returns NULL for version == 0 (the LIVE path — use dl_intern_str_of), for
/// sym_id == 0, for an id this snapshot never assigned, and for a snapshot
/// whose array is missing/unreadable/damaged; a caller printing ids then
/// renders the raw number, which is what it already does for an id past the
/// live interner's bound.  LOOKUP-ONLY, like dl_intern_str_version.
pub export fn dl_intern_str_of_version(db: ?*DlDb, version: u32, sym_id: u32) ?[*:0]const u8 {
    const d = db orelse return null;
    if (version == 0 or sym_id == 0) return null;
    const ir = snapshotInterner(d, version) orelse return null;
    return intern_str_of(ir, sym_id);
}

pub export fn dl_intern_fwd_mutable(db: ?*DlDb) ?*anyopaque {
    const d = db orelse return null;
    if (d.ir == null) return null;
    return @constCast(intern_fwd_mutable(d.ir));
}

// ─── List term-store access (v2 lists) ─────────────────────────────────────

pub export fn dl_term_cons(db: ?*DlDb, head: u32, tail: u32) u32 {
    const d = db orelse return 0;
    if (d.terms == null) return 0;
    if (d.read_only != 0) return 0;
    return term_cons(d.terms, head, tail);
}

pub export fn dl_term_append(db: ?*DlDb, a: u32, b: u32) u32 {
    const d = db orelse return 0;
    if (d.terms == null) return 0;
    if (d.read_only != 0) return 0;
    return term_append(d.terms, a, b);
}

pub export fn dl_term_is_list(db: ?*const DlDb, v: u32) c_int {
    const d = db orelse return 0;
    if (d.terms == null) return 0;
    return term_is_list(d.terms, v);
}

pub export fn dl_term_car(db: ?*const DlDb, h: u32) u32 {
    const d = db orelse return 0;
    if (d.terms == null) return 0;
    return term_car(d.terms, h);
}

pub export fn dl_term_cdr(db: ?*const DlDb, h: u32) u32 {
    const d = db orelse return 0;
    if (d.terms == null) return 0;
    return term_cdr(d.terms, h);
}

// ─── Query primitives ──────────────────────────────────────────────────────

pub export fn dl_lookup(db: ?*const DlDb, rel_name: [*c]const u8, cols: [*c]const u32, arity: u8) c_int {
    const d = db orelse return 0;
    if (rel_name == null or cols == null) return 0;

    const idx = findRel(d, rel_name);
    if (idx < 0) return 0;

    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC) {
        if (arity == 0 or arity > 8) return 0;
        return vrelation.vrel_exact(d.rels[@intCast(idx)].vrel, cols, arity);
    }

    if (arity != relation.rel_arity(d.rels[@intCast(idx)].rel)) return 0;
    return relation.rel_exact(d.rels[@intCast(idx)].rel, cols);
}

pub export fn dl_prefix(db: ?*const DlDb, rel_name: [*c]const u8, leading: ?[*]const u32, k: u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (rel_name == null or cb == null) return -1;

    const idx = findRel(d, rel_name);
    if (idx < 0) return -1;

    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC)
        return vrelation.vrel_prefix(d.rels[@intCast(idx)].vrel, leading, k, cb, user);

    return relation.rel_prefix(d.rels[@intCast(idx)].rel, leading, k, cb, user);
}

// ─── Order statistics (Tier-2) ─────────────────────────────────────────────

fn ordstatsRelRo(db: ?*const DlDb, rel_name: [*c]const u8, arity: u8) ?*relation.Relation {
    const d = db orelse return null;
    if (rel_name == null) return null;
    const idx = findRel(d, rel_name);
    if (idx < 0) return null;
    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC) return null;
    if (d.rels[@intCast(idx)].arity != arity) return null;
    return d.rels[@intCast(idx)].rel;
}

/// Resolve the mmap snapshot view for a fixed-arity relation.
fn snapshotViewOpenRel(db: ?*const DlDb, rel_name: [*c]const u8, arity_out: *u8, variadic_out: *c_int) ?*anyopaque {
    var sdir: [8192:0]u8 = undefined;
    const d = db orelse return null;
    if (d.dir == null or rel_name == null) return null;
    _ = snprintf(&sdir, 8192, "%s/snapshots/%u", d.dir.?, d.snap_version);
    if (manifest_find_rel_ex(&sdir, rel_name, arity_out, variadic_out) == 0)
        return null;
    return view_open_cached(@constCast(&d.vcache), rel_name, d.snap_version, &sdir);
}

pub export fn dl_rank(db: ?*const DlDb, rel_name: [*c]const u8, cols: [*c]const u32, arity: u8) u64 {
    if (cols == null) return std.math.maxInt(u64);

    if (db != null and db.?.snap_version > 0) {
        var v_arity: u8 = 0;
        var variadic: c_int = 0;
        const v = snapshotViewOpenRel(db.?, rel_name, &v_arity, &variadic);
        if (v == null or variadic != 0 or v_arity != arity) return std.math.maxInt(u64);
        return view_rank(v, arity, cols);
    }

    const rel = ordstatsRelRo(db, rel_name, arity) orelse return std.math.maxInt(u64);
    return relation.rel_rank(rel, cols);
}

pub export fn dl_select(db: ?*const DlDb, rel_name: [*c]const u8, k: u64, cols_out: [*c]u32, arity: u8) c_int {
    if (cols_out == null) return -1;

    if (db != null and db.?.snap_version > 0) {
        var v_arity: u8 = 0;
        var variadic: c_int = 0;
        const v = snapshotViewOpenRel(db.?, rel_name, &v_arity, &variadic);
        if (v == null or variadic != 0 or v_arity != arity) return -1;
        return view_select(v, arity, k, cols_out);
    }

    const rel = ordstatsRelRo(db, rel_name, arity) orelse return -1;
    return relation.rel_select(rel, k, cols_out);
}

pub export fn dl_range_count(db: ?*const DlDb, rel_name: [*c]const u8, lo: [*c]const u32, hi: [*c]const u32, arity: u8) u64 {
    if (lo == null or hi == null) return std.math.maxInt(u64);

    if (db != null and db.?.snap_version > 0) {
        var v_arity: u8 = 0;
        var variadic: c_int = 0;
        const v = snapshotViewOpenRel(db.?, rel_name, &v_arity, &variadic);
        if (v == null or variadic != 0 or v_arity != arity) return std.math.maxInt(u64);
        return view_range_count(v, arity, lo, hi);
    }

    const rel = ordstatsRelRo(db, rel_name, arity) orelse return std.math.maxInt(u64);
    return relation.rel_range_count(rel, lo, hi);
}

// ─── M6 follow-up #2: permuted order statistics ────────────────────────────

fn permRelRo(db: ?*DlDb, rel_name: [*c]const u8, perm_id: c_int, arity: u8) ?*relation.Relation {
    const d = db orelse return null;
    if (rel_name == null) return null;
    if (perm_id < 0 or perm_id >= d.n_perms) return null;
    const idx = findRel(d, rel_name);
    if (idx < 0) return null;
    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC) return null;
    if (d.rels[@intCast(idx)].arity != arity) return null;
    if (d.perms[@intCast(perm_id)].rel_id != idx) return null;
    if (d.perms[@intCast(perm_id)].arity != arity) return null;

    var pidx = d.perms[@intCast(perm_id)].pidx_rel;
    if (pidx == null or d.perms[@intCast(perm_id)].dirty != 0) {
        if (permindex_build(d, idx, perm_id) != 0)
            return null;
        pidx = d.perms[@intCast(perm_id)].pidx_rel;
        if (pidx == null) return null;
    }
    return pidx;
}

pub export fn dl_rank_perm(db: ?*DlDb, rel_name: [*c]const u8, perm_id: c_int, cols: [*c]const u32, arity: u8) u64 {
    if (cols == null) return std.math.maxInt(u64);
    const pidx = permRelRo(db, rel_name, perm_id, arity) orelse return std.math.maxInt(u64);
    const perm = &db.?.perms[@intCast(perm_id)].perm;
    var pcols: [8]u32 = undefined;
    var j: u8 = 0;
    while (j < arity) : (j += 1)
        pcols[j] = cols[perm[j]]; // FORWARD map
    return relation.rel_rank(pidx, &pcols);
}

pub export fn dl_select_perm(db: ?*DlDb, rel_name: [*c]const u8, perm_id: c_int, k: u64, cols_out: [*c]u32, arity: u8) c_int {
    if (cols_out == null) return -1;
    const pidx = permRelRo(db, rel_name, perm_id, arity) orelse return -1;
    var pcols: [8]u32 = undefined;
    if (relation.rel_select(pidx, k, &pcols) != 0) return -1;
    const perm = &db.?.perms[@intCast(perm_id)].perm;
    var j: u8 = 0;
    while (j < arity) : (j += 1)
        cols_out[perm[j]] = pcols[j]; // INVERSE map
    return 0;
}

pub export fn dl_range_count_perm(db: ?*DlDb, rel_name: [*c]const u8, perm_id: c_int, lo: [*c]const u32, hi: [*c]const u32, arity: u8) u64 {
    if (lo == null or hi == null) return std.math.maxInt(u64);
    const pidx = permRelRo(db, rel_name, perm_id, arity) orelse return std.math.maxInt(u64);
    const perm = &db.?.perms[@intCast(perm_id)].perm;
    var plo: [8]u32 = undefined;
    var phi: [8]u32 = undefined;
    var j: u8 = 0;
    while (j < arity) : (j += 1) {
        plo[j] = lo[perm[j]];
        phi[j] = hi[perm[j]];
    }
    return relation.rel_range_count(pidx, &plo, &phi);
}

pub export fn dl_count(db: ?*const DlDb, rel_name: [*c]const u8) u64 {
    const d = db orelse return std.math.maxInt(u64);
    if (rel_name == null) return std.math.maxInt(u64);

    if (d.snap_version > 0) {
        var v_arity: u8 = 0;
        var variadic: c_int = 0;
        const v = snapshotViewOpenRel(d, rel_name, &v_arity, &variadic);
        if (v == null or variadic != 0) return std.math.maxInt(u64);
        return view_count(v);
    }

    const idx = findRel(d, rel_name);
    if (idx < 0) return std.math.maxInt(u64);
    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC) return std.math.maxInt(u64);
    return relation.rel_count_subtree(d.rels[@intCast(idx)].rel);
}

pub export fn dl_rank_bound(db: ?*const DlDb, rel_name: [*c]const u8, leading: ?[*]const u32, k: u8, cols: [*c]const u32, arity: u8) u64 {
    if (cols == null) return std.math.maxInt(u64);
    if (k > 0 and leading == null) return std.math.maxInt(u64);
    const rel = ordstatsRelRo(db, rel_name, arity) orelse return std.math.maxInt(u64);
    return relation.rel_rank_bound(rel, leading, k, cols);
}

pub export fn dl_select_bound(db: ?*const DlDb, rel_name: [*c]const u8, leading: ?[*]const u32, k: u8, idx: u64, cols_out: [*c]u32, arity: u8) c_int {
    if (cols_out == null) return -1;
    if (k > 0 and leading == null) return -1;
    const rel = ordstatsRelRo(db, rel_name, arity) orelse return -1;
    return relation.rel_select_bound(rel, leading, k, idx, cols_out);
}

pub export fn dl_range_count_bound(db: ?*const DlDb, rel_name: [*c]const u8, leading: ?[*]const u32, k: u8, lo: [*c]const u32, hi: [*c]const u32, arity: u8) u64 {
    if (lo == null or hi == null) return std.math.maxInt(u64);
    if (k > 0 and leading == null) return std.math.maxInt(u64);
    const rel = ordstatsRelRo(db, rel_name, arity) orelse return std.math.maxInt(u64);
    return relation.rel_range_count_bound(rel, leading, k, lo, hi);
}

// ─── Rule loading & compilation (M1) ───────────────────────────────────────

/// AST deep-copy — retain the resident rules (see dl_load_rules).
fn astTokFree(t: ?*parser.token) void {
    const tt = t orelse return;
    if (tt.children) |ch| {
        var i: c_int = 0;
        while (i < tt.nchildren) : (i += 1)
            astTokFree(ch[@intCast(i)]);
        c.free(@ptrCast(ch));
    }
    astTokFree(tt.tail);
    if (tt.text) |txt| c.free(@ptrCast(txt));
    c.free(@ptrCast(tt));
}

fn astTokClone(t: ?*const parser.token) ?*parser.token {
    const tt = t orelse return null;
    const mem = c.calloc(1, @sizeOf(parser.token)) orelse return null;
    const n: *parser.token = @ptrCast(@alignCast(mem));
    n.* = std.mem.zeroes(parser.token);
    n.kind = tt.kind;
    n.ival = tt.ival;
    if (tt.text) |txt| {
        n.text = strdup(txt);
        if (n.text == null) {
            c.free(mem);
            return null;
        }
    }
    if (tt.nchildren > 0) {
        const ch = c.calloc(@intCast(tt.nchildren), @sizeOf(?*parser.token)) orelse {
            if (n.text) |x| c.free(@ptrCast(x));
            c.free(mem);
            return null;
        };
        n.children = @ptrCast(@alignCast(ch));
        n.nchildren = tt.nchildren;
        var i: c_int = 0;
        while (i < tt.nchildren) : (i += 1) {
            n.children.?[@intCast(i)] = astTokClone(tt.children.?[@intCast(i)]);
            if (n.children.?[@intCast(i)] == null) {
                astTokFree(n);
                return null;
            }
        }
    }
    if (tt.tail) |tl| {
        n.tail = astTokClone(tl);
        if (n.tail == null) {
            astTokFree(n);
            return null;
        }
    }
    return n;
}

fn astAtomClone(a: ?*const parser.atom) ?*parser.atom {
    const aa = a orelse return null;
    const mem = c.calloc(1, @sizeOf(parser.atom)) orelse return null;
    const n: *parser.atom = @ptrCast(@alignCast(mem));
    n.* = std.mem.zeroes(parser.atom);
    n.pred = if (aa.pred) |p| strdup(p) else null;
    n.negated = aa.negated;
    n.aggregate = aa.aggregate;
    n.pattern_col = aa.pattern_col;
    if (aa.pattern) |pat| {
        n.pattern = strdup(pat);
        if (n.pattern == null) {
            astAtomFreePartial(n);
            return null;
        }
    }
    if (aa.agg_op) |ao| {
        n.agg_op = astTokClone(ao);
        if (n.agg_op == null) {
            astAtomFreePartial(n);
            return null;
        }
    }
    if (aa.arith) |ar| {
        n.arith = expr_clone(ar);
        if (n.arith == null) {
            astAtomFreePartial(n);
            return null;
        }
    }
    if (aa.nargs > 0) {
        const args = c.calloc(@intCast(aa.nargs), @sizeOf(?*parser.token)) orelse {
            astAtomFreePartial(n);
            return null;
        };
        n.args = @ptrCast(@alignCast(args));
        n.nargs = aa.nargs;
        var i: c_int = 0;
        while (i < aa.nargs) : (i += 1) {
            n.args.?[@intCast(i)] = astTokClone(aa.args.?[@intCast(i)]);
            if (n.args.?[@intCast(i)] == null) {
                astAtomFreePartial(n);
                return null;
            }
        }
    }
    return n;
}

/// Mirror parser.c atom_free (static) for partial cleanup.
fn astAtomFreePartial(n: *parser.atom) void {
    if (n.pred) |p| c.free(@ptrCast(p));
    if (n.pattern) |p| c.free(@ptrCast(p));
    if (n.args) |args| {
        var j: c_int = 0;
        while (j < n.nargs) : (j += 1)
            astTokFree(args[@intCast(j)]);
        c.free(@ptrCast(args));
    }
    astTokFree(n.agg_op);
    expr_free(n.arith);
    c.free(@ptrCast(n));
}

fn astRuleClone(r: ?*const parser.rule) ?*parser.rule {
    const rr = r orelse return null;
    const mem = c.calloc(1, @sizeOf(parser.rule)) orelse return null;
    const n: *parser.rule = @ptrCast(@alignCast(mem));
    n.* = std.mem.zeroes(parser.rule);
    n.has_negation = rr.has_negation;
    n.has_aggregate = rr.has_aggregate;
    n.head = astAtomClone(rr.head);
    if (n.head == null) {
        c.free(mem);
        return null;
    }
    if (rr.nbody > 0) {
        const body = c.calloc(@intCast(rr.nbody), @sizeOf(?*parser.atom)) orelse {
            rule_free(n);
            return null;
        };
        n.body = @ptrCast(@alignCast(body));
        n.nbody = rr.nbody;
        var i: c_int = 0;
        while (i < rr.nbody) : (i += 1) {
            n.body.?[@intCast(i)] = astAtomClone(rr.body.?[@intCast(i)]);
            if (n.body.?[@intCast(i)] == null) {
                rule_free(n);
                return null;
            }
        }
    }
    return n;
}

pub export fn dl_attach_schema(db: ?*DlDb, schm: ?*const schema_mod.dl_schema) c_int {
    const d = db orelse return -1;
    d.schema = schm;
    return 0;
}

/// Free a parsed-rules array + parser, mirroring dl.c's error cleanup.
fn freeRulesAndParser(rules: ?[*]?*parser.rule, n_rules: c_int, p: ?*anyopaque) void {
    if (rules) |rs| {
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) rule_free(rs[@intCast(i)]);
        c.free(@ptrCast(rs));
    }
    parse_free(p);
}

/// Free a batch of freshly compiled rules (compile_rules' out-params) — every
/// dl_load_rules path that gives up after a successful compile needs exactly
/// this, so it is not repeated at each failure site.
fn freeNewCompiled(rules: ?[*]?*compiler.compiled_rule, n: c_int) void {
    if (rules) |rs| {
        var i: c_int = 0;
        while (i < n) : (i += 1) compiler.compiled_rule_free(rs[@intCast(i)]);
        c.free(@ptrCast(rs));
    }
}

/// 1 if the parsed rules reference the reserved "rev" relation.
fn rulesReferenceRev(rules: ?[*]?*parser.rule, n_rules: c_int) bool {
    var i: c_int = 0;
    while (i < n_rules) : (i += 1) {
        const r = rules.?[@intCast(i)] orelse continue;
        if (r.head != null and r.head.?.pred != null and strEq(r.head.?.pred, "rev")) return true;
        var j: c_int = 0;
        while (j < r.nbody) : (j += 1) {
            const a = r.body.?[@intCast(j)] orelse continue;
            if (a.pred != null and strEq(a.pred, "rev")) return true;
        }
    }
    return false;
}

pub export fn dl_load_rules(db: ?*DlDb, dl_source: [*c]const u8) c_int {
    const d = db orelse return -1;
    if (dl_source == null) return -1;
    if (d.read_only != 0) return -1;

    const p = parse_create(dl_source) orelse return -1;

    var n_rules: c_int = 0;
    const rules = parse_rules(p, &n_rules);
    if (rules == null) {
        parse_free(p);
        return -1;
    }

    if (rulesReferenceRev(rules, n_rules)) {
        freeRulesAndParser(rules, n_rules, p);
        return -1;
    }

    // schema typecheck hook (S2/S3).
    if (d.schema != null) {
        var errbuf: [256]u8 = undefined;
        if (dl_typecheck_rules(d.schema, @ptrCast(rules), n_rules, null, &errbuf, errbuf.len) != 0) {
            freeRulesAndParser(rules, n_rules, p);
            return -1;
        }
    }

    var new_crules: ?[*]?*compiler.compiled_rule = null;
    var n_compiled: c_int = 0;

    // U-homog (review SH2): compile_rules checks only the NEW rules, so a
    // constant that conflicts through a PREVIOUSLY loaded rule's head kind
    // would slip in when the same program is split across load_rules calls.
    // Re-check over the CONCATENATED resident+new rule set (borrowed
    // pointers, nothing cloned) BEFORE anything is appended — and BEFORE
    // compile_rules, so a cross-rule conflict reports the MERGED rule
    // numbering (naming both rules) rather than the new-only numbering the
    // subset check would print first.  A rejected load leaves the db exactly
    // as it was — the same diagnostic the equivalent single combined load
    // produces.
    {
        const nres: usize = @intCast(d.n_ast_rules);
        const total: usize = nres + @as(usize, @intCast(n_rules));
        if (total > 0) {
            const merged_mem = c.malloc(total * @sizeOf(?*parser.rule)) orelse {
                freeRulesAndParser(rules, n_rules, p);
                return -1;
            };
            const merged: [*]?*parser.rule = @ptrCast(@alignCast(merged_mem));
            if (nres > 0) @memcpy(merged[0..nres], d.ast_rules.?[0..nres]);
            @memcpy(merged[nres..total], rules.?[0..@intCast(n_rules)]);
            const bad = compiler.checkAllRuleConstKinds(d, merged, @intCast(total));
            c.free(merged_mem);
            if (bad != 0) {
                freeRulesAndParser(rules, n_rules, p);
                return -1;
            }
        }
    }

    if (compile_rules(d, rules, n_rules, &new_crules, &n_compiled) != 0) {
        freeRulesAndParser(rules, n_rules, p);
        return -1;
    }

    // ── Union stratification: strata are a property of the PROGRAMME ───────
    //
    // compile_rules stratifies only THIS batch, so a split programme's strata
    // were a function of the batching.  MEASURED pre-fix over m =
    // {(1,2),(2,3),(4,4)} with
    //     batch1  r(X) :- m(X,Y), !q(Y).     batch2  q(X) :- m(X,Y), !s(Y).
    // both heads landed in stratum 1, the non-recursive path's stable stratum
    // sort (vm.zig ruleStratumOrder) fell back to BATCH order, and r was
    // evaluated while q was still empty: r = {1,2,4} for r-then-q but r = {2}
    // for q-then-r — a silent, order-dependent wrong answer (the single-batch
    // programme containing the same two rules answers {2}).
    //
    // Fix: stratify the UNION (resident rule AST + this batch) and publish the
    // result onto every retained compiled rule (below, after the append).
    // Placed AFTER compile_rules — which declares this batch's head relations,
    // and compute_strata skips a rule whose head is not declared yet — and
    // BEFORE either append, so an unstratifiable UNION is refused loudly with
    // nothing loaded: batch1 p(X) :- m(X,Y), !q(Y). plus batch2
    // q(X) :- m(X,Y), !p(Y). is exactly the negation cycle the single-batch
    // compiler rejects.  (This block runs AFTER compile_rules, so a refused
    // load has already declared this batch's head relations AND any perm
    // indices compile_rules declared for them — perms are inert if unused but
    // are a finite resource, so a caller repeating refused loads can reach
    // "too many permutation indices" sooner on a later legitimate load.)
    var uni_strata: ?[*]c_int = null;
    var uni_recursive: ?[*]c_int = null;
    var uni_nrels: c_int = 0;
    defer if (uni_strata) |sp| c.free(@ptrCast(sp));
    defer if (uni_recursive) |rp| c.free(@ptrCast(rp));
    {
        const nres: usize = @intCast(d.n_ast_rules);
        const total: usize = nres + @as(usize, @intCast(n_rules));
        uni_nrels = @intCast(d.nrels);
        if (total > 0 and uni_nrels > 0) {
            const merged_mem = c.malloc(total * @sizeOf(?*parser.rule)) orelse {
                freeNewCompiled(new_crules, n_compiled);
                freeRulesAndParser(rules, n_rules, p);
                return -1;
            };
            const merged: [*]?*parser.rule = @ptrCast(@alignCast(merged_mem));
            if (nres > 0) @memcpy(merged[0..nres], d.ast_rules.?[0..nres]);
            @memcpy(merged[nres..total], rules.?[0..@intCast(n_rules)]);
            uni_strata = @ptrCast(@alignCast(c.calloc(@intCast(uni_nrels), @sizeOf(c_int))));
            uni_recursive = @ptrCast(@alignCast(c.calloc(@intCast(uni_nrels), @sizeOf(c_int))));
            var rc: c_int = if (uni_strata != null and uni_recursive != null)
                compiler.compute_strata(d, merged, @intCast(total), uni_strata.?, uni_recursive.?, uni_nrels, true)
            else
                -1;
            // The union can also make a RESIDENT rule recursive; the three
            // shapes the compiler refuses in that case must be refused here too
            // (see compiler.checkUnionRecursion).
            if (rc == 0 and compiler.checkUnionRecursion(d, merged, @intCast(total), uni_recursive.?) != 0) rc = 1;
            c.free(merged_mem);
            if (rc != 0) {
                freeNewCompiled(new_crules, n_compiled);
                freeRulesAndParser(rules, n_rules, p);
                return -1;
            }
        }
    }

    // ── Union recompile: the PLAN is a property of the PROGRAMME, too ──────
    //
    // What a non-leading, partially-bound body atom becomes — OP_LOOKUP_PERM or
    // OP_HASH_JOIN — is decided by compiler.zig emit_nonleading_join from the
    // recursive set AT COMPILE TIME, and (for a recursive relation) it DECLARES
    // a perm index, which can fail with "too many permutation indices".
    // Publishing is_recursive onto a retained rule cannot reach either
    // decision, so a rule from an earlier batch kept the plan its own batch
    // chose.  MEASURED PRE-FIX on 75846fb (the tests below carry the full
    // readings): r(X) :- edge(X,Y), reach(W,Y). with the reach rules in a LATER
    // batch installed SCAN,HASH_JOIN,PROJECT,HALT and 0 perms where the single
    // batch installs SCAN,LOOKUP_PERM,PROJECT,HALT and 1 perm — and 70
    // non-leading joins over one arity-8 relation were REFUSED as one batch and
    // accepted as two.
    //
    // compile_rules stratifies and compiles ONE batch, so the fix is to compile
    // the UNION — every retained rule plus this batch — against the union's
    // vectors whenever the union's recursion vector DIFFERS from the one the
    // resident rules were compiled under.  That vector needs no storage: the
    // republish below writes it onto every retained rule, so "relation k is
    // recursive" is readable as "some retained rule with head k says so" — and
    // the batch compile has already used the same vector, because
    // compute_strata SEEDS out_recursive from the resident rules.  When the two
    // agree nothing about recursion changed, emit_nonleading_join would take
    // the cost path it already took, and only the card-dependent decisions
    // could move — which a single-batch compile freezes at ITS own load too, so
    // re-planning them would be churn, not parity.  The first load (union IS
    // this batch, n_ast_rules == 0) needs nothing either.
    //
    // INVARIANT this trigger maintains: after every accepted load, every
    // retained rule was planned under the union vector published at that load.
    // If the vectors agree, the residents already were (that is what the
    // published flag means) and this batch's own compile used V_prev too — its
    // vector is SCC(batch) | V_prev, and V_new == V_prev forces
    // SCC(union) ⊆ V_prev.  If they differ, the recompile below re-plans
    // everything under V_new.
    //
    // Direction note: compute_strata already SEEDS out_recursive from resident
    // compiled rules ("carry recursion across dl_load_rules batches"), so the
    // forward batching (recursion loaded first) was already planned correctly
    // pre-fix — MEASURED, see the plan test.  The carry cannot flow backwards,
    // which is the case this block closes.
    //
    // The recompile reads the retained AST clones, which are deep copies
    // (astRuleClone: strings, tokens, exprs) and outlive the parser of the call
    // that loaded them, so a resident rule recompiles byte-for-byte from its
    // own AST.  Only the source OFFSET is lost (0 in the clone), which is the
    // same convention checkUnionRecursion uses for its diagnostics.
    var union_crules: ?[*]?*compiler.compiled_rule = null;
    var union_n: c_int = 0;
    defer if (union_crules) |ur| freeNewCompiled(ur, union_n);
    if (uni_recursive != null and d.n_ast_rules > 0) {
        // The vector the resident rules were compiled under, read off their
        // published flags (every retained rule's is_recursive was rewritten by
        // the republish at the end of the load that installed it).
        var prev_rec: [MAX_RELS]c_int = [_]c_int{0} ** MAX_RELS;
        var q: c_int = 0;
        while (q < d.n_crules) : (q += 1) {
            const cr = d.crules.?[@intCast(q)] orelse continue;
            const hid: usize = @intCast(cr.head_rel_id);
            if (hid < MAX_RELS and cr.is_recursive != 0) prev_rec[hid] = 1;
        }
        var changed = false;
        var k: c_int = 0;
        while (k < uni_nrels and !changed) : (k += 1) {
            if (prev_rec[@intCast(k)] != uni_recursive.?[@intCast(k)]) changed = true;
        }
        if (changed) {
            const nres: usize = @intCast(d.n_ast_rules);
            const total: usize = nres + @as(usize, @intCast(n_rules));
            const mem = c.malloc(total * @sizeOf(?*parser.rule)) orelse {
                freeNewCompiled(new_crules, n_compiled);
                freeRulesAndParser(rules, n_rules, p);
                return -1;
            };
            const merged: [*]?*parser.rule = @ptrCast(@alignCast(mem));
            @memcpy(merged[0..nres], d.ast_rules.?[0..nres]);
            @memcpy(merged[nres..total], rules.?[0..@intCast(n_rules)]);
            const perms_before: c_int = d.n_perms;
            const rc = compiler.compileUnionRules(d, merged, @intCast(total), uni_strata.?, uni_recursive.?, uni_nrels, &union_crules, &union_n);
            c.free(mem);
            if (rc != 0) {
                // Refused (e.g. "too many permutation indices"): the same
                // programme compiled as ONE batch is refused, so this load
                // loads nothing and the resident programme is untouched.  The
                // perm indices the refused compile declared are released again
                // — nothing references them (their rules are already freed) and
                // the table is a finite resource a caller may retry against.
                d.n_perms = perms_before;
                freeNewCompiled(new_crules, n_compiled);
                freeRulesAndParser(rules, n_rules, p);
                return -1;
            }
        }
    }

    // Retain a DEEP copy of the rule AST: dl_add_fact's constant-kind
    // recheck (compiler.checkAllRuleConstKinds) consults the resident rules.
    {
        const cloned = c.calloc(@intCast(n_rules), @sizeOf(?*parser.rule)) orelse {
            var i: c_int = 0;
            while (i < n_compiled) : (i += 1) compiler.compiled_rule_free(new_crules.?[@intCast(i)]);
            c.free(@ptrCast(new_crules));
            freeRulesAndParser(rules, n_rules, p);
            return -1;
        };
        const cloned_rules: [*]?*parser.rule = @ptrCast(@alignCast(cloned));
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) {
            cloned_rules[@intCast(i)] = astRuleClone(rules.?[@intCast(i)]);
            if (cloned_rules[@intCast(i)] == null) break;
        }
        if (i < n_rules) {
            var j: c_int = 0;
            while (j < i) : (j += 1) rule_free(cloned_rules[@intCast(j)]);
            c.free(cloned);
            while (j < n_compiled) : (j += 1) compiler.compiled_rule_free(new_crules.?[@intCast(j)]);
            c.free(@ptrCast(new_crules));
            freeRulesAndParser(rules, n_rules, p);
            return -1;
        }
        {
            const na = c.realloc(@ptrCast(d.ast_rules), @as(usize, @intCast(d.n_ast_rules + n_rules)) * @sizeOf(?*parser.rule)) orelse {
                var j: c_int = 0;
                while (j < n_rules) : (j += 1) rule_free(cloned_rules[@intCast(j)]);
                c.free(cloned);
                while (j < n_compiled) : (j += 1) compiler.compiled_rule_free(new_crules.?[@intCast(j)]);
                c.free(@ptrCast(new_crules));
                freeRulesAndParser(rules, n_rules, p);
                return -1;
            };
            const na_rules: [*]?*parser.rule = @ptrCast(@alignCast(na));
            @memcpy(na_rules[@intCast(d.n_ast_rules)..@intCast(d.n_ast_rules + n_rules)], cloned_rules[0..@intCast(n_rules)]);
            c.free(cloned);
            d.ast_rules = na_rules;
            d.n_ast_rules += n_rules;
        }
    }

    // Install the compiled programme.  When the union compile ran it IS the
    // whole programme (resident rules included), so it REPLACES the list; the
    // batch compile's output is dropped with the batch strata it was compiled
    // under.  Otherwise this batch appends, exactly as before.
    if (union_crules) |ur| {
        freeNewCompiled(new_crules, n_compiled);
        var i: c_int = 0;
        while (i < d.n_crules) : (i += 1) {
            if (d.crules.?[@intCast(i)]) |cr| compiler.compiled_rule_free(cr);
        }
        c.free(@ptrCast(d.crules));
        d.crules = ur;
        d.n_crules = union_n;
        union_crules = null;
    } else {
        const new_total = d.n_crules + n_compiled;
        const merged = c.realloc(@ptrCast(d.crules), @as(usize, @intCast(new_total)) * @sizeOf(?*compiler.compiled_rule)) orelse {
            var i: c_int = 0;
            while (i < n_compiled) : (i += 1) compiler.compiled_rule_free(new_crules.?[@intCast(i)]);
            c.free(@ptrCast(new_crules));
            freeRulesAndParser(rules, n_rules, p);
            return -1;
        };
        const merged_rules: [*]?*compiler.compiled_rule = @ptrCast(@alignCast(merged));
        @memcpy(merged_rules[@intCast(d.n_crules)..@intCast(new_total)], new_crules.?[0..@intCast(n_compiled)]);
        c.free(@ptrCast(new_crules));
        d.crules = merged_rules;
        d.n_crules = new_total;
    }

    // Publish the UNION stratification onto EVERY retained rule, resident
    // batches included: two rules with the same head must agree on its
    // stratum, and it must be the stratum the single-batch programme would
    // have assigned — that is what makes the non-recursive path's stratum sort
    // (vm.zig ruleStratumOrder) place a negating rule strictly after the
    // relations it negates, no matter how the programme was batched.
    //
    // is_recursive is republished for the same reason, and it matters as much:
    // a base rule loaded first and a recursive rule loaded second left the
    // base rule flagged non-recursive, and the recursive driver's delta
    // seeding then UNDER-DERIVED (MEASURED pre-fix: closure of the 4-edge chain
    // 1->2->3->4->5 came to 8 of the 10 pairs when split, 10 as one batch).
    // Both flags are now exactly what compiling the same rules as ONE batch
    // yields.  The flag also steered TWO compile-time choices that are NOT
    // re-run here: the perm-vs-hash join choice for a non-leading body atom
    // (compiler.zig emit_nonleading_join) and the three rejects
    // checkUnionRecursion now applies to the union.
    //
    // The join choice is a COST choice only because the recursive driver feeds
    // BOTH ops: an atom emitted as OP_HASH_JOIN (the relation was not recursive
    // when this batch compiled) reads the driver's idb/delta tupleset exactly
    // as an OP_LOOKUP_PERM atom does.  That holds because vm.driverFeeds() is
    // the single list of ops the driver substitutes into (SCAN, LOOKUP,
    // LOOKUP_PERM, HASH_JOIN) — the three op filters in evalStratumRecursive
    // used to be copies that omitted OP_HASH_JOIN, and a promoted rule then
    // read an EMPTY relation for the whole fixpoint and never fired (MEASURED:
    // h = {} instead of {(7),(8)} for the three-rule programme in the
    // cross-batch property test).  Anything the driver cannot feed (OP_WALK,
    // OP_MAT_JOIN) must not be reachable here; do not add an op to that list
    // without adding its override lookup.
    //
    // Making a rule count as recursive can only make the incremental-maintenance
    // probes MORE conservative (a false "ineligible" falls back to a full
    // re-evaluate, never to a wrong answer).  RESIDUAL, still open: the flag
    // does not re-derive the PLAN, so a split programme can differ from its
    // single-batch twin in cost, and cannot be REFUSED with "too many
    // permutation indices" the way the single-batch compile would be — answers
    // agree (that is the property the tests gate on), the resource-exhaustion
    // refusal does not.  See src/dl.h, ACCUMULATION CONTRACT.
    if (uni_strata) |strata| {
        var i: c_int = 0;
        while (i < d.n_crules) : (i += 1) {
            const cr = d.crules.?[@intCast(i)] orelse continue;
            const hid: c_int = @intCast(cr.head_rel_id);
            if (hid >= uni_nrels) continue;
            cr.stratum = strata[@intCast(hid)];
            cr.is_recursive = uni_recursive.?[@intCast(hid)];
        }
    }

    // IVM Slice 1: the rule set changed.
    d.full_reeval_pending = 1;
    d.fixpoint_dirty = 1;

    // U-homog (review SH3): record the rules' derived head-column kinds so
    // they survive this process — rels.txt carries them to a later one
    // (rules themselves are never persisted), and the in-db record makes
    // the settled kind visible to eval clones without re-running the
    // propagation.  Recorded DATA kinds always win (conflicts refused).
    compiler.recordHeadKindsPub(@ptrCast(d));

    freeRulesAndParser(rules, n_rules, p);
    return 0;
}

pub export fn dl_compile(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.read_only != 0) return -1;
    if (d.n_crules == 0) return 0;

    {
        const m = vm_execute(d, d.crules, d.n_crules);
        if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
    }

    d.fixpoint_dirty = 0;
    d.full_reeval_pending = 0;
    vm_clear_deltas(d);
    vm_clear_deletes(d);
    return 0;
}

pub export fn dl_query(db: ?*DlDb, goal_rel: [*c]const u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;

    // M4 snapshot path.
    if (d.snap_version > 0 and goal_rel != null and cb != null) {
        return snapshot_query_scan(d.dir.?, d.snap_version, &d.vcache, goal_rel, null, 0, cb, user);
    }

    // Legacy M3 path: compile and run rules, then stream in-memory.
    if (d.n_crules > 0) {
        const m = vm_execute(d, d.crules, d.n_crules);
        if (m != 0) return @intCast(m); // DL_E_FRAMES (-2) propagates (dl.h)
    }

    if (goal_rel != null and cb != null) {
        const idx = findRel(d, goal_rel);
        if (idx < 0) return -1;
        if (d.rels[@intCast(idx)].kind == RELK_VARIADIC)
            return vrelation.vrel_prefix(d.rels[@intCast(idx)].vrel, null, 0, cb, user);
        return relation.rel_prefix(d.rels[@intCast(idx)].rel, null, 0, cb, user);
    }

    return 0;
}

pub export fn dl_query_bound(db: ?*DlDb, goal_rel: [*c]const u8, leading: ?[*]const u32, k: u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;

    // Snapshot path.
    if (d.snap_version > 0 and goal_rel != null and cb != null) {
        return snapshot_query_scan(d.dir.?, d.snap_version, &d.vcache, goal_rel, leading, k, cb, user);
    }

    // Legacy path.
    {
        const idx = findRel(d, goal_rel);
        if (idx < 0) return -1;
        if (d.rels[@intCast(idx)].kind == RELK_VARIADIC)
            return vrelation.vrel_prefix(d.rels[@intCast(idx)].vrel, leading, k, cb, user);
        return relation.rel_prefix(d.rels[@intCast(idx)].rel, leading, k, cb, user);
    }
}

// ─── Read-only arbitrary-rule query (clone-and-evaluate) ───────────────────

/// Deep-copy a relation's VIEW into a fresh in-mem relation.
fn relDeepcopyView(src: ?*const relation.Relation) ?*relation.Relation {
    const s = src orelse return null;
    const ar = relation.rel_arity(s);
    const dst = relation.rel_create(ar) orelse return null;
    var ts: tupleset.tuple_set = undefined;
    if (tupleset.ts_init(&ts, ar) != 0) {
        relation.rel_free(dst);
        return null;
    }
    if (relation.rel_prefix(s, null, 0, relation.ts_sink_cb, &ts) < 0) {
        tupleset.ts_free(&ts);
        relation.rel_free(dst);
        return null;
    }
    tupleset.ts_sort(&ts);
    if (relation.rel_build_from_tupleset(dst, &ts) != 0) {
        tupleset.ts_free(&ts);
        relation.rel_free(dst);
        return null;
    }
    tupleset.ts_free(&ts);
    return dst;
}

fn vrelDeepcopyView(src: ?*const vrelation.Vrelation) ?*vrelation.Vrelation {
    const dst = vrelation.vrel_create() orelse return null;
    var a: u8 = 1;
    while (a <= MAX_VAR_ARITY) : (a += 1) {
        const v = vrelation.vrel_variant_or_null(src, a);
        if (v == null) continue;
        const cv = relDeepcopyView(v);
        if (cv == null or vrelation.vrel_attach(dst, a, cv) != 0) {
            if (cv) |x| relation.rel_free(x);
            vrelation.vrel_free(dst);
            return null;
        }
    }
    return dst;
}

pub export fn dl_query_rules_ro(db: ?*DlDb, dl_source: [*c]const u8, goal_rel: [*c]const u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (dl_source == null or goal_rel == null or cb == null) return -1;

    const p = parse_create(dl_source) orelse return -1;
    var n_rules: c_int = 0;
    const rules = parse_rules(p, &n_rules);
    if (rules == null) {
        parse_free(p);
        return -1;
    }

    if (rulesReferenceRev(rules, n_rules)) {
        freeRulesAndParser(rules, n_rules, p);
        return -1;
    }

    var edb: DlDb = undefined;
    var n_aliased: usize = undefined;
    var result: c_long = -1;
    var owned: [MAX_RELS]u8 = undefined;
    @memset(&owned, 0);
    evalDbClone(d, &edb);
    n_aliased = edb.nrels;
    var crules: ?[*]?*compiler.compiled_rule = null;
    var n_crules: c_int = 0;

    // Pre-declare / replace head relations in the clone.
    {
        var i: c_int = 0;
        while (i < n_rules) : (i += 1) {
            const r = rules.?[@intCast(i)] orelse continue;
            if (r.head == null or r.head.?.pred == null) {
                // fall through to free
                break;
            }
            const name = r.head.?.pred;
            const ar = r.head.?.nargs;
            const ri = findRel(&edb, name);
            if (ri < 0) {
                if (evalDbDeclareInmem(&edb, name, @intCast(ar)) != 0) break;
            } else if (@as(usize, @intCast(ri)) < n_aliased) {
                const e = &edb.rels[@intCast(ri)];
                if (e.kind == RELK_VARIADIC) {
                    const vc = vrelDeepcopyView(e.vrel) orelse break;
                    e.vrel = vc;
                } else {
                    const rc = relDeepcopyView(e.rel) orelse break;
                    e.rel = rc;
                }
                owned[@intCast(ri)] = 1;
            }
        }
        if (i < n_rules) {
            // head pre-declare failed
            evalDbFreeOwned(&edb, n_aliased, &owned);
            freeRulesAndParser(rules, n_rules, p);
            return -1;
        }
    }

    if (compile_rules(&edb, rules, n_rules, &crules, &n_crules) != 0) {
        dlErr("dl_query_rules_ro: compile failed\n", .{});
        {
            var i: c_int = 0;
            while (crules != null and i < n_crules) : (i += 1) compiler.compiled_rule_free(crules.?[@intCast(i)]);
            if (crules) |x| c.free(@ptrCast(x));
        }
        evalDbFreeOwned(&edb, n_aliased, &owned);
        freeRulesAndParser(rules, n_rules, p);
        return -1;
    }

    if (edb.dir != null) {
        dlErr("dl_query_rules_ro: internal error: clone grew a dir\n", .{});
        {
            var i: c_int = 0;
            while (i < n_crules) : (i += 1) compiler.compiled_rule_free(crules.?[@intCast(i)]);
            if (crules) |x| c.free(@ptrCast(x));
        }
        evalDbFreeOwned(&edb, n_aliased, &owned);
        freeRulesAndParser(rules, n_rules, p);
        return -1;
    }

    {
        const m = vm_execute(&edb, crules, n_crules);
        if (m != 0) {
            if (m != DL_E_FRAMES) dlErr("dl_query_rules_ro: evaluation failed\n", .{});
            {
                var i: c_int = 0;
                while (i < n_crules) : (i += 1) compiler.compiled_rule_free(crules.?[@intCast(i)]);
                if (crules) |x| c.free(@ptrCast(x));
            }
            evalDbFreeOwned(&edb, n_aliased, &owned);
            freeRulesAndParser(rules, n_rules, p);
            return m; // DL_E_FRAMES (-2) propagates (dl.h)
        }
    }

    {
        const gi = findRel(&edb, goal_rel);
        if (gi < 0) {
            dlErr("dl_query_rules_ro: goal '{s}' not found\n", .{goal_rel});
            {
                var i: c_int = 0;
                while (i < n_crules) : (i += 1) compiler.compiled_rule_free(crules.?[@intCast(i)]);
                if (crules) |x| c.free(@ptrCast(x));
            }
            evalDbFreeOwned(&edb, n_aliased, &owned);
            freeRulesAndParser(rules, n_rules, p);
            return -1;
        }
        if (edb.rels[@intCast(gi)].kind == RELK_VARIADIC)
            result = vrelation.vrel_prefix(edb.rels[@intCast(gi)].vrel, null, 0, cb, user)
        else
            result = relation.rel_prefix(edb.rels[@intCast(gi)].rel, null, 0, cb, user);
    }

    {
        var i: c_int = 0;
        while (i < n_crules) : (i += 1) compiler.compiled_rule_free(crules.?[@intCast(i)]);
        if (crules) |x| c.free(@ptrCast(x));
    }
    evalDbFreeOwned(&edb, n_aliased, &owned);
    freeRulesAndParser(rules, n_rules, p);
    return result;
}

// ─── Well-founded semantics query (negation over recursion) ────────────────

/// long dl_query_wfs_ro(dl_db*, const char *dl_source, const char *goal_rel,
///                       int truth, dl_tuple_cb cb, void *user)
///
/// Read-only arbitrary-rule query evaluated under WELL-FOUNDED semantics
/// (van Gelder alternating fixpoint, zig/src/wfs.zig): the program may
/// negate a predicate of its own recursion cycle — the class every other
/// entry rejects as "unstratifiable" (compiler.zig:1048/1365/1384).  The
/// canonical case is  win(X) :- move(X,Y), !win(Y).
///
/// Mirrors dl_query_rules_ro's shape (parse -> clone -> declare -> rewrite
/// -> compile -> driver -> goal scan -> teardown) and leaves `db` 100%
/// untouched.  Slice 1 subset: pure relational rules over EDB + IDB,
/// negation of IDB predicates only, safe negation; aggregates/builtins/
/// lists/patterns/variadic/arithmetic are rejected with a diagnostic.
///
/// `truth` selects the streamed slice of the 3-valued model:
///   0 = TRUE-only (implemented), 1 = FALSE-only, 2 = UNDEFINED-only,
///   3 = all-with-tag (modes 1-3 return DL_WFS_ERR_NOT_IMPLEMENTED).
/// Returns the tuple count streamed, or a negative DL_WFS_ERR_* (dl.h).
pub export fn dl_query_wfs_ro(db: ?*DlDb, dl_source: [*c]const u8, goal_rel: [*c]const u8, truth: c_int, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (dl_source == null or goal_rel == null or cb == null) return -1;

    const p = parse_create(dl_source) orelse return -1;
    var n_rules: c_int = 0;
    const rules = parse_rules(p, &n_rules);
    if (rules == null) {
        parse_free(p);
        return -1;
    }
    if (rulesReferenceRev(rules, n_rules)) {
        freeRulesAndParser(rules, n_rules, p);
        return -1;
    }

    // wfs_eval_query rewrites the AST in place; the nodes are freed here
    // right after, exactly like dl_query_rules_ro's ownership discipline.
    const rc = wfs.wfs_eval_query(@ptrCast(d), rules, n_rules, goal_rel, truth, cb, user);
    freeRulesAndParser(rules, n_rules, p);
    return rc;
}

// ─── M4: time-travel (as-of) queries ───────────────────────────────────────

pub export fn dl_query_version(db: ?*DlDb, version: u32, goal_rel: [*c]const u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (version == 0 or goal_rel == null or cb == null) return -1;
    var local: [DL_VIEW_CACHE_SZ]ViewCacheSlot = std.mem.zeroes([DL_VIEW_CACHE_SZ]ViewCacheSlot);
    const r = snapshot_query_scan(d.dir.?, version, &local, goal_rel, null, 0, cb, user);
    vcache_invalidate(&local);
    return r;
}

pub export fn dl_query_bound_version(db: ?*DlDb, version: u32, goal_rel: [*c]const u8, leading: ?[*]const u32, k: u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (version == 0 or goal_rel == null or cb == null) return -1;
    var local: [DL_VIEW_CACHE_SZ]ViewCacheSlot = std.mem.zeroes([DL_VIEW_CACHE_SZ]ViewCacheSlot);
    const r = snapshot_query_scan(d.dir.?, version, &local, goal_rel, leading, k, cb, user);
    vcache_invalidate(&local);
    return r;
}

/// long dl_snapshot_relations(dl_db *db, uint32_t version,
///                             dl_relation_cb cb, void *user)
/// Manifest enumeration (dl.h): stream every fixed-arity relation recorded
/// in snapshot `version`'s manifest.  Errors are the manifest walker's
/// (-1: NULL args, version == 0, no db dir, unreadable/nonexistent version).
pub export fn dl_snapshot_relations(db: ?*DlDb, version: u32, cb: DlRelationCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (version == 0 or cb == null) return -1;
    if (d.dir == null) return -1;
    var sdir: [8192:0]u8 = undefined;
    _ = snprintf(&sdir, 8192, "%s/snapshots/%u", d.dir.?, version);
    return manifest_enumerate_rels(&sdir, cb, user);
}

// ─── Query-scoped eval clone (EDB aliasing for dl_query_rules_ro) ──────────

/// Clone-and-scope: build an eval-only dl_db that shallow-aliases EDB rels.
fn evalDbClone(src: *const DlDb, out: *DlDb) void {
    out.* = std.mem.zeroes(DlDb);
    out.dir = null;
    out.ir = src.ir;
    out.terms = src.terms;
    out.lock_fd = -1;
    // U-homog: the schema MUST ride along — the compile-time kind check
    // uses the DECLARED coltype when a schema is attached, and a clone
    // without it would fall back to recorded kinds and answer differently
    // than dl_load_rules did on the same db/program.
    out.schema = src.schema;
    var i: usize = 0;
    while (i < src.nrels) : (i += 1) {
        out.rels[i].name = src.rels[i].name;
        out.rels[i].kind = src.rels[i].kind;
        out.rels[i].arity = src.rels[i].arity;
        out.rels[i].rel = src.rels[i].rel;
        out.rels[i].vrel = src.rels[i].vrel;
    }
    out.nrels = src.nrels;
    out.n_perms = 0;
}

/// Free the fresh relations/names of an eval clone (indices >= n_aliased),
/// plus any deep-copied collision heads (owned[i]).
fn evalDbFreeOwned(edb: *DlDb, n_aliased: usize, owned: *const [MAX_RELS]u8) void {
    if (edb.crules) |crs| {
        var i: usize = 0;
        while (i < @as(usize, @intCast(edb.n_crules))) : (i += 1)
            compiler.compiled_rule_free(crs[i]);
        c.free(@ptrCast(crs));
    }
    permindex_free_all(edb);
    var i: usize = 0;
    while (i < edb.nrels) : (i += 1) {
        if (i >= n_aliased) {
            if (edb.rels[i].kind == RELK_VARIADIC)
                vrelation.vrel_free(edb.rels[i].vrel)
            else
                relation.rel_free(edb.rels[i].rel);
            if (edb.rels[i].name) |n| c.free(@ptrCast(n));
        } else if (owned[i] != 0) {
            if (edb.rels[i].kind == RELK_VARIADIC)
                vrelation.vrel_free(edb.rels[i].vrel)
            else
                relation.rel_free(edb.rels[i].rel);
            // name borrowed from db — do not free
        }
    }
}

/// Pre-declare one relation into the clone using rel_create (in-memory only).
fn evalDbDeclareInmem(edb: *DlDb, name: [*c]const u8, arity: u8) c_int {
    if (edb.nrels >= MAX_RELS) return -1;
    if (findRel(edb, name) >= 0) return -1;
    const rel = relation.rel_create(arity) orelse return -1;
    edb.rels[edb.nrels].name = strdup(name);
    if (edb.rels[edb.nrels].name == null) {
        relation.rel_free(rel);
        return -1;
    }
    edb.rels[edb.nrels].kind = RELK_FIXED;
    edb.rels[edb.nrels].arity = arity;
    edb.rels[edb.nrels].rel = rel;
    edb.rels[edb.nrels].vrel = null;
    edb.nrels += 1;
    return 0;
}

// ─── M4: snapshot publish ───────────────────────────────────────────────────

// ─── publish timing instrumentation (selfreg-dl-storage) ───────────────────
//
// Always-compiled cumulative timers for the materialization-vs-serialization
// split.  Zero overhead when DL_PUB_TIMING is unset (one getenv at publish
// entry); when set, each publish accumulates into these counters and
// dl_publish_timing_reset() zeroes them.  dl_publish_timing_get() copies
// them out.  NOT ABI-gated symbols (new dl_* additions only; the audit
// checks the header-declared surface).
//
// Scope (selfreg periodic snapshot): n_publishes and t_total_ns count
// dl_publish_snapshot CALLS only — dl_consolidate bumps neither, so a
// consolidate-only workload is invisible to them (do not read n_publishes
// as the number of consolidation cycles; nothing in-tree reads these,
// tests only).  Once a publish has enabled the timers, dl_consolidate's
// shared paths do accumulate: consolidate() feeds t_ivm_ns and a
// budget-triggered materializeSnapshot fills the serialize buckets.
const timec = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("time.h");
});

// Local timespec shape (C ABI: two isize fields) — musl's time.h struct
// timespec does not survive translate-c either; clock_gettime is handed
// the same layout via a local extern.
const Timespec = extern struct {
    sec: isize,
    nsec: isize,
};
const CLOCK_MONOTONIC: c_int = 1;
extern fn clock_gettime(clk_id: c_int, tp: *Timespec) c_int;

pub const dl_publish_timing_out = extern struct {
    n_publishes: u64, // number of publishes accumulated
    t_intern_ns: u64, // interner save (symbols.array; + derived symbols.dafsa when DL_PUB_DERIVED=1)
    t_terms_ns: u64, // term store save (terms.bin)
    t_rel_save_ns: u64, // sum over relations: rel_save (dafsa serialization)
    t_rel_other_ns: u64, // manifest writes + misc per-relation work
    t_fsync_ns: u64, // dir fsyncs (tmp dir + snapshots dir)
    t_rename_ns: u64, // atomic rename of tmp -> final
    t_current_ns: u64, // CURRENT flip (atomic_write_str)
    t_prune_ns: u64, // snapshot retention prune
    t_ivm_ns: u64, // fixpoint materialization (step 1 cascade)
    t_total_ns: u64, // whole dl_publish_snapshot call
};

var pub_timers = std.mem.zeroes(dl_publish_timing_out);
var pub_timers_on: bool = false;
var pub_timers_env_checked: bool = false;

fn pubTimersCheckEnv() void {
    if (pub_timers_env_checked) return;
    pub_timers_env_checked = true;
    const v = getenv("DL_PUB_TIMING");
    pub_timers_on = (v != null and v.?[0] != 0 and !strEq(v.?, "0"));
}

fn nowNs() u64 {
    // std.time.nanoTimestamp was dropped in 0.16; clock_gettime directly.
    var ts: Timespec = undefined;
    _ = clock_gettime(CLOCK_MONOTONIC, &ts);
    return @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
}

/// Nanoseconds as (fractional) milliseconds, for the DL_PUB_TIMING report.
fn nsToMs(v: u64) f64 {
    return @as(f64, @floatFromInt(v)) / 1e6;
}

/// void dl_publish_timing_get(dl_publish_timing_out *out)
pub export fn dl_publish_timing_get(out: ?*dl_publish_timing_out) void {
    const o = out orelse return;
    o.* = pub_timers;
}

/// void dl_publish_timing_reset(void)
pub export fn dl_publish_timing_reset() void {
    pub_timers = std.mem.zeroes(dl_publish_timing_out);
}

/// Best-effort recursive removal of a file or directory.
fn rmRf(path: [*c]const u8) void {
    const d = posix.opendir(path);
    if (d != null) {
        var e = posix.readdir(d);
        while (e != null) : (e = posix.readdir(d)) {
            var child: [4096:0]u8 = undefined;
            var st: Stat = undefined;
            if (strEq(@ptrCast(&e.*.d_name), ".") or strEq(@ptrCast(&e.*.d_name), ".."))
                continue;
            _ = snprintf(&child, 4096, "%s/%s", path, @as([*c]const u8, @ptrCast(&e.*.d_name)));
            if (lstat(&child, &st) == 0 and posix.S_ISDIR(st.st_mode))
                rmRf(&child) // recurse first
            else
                _ = posix.unlink(&child);
        }
        _ = posix.closedir(d);
        _ = posix.rmdir(path);
        return;
    }
    _ = posix.unlink(path);
}

/// Enumerate all published snapshot versions, sorted ascending.  Returns the
/// count, with *out pointing at a malloc'd array (caller frees).
fn snapshotEnumerate(db_dir: [*c]const u8, out: *?[*]u32, count: *usize) c_long {
    out.* = null;
    count.* = 0;

    if (db_dir == null) return 0;

    var snapshots_dir: [4096:0]u8 = undefined;
    _ = snprintf(&snapshots_dir, 4096, "%s/snapshots", db_dir);
    const d = posix.opendir(&snapshots_dir) orelse return 0; // ENOENT → empty

    var vers: ?[*]u32 = null;
    var n: usize = 0;
    var alloc: usize = 0;

    var e = posix.readdir(d);
    while (e != null) : (e = posix.readdir(d)) {
        var st: Stat = undefined;
        const name: [*c]const u8 = @ptrCast(&e.*.d_name);

        if (strEq(name, ".") or strEq(name, "..")) continue;
        if (name[0] < '0' or name[0] > '9') continue;

        var endp: [*c]u8 = null;
        const v = strtoul(name, @ptrCast(&endp), 10);
        if (endp[0] != 0) continue; // not a clean all-digit parse
        if (v < 1 or v > 0xFFFFFFFF) continue;

        // Only real version DIRECTORIES are enumerated.
        if (fstatat(posix.dirfd(d), name, &st, 0) != 0 or !posix.S_ISDIR(st.st_mode))
            continue;

        if (n == alloc) {
            const na: usize = if (alloc != 0) alloc * 2 else 16;
            const nv = c.realloc(@ptrCast(vers), na * @sizeOf(u32)) orelse {
                if (vers) |x| c.free(@ptrCast(x));
                _ = posix.closedir(d);
                return -1;
            };
            vers = @ptrCast(@alignCast(nv));
            alloc = na;
        }
        vers.?[n] = @truncate(v);
        n += 1;
    }
    _ = posix.closedir(d);

    if (n > 1) {
        std.mem.sort(u32, vers.?[0..n], {}, comptime std.sort.asc(u32));
    }

    out.* = vers;
    count.* = n;
    return @intCast(n);
}

pub export fn dl_snapshot_versions(db: ?*const DlDb, out: ?[*]u32, cap: usize) c_long {
    const d = db orelse return -1;

    var vers: ?[*]u32 = null;
    var n: usize = 0;
    const total = snapshotEnumerate(d.dir.?, &vers, &n);
    if (total < 0) return -1;

    if (out != null and cap > 0 and n > 0) {
        const m = if (n < cap) n else cap;
        @memcpy(out.?[0..m], vers.?[0..m]);
    }
    if (vers) |x| c.free(@ptrCast(x));
    return total;
}

pub export fn dl_set_snapshot_retain(db: ?*DlDb, n: c_uint) c_int {
    const d = db orelse return -1;
    d.snapshot_retain = n;
    return 0;
}

/// Opt-in retention: keep at most db->snapshot_retain most-recent versions.
fn pruneSnapshots(db: *DlDb) void {
    if (db.snapshot_retain == 0) return;

    var vers: ?[*]u32 = null;
    var n: usize = 0;
    const total = snapshotEnumerate(db.dir.?, &vers, &n);
    if (total < 0 or n <= db.snapshot_retain) {
        if (vers) |x| c.free(@ptrCast(x));
        return;
    }

    const drop = n - db.snapshot_retain;
    var i: usize = 0;
    while (i < drop) : (i += 1) {
        var vdir: [4096:0]u8 = undefined;
        _ = snprintf(&vdir, 4096, "%s/snapshots/%u", db.dir.?, vers.?[i]);
        rmRf(&vdir);
        // A view of the pruned version maps a file that no longer exists —
        // close those slots rather than keep serving them.
        vcache_drop_version(&db.vcache, vers.?[i]);
    }
    if (vers) |x| c.free(@ptrCast(x));
}

// ─── Access-epoch decay (selfreg-dl-storage, design decision C20) ─────────
//
// WHY EPOCHS, NOT PER-ACCESS WRITES: tracking last-access per fact would
// make every read a write — impossible on a read-only handle (dl_open_ro
// rejects every write API; the reader coexists with the writer and holds no
// lock after its open) and outside the ~10us
// per-step budget (point reads measure 1.5-8.5us; a per-read WAL append +
// fsync measures 0.7us BEST-case on tmpfs and forces reader/writer
// alternation per read).  So "last access" is approximated at CYCLE
// (consolidation) granularity: each relation carries ov_epoch, bumped when
// the relation is written or read through the overlay during a cycle; decay
// is applied AT CONSOLIDATION ONLY — the same sleep cycle the overlay
// introduces.  Decay costs O(relations), never O(store), and the durable
// store never sees a read.
//
// WHAT DECAYS AND WHAT MUST NOT (C9 split): DECISIONS/OBSERVATIONS (EDB
// facts) are the unrecoverable class — append-only, never decayed, only
// REWEIGHTED.  DERIVED (IDB) facts are recomputable from premises — the
// recomputable layer may be dropped at consolidation because the IVM
// cascade re-derives it.  decayApply therefore only prunes IDB view
// relations that the fixpoint will rebuild; EDB base relations are never
// touched.
//
// RATE DERIVATION (not tuned): steady-state overlay memory under decay is
// bounded by (arrivals per cycle) * (cycles of retention).  The budget
// criterion is: decay must retain at least one full hot cycle of writes
// (err SLOW — decaying something the mechanism still needs is a SILENT
// failure, while unbounded memory is LOUD), so retention >= 1 cycle by
// construction and the knob is the SIZE GATE: if the total overlay
// occupancy across relations exceeds DL_OVERLAY_BUDGET_BYTES, consolidation
// flushes (materializes) ALL overlays early rather than letting the working
// set grow unbounded.  Flushing is always safe (the overlay is a cache of
// WAL-durable facts, not the durability record itself).
//
// SIZE GATE: the overlay working set is bounded by DL_OVERLAY_BUDGET_BYTES
// (default 256 MiB, override via DL_OVERLAY_BUDGET_MB).  Exceeding it at
// consolidation forces a full materialization — memory explosion is caught
// LOUDLY (a slow publish) instead of silently (OOM).

/// Default overlay working-set budget before the size gate forces a full
/// materialization (bytes).
const DL_OVERLAY_BUDGET_BYTES_DEFAULT: u64 = 256 * 1024 * 1024;

fn overlayBudgetBytes() u64 {
    const v = getenv("DL_OVERLAY_BUDGET_MB");
    if (v != null and v.?[0] != 0) {
        const mb = strtoul(v, null, 10);
        if (mb > 0) return mb * 1024 * 1024;
    }
    return DL_OVERLAY_BUDGET_BYTES_DEFAULT;
}

/// Default WAL-append byte budget before dl_consolidate materializes a
/// snapshot (selfreg periodic snapshot).  Env override DL_SNAPSHOT_BUDGET_MB.
const DL_SNAPSHOT_BUDGET_BYTES_DEFAULT: u64 = 64 * 1024 * 1024;

fn snapshotBudgetBytes() u64 {
    const v = getenv("DL_SNAPSHOT_BUDGET_MB");
    if (v != null and v.?[0] != 0) {
        const mb = strtoul(v, null, 10);
        if (mb > 0) return mb * 1024 * 1024;
    }
    return DL_SNAPSHOT_BUDGET_BYTES_DEFAULT;
}

/// Total overlay occupancy across all relations, in approximate bytes
/// (tuples * arity * 4 for the sorted array + 4 for the hash slot).
fn overlayTotalBytes(d: *DlDb) u64 {
    var total: u64 = 0;
    var ri: usize = 0;
    while (ri < d.nrels) : (ri += 1) {
        if (d.rels[ri].kind == RELK_VARIADIC) {
            var a: u8 = 1;
            while (a <= MAX_VAR_ARITY) : (a += 1) {
                const vr = vrelation.vrel_variant_or_null(d.rels[ri].vrel, a);
                if (vr != null)
                    total += relation.rel_overlay_count(vr) * (@as(u64, d.rels[ri].arity + 1) * 4 + 4);
            }
        } else {
            total += relation.rel_overlay_count(d.rels[ri].rel) * (@as(u64, d.rels[ri].arity) * 4 + 4);
        }
    }
    return total;
}

/// Decay + size-gate pass, run at consolidation.  Returns 0 ok / -1 error.
/// Err-slow by construction: nothing is dropped; the only action is
/// materializing overlays (always safe) when the working set exceeds the
/// budget.  IDB relations whose facts are recomputable rely on the IVM
/// cascade (step 1 of publish) to rebuild — decay never deletes a premise.
fn decayApply(d: *DlDb) c_int {
    if (overlayTotalBytes(d) > overlayBudgetBytes()) {
        // SIZE GATE fired: the working set is exploding; materialize all
        // overlays NOW (this publish is slower, i.e. LOUD, but memory is
        // bounded).  rel_overlay_flush is idempotent after the publish-time
        // flush — this only triggers if the budget was exceeded BEFORE the
        // flush pass, i.e. on the next cycle after a huge batch.
        var ri: usize = 0;
        while (ri < d.nrels) : (ri += 1) {
            if (d.rels[ri].kind == RELK_VARIADIC) {
                var a: u8 = 1;
                while (a <= MAX_VAR_ARITY) : (a += 1) {
                    const vr = vrelation.vrel_variant_or_null(d.rels[ri].vrel, a);
                    if (vr != null and relation.rel_overlay_flush(vr) != 0)
                        return -1;
                }
            } else {
                if (relation.rel_overlay_flush(d.rels[ri].rel) != 0)
                    return -1;
            }
        }
    }
    return 0;
}

/// double dl_overlay_bytes(db) — test/bench observable: current overlay
/// working-set size in bytes.
pub export fn dl_overlay_bytes(db: ?*const DlDb) f64 {
    const d = db orelse return 0;
    return @floatFromInt(overlayTotalBytes(@constCast(d)));
}

/// uint32_t dl_access_epoch(db) — test observable: current access epoch.
pub export fn dl_access_epoch(db: ?*const DlDb) u32 {
    const d = db orelse return 0;
    return d.access_epoch;
}

/// Cycle-boundary durability barrier for the fact-write path
/// (selfreg-batched-fsync): fsync every per-relation WAL so all EDB facts
/// appended since the previous barrier are durable.  Mirrors the txn path's
/// one-sync-per-commit shape (txnwal_append_record ... then ONE txnwal_sync),
/// applied at consolidation granularity instead of per fact.  Only EDB facts
/// reach these WALs (derived IDB facts are recomputable and never appended),
/// so this barrier is exactly the durability contract for the unrecoverable
/// class.  Errors are swallowed: the caller (publish / close) is the
/// boundary, and a failed fsync here is surfaced by the same path's later
/// durable operations (rel_compact / snapshot fsync); this mirrors dl_close's
/// existing ignore-on-error convention.
fn syncAllWals(d: *DlDb) void {
    var ri: usize = 0;
    while (ri < d.nrels) : (ri += 1) {
        if (d.rels[ri].kind == RELK_VARIADIC) {
            var a: u8 = 1;
            while (a <= MAX_VAR_ARITY) : (a += 1) {
                const vr = vrelation.vrel_variant_or_null(d.rels[ri].vrel, a);
                if (vr != null) _ = relation.rel_wal_sync(vr);
            }
        } else {
            _ = relation.rel_wal_sync(d.rels[ri].rel);
        }
    }
}

/// Sum of rel_wal_appended() over every relation that carries a WAL
/// (fixed relations + variadic variants).  This is the monotonic append
/// traffic since open, immune to rel_compact's ftruncate — the signal the
/// periodic-snapshot trigger compares against wal_appended_at_snapshot.
fn sumRelWalAppended(d: *DlDb) u64 {
    var total: u64 = 0;
    var ri: usize = 0;
    while (ri < d.nrels) : (ri += 1) {
        if (d.rels[ri].kind == RELK_VARIADIC) {
            var a: u8 = 1;
            while (a <= MAX_VAR_ARITY) : (a += 1) {
                const vr = vrelation.vrel_variant_or_null(d.rels[ri].vrel, a);
                if (vr != null) total += relation.rel_wal_appended(vr);
            }
        } else {
            total += relation.rel_wal_appended(d.rels[ri].rel);
        }
    }
    return total;
}

/// Consolidation (selfreg-dl-storage): the single maintenance point shared by
/// dl_publish_snapshot and dl_consolidate.  Readers DO coexist with the
/// writer (dl.h CONCURRENCY MODEL), so this is not a stop-the-world point for
/// them: a reader's visibility boundary is its own open.  What it IS: the
/// writer's quiescent point, where all non-materializing store maintenance
/// happens:
///   0. fsync every per-relation WAL (the cycle-boundary durability
///      barrier — selfreg-batched-fsync);
///   1. fold every relation's hot-write overlay into its DAFSA (C19);
///   2. resolve supersession (latest-wins is the flush's set union);
///   3. advance the access epoch (C20 decay bookkeeping);
///   4. apply decay / fire the size gate (see decayApply below);
///   5. materialize derived facts (the IVM cascade, step 1 below).
/// Durability contract (selfreg-batched-fsync): the per-relation WALs are
/// fsync'd HERE and at dl_close — NOT per append.  Un-consolidated EDB
/// writes are durable only from this barrier onward; a crash before the
/// next barrier loses exactly the current cycle's writes (a bounded,
/// stated exposure), while everything before it replays at open.
/// This is the CHEAP half: it does NOT serialize the store into a snapshot
/// (that is materializeSnapshot, gated by the budget trigger in
/// dl_consolidate and unconditional in dl_publish_snapshot).
fn consolidate(d: *DlDb) c_int {
    return consolidateInner(d);
}

fn consolidateInner(d: *DlDb) c_int {
    const t_mark: u64 = if (pub_timers_on) nowNs() else 0;
    {
        syncAllWals(d);
        var ri: usize = 0;
        while (ri < d.nrels) : (ri += 1) {
            if (d.rels[ri].kind == RELK_VARIADIC) {
                var a: u8 = 1;
                while (a <= MAX_VAR_ARITY) : (a += 1) {
                    const vr = vrelation.vrel_variant_or_null(d.rels[ri].vrel, a);
                    if (vr == null) continue;
                    if (relation.rel_overlay_flush(vr) != 0)
                        return -1;
                }
            } else {
                if (relation.rel_overlay_flush(d.rels[ri].rel) != 0)
                    return -1;
            }
        }
        d.access_epoch +%= 1;
        if (decayApply(d) != 0) return -1;
    }

    // 1. Materialize derived views if rules exist and the fixpoint is dirty.
    //    IVM Slice 1/3 dispatch cascade (the correctness crux).
    if (d.n_crules > 0 and d.fixpoint_dirty != 0) {
        var has_del: c_int = 0;
        var has_ins: c_int = 0;
        var has_agg: c_int = 0;
        var dri: usize = 0;
        while (dri < MAX_RELS) : (dri += 1) {
            if (d.del_pending[dri] != null and d.del_pending[dri].?.count > 0) has_del = 1;
            if (d.delta_pending[dri] != null and d.delta_pending[dri].?.count > 0) has_ins = 1;
        }
        {
            var ci: c_int = 0;
            while (ci < d.n_crules) : (ci += 1) {
                if (d.crules.?[@intCast(ci)].?.has_aggregate != 0) has_agg = 1;
            }
        }
        if (d.full_reeval_pending != 0) {
            {
                const m = vm_execute(d, d.crules, d.n_crules);
                if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
            }
            vm_clear_deltas(d);
            vm_clear_deletes(d);
        } else if (has_agg != 0) {
            if (vm_agg_eligible(d) != 0) {
                if (vm_agg_maintain(d) != 0) {
                    d.full_reeval_pending = 1;
                    return -1;
                }
            } else {
                    {
                        const m = vm_execute(d, d.crules, d.n_crules);
                        if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
                    }
                vm_clear_deltas(d);
                vm_clear_deletes(d);
            }
        } else if (has_del != 0 and vm_dred_eligible(d) == 0) {
            {
                const m = vm_execute(d, d.crules, d.n_crules);
                if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
            }
            vm_clear_deltas(d);
            vm_clear_deletes(d);
        } else if (has_ins != 0 and vm_ivm_eligible(d) == 0 and vm_dred_eligible(d) == 0) {
            {
                const m = vm_execute(d, d.crules, d.n_crules);
                if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
            }
            vm_clear_deltas(d);
            vm_clear_deletes(d);
        } else if (has_del != 0 or (has_ins != 0 and vm_ivm_eligible(d) == 0)) {
            if (vm_dred_delete(d) != 0) {
                d.full_reeval_pending = 1;
                return -1;
            }
        } else if (vm_ivm_eligible(d) == 0) {
            {
                const m = vm_execute(d, d.crules, d.n_crules);
                if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
            }
            vm_clear_deltas(d);
            vm_clear_deletes(d);
        } else if (vm_has_recursive(d) != 0) {
            if (vm_execute_ivm(d) != 0) {
                d.full_reeval_pending = 1;
                vm_clear_deltas(d);
                return -1;
            }
            vm_clear_deltas(d);
        } else if (vm_propagate_deltas(d) != 0) {
            d.full_reeval_pending = 1;
            return -1;
        }
        d.full_reeval_pending = 0;
        d.fixpoint_dirty = 0;
    }
    if (pub_timers_on) {
        pub_timers.t_ivm_ns += nowNs() - t_mark;
    }
    return 0;
}

// ─── Derived-file publish opt-in ───────────────────────────────────────────
//
// DL_PUB_DERIVED=1 makes publish persist <snapshot>/symbols.dafsa again (the
// pre-2026-08 behaviour).  Nothing in the ENGINE reads it: the version pattern
// path scans the snapshot's symbols.array (snapshotSymSet), so the file is
// produced for an external consumer that wants the key set as a DAFSA and has
// no interner.  Zero overhead when unset (one getenv at the first publish);
// OFF is the default.  Motivated in the 4a block comment of
// materializeSnapshot: the file is a pure derived cache of symbols.array, and
// re-serializing it on every vocabulary change was the entire cost of a
// content write.
var pub_derived_on: bool = false;
var pub_derived_env_checked: bool = false;

fn pubDerivedCheckEnv() bool {
    if (!pub_derived_env_checked) {
        pub_derived_env_checked = true;
        const v = getenv("DL_PUB_DERIVED");
        pub_derived_on = (v != null and v.?[0] != 0 and !strEq(v.?, "0"));
    }
    return pub_derived_on;
}

/// Test hook (not exported): the environment is read once per PROCESS, so a
/// test cannot flip the switch with setenv after another test has published.
/// Sets the cached decision directly; tests restore it when they are done.
fn pubDerivedForce(on: bool) void {
    pub_derived_on = on;
    pub_derived_env_checked = true;
}

/// Write the staged snapshot's derived symbols.dafsa.  Hardlinks the previous
/// snapshot's pair when the staged symbols.array is byte-identical to the
/// previous snapshot's (it then describes the same key set, and a DAFSA of the
/// same keys is answer-equivalent — dafsa_view surfaces keys, never state ids),
/// else serializes the live forward DAFSA.  `rev_inherited` says the array was
/// ALREADY hardlinked from the previous snapshot by inheritSymbolsArray, which
/// makes the byte comparison a tautology — skip reading 44 MB to prove it.
/// Returns 0 on success, -1 on failure.
fn publishDerivedDafsa(d: *DlDb, rev: *[4096:0]u8, fwd: *[4096:0]u8, prev_snap_dir: [*c]const u8, rev_inherited: bool) c_int {
    // Test hook: this file is the one optional publish product, and its
    // failure path needs to be reachable deterministically (a full filesystem
    // is not).
    if (d.fault_hook) |hook| {
        if (hook(DL_FPOINT_DERIVED_SAVE, d.fault_user) != 0) return -1;
    }
    var inherited = false;
    if (prev_snap_dir[0] != 0) {
        var prev_fwd: [4096:0]u8 = undefined;
        var prev_rev: [4096:0]u8 = undefined;
        _ = snprintf(&prev_fwd, 4096, "%s/symbols.dafsa", prev_snap_dir);
        _ = snprintf(&prev_rev, 4096, "%s/symbols.array", prev_snap_dir);
        if (rev_inherited or filesIdentical(rev, &prev_rev)) {
            _ = posix.unlink(rev);
            if (posix.link(&prev_rev, rev) == 0 and posix.link(&prev_fwd, fwd) == 0) {
                inherited = true;
            } else {
                // Half-made link, or the previous snapshot carries no derived
                // file (it was published with the default, or predates it):
                // drop the links and re-serialize rather than publish one half
                // of the pair.
                _ = posix.unlink(rev);
                _ = posix.unlink(fwd);
                if (intern_save(d.ir, fwd, rev) != 0) return -1;
            }
        }
    }
    if (inherited) return 0;
    const f = intern_fwd(d.ir) orelse return -1;
    return dc.dafsa_save(@ptrCast(@alignCast(f)), fwd);
}

/// Snapshot materialization: serialize the consolidated state into a NEW
/// versioned snapshot dir and flip CURRENT (steps 2-8 of the publish
/// contract).  This is the EXPENSIVE half — O(store) serialize + the tmp/
/// snapshots dir fsyncs — and is what the periodic-snapshot trigger gates.
/// Also resets the WAL-append watermark so the next budget window counts
/// only traffic since THIS materialization.
fn materializeSnapshot(d: *DlDb) c_int {
    // The whole materialization is a state replacement (new versioned dir,
    // CURRENT flip, prune) — one mutation window for all of it, including
    // the dlPublishFail cleanup on the error paths (defers run after the
    // cleanup call returns).
    mutWindowBegin(d);
    defer mutWindowEnd(d);
    var snapshots_dir: [4096:0]u8 = undefined;
    var tmp_dir: [4096:0]u8 = undefined;
    var new_dir: [4096:0]u8 = undefined;
    var current_path: [4096:0]u8 = undefined;
    var buf: [4096:0]u8 = undefined;
    var renamed: c_int = 0;
    // Publish-gate staging (defect: watermark committed before durability).
    // relSaveGated writes the NEW rev here instead of into d.saved_rev*;
    // the staged values are COMMITTED into the live watermarks only at the
    // successful end of this publish (step 7).  Until then the live
    // watermarks keep describing the LAST durable snapshot, so a publish
    // that fails after a gated save (fclose, fsync, CURRENT flip) leaves
    // them untouched — the invariant "a watermark never claims content is
    // in the previous snapshot unless it is actually durable in it" holds
    // even on the failure path, and a mutation-free retry re-serializes
    // instead of hardlinking bytes that were never published.
    var stage_rev: [MAX_RELS]u64 = @splat(SAVED_REV_NONE);
    var stage_rev_var: [MAX_RELS][MAX_VAR_ARITY + 1]u64 = @splat(@splat(SAVED_REV_NONE));
    var stage_rev_perm: [MAX_PERMS]u64 = @splat(SAVED_REV_NONE);
    var stage_perm_rel: [MAX_PERMS]?*anyopaque = @splat(null);

    var t_mark: u64 = if (pub_timers_on) nowNs() else 0;

    // 2. Determine new version
    const new_version = d.snap_version +% 1;

    // Publish gate: the previous snapshot's directory, for hardlinking
    // unchanged relations (see relSaveGated).  Empty string when there is
    // no previous snapshot in this process — the gate then always saves.
    var prev_snap_dir: [4096:0]u8 = undefined;
    if (d.snap_version > 0) {
        _ = snprintf(&prev_snap_dir, 4096, "%s/snapshots/%u", d.dir.?, d.snap_version);
    } else {
        prev_snap_dir[0] = 0;
    }

    // 3. Ensure snapshots directory exists
    _ = snprintf(&snapshots_dir, 4096, "%s/snapshots", d.dir.?);
    _ = mkdir(&snapshots_dir, @as(posix.mode_t, 0o755));

    // 4. Build into <db>/snapshots/<new_v>.tmp/
    _ = snprintf(&tmp_dir, 4096, "%s/snapshots/%u.tmp", d.dir.?, new_version);
    rmRf(&tmp_dir);
    if (mkdir(&tmp_dir, @as(posix.mode_t, 0o755)) != 0)
        return -1;

    // 4a. Save the interner — symbols.array, the SOURCE OF TRUTH, and nothing
    // else by default.
    //
    // The forward DAFSA is a PURE DERIVED CACHE of symbols.array: nothing ever
    // reads <snapshot>/symbols.dafsa as the source of truth (intern_load builds
    // the interner from symbols.array alone and ensureFwd rebuilds the forward
    // DAFSA from it — intern.zig), and NO READ PATH reads the file at all any
    // more: dl_pattern's version route scans the snapshot's symbols.array
    // directly (snapshotSymSet), so the file's last consumer is gone.
    // Publishing it therefore buys nothing and costs a full re-serialization
    // of every state on any vocabulary change:
    //
    // MEASURED (agent store: 166,745 symbols, 44 MB array): a publish that
    // interns ONE new symbol pays a 11.7 s ensureFwd rebuild + 17.5 s
    // dafsa_save ≈ 298 MB, which at ~0.3 s of actual query work is the entire
    // cost of a content write — and every new agent-memory node adds symbols,
    // so every content write paid it.  The snapshot's own copy was never
    // consulted on a publishing process (the live in-memory map answers
    // identically; see snapshotSymSet), so the default is: DO NOT PERSIST IT.
    //
    // DL_PUB_DERIVED=1 still produces the file for an EXTERNAL consumer (a
    // tool that wants the key set as a DAFSA and has no interner of its own —
    // dl_abi still exports dafsa_view/the walkers; NO engine code path reads
    // <snapshot>/symbols.dafsa any more).
    // The old hardlink gate lives there: when the freshly serialized array is
    // byte-identical to the previous snapshot's, that snapshot's two files
    // already describe the same key set (a DAFSA of the same keys is
    // answer-equivalent even if a fresh save would number its states in
    // another order — dafsa_view surfaces keys, never state ids), so the pair
    // is hardlinked instead of re-serialized.  Snapshot files are immutable
    // once renamed into place (only rmRf unlinks them), so the links are safe
    // exactly as relSaveGated's are.
    {
        var fwd: [4096:0]u8 = undefined;
        var rev: [4096:0]u8 = undefined;
        _ = snprintf(&fwd, 4096, "%s/symbols.dafsa", &tmp_dir);
        _ = snprintf(&rev, 4096, "%s/symbols.array", &tmp_dir);
        // Steady-state path: the previous snapshot recorded the same (count,
        // size, content-hash) stamp, so its array IS the byte stream
        // intern_save would write — hardlink it instead of rewriting 44 MB.
        // The decision itself is O(1); only the stamp's hash pass is O(array),
        // at most once per process (see intern.zig symStamp).
        // Every other case (first publish, vocabulary change, missing/old
        // stamp, same count with different content, link failure) falls
        // through to the full save.
        const inherited = inheritSymbolsArray(d.ir, &rev, &prev_snap_dir);
        // A dirty interner at entry means the ROOT <db>/symbols.array lags the
        // live one (every save path clears the flag).  Snapshot-only persistence
        // is NOT enough: a reopen loads next_id from the root array, so a short
        // root hands the ids this snapshot already assigned to other strings —
        // sym-id reuse, which dl_pattern's VERSION route then reads as a
        // different symbol.  Publish therefore establishes the invariant
        // itself: after it, the root array equals the live serialization.  The
        // check is O(1) and the write is skipped on the steady path (not dirty
        // means some earlier save already wrote the live bytes — dl_add_fact
        // and dl_close both do it, and a vocabulary-changing publish is always
        // preceded by such an append).
        const root_lagged = intern_is_dirty(d.ir) != 0;
        if (!inherited and intern_save(d.ir, &fwd, &rev) != 0)
            return dlPublishFail(&tmp_dir, &new_dir, renamed);
        if (root_lagged) {
            const root_fwd = makePath(d, "symbols", ".dafsa") orelse
                return dlPublishFail(&tmp_dir, &new_dir, renamed);
            defer c.free(@ptrCast(root_fwd));
            const root_rev = makePath(d, "symbols", ".array") orelse
                return dlPublishFail(&tmp_dir, &new_dir, renamed);
            defer c.free(@ptrCast(root_rev));
            if (intern_save(d.ir, root_fwd, root_rev) != 0)
                return dlPublishFail(&tmp_dir, &new_dir, renamed);
        }

        if (pubDerivedCheckEnv() and publishDerivedDafsa(d, &rev, &fwd, &prev_snap_dir, inherited) != 0)
            return dlPublishFail(&tmp_dir, &new_dir, renamed);
    }
    if (pub_timers_on) {
        pub_timers.t_intern_ns += nowNs() - t_mark;
        t_mark = nowNs();
    }

    // 4a'. Save the list term store.
    {
        var tpath: [4096:0]u8 = undefined;
        _ = snprintf(&tpath, 4096, "%s/terms.bin", &tmp_dir);
        if (term_save(d.terms, &tpath) != 0)
            return dlPublishFail(&tmp_dir, &new_dir, renamed);
    }
    if (pub_timers_on) {
        pub_timers.t_terms_ns += nowNs() - t_mark;
        t_mark = nowNs();
    }

    // 4b. Save each relation + write manifest.
    {
        var manifest_path: [4096:0]u8 = undefined;
        _ = snprintf(&manifest_path, 4096, "%s/manifest.txt", &tmp_dir);
        const mf = c.fopen(&manifest_path, "w") orelse return dlPublishFail(&tmp_dir, &new_dir, renamed);
        var t_rel_mark: u64 = if (pub_timers_on) nowNs() else 0;

        _ = fprintf(mf, "# Datalog-DAFSA snapshot version %u\n", new_version);

        // Symbols stamp for the NEXT publish's inheritance gate (see
        // inheritSymbolsArray): the interner's content stamp (count + 128-bit
        // hash of the canonical array bytes) plus the array FILE's size, which
        // the gate stats for an O(1) integrity check.  A 'D:' directive line,
        // so every manifest walker (manifest_find_rel/_ex/_variants/
        // manifestParseLine) skips it exactly as it skips 'D:some-directive'.
        {
            const stamp = intern_mod.symStamp(d.ir);
            var srev: [4096:0]u8 = undefined;
            _ = snprintf(&srev, 4096, "%s/symbols.array", &tmp_dir);
            const nbytes = fileSize(&srev) orelse {
                _ = c.fclose(mf);
                return dlPublishFail(&tmp_dir, &new_dir, renamed);
            };
            _ = fprintf(mf, "D:symbols:%u:%llu:%016llx:%016llx\n", stamp.count, nbytes, stamp.h0, stamp.h1);
        }

        var i: usize = 0;
        while (i < d.nrels) : (i += 1) {
            var rel_path: [4096:0]u8 = undefined;

            if (d.rels[i].kind == RELK_VARIADIC) {
                var a: u8 = 0;
                _ = fprintf(mf, "%s:*:%s\n", d.rels[i].name, if (vrelation.vrel_any_idb(d.rels[i].vrel) != 0) "idb" else "edb");
                a = 1;
                while (a <= MAX_VAR_ARITY) : (a += 1) {
                    var vname: [384:0]u8 = undefined;
                    const vr = vrelation.vrel_variant_or_null(d.rels[i].vrel, a);
                    if (vr == null) continue;
                    if (snprintf(&vname, 384, "%s.%d", d.rels[i].name, @as(c_int, a)) >= 384) {
                        _ = c.fclose(mf);
                        return dlPublishFail(&tmp_dir, &new_dir, renamed);
                    }
                    _ = fprintf(mf, "%s:%d:%s\n", @as([*c]const u8, @ptrCast(&vname)), @as(c_int, a), if (relation.rel_is_idb(vr) != 0) "idb" else "edb");
                    _ = snprintf(&rel_path, 4096, "%s/%s.dafsa", &tmp_dir, @as([*c]const u8, @ptrCast(&vname)));
                    stage_rev_var[i][a] = d.saved_rev_var[i][a];
                    if (relSaveGated(d, vr, &rel_path, &stage_rev_var[i][a], &prev_snap_dir) != 0) {
                        _ = c.fclose(mf);
                        return dlPublishFail(&tmp_dir, &new_dir, renamed);
                    }
                    if (pub_timers_on) {
                        const nw = nowNs();
                        pub_timers.t_rel_save_ns += nw - t_rel_mark;
                        t_rel_mark = nw;
                    }
                    if (d.fault_hook) |hook| {
                        if (hook(DL_FPOINT_AFTER_REL_SAVE, d.fault_user) != 0) {
                            _ = c.fclose(mf);
                            return dlPublishFail(&tmp_dir, &new_dir, renamed);
                        }
                    }
                }
                continue;
            }

            const arity = relation.rel_arity(d.rels[i].rel);
            _ = fprintf(mf, "%s:%d:%s\n", d.rels[i].name, @as(c_int, arity), if (relation.rel_is_idb(d.rels[i].rel) != 0) "idb" else "edb");

            _ = snprintf(&rel_path, 4096, "%s/%s.dafsa", &tmp_dir, d.rels[i].name);
            stage_rev[i] = d.saved_rev[i];
            if (relSaveGated(d, d.rels[i].rel, &rel_path, &stage_rev[i], &prev_snap_dir) != 0) {
                _ = c.fclose(mf);
                return dlPublishFail(&tmp_dir, &new_dir, renamed);
            }
            if (pub_timers_on) {
                const nw = nowNs();
                pub_timers.t_rel_save_ns += nw - t_rel_mark;
                t_rel_mark = nw;
            }

            if (d.fault_hook) |hook| {
                if (hook(DL_FPOINT_AFTER_REL_SAVE, d.fault_user) != 0) {
                    _ = c.fclose(mf);
                    return dlPublishFail(&tmp_dir, &new_dir, renamed);
                }
            }
        }

        // M6: save permutation indices
        {
            var pi: c_int = 0;
            while (pi < d.n_perms) : (pi += 1) {
                const pe = &d.perms[@intCast(pi)];
                if (pe.pidx_rel == null) continue;
                var pi_name: [128:0]u8 = undefined;
                var pi_path: [4096:0]u8 = undefined;
                const ar = pe.arity;

                _ = snprintf(&pi_name, 128, "%s__PI%x__", d.rels[@intCast(pe.rel_id)].name, pi);
                _ = fprintf(mf, "%s:%d # perm index of %s\n", @as([*c]const u8, @ptrCast(&pi_name)), @as(c_int, ar), d.rels[@intCast(pe.rel_id)].name);

                _ = snprintf(&pi_path, 4096, "%s/%s.dafsa", &tmp_dir, @as([*c]const u8, @ptrCast(&pi_name)));
                stage_rev_perm[@intCast(pi)] = d.saved_rev_perm[@intCast(pi)];
                if (permSaveGated(d, pe.pidx_rel, &pi_path, @intCast(pi), &prev_snap_dir, &stage_rev_perm, &stage_perm_rel) != 0) {
                    _ = c.fclose(mf);
                    return dlPublishFail(&tmp_dir, &new_dir, renamed);
                }
            }
        }

        if (c.fclose(mf) != 0) return dlPublishFail(&tmp_dir, &new_dir, renamed);
        if (pub_timers_on) pub_timers.t_rel_other_ns += nowNs() - t_rel_mark;
    }
    if (pub_timers_on) t_mark = nowNs();

    // 4c. fsync the tmp dir
    if (fsync_dir_path(&tmp_dir) != 0) return dlPublishFail(&tmp_dir, &new_dir, renamed);
    if (pub_timers_on) {
        const nw = nowNs();
        pub_timers.t_fsync_ns += nw - t_mark;
        t_mark = nw;
    }

    // 5. Atomic rename: .tmp → final dir
    _ = snprintf(&new_dir, 4096, "%s/snapshots/%u", d.dir.?, new_version);
    rmRf(&new_dir);
    if (posix.rename(&tmp_dir, &new_dir) != 0) return dlPublishFail(&tmp_dir, &new_dir, renamed);
    renamed = 1;
    if (pub_timers_on) {
        const nw = nowNs();
        pub_timers.t_rename_ns += nw - t_mark;
        t_mark = nw;
    }
    if (fsync_dir_path(&snapshots_dir) != 0) return dlPublishFail(&tmp_dir, &new_dir, renamed);
    if (pub_timers_on) {
        const nw = nowNs();
        pub_timers.t_fsync_ns += nw - t_mark;
        t_mark = nw;
    }

    // FAULT HOOK: after rename (before CURRENT flip)
    if (d.fault_hook) |hook| {
        if (hook(DL_FPOINT_AFTER_RENAME, d.fault_user) != 0) {
            rmRf(&new_dir);
            return dlPublishFail(&tmp_dir, &new_dir, renamed);
        }
    }

    // 6. Atomic CURRENT flip
    _ = snprintf(&current_path, 4096, "%s/snapshots/CURRENT", d.dir.?);
    _ = snprintf(&buf, 4096, "%u\n", new_version);
    if (atomic_write_str(&current_path, &buf) != 0)
        return dlPublishFail(&tmp_dir, &new_dir, renamed);
    if (pub_timers_on) {
        const nw = nowNs();
        pub_timers.t_current_ns += nw - t_mark;
        t_mark = nw;
    }

    // 7. Invalidate cache + update snap_version
    vcache_invalidate(&d.vcache);
    d.snap_version = new_version;

    // 7b. Commit the publish-gate watermarks.  The publish is durable NOW
    // (rename + CURRENT flip both succeeded), so — and only so — the
    // staged revs may become the live "already serialized into the
    // previous snapshot" claims.  Slots never offered a save this publish
    // keep their sentinel (absent variant / null pidx_rel) and so force a
    // full save if they ever appear later; the perm identity array commits
    // the same way (its null entries pair with the sentinel revs).
    d.saved_rev = stage_rev;
    d.saved_rev_var = stage_rev_var;
    d.saved_rev_perm = stage_rev_perm;
    d.saved_perm_rel = stage_perm_rel;

    // 8. Opt-in retention
    if (d.snapshot_retain > 0)
        pruneSnapshots(d);

    if (pub_timers_on) {
        pub_timers.t_prune_ns += nowNs() - t_mark;
    }

    // Reset the WAL-append watermark: the snapshot just materialized
    // includes everything appended up to this consolidation, so the next
    // budget window counts only traffic since THIS materialization.
    d.wal_appended_at_snapshot = sumRelWalAppended(d);

    return 0;
}

/// Atomic publish (M4): consolidate + ALWAYS materialize a new versioned
/// snapshot (the explicit checkpoint).  This is the unchanged per-call
/// semantic that tests and the binding rely on; dl_consolidate is the
/// cheaper periodic path that gates the materialization on the WAL budget.
pub export fn dl_publish_snapshot(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1;
    return publishFull(d);
}

/// The publish body, shared by dl_publish_snapshot (which always reaches it)
/// and dl_publish_if_needed (which reaches it only after its early-out
/// declined): consolidate, then ALWAYS materialize a new version.
fn publishFull(d: *DlDb) c_int {
    pubTimersCheckEnv();
    const t_start = nowNs();

    const t_cons = nowNs();
    {
        const m = consolidate(d);
        if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
    }
    return finishPublish(d, t_start, t_cons);
}

/// The half of a publish that follows consolidation: materialize the new
/// version (and report the split when DL_PUB_TIMING asked for it).
fn finishPublish(d: *DlDb, t_start: u64, t_cons: u64) c_int {
    const t_mat = nowNs();
    if (materializeSnapshot(d) != 0) return -1;
    if (pub_timers_on) {
        // DL_PUB_TIMING asked for timings: report this publish's split so the
        // counters are readable without a dl_publish_timing_get() caller.
        dlErr("pubtiming: consolidate={d:.1}ms materialize={d:.1}ms | intern={d:.1} terms={d:.1} rel_save={d:.1} rel_other={d:.1} fsync={d:.1} rename={d:.1} current={d:.1} prune={d:.1} ivm={d:.1}\n", .{
            nsToMs(t_mat - t_cons),           nsToMs(nowNs() - t_mat),
            nsToMs(pub_timers.t_intern_ns),   nsToMs(pub_timers.t_terms_ns),
            nsToMs(pub_timers.t_rel_save_ns), nsToMs(pub_timers.t_rel_other_ns),
            nsToMs(pub_timers.t_fsync_ns),    nsToMs(pub_timers.t_rename_ns),
            nsToMs(pub_timers.t_current_ns),  nsToMs(pub_timers.t_prune_ns),
            nsToMs(pub_timers.t_ivm_ns),
        });
    }

    if (pub_timers_on) {
        pub_timers.t_total_ns += nowNs() - t_start;
        pub_timers.n_publishes += 1;
    }
    return 0;
}

// ─── "Already current" publish early-out ────────────────────────────────────

/// Do two relation files hold the same content?  O(1), and the answer this gate
/// needs when the live file and the snapshot's copy are separate INODES holding
/// the same bytes — the normal shape after a publish from memory (the publish
/// writes <snapshot>/<rel>.dafsa, dl_close then rewrites the live path: same
/// content, two inodes).
///
/// Proof: equal size, both CRC-bearing format (v4 or v5: 'PDWG' + version >= 4),
/// and the same trailing CRC32 — the checksum the format keeps over EVERY
/// preceding byte.
/// dafsa_save renumbers states in canonical BFS order, so equal content
/// serializes to equal bytes and therefore to an equal trailer, and that
/// trailer is the same integrity check dafsa_load itself trusts (a file failing
/// it is rejected as corrupt).  A legacy pre-v4 file (no trailer) and any read
/// failure answer false — publish.
///
/// (A byte-for-byte memcmp — filesIdentical — would drop the CRC's 2^-32
/// accidental-collision bound, but it reads the whole store and so costs what
/// the early-out exists to save.)
fn relFilesSameContent(a: [*c]const u8, b: [*c]const u8, size: i64) bool {
    if (size < 12) return false; // header (8) + trailer (4)
    var ta: [4]u8 = undefined;
    var tb: [4]u8 = undefined;
    if (!relFileCrc(a, size, &ta)) return false;
    if (!relFileCrc(b, size, &tb)) return false;
    return ta[0] == tb[0] and ta[1] == tb[1] and ta[2] == tb[2] and ta[3] == tb[3];
}

// fseek is not in the musl @cImport surface dl.zig gets; declared directly
// (the same reason the other libc calls in this file are).
extern "c" fn fseek(stream: *c.FILE, off: c_long, whence: c_int) c_int;
const SEEK_SET: c_int = 0;

/// Read a v4 relation file's identity head and its trailing CRC32.  Returns
/// false for a foreign, pre-v4 or unreadable file (no trailer to compare).
fn relFileCrc(path: [*c]const u8, size: i64, crc_out: *[4]u8) bool {
    const f = c.fopen(path, "r") orelse return false;
    defer _ = c.fclose(f);
    var head: [8]u8 = undefined;
    if (c.fread(&head, 1, head.len, f) != head.len) return false;
    if (!std.mem.eql(u8, head[0..4], "PDWG")) return false;
    // v4 introduced the trailing CRC32 this function reads; v5 adds a
    // persisted state_off table BEFORE the CSR, so the trailer stays the last
    // 4 bytes and the comparison remains valid.  Anything older has no trailer.
    const ver = std.mem.readInt(u32, head[4..8], .little);
    if (ver < 4) return false;
    if (fseek(f, @intCast(size - 4), SEEK_SET) != 0) return false;
    if (c.fread(crc_out, 1, 4, f) != 4) return false;
    return true;
}

/// Is this relation's content the same BYTES the snapshot holds for it?
/// Serializes it to a scratch path in the db dir and compares size + the
/// format's own trailing CRC32 (relFilesSameContent) — the same comparison
/// relSaveGated makes when a relation's live file and the snapshot's copy are
/// separate inodes.  Costs one dafsa_save of THAT relation and nothing else (no
/// fsync, no rename into a version dir, no interner work), and the gate pays it
/// only for a relation whose content memory holds rather than disk: an .owned
/// one (the VM rewrote it) or one whose live file was never written.
///
/// The scratch file is unlinked on every path.  Its name is derived from the
/// relation, which is unique inside the writer lock dl_open holds for a
/// read-write handle, so two processes cannot collide on it.
///
/// Conservative on every doubt — serialize failure, size mismatch, a pre-v4 or
/// unreadable file — the caller publishes.
fn relContentIsInSnapshot(
    d: *DlDb,
    r: *relation.Relation,
    name: [*c]const u8,
    spath: [*c]const u8,
    ssize: i64,
) bool {
    var scratch: [4096:0]u8 = undefined;
    const n = snprintf(&scratch, 4096, "%s/.pubgate-%s.tmp", d.dir.?, name);
    if (n < 0 or @as(usize, @intCast(n)) >= 4096) return false;
    const rc = relation.rel_save(r, &scratch);
    defer _ = posix.unlink(&scratch);
    if (rc != 0) return false;
    var st: Stat = undefined;
    if (stat(&scratch, &st) != 0) return false;
    if (st.st_size != ssize) return false;
    return relFilesSameContent(&scratch, spath, ssize);
}

/// Is the store already represented by the CURRENT snapshot — i.e. would a
/// publish right now create a version whose every file is a hardlink (or an
/// identical re-serialization) of what CURRENT already holds?
///
/// CONSERVATIVE BY CONSTRUCTION: every check is O(1) or O(#relations), and any
/// doubt answers NO.  An unnecessary publish costs time; a skipped NEEDED
/// publish costs correctness — so each check asks "can I PROVE nothing is
/// pending", never "does it look unchanged":
///
///   1. fixpoint_dirty — the VM still owes a materialization.  This gate is
///      only consulted AFTER consolidation (dl_publish_if_needed runs it
///      first), so the flag is 0 once the run happened and the check only
///      catches a run that was skipped.  Only meaningful with a compiled
///      programme: dl_add_fact sets the flag unconditionally, but consolidate
///      clears it only inside its own `n_crules > 0` guard, so a rule-free
///      store keeps it set forever and has nothing to derive.
///   2. meta_dirty — rels.txt (declarations + recorded column kinds) changed.
///      A publish is what writes the snapshot's manifest, so the snapshot would
///      describe a schema the store no longer has.
///   3. txn — buffered transaction ops are not applied yet (publish refuses an
///      open txn for the same reason).
///   4. snap_version != 0 AND CURRENT still names it.  saved_rev is a
///      PER-PROCESS watermark, so "owned content is published" can only be
///      proven against the version THIS process published.  The store is
///      multi-writer: another process publishing since moves CURRENT and
///      leaves our watermarks describing an older version.
///   5. the interner needs no save: intern_is_dirty == 0 (the root
///      <db>/symbols.array does not lag the live interner) AND the live symbol
///      count equals the count the CURRENT snapshot's manifest records.  The
///      interner is APPEND-ONLY (no path rewrites an existing entry), so count
///      equality means no symbol was interned since the snapshot — the whole
///      question in O(1) (one small manifest read + one counter).  The stamp's
///      128-bit content hash is deliberately NOT consulted: it exists to prove
///      byte identity for a HARD LINK decision, and a skipped publish makes no
///      such decision (nothing is written).
///   6. term_is_dirty == 0 — no list node was created since the last save, so
///      terms.bin needs no rewrite.
///   7. every relation's content is already in the snapshot, proven per state:
///        * .deferred/.view — never mutated in this process, so its content IS
///          the live <db>/<rel>.dafsa (rel_content_file's contract, the
///          cross-process half of the publish gate).  That file must hold the
///          same bytes as the snapshot's copy (relFilesSameContent: size + the
///          format's own trailing CRC32, so the two may be separate inodes —
///          which is the normal shape after a publish from memory).  Equal
///          content is the proof a later save cannot have replaced it: a save
///          commits by tmp+rename, so the live PATH gets a new inode and the
///          snapshot keeps the old bytes.  Unequal content means a writer that
///          never published — publish.
///        * .owned — content exists only in memory.  The revision watermark
///          (saved_rev, the rev relSaveGated commits at publish step 7b, i.e.
///          only after the rename AND the CURRENT flip succeeded) proves it
///          for free while it matches.  It no longer can on its own: a VM run
///          resets every head's view and bumps its rev even when it re-derives
///          the very same tuples, and a FRESH process has no watermark at all
///          (saved_rev is per-process, re-armed to the sentinel at open) — and
///          a fresh process is exactly what `dl query` is.  So when the rev
///          does not match, prove the CONTENT: serialize the relation and
///          compare bytes with the snapshot's copy (relContentIsInSnapshot).  A
///          mutation or a real derivation moves the bytes and disqualifies;
///          a rewrite of identical tuples does not, which is the whole point.
///        * a .deferred/.view relation whose live file does not exist — nothing
///          was ever written for it, so its content is EMPTY; the same content
///          proof decides (an empty relation against the snapshot's copy, which
///          is what a derived relation whose rows live only in snapshots looks
///          like).  Its state is NOT required to be .owned: the empty content
///          is the fact being compared, not where it lives.
///   8. no variadic relations and no permutation indices: both carry their own
///      watermark arrays (saved_rev_var / saved_rev_perm, plus object identity
///      for a rebuilt index) that this check does not re-derive.  Skip the
///      early-out for them rather than re-implement permSaveGated.
///   9. the snapshot directory must actually carry its vocabulary, term store
///      and every relation file — a pruned, truncated or copied-over version
///      dir is not "current" even when the store side looks clean.
///
/// DELIBERATELY NOT CHECKED — durability.  A skipped publish writes nothing, so
/// there is no fsync obligation to satisfy: the WAL, the root arrays and the
/// snapshot files are already exactly the state being described.  Every write
/// path that WOULD need a fsync (a fact, a symbol, a list node, a declaration)
/// trips one of the checks above first.

/// Optional diagnostic (DL_PUB_GATE=1): name the check that declined the
/// early-out, so "why did this publish" is answerable without a rebuild.
/// One getenv per process; the reason strings are compile-time constants.
var pub_gate_debug_on: bool = false;
var pub_gate_env_checked: bool = false;

fn gateDecline(comptime why: []const u8) bool {
    return gateDeclineRel(why, null);
}

fn gateDeclineRel(comptime why: []const u8, rel_name: [*c]const u8) bool {
    if (!pub_gate_env_checked) {
        pub_gate_env_checked = true;
        const v = getenv("DL_PUB_GATE");
        pub_gate_debug_on = (v != null and v.?[0] != 0 and !strEq(v.?, "0"));
    }
    if (pub_gate_debug_on) {
        if (rel_name == null) dlErr("pubgate: publish ({s})\n", .{why}) else dlErr("pubgate: publish ({s}: {s})\n", .{ why, rel_name });
    }
    return false;
}

fn storeIsCurrent(d: *DlDb) bool {
    if (d.n_crules > 0 and d.fixpoint_dirty != 0) return gateDecline("vm still owes a run"); // 1
    if (d.meta_dirty != 0) return gateDecline("rels.txt stale"); // 2
    if (d.txn != null) return gateDecline("open txn"); // 3
    if (d.snap_version == 0 or d.dir == null) return gateDecline("no snapshot"); // 4
    if (snapshot_read_current(d.dir.?) != d.snap_version) return gateDecline("CURRENT moved"); // 4
    if (d.n_perms != 0) return gateDecline("permutation indices"); // 8

    var sdir: [4096:0]u8 = undefined;
    _ = snprintf(&sdir, 4096, "%s/snapshots/%u", d.dir.?, d.snap_version);

    if (intern_is_dirty(d.ir) != 0) return gateDecline("interner dirty"); // 5
    var stamp: SymStampOnDisk = undefined;
    if (!readSymStamp(&sdir, &stamp)) return gateDecline("no symbols stamp"); // 5: cannot prove
    if (intern_mod.liveSymBound(d.ir) -% 1 != stamp.count) return gateDecline("new symbols"); // 5
    if (term_is_dirty(d.terms) != 0) return gateDecline("terms dirty"); // 6

    var i: usize = 0;
    while (i < d.nrels) : (i += 1) { // 7
        const name = d.rels[i].name;
        if (d.rels[i].kind == RELK_VARIADIC) return gateDeclineRel("variadic relation", name); // 8
        const r = d.rels[i].rel orelse return gateDeclineRel("null relation", name);
        var spath: [4096:0]u8 = undefined;
        _ = snprintf(&spath, 4096, "%s/%s.dafsa", &sdir, name);
        var ss: Stat = undefined;
        if (stat(&spath, &ss) != 0) return gateDeclineRel("relation missing from snapshot", name); // 9
        var require_owned = true;
        if (relation.rel_content_file(r)) |live| {
            var ls: Stat = undefined;
            if (stat(live, &ls) == 0) {
                if (ls.st_size != ss.st_size or !relFilesSameContent(live, &spath, ss.st_size))
                    return gateDeclineRel("live content differs", name);
                continue;
            }
            // No live file at all: nothing was ever written for this relation,
            // so its content is EMPTY and the snapshot's copy is the only
            // candidate.  Fall through to the content proof, which serializes
            // the (empty) relation — relEnsureOwned creates an empty DAFSA for
            // a missing path — and compares bytes.  A derived relation whose
            // rows live only in snapshots is the normal shape of this case, and
            // it is why the state check below does not apply to it.
            require_owned = false;
        }
        if (require_owned and relation.rel_read_state(r) != @intFromEnum(relation.ReadState.owned))
            return gateDeclineRel("relation state", name);
        // The rev watermark proves it while it matches (this process published
        // this content); otherwise the CONTENT has to prove it.
        if (d.saved_rev[i] != SAVED_REV_NONE and relation.rel_rev(r) == d.saved_rev[i]) continue;
        if (!relContentIsInSnapshot(d, r, name, &spath, ss.st_size)) return gateDeclineRel("content differs", name);
    }

    var arr: [4096:0]u8 = undefined;
    _ = snprintf(&arr, 4096, "%s/symbols.array", &sdir);
    var terms: [4096:0]u8 = undefined;
    _ = snprintf(&terms, 4096, "%s/terms.bin", &sdir);
    var st: Stat = undefined;
    if (stat(&arr, &st) != 0) return gateDecline("no symbols.array"); // 9
    if (stat(&terms, &st) != 0) return gateDecline("no terms.bin"); // 9
    return true;
}

/// Publish only if the store is not already current — the no-op publish
/// early-out.  Returns 0 in BOTH the skipped and the published case, and -1 on
/// error (NULL db, read-only handle, open txn, or a consolidation /
/// materialization failure; DL_E_FRAMES propagates exactly as
/// dl_publish_snapshot's does).
///
/// When the early-out declines, this is byte-for-byte dl_publish_snapshot: a
/// new version, the same durability, the same retention prune.  When it fires,
/// NO version is created and no snapshot file is written — the store is
/// provably the state CURRENT already describes (see storeIsCurrent).
/// Consolidation still ran (it is the maintenance point this gate sits behind,
/// and the VM run it may perform is what makes the question answerable), so
/// the skipped path keeps consolidation's own writes: WAL fsyncs, overlay
/// folds and decay.  Retention is not a publish, so a requested prune
/// (dl_set_snapshot_retain / --keep N) still happens on the skipped path.
///
/// THE QUERY PATH: `dl query` loads rules (which sets fixpoint_dirty), so the
/// gate is only reachable when it judges the store AFTER that run — a gate on
/// "nothing was loaded" could never fire there.  The run itself is not
/// evidence: a full re-evaluation resets every rule head's view (bumping its
/// revision) and re-adds the same tuples, so "the VM ran" and "a revision
/// moved" both stay true when nothing was derived.  What the gate asks
/// instead is the ground truth a publish would otherwise write: is every
/// relation's CONTENT — byte for byte, the format's own CRC — already in
/// CURRENT's snapshot?  Derived relations are monotone here (rel_reset_view
/// splits base from the view and rel_build_from_tupleset only ever unions
/// into it), so a run that derives nothing new leaves the bytes untouched.
///
/// dl_publish_snapshot keeps its own contract: it ALWAYS creates a version
/// (v1, v2, ...), which the C oracle and every explicit checkpoint rely on.
pub export fn dl_publish_if_needed(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1;

    pubTimersCheckEnv();
    const t_start = nowNs();
    const t_cons = nowNs();
    {
        // The VM run a dirty fixpoint owes is NOT a publish.  Consolidation is
        // the maintenance point publishFull runs first anyway, so doing it
        // here leaves the published path byte-for-byte what it was — and it
        // lets the gate judge the store AFTER the derivation (which is the
        // only point at which "nothing new was derived" is answerable).
        const m = consolidate(d);
        if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
    }

    if (storeIsCurrent(d)) {
        if (d.snapshot_retain > 0) pruneSnapshots(d);
        return 0;
    }
    return finishPublish(d, t_start, t_cons);
}

/// Periodic consolidation (selfreg periodic snapshot): the per-turn
/// maintenance point.  Consolidates (sync WAL + flush overlay + decay + IVM)
/// every call, and materializes a NEW snapshot only when the accumulated
/// WAL-append bytes since the last materialization exceed the budget
/// (default 64 MiB via DL_SNAPSHOT_BUDGET_MB, or dl_set_snapshot_budget;
/// 0 disables auto-materialization entirely).  This makes the snapshot
/// PERIODIC (budget-triggered at consolidation-time) instead of per-publish:
/// the expensive O(store) serialize + fsyncs are paid once per W-byte
/// budget, while every turn pays only the O(delta) consolidation.
pub export fn dl_consolidate(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1;

    {
        const m = consolidate(d);
        if (m != 0) return m; // DL_E_FRAMES (-2) propagates (dl.h)
    }

    if (d.snapshot_budget_bytes > 0) {
        const pending = sumRelWalAppended(d) -| d.wal_appended_at_snapshot;
        if (pending >= d.snapshot_budget_bytes) {
            if (materializeSnapshot(d) != 0) return -1;
        }
    }
    return 0;
}

/// Set the WAL-append byte budget that triggers a snapshot materialization
/// inside dl_consolidate.  0 disables auto-materialization — dl_consolidate
/// then only consolidates and never snapshots (the pure "in-memory with
/// sync" mode).  The default (set at open) is 64 MiB / DL_SNAPSHOT_BUDGET_MB.
/// Returns 0 on success, -1 on a NULL db.
pub export fn dl_set_snapshot_budget(db: ?*DlDb, bytes: u64) c_int {
    const d = db orelse return -1;
    d.snapshot_budget_bytes = bytes;
    return 0;
}

/// Best-effort publish failure cleanup: rm_rf(tmp) and, if renamed, rm_rf(new).
fn dlPublishFail(tmp_dir: [*c]const u8, new_dir: [*c]const u8, renamed: c_int) c_int {
    rmRf(tmp_dir);
    if (renamed != 0) rmRf(new_dir);
    return -1;
}

/// Publish-side save gate (selfreg-relsave-fix): re-serializing a relation
/// whose content already exists on disk (BFS renumbering + three O(nstates)
/// allocations + full buffer build, per relation per publish) is pure waste —
/// the dominant cost of a delta publish at a large store.  Two gates decide,
/// in this order:
///
///   1. UNMUTATED (cross-process): the relation is .deferred/.view, so its
///      content IS its live <db>/<rel>.dafsa file — hardlink that file.
///      This is the only gate a fresh process can apply, and the one that
///      makes a no-op publish O(#relations) instead of O(store).
///   2. UNCHANGED IN THIS PROCESS: the content revision equals the revision
///      already serialized into the previous snapshot — hardlink the previous
///      snapshot's file.
///
/// Snapshot files are immutable once renamed into place (only rmRf unlinks
/// them, and unlinking one hardlink never disturbs the other), so a link is
/// exactly the bytes rel_save would have produced for the same content.
/// Falls back to a full rel_save whenever neither gate applies (a mutated
/// relation, WAL-replayed content, a fresh vocabulary) or a hardlink fails
/// (previous snapshot pruned, no live file yet, cross-device, EMLINK).
/// `saved_rev` points at the caller's STAGED slot (pre-seeded from the live
/// watermark): the decision reads the last durable value, the write is
/// committed into d.saved_rev* only if the publish succeeds (see
/// dl_publish_snapshot step 7b).  Returns 0 on success (file present at
/// dst), -1 on error.
fn relSaveGated(
    d: *DlDb,
    rel: ?*relation.Relation,
    dst: [*c]const u8,
    saved_rev: *u64,
    prev_snap_dir: [*c]const u8,
) c_int {
    // CROSS-PROCESS gate: an UNMUTATED relation's content IS its live
    // <db>/<rel>.dafsa file (rel_content_file's contract: .deferred/.view
    // means never mutated, no WAL records pending, nothing read from anywhere
    // else), so the file can be hardlinked into the snapshot — O(1), zero
    // bytes read.  This is the only gate that survives a fresh process: rev /
    // saved_rev is a PER-PROCESS counter re-armed to the sentinel on every
    // open (dl_open), so on its own it re-serialized EVERY relation of the
    // agent store on the first publish of every invocation (MEASURED: 26
    // relations, 61,878,035 bytes, 1.44 s of rel_save per fresh process).
    //
    // Content equivalence with the rel_save it replaces: rel_save on a
    // non-owned relation promotes (relEnsureOwned) — which loads the SAME live
    // path — and then serializes; dafsa_save's BFS renumbering is canonical,
    // so load-then-save reproduces the file byte for byte (MEASURED on the
    // agent store: 26/26 live relation files identical to their snapshot's
    // re-serialization).
    //
    // Safety of the link: dafsa_save commits via tmp + rename (never an
    // in-place write), so a later save of this relation replaces the live
    // PATH, leaving this inode — and every snapshot that links it — untouched.
    if (relation.rel_content_file(rel)) |live| {
        if (posix.link(live, dst) == 0) return 0;
        // No live file (relation created in this process) or a link failure
        // (cross-device, EMLINK, ENOENT): serialize as before.
    }

    const rev = relation.rel_rev(rel);

    if (rev != saved_rev.* or d.snap_version == 0) {
        if (relation.rel_save(rel, dst) != 0) return -1;
        saved_rev.* = rev;
        return 0;
    }

    // Unchanged since the previous snapshot: link prev's file.  dst's
    // basename is the same as prev's (both <name>.dafsa inside their
    // snapshot dir).
    {
        var s: usize = 0;
        var i: usize = 0;
        while (dst[s] != 0) : (s += 1) {
            if (dst[s] == '/') i = s + 1;
        }
        var src: [4096:0]u8 = undefined;
        const n = snprintf(&src, 4096, "%s/%s", prev_snap_dir, dst + i);
        if (n < 0 or @as(usize, @intCast(n)) >= 4096) return -1;
        if (posix.link(&src, dst) != 0) {
            // Fallback: serialize as before (correctness over speed).
            if (relation.rel_save(rel, dst) != 0) return -1;
        }
    }
    return 0;
}

/// Read exactly dst.len bytes from fd (short reads are normal on files).
fn readExact(fd: c_int, dst: []u8) bool {
    var got: usize = 0;
    while (got < dst.len) {
        const n = posix.read(fd, dst[got..].ptr, dst.len - got);
        if (n <= 0) return false;
        got += @intCast(n);
    }
    return true;
}

/// Byte-identity of two files: size first, then a chunked memcmp.  The
/// interner publish gate (materializeSnapshot step 4a) uses this to decide
/// whether a snapshot's symbols.dafsa — a pure derived cache, so a function of
/// symbols.array alone — can be inherited from the previous snapshot instead of
/// being rebuilt and re-serialized.  Returns false on any I/O error, which the
/// caller reads as "not identical" and falls back to the full save.
fn filesIdentical(a: [*c]const u8, b: [*c]const u8) bool {
    var sa: Stat = undefined;
    var sb: Stat = undefined;
    if (stat(a, &sa) != 0) return false;
    if (stat(b, &sb) != 0) return false;
    if (sa.st_size != sb.st_size or sa.st_size < 0) return false;

    const fa = posix.open(a, posix.O_RDONLY);
    if (fa < 0) return false;
    defer _ = posix.close(fa);
    const fb = posix.open(b, posix.O_RDONLY);
    if (fb < 0) return false;
    defer _ = posix.close(fb);

    const CHUNK: usize = 1 << 16;
    var buf_a: [CHUNK]u8 = undefined;
    var buf_b: [CHUNK]u8 = undefined;
    var left: i64 = sa.st_size;
    while (left > 0) {
        const want: usize = @intCast(@min(@as(i64, CHUNK), left));
        if (!readExact(fa, buf_a[0..want]) or !readExact(fb, buf_b[0..want])) return false;
        if (!std.mem.eql(u8, buf_a[0..want], buf_b[0..want])) return false;
        left -= @intCast(want);
    }
    return true;
}

/// What a snapshot records about its symbols.array: the interner's content
/// stamp plus the array's byte size.  `bytes` is the O(1) integrity check on
/// the FILE (the hash proves the CONTENT the live interner holds matches what
/// was recorded; the size catches a truncated or appended-to snapshot file,
/// which the count alone cannot — the count lives in the manifest, not in the
/// array).
const SymStampOnDisk = struct {
    count: u32,
    bytes: u64,
    h0: u64,
    h1: u64,
};

/// Read the symbols stamp a snapshot recorded in its manifest
/// ('D:symbols:<count>:<bytes>:<h0>:<h1>', written by materializeSnapshot
/// step 4b — a directive line every manifest walker already skips).  Returns
/// false when the line is absent (a snapshot published before the stamp
/// existed), which the caller reads as "cannot inherit, do the full save".
fn readSymStamp(sdir: [*c]const u8, out: *SymStampOnDisk) bool {
    var path: [8192:0]u8 = undefined;
    _ = snprintf(&path, 8192, "%s/manifest.txt", sdir);
    const f = c.fopen(&path, "r") orelse return false;
    defer _ = c.fclose(f);

    var line: ?[*]u8 = null;
    var cap: usize = 0;
    defer if (line) |lp| c.free(@ptrCast(lp));

    const prefix = "D:symbols:";
    var len: isize = getline(&line, &cap, f);
    while (len > 0) : (len = getline(&line, &cap, f)) {
        const buf = line.?;
        var l: usize = @intCast(len);
        while (l > 0 and (buf[l - 1] == '\n' or buf[l - 1] == '\r')) l -= 1;
        const s = buf[0..l];
        if (!std.mem.startsWith(u8, s, prefix)) continue;

        var it = std.mem.splitScalar(u8, s[prefix.len..], ':');
        const count_s = it.next() orelse return false;
        const bytes_s = it.next() orelse return false;
        const h0_s = it.next() orelse return false;
        const h1_s = it.next() orelse return false;
        out.count = std.fmt.parseInt(u32, count_s, 10) catch return false;
        out.bytes = std.fmt.parseInt(u64, bytes_s, 10) catch return false;
        out.h0 = std.fmt.parseInt(u64, h0_s, 16) catch return false;
        out.h1 = std.fmt.parseInt(u64, h1_s, 16) catch return false;
        return true;
    }
    return false;
}

/// Symbols-array publish gate (3dd4ae0/81f5581 removed the derived DAFSA and
/// the gated vocabulary save; this removes the remaining fixed cost, the
/// unconditional rewrite of the whole array).
///
/// The interner's rev[] is APPEND-ONLY (see intern.zig's module doc: only new
/// ids are written, entries are never rewritten or freed before destroy), so
/// for any given (count, content) the bytes intern_save would write are a pure
/// function — and intern.zig maintains that function's 128-bit hash
/// INCREMENTALLY as a running stamp over bytes it has already appended.
///
/// When the previous snapshot recorded the SAME stamp, its symbols.array holds
/// exactly the byte stream intern_save would produce now, so it is hardlinked
/// into the staged snapshot instead of rewritten.  MEASURED (88,000 symbols /
/// 44 MB array, ReleaseFast, page-warm): step 4a went 138 ms ->
/// 26 ms — and that 26 ms is the ONE content-hash pass the stamp needs, paid
/// at most once per process; the decision itself is O(1) (a manifest read
/// and a link).  The count compare short-circuits BEFORE the hash, so a
/// vocabulary-changing publish never pays it for the decision.
///
/// Why a hash and not the count alone: the live rev[] is loaded from the db
/// ROOT symbols.array (dlOpenCommon), not from the previous SNAPSHOT's copy, so
/// equal counts do not by themselves prove equal content — a store whose two
/// copies diverged (restored/copied/tampered root array) would then silently
/// publish a snapshot with the WRONG bytes at the RIGHT count, and every
/// symbol-id lookup in it would be wrong.  The hash is computed from the live
/// content, so it sees that divergence and falls back to the full write.
///
/// Returns true when the link is in place (caller skips intern_save); false
/// means "no previous stamp, stamp mismatch, or link failure — write it".
fn inheritSymbolsArray(ir: ?*anyopaque, rev: [*c]const u8, prev_snap_dir: [*c]const u8) bool {
    if (prev_snap_dir[0] == 0) return false;
    var prev: SymStampOnDisk = undefined;
    if (!readSymStamp(prev_snap_dir, &prev)) return false;
    // Cheap key first: a vocabulary change moves the count, and then the answer
    // is "write it" regardless of content — so the content hash (one pass over
    // the whole array, the first time a process publishes) is never paid on the
    // path that is about to rewrite the array anyway.
    if (intern_mod.liveSymBound(ir) -% 1 != prev.count) return false;
    // The file must be exactly the array the manifest recorded (O(1) stat):
    // a truncated or padded snapshot file is never inherited.
    var prev_rev: [4096:0]u8 = undefined;
    _ = snprintf(&prev_rev, 4096, "%s/symbols.array", prev_snap_dir);
    var stst: Stat = undefined;
    if (stat(&prev_rev, &stst) != 0 or stst.st_size < 0 or @as(u64, @intCast(stst.st_size)) != prev.bytes)
        return false;
    const live = intern_mod.symStamp(ir);
    if (prev.h0 != live.h0 or prev.h1 != live.h1) return false;
    return posix.link(&prev_rev, rev) == 0;
}

/// Byte size of a file, or null when it cannot be stat'd.
fn fileSize(path: [*c]const u8) ?u64 {
    var st: Stat = undefined;
    if (stat(path, &st) != 0 or st.st_size < 0) return null;
    return @intCast(st.st_size);
}

/// Perm-index variant of the gate: the index relation is freed and
/// re-created on rebuild, so besides the revision watermark the object
/// identity must match — a fresh object with a coincidentally-equal rev is
/// always re-serialized.  Both the rev watermark and the recorded identity
/// are staged (committed with the rest at publish success).
fn permSaveGated(
    d: *DlDb,
    rel: ?*relation.Relation,
    dst: [*c]const u8,
    pi: usize,
    prev_snap_dir: [*c]const u8,
    stage_rev_perm: *[MAX_PERMS]u64,
    stage_perm_rel: *[MAX_PERMS]?*anyopaque,
) c_int {
    const ident: ?*anyopaque = @ptrCast(rel);
    if (rel == null or ident != d.saved_perm_rel[pi]) {
        // The object changed since the last publish: the watermark may be
        // STALE in a way rev cannot see.  A rebuilt index restarts its rev
        // counter at 0, so the fresh object's rev can coincide with the
        // value recorded for the OLD object (glibc tcache can also hand
        // the new pidx_rel the freed chunk's address, defeating the
        // identity check — the rev CARRY in permindex_build closes that
        // side; this neutralization is defense-in-depth).  Delegating with
        // the staged-as-live watermark would let relSaveGated's own
        // (rev == saved_rev) check pass and hardlink the PREVIOUS
        // snapshot's bytes while the index content differs.  Neutralize it
        // first: the sentinel forces a full re-serialize.
        stage_rev_perm[pi] = SAVED_REV_NONE;
        const rc = relSaveGated(d, rel, dst, &stage_rev_perm[pi], prev_snap_dir);
        if (rc == 0) stage_perm_rel[pi] = ident;
        return rc;
    }
    return relSaveGated(d, rel, dst, &stage_rev_perm[pi], prev_snap_dir);
}

// ─── M6: Permutation index API ─────────────────────────────────────────────

pub export fn dl_db_declare_perm(db: ?*DlDb, rel_id: c_int, arity: u8, perm: [*c]const u8) c_int {
    const d = db orelse return -1;
    if (rel_id < 0 or arity == 0 or arity > 8) return -1;
    if (d.n_perms >= MAX_PERMS) return -1;

    // Validate perm is a bijection of {0..arity-1}.
    {
        var seen: [8]u8 = [_]u8{0} ** 8;
        var pi: u8 = 0;
        while (pi < arity) : (pi += 1) {
            if (perm[pi] >= arity or seen[perm[pi]] != 0) return -1;
            seen[perm[pi]] = 1;
        }
    }

    {
        const existing = dl_db_find_perm(d, rel_id, arity, perm);
        if (existing >= 0) return existing;
    }

    const i: c_int = d.n_perms;
    d.n_perms += 1;
    d.perms[@intCast(i)].rel_id = rel_id;
    d.perms[@intCast(i)].arity = arity;
    @memcpy(d.perms[@intCast(i)].perm[0..8], perm[0..8]);
    d.perms[@intCast(i)].pidx_rel = null;
    d.perms[@intCast(i)].dirty = 1;
    return i;
}

pub export fn dl_db_find_perm(db: ?*DlDb, rel_id: c_int, arity: u8, perm: [*c]const u8) c_int {
    const d = db orelse return -1;
    if (rel_id < 0 or arity == 0 or arity > 8) return -1;

    var i: c_int = 0;
    while (i < d.n_perms) : (i += 1) {
        if (d.perms[@intCast(i)].rel_id == rel_id and
            d.perms[@intCast(i)].arity == arity and
            std.mem.eql(u8, d.perms[@intCast(i)].perm[0..arity], perm[0..arity]))
            return i;
    }
    return -1;
}

pub export fn dl_db_get_perm(db: ?*DlDb, rel_id: c_int, perm_id: c_int) ?[*]const u8 {
    const d = db orelse return null;
    if (perm_id < 0 or perm_id >= d.n_perms) return null;
    if (d.perms[@intCast(perm_id)].rel_id != rel_id) return null;
    return &d.perms[@intCast(perm_id)].perm;
}

pub export fn dl_db_get_perm_rel(db: ?*DlDb, rel_id: c_int, perm_id: c_int) ?*relation.Relation {
    const d = db orelse return null;
    if (perm_id < 0 or perm_id >= d.n_perms) return null;
    if (d.perms[@intCast(perm_id)].rel_id != rel_id) return null;
    return d.perms[@intCast(perm_id)].pidx_rel;
}

pub export fn dl_db_perm_count(db: ?*const DlDb) c_int {
    const d = db orelse return 0;
    return d.n_perms;
}

// ─── Fault hook registration ───────────────────────────────────────────────

pub export fn dl_set_fault_hook(db: ?*DlDb, hook: FaultHook, user: ?*anyopaque) void {
    const d = db orelse return;
    d.fault_hook = hook;
    d.fault_user = user;
}

// ─── Regex pattern query ────────────────────────────────────────────────────

fn symsetAddCb(sym_id: u32, user: ?*anyopaque) callconv(.c) c_int {
    const set: *regexwalk.sym_set = @ptrCast(@alignCast(user.?));
    return regexwalk.symset_add(set, sym_id);
}

/// Load (or reuse) the interner that answers for snapshot `version`'s OWN
/// <snapshots/version>/symbols.array — THE SNAPSHOT'S VOCABULARY.  Cached in
/// DlDb per version (snap_sym_ir / snap_sym_version: freed by dl_close,
/// replaced when a different version is asked for), so a process pays the
/// array read once per version it reads.
///
/// TWO READERS SHARE IT, which is why it is one function: the VERSION pattern
/// path (snapshotSymSet, which scans this table with the compiled regex DFA)
/// and VERSION term interning (dl_intern_str_version).  Both need the mapping
/// "id → the string THIS snapshot means by it", and neither may ASSUME the live
/// interner — sym ids are reused across a publish, so the two maps can name
/// different strings at the same id.
///
/// WHAT MAKES THE LIVE INTERNER USABLE ANYWAY.  When the snapshot's own
/// manifest stamp and the live interner's content stamp agree on all of
/// (count, bytes, hash), the snapshot's symbols.array holds exactly the bytes
/// the live interner holds — so borrowing it is not an assumption, it is the
/// same file content.  That is the whole reason intern_load maintains the stamp
/// as it parses (intern.zig SymHash): the comparison is O(1) here, and the
/// alternative is parsing the same 49.5 MB array a second time (MEASURED
/// 22.8 ms in `dl query` on the agent store — the largest remaining term).
///
/// The COUNT ALONE IS NOT ENOUGH, and that is the shape a previous review
/// proved reachable: a restored or hand-edited db-ROOT symbols.array has the
/// same count with different bytes, and the live interner is loaded from the
/// ROOT array, not from the snapshot's copy.  Gating on the count would then
/// hand a version read the WRONG map and answer with the right rows under the
/// wrong names — exit 0, silently wrong.  The 128-bit hash is what sees that
/// divergence; `bytes` is the O(1) check on the FILE, and the manifest stamp is
/// the snapshot's own record of what its array contains (the same record
/// inheritSymbolsArray trusts to hardlink it into the next publish).
///
/// Returns null when the snapshot's array is missing, unreadable, a directory,
/// or DISAGREES with the size/count the snapshot's own manifest recorded for
/// it; every caller turns that into an explicit ERROR.  A damaged snapshot is
/// not an empty vocabulary, and it is not a licence to fall back to the live
/// interner — that fallback IS the defect (see snapshotSymSet).
fn snapshotInterner(d: *DlDb, version: u32) ?*anyopaque {
    if (d.snap_sym_ir != null and d.snap_sym_version == version) return d.snap_sym_ir;
    if (d.snap_sym_ir) |old| {
        if (!d.snap_sym_shared) intern_free(old);
        d.snap_sym_ir = null;
    }
    d.snap_sym_version = 0;
    d.snap_sym_shared = false;
    if (d.dir == null or version == 0) return null;
    var sdir: [16384:0]u8 = undefined;
    _ = snprintf(&sdir, 16384, "%s/snapshots/%u", d.dir.?, version);
    var arr_path: [16384:0]u8 = undefined;
    _ = snprintf(&arr_path, 16384, "%s/symbols.array", &sdir);
    var ast: Stat = undefined;
    if (stat(&arr_path, &ast) != 0) return null;
    // A DIRECTORY passes the fopen probe below (open(2) accepts O_RDONLY on a
    // directory) and getline then answers EISDIR, so intern_load would hand
    // back an EMPTY interner — every term absent, every query silently empty.
    if (posix.S_ISDIR(ast.st_mode)) return null;
    // stat() says it EXISTS; prove it can be READ.  intern_load answers an
    // unopenable path with an EMPTY interner ("no file: start empty", intern.zig),
    // which would turn a permissions/IO failure into "this snapshot has no
    // symbols" — i.e. every term absent and every query silently empty.  One
    // extra open/close per version (the parse, not the open, is the cost).
    {
        const probe = c.fopen(&arr_path, "r") orelse return null;
        _ = c.fclose(probe);
    }
    // DAMAGED-ARRAY GATE (O(1), no extra pass over the file).  The snapshot's
    // own manifest records D:symbols:<count>:<bytes>:<h0>:<h1>, and
    // inheritSymbolsArray already refuses to inherit a file whose SIZE
    // disagrees with it.  Without this gate a truncated or padded array — or a
    // read that failed midway — parses into a SHORTER vocabulary: every term
    // past the cut becomes "absent" and a version query over it answers "no
    // matches" with rc 0.  That is the silent-wrong shape this module exists to
    // prevent, so it is an ERROR here.  A legacy snapshot with no stamp has
    // nothing to compare against and skips the gate.
    var stamp: SymStampOnDisk = undefined;
    const have_stamp = readSymStamp(&sdir, &stamp);
    const bytes = fileSize(&arr_path);

    // REUSE THE LIVE INTERNER when the two vocabularies are PROVABLY identical:
    // the snapshot's manifest stamp (count, bytes, hash — the record its own
    // publisher made of the array it wrote or inherited) and the live interner's
    // content stamp must agree on all three, and the array file must be exactly
    // the recorded size.  All three are required; see the count-only hazard in
    // this function's doc comment.
    //
    // Why this is sound for the ids a version read resolves: the live interner
    // was loaded from the db-ROOT symbols.array, and an interner is APPEND-ONLY
    // (intern.zig's module doc), so a live append can only ADD ids above the
    // stamp's count — never renumber one the snapshot references.  A live read
    // between two version reads therefore cannot make the borrowed map lie about
    // ids 1..count, which is the entire range a snapshot's rows use.
    if (have_stamp and bytes != null and bytes.? == stamp.bytes) {
        if (d.ir) |live| {
            const ls = intern_mod.symStamp(live);
            if (ls.count == stamp.count and ls.h0 == stamp.h0 and ls.h1 == stamp.h1) {
                d.snap_sym_ir = live;
                d.snap_sym_version = version;
                d.snap_sym_shared = true;
                return live;
            }
        }
    }

    // internLoadNoStamp: this map is only ever looked up in (never published,
    // never stamped), so it does not pay the load-time content-stamp pass.
    const ir = intern_mod.internLoadNoStamp(&arr_path) orelse return null;
    if (have_stamp) {
        if (bytes == null or bytes.? != stamp.bytes or
            intern_mod.liveSymBound(ir) -% 1 != stamp.count)
        {
            intern_free(ir);
            return null;
        }
    }
    d.snap_sym_ir = ir;
    d.snap_sym_version = version;
    d.snap_sym_shared = false;
    return ir;
}

/// Symbol set for a VERSION pattern query: scan that SNAPSHOT's own
/// symbols.array (via snapshotInterner) and match each of its names with the
/// compiled regex DFA.
///
/// NO DAFSA, IN EITHER FORM.  This used to prefer <snapshot>/symbols.dafsa
/// (mmap'd, symbols_dfa_walk_view) when a snapshot carried one, and otherwise
/// materialize a forward DAFSA from the array (intern_fwd → ensureFwd, the
/// once-per-process rebuild: MEASURED 6.3 µs/symbol here, 8.8 µs at 500k, and
/// recorded at ~60 µs/symbol on the agent store).  Both routes are gone: the
/// array IS the vocabulary, parsing it costs ~19 ms at 166k symbols
/// (MEASURED) and scanning it costs ~20-40 ms, so materializing a 0.4-3.7 GB
/// index for one query was never the cheaper half.  A snapshot's
/// symbols.dafsa is now never read here — DL_PUB_DERIVED=1 still WRITES one,
/// for a consumer that wants the file (dl_abi still exports the walkers), but
/// no read path needs it.
///
/// WHAT SOUNDNESS REQUIRES HERE.  dl_pattern filters BY SYM-ID
/// (view_filter_col): the map decides which ids are in the set, and the rows
/// decide which ids exist.  The result is therefore correct exactly when the
/// map sends every id the snapshot's rows reference to the SAME string the
/// snapshot's own array does.  It is NOT enough for the map's key set to be a
/// superset of the snapshot's:
///
///  - an id the map assigns to a DIFFERENT string keeps a row whose snapshot
///    string does not match (a false positive);
///  - an id the map is missing drops a row that does match (a false negative).
///
/// A live interner read is neither of those, because SYM-IDS ARE REUSED ACROSS
/// A PUBLISH: publish writes only <snapshot>/symbols.array and intern_save
/// clears the interner's dirty flag (intern.zig), so dl_add_fact / dl_close
/// skip the ROOT <db>/symbols.array save; a reopen then loads a SHORTER root
/// array, intern_load sets next_id = root_count + 1, and ids the snapshot
/// already assigned are free to be handed to other strings.  MEASURED before
/// this fix: `intern -> publish -> add_fact -> publish -> close` left the root
/// array 2 lines against the v3 array's 3 (id 3 = "zzz"); after reopen,
/// dl_pattern("zzz") answered 0 rows (dropping the row the snapshot holds) and
/// dl_pattern("qqq") answered 1 row (a row whose id 3 the reopened interner
/// had just reused for "qqq").  Reading the snapshot's OWN array makes both
/// impossible regardless of what the root array or the live interner holds.
///
/// materializeSnapshot now also persists the root array, so the reuse itself
/// is closed; this read is the boundary that does not depend on that
/// discipline holding (an old store, a restored/copied root array, a crashed
/// process).
///
/// Cost: one symbols.array read (MEASURED ~19 ms for 166k symbols, ~55 ms for
/// 500k) + one linear scan, cached per snapshot version (so repeated queries
/// pay it once) — and NO read at all when snapshotInterner proves the
/// snapshot's vocabulary identical to the live one and lends its interner.  A
/// process that publishes AND issues a VERSION pattern query otherwise holds
/// two symbol tables; sharing the live map UNPROVEN is exactly the assumption
/// that broke, and the manifest content stamp is what makes proving it O(1)
/// instead of a full array comparison per query.
fn snapshotSymSet(d: *DlDb, version: u32, dfa: ?*const regexwalk.regex_dfa, set: *regexwalk.sym_set) c_long {
    if (d.dir == null) return -1;
    // A snapshot without symbols.array is a damaged store, not an empty
    // vocabulary — say so instead of answering "no symbols match" from an
    // empty interner.
    const sir = snapshotInterner(d, version) orelse return -1;
    return regexwalk.symbolsScanTable(sir, intern_mod.liveSymBound(sir), dfa, symsetAddCb, set);
}

pub export fn dl_pattern(db: ?*DlDb, rel_name: [*c]const u8, col: u8, dfa: ?*const regexwalk.regex_dfa, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (rel_name == null or dfa == null or cb == null) return -1;

    // Snapshot path
    if (d.snap_version > 0) {
        var sdir: [8192:0]u8 = undefined;
        var arity: u8 = 0;
        var variadic: c_int = 0;

        _ = snprintf(&sdir, 8192, "%s/snapshots/%u", d.dir.?, d.snap_version);
        if (manifest_find_rel_ex(&sdir, rel_name, &arity, &variadic) == 0)
            return -1;

        var set: regexwalk.sym_set = std.mem.zeroes(regexwalk.sym_set);
        if (regexwalk.symset_init(&set) != 0) return -1;
        const ns = snapshotSymSet(d, d.snap_version, dfa, &set);
        if (ns < 0) {
            regexwalk.symset_free(&set);
            return -1;
        }

        if (variadic != 0) {
            var present: [MAX_VAR_ARITY + 1]u8 = undefined;
            var total: c_long = 0;
            var a: u8 = 0;
            manifest_find_variants(&sdir, rel_name, &present);
            a = 1;
            while (a <= MAX_VAR_ARITY) : (a += 1) {
                var vname: [384:0]u8 = undefined;
                if (present[a] == 0) continue;
                if (snprintf(&vname, 384, "%s.%d", rel_name, @as(c_int, a)) >= 384) {
                    regexwalk.symset_free(&set);
                    return -1;
                }
                const vv = view_open_cached(@constCast(&d.vcache), @ptrCast(&vname), d.snap_version, &sdir) orelse {
                    regexwalk.symset_free(&set);
                    return -1;
                };
                const n = view_filter_col(vv, a, col, &set, cb, user);
                if (n < 0) {
                    regexwalk.symset_free(&set);
                    return -1;
                }
                total += n;
            }
            regexwalk.symset_free(&set);
            return total;
        }

        const vv = view_open_cached(@constCast(&d.vcache), rel_name, d.snap_version, &sdir) orelse {
            regexwalk.symset_free(&set);
            return -1;
        };
        const n = view_filter_col(vv, arity, col, &set, cb, user);
        regexwalk.symset_free(&set);
        return n;
    }

    // In-memory path.  The symbol set comes from the SAME linear scan the
    // VERSION path uses (regexwalk.symbolsScanTable over the live interner):
    // no forward DAFSA is materialized for a pattern query any more, so the
    // first pattern query in a process no longer pays ensureFwd's
    // per-symbol rebuild (see symbolsScanTable's doc).
    const idx = findRel(d, rel_name);
    if (idx < 0) return -1;

    var set: regexwalk.sym_set = std.mem.zeroes(regexwalk.sym_set);
    if (regexwalk.symset_init(&set) != 0) return -1;
    const ns = regexwalk.symbolsScanTable(d.ir, intern_mod.liveSymBound(d.ir), dfa, symsetAddCb, &set);
    if (ns < 0) {
        regexwalk.symset_free(&set);
        return -1;
    }

    if (d.rels[@intCast(idx)].kind == RELK_VARIADIC) {
        const n = vrelation.vrel_filter_col(d.rels[@intCast(idx)].vrel, col, &set, cb, user);
        regexwalk.symset_free(&set);
        return n;
    }

    const n = relation.rel_filter_col(d.rels[@intCast(idx)].rel, col, &set, cb, user);
    regexwalk.symset_free(&set);
    return n;
}

// ─── Fuzzy (Levenshtein) symbol lookup ─────────────────────────────────────

/// long dl_fuzzy_lookup(dl_db *db, const char *needle, uint32_t k,
///                      dl_fuzzy_cb cb, void *user)
///
/// Byte edit-distance lookup — a BANDED LINEAR SCAN over the live symbol table
/// (the interner's reverse array), needing no index at all.
///
/// WHY NOT A DAFSA WALK (MEASURED at 3dd4ae0, 166,743-symbol store): the
/// product DFS over the symbol DAFSA is ALREADY depth-bounded by the automaton
/// itself — a Levenshtein row whose minimum entry exceeds k is DFA_DEAD, and
/// min_j dp[i][j] is non-decreasing in the row index i, so no state is alive
/// deeper than |needle| + k; the walk costs 0-2 ms and a depth bound would
/// prune nothing it has not already pruned.  The cost of `dl similar` was
/// `intern_fwd()` MATERIALIZING that DAFSA from symbols.array on first use:
/// 10-19 s (≈60 µs per symbol) against 0.4 s to open the store.  The scan
/// below is ~15 ms on the same store, so it is what makes the command fast at
/// scale; fuzzy lookup is the one symbol query that cannot use the index
/// cheaply enough to pay for building it.
///
/// The band prune is exact, not heuristic: every edit costs at least one byte
/// of length, so a name outside [m-k, m+k] cannot be within k edits.  It also
/// bounds the DP to O((m+k)·(m+k)) per surviving candidate.
pub export fn dl_fuzzy_lookup(db: ?*DlDb, needle: [*c]const u8, k: u32, cb: DlFuzzyCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (needle == null or cb == null) return -1;
    if (k > 2) return -1; // refuse loudly; lev_compile would too
    if (needle[0] == 0) return -1; // lev_compile: "lev: empty pattern"
    const m: usize = strlen(needle);
    if (m > regexwalk.LEV_PATTERN_MAX) return -1; // lev_compile: "pattern too long"

    const kk: usize = @intCast(k);
    const bound = intern_mod.liveSymBound(d.ir);
    var count: c_long = 0;
    var id: u32 = 1;
    while (id < bound) : (id += 1) {
        const name = intern_str_of(d.ir, id) orelse continue;
        const nm: [*]const u8 = @ptrCast(name);
        const nlen = strlen(nm);
        if (nlen + kk < m or nlen > m + kk) continue; // band: cannot be within k
        const dist = regexwalk.lev_distance_cap(needle, m, nm, nlen, k);
        if (dist > k) continue;
        count +%= 1;
        if (cb.?(id, name, nlen, dist, user) != 0) break;
    }
    return count;
}

// ─── Graph traversal (Tier-2) ───────────────────────────────────────────────

const NeighborCollectCtx = struct {
    neighbors: [*]u32,
    n_neighbors: *c_int,
    max_n: c_int,
};

fn neighborPush(neighbors: [*]u32, n: *c_int, max_n: c_int, node: u32) c_int {
    if (max_n <= 0) return 0;
    var i: c_int = 0;
    while (i < n.*) : (i += 1) {
        if (neighbors[@intCast(i)] == node) return 0; // already present
    }
    if (n.* < max_n) {
        neighbors[@intCast(n.*)] = node;
        n.* += 1;
    }
    return 0;
}

fn collectFwdCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ctx: *NeighborCollectCtx = @ptrCast(@alignCast(user.?));
    return neighborPush(ctx.neighbors, ctx.n_neighbors, ctx.max_n, cols.?[1]);
}

fn collectRevCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ctx: *NeighborCollectCtx = @ptrCast(@alignCast(user.?));
    return neighborPush(ctx.neighbors, ctx.n_neighbors, ctx.max_n, cols.?[1]);
}

fn isVisited(node: u32, visited: [*]const u32, max_nodes: c_int) bool {
    var i: c_int = 0;
    while (i < max_nodes) : (i += 1) {
        if (visited[@intCast(i)] == node) return true;
    }
    return false;
}

fn markVisited(node: u32, visited: [*]u32, max_nodes: c_int) void {
    var i: c_int = 0;
    while (i < max_nodes) : (i += 1) {
        if (visited[@intCast(i)] == 0) {
            visited[@intCast(i)] = node;
            return;
        }
    }
}

fn collectForward(db: *DlDb, node: u32, neighbors: [*]u32, n_neighbors: *c_int, max_n: c_int) c_int {
    const edge_idx = findRel(db, "edge");
    if (edge_idx < 0) return -1;
    if (db.rels[@intCast(edge_idx)].kind == RELK_VARIADIC) return -1;
    if (db.rels[@intCast(edge_idx)].arity != 3) return -1;
    const edge_rel = db.rels[@intCast(edge_idx)].rel orelse return -1;

    var leading = [1]u32{node};
    n_neighbors.* = 0;
    var ctx = NeighborCollectCtx{ .neighbors = neighbors, .n_neighbors = n_neighbors, .max_n = max_n };
    const rc = relation.rel_prefix(edge_rel, &leading, 1, collectFwdCb, &ctx);
    if (rc < 0) return -1;
    return 0;
}

fn collectReverse(db: *DlDb, node: u32, neighbors: [*]u32, n_neighbors: *c_int, max_n: c_int) c_int {
    const edge_idx = findRel(db, "edge");
    if (edge_idx < 0) return -1;
    if (db.rels[@intCast(edge_idx)].kind == RELK_VARIADIC) return -1;
    if (db.rels[@intCast(edge_idx)].arity != 3) return -1;

    var perm = [8]u8{ 1, 0, 2, 0, 0, 0, 0, 0 };
    const perm_id = dl_db_declare_perm(db, edge_idx, 3, &perm);
    if (perm_id < 0) return -1;

    if (db.perms[@intCast(perm_id)].dirty != 0) {
        if (permindex_build(db, edge_idx, perm_id) != 0)
            return -1;
    }

    const pidx_rel = dl_db_get_perm_rel(db, edge_idx, perm_id) orelse return -1;

    var leading = [1]u32{node};
    n_neighbors.* = 0;
    var ctx = NeighborCollectCtx{ .neighbors = neighbors, .n_neighbors = n_neighbors, .max_n = max_n };
    const rc = relation.rel_prefix(pidx_rel, &leading, 1, collectRevCb, &ctx);
    if (rc < 0) return -1;
    return 0;
}

pub export fn dl_traverse(db: ?*DlDb, start: [*c]const u8, depth: c_int, max_nodes: c_int, cb: DlTraverseCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (start == null or cb == null or depth < 1 or max_nodes < 1)
        return -1;

    var depth_v = depth;
    if (depth_v > 3) depth_v = 3;

    {
        const edge_idx = findRel(d, "edge");
        if (edge_idx < 0) return -1;
        if (d.rels[@intCast(edge_idx)].kind == RELK_VARIADIC) return -1;
        if (d.rels[@intCast(edge_idx)].arity != 3) return -1;
    }

    const start_sym = dl_intern_str_find(d, start);
    if (start_sym == 0) return 0;

    const visited = @as([*]u32, @ptrCast(@alignCast(c.calloc(@intCast(max_nodes), @sizeOf(u32)) orelse return -1)));
    const queue = @as([*]u32, @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(max_nodes)) * @sizeOf(u32)) orelse {
        c.free(@ptrCast(visited));
        return -1;
    })));
    const queue_depth = @as([*]u8, @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(max_nodes)) * @sizeOf(u8)) orelse {
        c.free(@ptrCast(visited));
        c.free(@ptrCast(queue));
        return -1;
    })));
    const fwd_buf = @as([*]u32, @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(max_nodes)) * @sizeOf(u32)) orelse {
        c.free(@ptrCast(visited));
        c.free(@ptrCast(queue));
        c.free(@ptrCast(queue_depth));
        return -1;
    })));
    const rev_buf = @as([*]u32, @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(max_nodes)) * @sizeOf(u32)) orelse {
        c.free(@ptrCast(visited));
        c.free(@ptrCast(queue));
        c.free(@ptrCast(queue_depth));
        c.free(@ptrCast(fwd_buf));
        return -1;
    })));

    var q_head: c_int = 0;
    var q_tail: c_int = 0;
    var visited_count: c_int = 0;

    visited[0] = start_sym;
    queue[@intCast(q_tail)] = start_sym;
    queue_depth[@intCast(q_tail)] = 0;
    q_tail += 1;
    visited_count += 1;

    if (cb.?(start_sym, 0, user) != 0) {
        c.free(@ptrCast(visited));
        c.free(@ptrCast(queue));
        c.free(@ptrCast(queue_depth));
        c.free(@ptrCast(fwd_buf));
        c.free(@ptrCast(rev_buf));
        return visited_count;
    }

    while (q_head < q_tail and q_tail < max_nodes) {
        const current = queue[@intCast(q_head)];
        const current_depth = queue_depth[@intCast(q_head)];
        q_head += 1;

        if (current_depth >= @as(u8, @intCast(depth_v))) continue;
        const next_depth: u8 = current_depth + 1;

        // Forward neighbors
        var n_fwd: c_int = 0;
        if (collectForward(d, current, fwd_buf, &n_fwd, max_nodes) != 0) {
            c.free(@ptrCast(visited));
            c.free(@ptrCast(queue));
            c.free(@ptrCast(queue_depth));
            c.free(@ptrCast(fwd_buf));
            c.free(@ptrCast(rev_buf));
            return -1;
        }
        {
            var i: c_int = 0;
            while (i < n_fwd) : (i += 1) {
                const nb = fwd_buf[@intCast(i)];
                if (!isVisited(nb, visited, max_nodes)) {
                    markVisited(nb, visited, max_nodes);
                    if (q_tail < max_nodes) {
                        queue[@intCast(q_tail)] = nb;
                        queue_depth[@intCast(q_tail)] = next_depth;
                        q_tail += 1;
                        visited_count += 1;
                        if (cb.?(nb, next_depth, user) != 0) {
                            c.free(@ptrCast(visited));
                            c.free(@ptrCast(queue));
                            c.free(@ptrCast(queue_depth));
                            c.free(@ptrCast(fwd_buf));
                            c.free(@ptrCast(rev_buf));
                            return visited_count;
                        }
                    }
                }
            }
        }

        // Reverse neighbors
        var n_rev: c_int = 0;
        if (collectReverse(d, current, rev_buf, &n_rev, max_nodes) != 0) {
            c.free(@ptrCast(visited));
            c.free(@ptrCast(queue));
            c.free(@ptrCast(queue_depth));
            c.free(@ptrCast(fwd_buf));
            c.free(@ptrCast(rev_buf));
            return -1;
        }
        {
            var i: c_int = 0;
            while (i < n_rev) : (i += 1) {
                const nb = rev_buf[@intCast(i)];
                if (!isVisited(nb, visited, max_nodes)) {
                    markVisited(nb, visited, max_nodes);
                    if (q_tail < max_nodes) {
                        queue[@intCast(q_tail)] = nb;
                        queue_depth[@intCast(q_tail)] = next_depth;
                        q_tail += 1;
                        visited_count += 1;
                        if (cb.?(nb, next_depth, user) != 0) {
                            c.free(@ptrCast(visited));
                            c.free(@ptrCast(queue));
                            c.free(@ptrCast(queue_depth));
                            c.free(@ptrCast(fwd_buf));
                            c.free(@ptrCast(rev_buf));
                            return visited_count;
                        }
                    }
                }
            }
        }
    }

    c.free(@ptrCast(visited));
    c.free(@ptrCast(queue));
    c.free(@ptrCast(queue_depth));
    c.free(@ptrCast(fwd_buf));
    c.free(@ptrCast(rev_buf));
    return visited_count;
}

// ─── Node observations ──────────────────────────────────────────────────────

const ObsCollectCtx = struct {
    db: *DlDb,
    cb: DlStrCb,
    user: ?*anyopaque,
    max_obs: c_int,
    count: c_int,
};

fn collectObsCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ctx: *ObsCollectCtx = @ptrCast(@alignCast(user.?));
    const content = dl_intern_str_of(ctx.db, cols.?[1]);
    if (content != null) {
        ctx.count += 1;
        if (ctx.cb.?(content.?, ctx.user) != 0) return 1;
    }
    if (ctx.max_obs > 0 and ctx.count >= ctx.max_obs) return 1;
    return 0;
}

pub export fn dl_node_observations(db: ?*DlDb, node: [*c]const u8, max_obs: c_int, cb: DlStrCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (node == null or cb == null) return -1;

    const obs_idx = findRel(d, "observation");
    if (obs_idx < 0) return -1;
    if (d.rels[@intCast(obs_idx)].kind == RELK_VARIADIC) return -1;
    if (d.rels[@intCast(obs_idx)].arity != 2) return -1;
    const obs_rel = d.rels[@intCast(obs_idx)].rel orelse return -1;

    const node_sym = dl_intern_str_find(d, node);
    if (node_sym == 0) return 0;

    var leading = [1]u32{node_sym};
    var ctx = ObsCollectCtx{ .db = d, .cb = cb, .user = user, .max_obs = max_obs, .count = 0 };
    const rc = relation.rel_prefix(obs_rel, &leading, 1, collectObsCb, &ctx);
    if (rc < 0) return -1;

    return ctx.count;
}

// ─── tests ──────────────────────────────────────────────────────────────────

const testing = std.testing;

/// Best-effort recursive removal of a test db dir (reuses rmRf).
fn rmrfTestDir(path: [*:0]const u8) void {
    rmRf(path);
}

/// Read a whole file into a malloc'd buffer (tests only); the caller frees it
/// with c.free.  Returns an empty slice on any error (the assertions then fail
/// loudly on the empty content rather than reading uninitialised memory).
fn readFileAlloc(path: [*c]const u8) []u8 {
    var st: Stat = undefined;
    if (stat(path, &st) != 0 or st.st_size < 0) return &[_]u8{};
    const n: usize = @intCast(st.st_size);
    const mem = c.malloc(n + 1) orelse return &[_]u8{};
    const buf: [*]u8 = @ptrCast(mem);
    const fd = posix.open(path, posix.O_RDONLY);
    if (fd < 0) {
        c.free(mem);
        return &[_]u8{};
    }
    defer _ = posix.close(fd);
    var got: usize = 0;
    while (got < n) {
        const r = posix.read(fd, buf + got, n - got);
        if (r <= 0) break;
        got += @intCast(r);
    }
    return buf[0..got];
}

const CollectCbCtx = struct {
    rows: [64][8]u32 = undefined,
    arities: [64]u8 = undefined,
    n: usize = 0,
};

fn collectCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const self: *CollectCbCtx = @ptrCast(@alignCast(user.?));
    var i: u8 = 0;
    while (i < arity) : (i += 1) self.rows[self.n][i] = cols.?[i];
    self.arities[self.n] = arity;
    self.n += 1;
    return 0;
}

test "dl_open + declare + add_fact + lookup roundtrip" {
    const dir = "/tmp/datalog_zig_u11_roundtrip";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "edge", 2));
    const a = [2]u32{ 1, 2 };
    const b = [2]u32{ 3, 4 };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &a, 2));
    try testing.expectEqual(@as(c_int, 0), dl_add_fact(db, "edge", &a, 2)); // dup
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &b, 2));
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "edge", &a, 2));
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "edge", &b, 2));
    try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "edge", &[2]u32{ 9, 9 }, 2));

    // delete
    try testing.expectEqual(@as(c_int, 1), dl_delete_fact(db, "edge", &a, 2));
    try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "edge", &a, 2));
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "edge", &b, 2));
}

// ─── lazyrel (lazy on-disk relation load) ──────────────────────────────────

/// mtime of `path` (stat(2)); -1 when unstat-able.  Used by the spurious-save
/// probe below: a close that rewrites an UNTOUCHED relation's .dafsa must
/// show up as a bumped mtime.
fn testStatMtime(path: [*:0]const u8) i64 {
    var st: Stat = undefined;
    if (stat(path, &st) != 0) return -1;
    return st.st_mtim.tv_sec;
}

test "lazyrel: open defers the DAFSA read; a touch loads ONLY the touched relation" {
    const dir = "/tmp/datalog_zig_lazyrel_touch";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // Seed a store with two relations, each with its own fact.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "beta", 3));
        const a1 = [2]u32{ 1, 2 };
        const b1 = [3]u32{ 5, 6, 7 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &a1, 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "beta", &b1, 3));
    }

    // Reopen: BOTH relations must still be deferred — open reads no relation
    // DAFSA (this is the assertion that fails on the eager tree).
    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    const ia = findRel(db, "alpha");
    const ib = findRel(db, "beta");
    try testing.expect(ia >= 0 and ib >= 0);
    try testing.expectEqual(@as(c_int, 0), relation.rel_is_loaded(db.rels[@intCast(ia)].rel));
    try testing.expectEqual(@as(c_int, 0), relation.rel_is_loaded(db.rels[@intCast(ib)].rel));

    // A read of alpha touches alpha ONLY — beta stays deferred.  Since
    // zerocopy-view the touch is a VIEW, not a parse: the relation is readable
    // but still NOT loaded (owned), which is the whole point.
    const a1 = [2]u32{ 1, 2 };
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &a1, 2));
    try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.view)), relation.rel_read_state(db.rels[@intCast(ia)].rel));
    try testing.expectEqual(@as(c_int, 0), relation.rel_is_loaded(db.rels[@intCast(ia)].rel));
    try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.deferred)), relation.rel_read_state(db.rels[@intCast(ib)].rel));

    // The deferred relation still answers from disk once touched.
    const b1 = [3]u32{ 5, 6, 7 };
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "beta", &b1, 3));
    try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.view)), relation.rel_read_state(db.rels[@intCast(ib)].rel));
    try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "beta", &[3]u32{ 5, 6, 8 }, 3));
}

test "lazyrel: writing a never-read relation persists it; untouched relations are not rewritten" {
    const dir = "/tmp/datalog_zig_lazyrel_write";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // Seed: alpha only.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "beta", 2));
        const a1 = [2]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &a1, 2));
    }

    var alpha_path: [4096:0]u8 = undefined;
    _ = snprintf(&alpha_path, 4096, "%s/alpha.dafsa", @as([*:0]const u8, dir));
    const mt_before = testStatMtime(&alpha_path);

    // Write to beta WITHOUT ever reading it: the lazy load happens at the
    // write, the fact must survive the close/save, and alpha (untouched,
    // never read this session) must NOT be written back spuriously.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        const ib = findRel(db, "beta");
        try testing.expect(ib >= 0);
        try testing.expectEqual(@as(c_int, 0), relation.rel_is_loaded(db.rels[@intCast(ib)].rel));
        const b1 = [2]u32{ 10, 20 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "beta", &b1, 2));
        // The write PROMOTED beta to owned (the duplicate check and the
        // insert both need a mutable DAFSA).
        try testing.expectEqual(@as(c_int, 1), relation.rel_is_loaded(db.rels[@intCast(ib)].rel));
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.owned)), relation.rel_read_state(db.rels[@intCast(ib)].rel));
    }

    // alpha.dafsa is byte-for-byte untouched: same size, same mtime.
    var st: Stat = undefined;
    try testing.expect(stat(&alpha_path, &st) == 0);
    try testing.expect(st.st_size > 0);
    try testing.expectEqual(mt_before, testStatMtime(&alpha_path));

    // Reopen: BOTH the old alpha fact and the never-read beta fact persist.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "beta", &[2]u32{ 10, 20 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "beta", &[2]u32{ 11, 20 }, 2));
    }
}

test "lazyrel: order statistics on a NEVER-loaded relation answer EMPTY, not error" {
    // Regression guard for the lazyrel gate ORDER: the pre-lazy accessors
    // checked `r.d == NULL` as an OOM guard BEFORE loading; on a deferred
    // relation that check fires first and rank/select/range return their
    // ERROR values (UINT64_MAX / -1) instead of the empty answers.  Caught by
    // the C oracle (test_m10_rank T6/T10), not by the CLI corpus.
    const dir = "/tmp/datalog_zig_lazyrel_ordstats";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // Seed a store with one fact in alpha; declare r but never write it.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "r", 2));
        const a1 = [2]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &a1, 2));
    }

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    const ir = findRel(db, "r");
    try testing.expect(ir >= 0);
    // r is deferred and stays deferred: nothing below loads it.
    try testing.expectEqual(@as(c_int, 0), relation.rel_is_loaded(db.rels[@intCast(ir)].rel));

    // Empty-relation contract (oracle test_m10_rank T6): select == -1,
    // rank == 0, count == 0, range_count == 0 — WITHOUT an error value.
    var out: [2]u32 = undefined;
    const k3 = [2]u32{ 3, 3 };
    const k_lo = [2]u32{ 0, 0 };
    const k_hi = [2]u32{ 9, 9 };
    const a1b = [2]u32{ 1, 2 };
    const a9 = [2]u32{ 9, 9 };
    try testing.expectEqual(@as(c_int, -1), dl_select(db, "r", 0, &out, 2));
    try testing.expectEqual(@as(c_int, -1), dl_select(db, "r", 5, &out, 2));
    try testing.expectEqual(@as(u64, 0), dl_rank(db, "r", &k3, 2));
    try testing.expectEqual(@as(u64, 0), dl_count(db, "r"));
    try testing.expectEqual(@as(u64, 0), dl_range_count(db, "r", &k_lo, &k_hi, 2));
    // prefix-bound stats over the empty bound: rank_bound 0, select_bound -1.
    try testing.expectEqual(@as(u64, 0), dl_rank_bound(db, "r", &.{3}, 1, &k3, 2));
    try testing.expectEqual(@as(c_int, -1), dl_select_bound(db, "r", &.{3}, 1, 0, &out, 2));
    // And a VIEW-BACKED relation still answers exactly.
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &a1b, 2));
    try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.view)), relation.rel_read_state(db.rels[@intCast(findRel(db, "alpha"))].rel));
    try testing.expectEqual(@as(u64, 0), dl_rank(db, "alpha", &a1b, 2));
    try testing.expectEqual(@as(u64, 1), dl_rank(db, "alpha", &a9, 2));
}

test "lazyrel: WAL records at open are recovered EAGERLY (never deferred)" {
    const dir = "/tmp/datalog_zig_lazyrel_wal";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // Session 1: add a fact and ABANDON the handle without dl_close — the
    // crash shape: the fact is durable ONLY through the WAL (never
    // consolidated into the DAFSA by a close/publish).
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        const a1 = [2]u32{ 7, 8 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &a1, 2));
        // NO dl_close: the WAL record stays unconsolidated.
    }

    // Session 2: the reopen must see the committed WAL records and load the
    // DAFSA EAGERLY (not lazily), replaying them — but it must NOT truncate
    // the WAL.  Truncating at open was the reader-loss defect: the open-time
    // replay+compact runs OUTSIDE the mutation window, so a reader that was
    // mid-open (in dafsa_load) captured the pre-truncate WAL size and then
    // replayed a valid EMPTY WAL into a DAFSA that predates the records —
    // success, no error, committed facts silently gone (/tmp/mw/m4.py).
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        const ia = findRel(db, "alpha");
        try testing.expect(ia >= 0);
        try testing.expectEqual(@as(c_int, 1), relation.rel_is_loaded(db.rels[@intCast(ia)].rel));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 7, 8 }, 2));
        // Still in the WAL: a concurrent opener's replay must find it.
        try testing.expect(relation.rel_wal_size(db.rels[@intCast(ia)].rel) > 16);
        dl_close(db); // windowed close sweep owns the compaction
    }

    // The WINDOWED close sweep compacted it: the WAL is back to its 16-byte
    // header and the fact lives in the base DAFSA.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        const ia = findRel(db, "alpha");
        try testing.expect(ia >= 0);
        try testing.expectEqual(@as(u64, 16), relation.rel_wal_size(db.rels[@intCast(ia)].rel));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 7, 8 }, 2));
    }
}

test "zerocopy: a READ-ONLY opener parses no relation (reads go through views)" {
    // The zerocopy contract at the stack level: a read-only handle reads every
    // relation through a zero-copy mmap view and NEVER parses one, so a
    // process that only reads pays the view open (~CRC + state_off walk), not
    // the states[]/trans[]/register rebuild.  FAILS on the pre-view tree: the
    // read there ends in .owned (rel_is_loaded == 1).
    const dir = "/tmp/datalog_zig_zerocopy_ro";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "beta", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 3 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "beta", &[2]u32{ 4, 5 }, 2));
    }

    var err: c_int = 0;
    const db = dl_open_ro(dir, &err) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    const ia = findRel(db, "alpha");
    const ib = findRel(db, "beta");
    try testing.expect(ia >= 0 and ib >= 0);

    // Nothing parsed at open.
    try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.deferred)), relation.rel_read_state(db.rels[@intCast(ia)].rel));

    // Lookup + prefix + order statistics all answer from the view.
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 3 }, 2));
    try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "alpha", &[2]u32{ 1, 4 }, 2));
    var got = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 2), dl_prefix(db, "alpha", &[1]u32{1}, 1, collectCb, &got));
    try testing.expectEqual(@as(usize, 2), got.n);
    try testing.expectEqual(@as(u64, 0), dl_rank(db, "alpha", &[2]u32{ 1, 2 }, 2));
    try testing.expectEqual(@as(u64, 2), dl_rank(db, "alpha", &[2]u32{ 9, 9 }, 2));
    var sel: [2]u32 = undefined;
    try testing.expectEqual(@as(c_int, 0), dl_select(db, "alpha", 1, &sel, 2));
    try testing.expectEqualSlices(u32, &[2]u32{ 1, 3 }, sel[0..2]);
    try testing.expectEqual(@as(u64, 2), dl_count(db, "alpha")); // words (subtree count)
    // rel_count is the FINAL-STATE count (minimal-DAFSA merging: both words
    // end in the same empty-right-language state), the same value dafsa_stats
    // reports for an owned relation.
    try testing.expectEqual(@as(u64, 1), relation.rel_count(db.rels[@intCast(ia)].rel));
    try testing.expectEqual(@as(u64, 2), dl_range_count(db, "alpha", &[2]u32{ 1, 0 }, &[2]u32{ 2, 0 }, 2));

    // ... and alpha is STILL not parsed, and beta (untouched) is still deferred.
    try testing.expectEqual(@as(c_int, 0), relation.rel_is_loaded(db.rels[@intCast(ia)].rel));
    try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.view)), relation.rel_read_state(db.rels[@intCast(ia)].rel));
    try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.deferred)), relation.rel_read_state(db.rels[@intCast(ib)].rel));
}

test "zerocopy: publishing with a VIEW-BACKED relation keeps the snapshot correct" {
    // The snapshot path must keep working when a relation was only read
    // (view-backed, non-dirty) and another was written (promoted).  Two
    // invariants:
    //   1. the published snapshot holds BOTH relations, and the live store
    //      still answers after the snapshot flip;
    //   2. a SECOND publish with no writes between them re-uses the previous
    //      snapshot's file by hardlink (the save gate) — the promoted-vs-view
    //      distinction must not disturb it.
    // NOTE: the FIRST publish of a process re-serializes only relations it
    // MUTATED (owned); an unmutated one is .view/.deferred and is hardlinked
    // from its live file, so the view-backed alpha below is linked, not parsed
    // (see relSaveGated's cross-process gate).
    const dir = "/tmp/datalog_zig_zerocopy_publish";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "beta", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "beta", &[2]u32{ 4, 5 }, 2));
    }

    var v1_alpha: [4096:0]u8 = undefined;
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        // alpha is only READ (view), beta is WRITTEN (promoted).
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        const ia = findRel(db, "alpha");
        const ib = findRel(db, "beta");
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.view)), relation.rel_read_state(db.rels[@intCast(ia)].rel));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "beta", &[2]u32{ 6, 7 }, 2));
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.owned)), relation.rel_read_state(db.rels[@intCast(ib)].rel));

        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1
        const v1 = db.snap_version;
        _ = snprintf(&v1_alpha, 4096, "%s/snapshots/%u/alpha.dafsa", @as([*:0]const u8, dir), v1);

        // No writes since: publish again — every relation must take the
        // hardlink path (same inode), i.e. the save gate still works.
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v2
        const v2 = db.snap_version;
        try testing.expect(v2 != v1);
        var v2_alpha: [4096:0]u8 = undefined;
        _ = snprintf(&v2_alpha, 4096, "%s/snapshots/%u/alpha.dafsa", @as([*:0]const u8, dir), v2);
        var s1: Stat = undefined;
        var s2: Stat = undefined;
        try testing.expectEqual(@as(c_int, 0), stat(&v1_alpha, &s1));
        try testing.expectEqual(@as(c_int, 0), stat(&v2_alpha, &s2));
        try testing.expectEqual(s1.st_ino, s2.st_ino); // hardlinked, not re-serialized
    }

    // The snapshot holds both relations; the live store answers after the flip.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "beta", &[2]u32{ 4, 5 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "beta", &[2]u32{ 6, 7 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "beta", &[2]u32{ 6, 8 }, 2));
        // The snapshot's alpha view answers the same as the live one.
        var v_arity: u8 = 0;
        var variadic: c_int = 0;
        const sv = snapshotViewOpenRel(db, "alpha", &v_arity, &variadic);
        try testing.expect(sv != null and variadic == 0);
        const lead = [1]u32{1};
        var got = CollectCbCtx{};
        try testing.expect(db.snap_version > 0);
        try testing.expectEqual(@as(c_long, 1), snapshot_query_scan(dir, db.snap_version, &db.vcache, "alpha", &lead, 1, collectCb, &got));
        try testing.expectEqual(@as(usize, 1), got.n);
    }
}

test "dl_publish_if_needed: no version when the store is already current" {
    // PRE-FIX BEHAVIOUR: the CLI's query arm called dl_publish_snapshot
    // unconditionally, and that ALWAYS creates a version — so a
    // publish-then-query loop bumped snap_version on every call even when
    // nothing had changed since the previous one (MEASURED on the 172k-symbol
    // store: ~31 ms of re-materialization per query, mostly the 46 MB interner
    // stamp hash, plus the tmp-dir/fsync/CURRENT work).
    const dir = "/tmp/datalog_zig_publish_if_needed";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // A publishing process: declare + write + publish v1.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        try testing.expectEqual(@as(u32, 1), db.snap_version);
    }

    // A LATER process — the shape every `dl query` has: the declarations come
    // from rels.txt (so meta_dirty is 0), nothing has been written, and the
    // relation is still .deferred with its live file hardlinked into v1.
    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    try testing.expectEqual(@as(c_int, 0), db.meta_dirty);
    try testing.expectEqual(@as(c_int, 0), db.fixpoint_dirty);
    {
        const ia = findRel(db, "alpha");
        try testing.expect(ia >= 0);
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.deferred)), relation.rel_read_state(db.rels[@intCast(ia)].rel));
    }

    // Nothing changed since: the early-out must return 0 WITHOUT a version.
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 1), db.snap_version); // NOT bumped
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 1), db.snap_version); // still not

    // dl_publish_snapshot's own contract is untouched: it always advances.
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
    try testing.expectEqual(@as(u32, 2), db.snap_version);
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
    try testing.expectEqual(@as(u32, 3), db.snap_version);

    // A fact added in this process MUST disqualify the early-out (the store is
    // no longer what CURRENT describes): the publish happens, and a fresh
    // reader of the new snapshot sees the new row.  The disqualifier here is
    // the relation's revision (raw-int values intern nothing, so the interner
    // count still matches).
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 3, 4 }, 2));
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 4), db.snap_version); // it DID publish

    var got = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 2), snapshot_query_scan(dir, db.snap_version, &db.vcache, "alpha", null, 0, collectCb, &got));
    try testing.expectEqualSlices(u32, &[2]u32{ 1, 2 }, got.rows[0][0..2]);
    try testing.expectEqualSlices(u32, &[2]u32{ 3, 4 }, got.rows[1][0..2]);

    // With that published, the early-out fires again...
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 4), db.snap_version);

    // ...and a NEW SYMBOL alone disqualifies it: one interned string (no fact,
    // no declaration) makes the snapshot's vocabulary stale.  Here the intern
    // leaves the root array lagging, so the interner gate that fires is the
    // dirty flag; the count half of that gate is belt-and-braces on top of it
    // (every engine path that reaches count-changed-with-dirty-clear also moves
    // a relation revision, so no path exercises the count check alone).
    try testing.expect(dl_intern_str(db, "hello") != 0);
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 5), db.snap_version); // published for the symbol
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 5), db.snap_version);
}

test "dl_publish_if_needed: the query path's own VM run creates no version" {
    // THE QUERY PATH, which the gate above cannot reach: `dl query` loads
    // rules first and dl_load_rules sets fixpoint_dirty unconditionally, so a
    // gate on "nothing was loaded" never fires there.  The gate has to judge
    // the store AFTER the run, on CONTENT — the run itself is not evidence,
    // because a full re-evaluation resets every head's view (bumping its rev)
    // and re-adds the very same tuples: "the VM ran" and "a revision moved"
    // are both true when nothing was derived.
    //
    // PRE-FIX BEHAVIOUR: the query arm called dl_publish_snapshot, which always
    // creates a version — so every read persisted one (that is where the
    // 172k-symbol store's ~390 historical versions came from).
    const dir = "/tmp/datalog_zig_publish_if_needed_rules";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // A publishing process: an EDB relation, a rule deriving an IDB head, v1.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "edge", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &[2]u32{ 2, 3 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "path(X,Y) :- edge(X,Y)."));
        try testing.expectEqual(@as(c_int, 0), dl_compile(db));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        try testing.expectEqual(@as(u32, 1), db.snap_version);
    }

    // A LATER process, shaped exactly like `dl query`: the same rules loaded
    // (so fixpoint_dirty is set), nothing written.
    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "path(X,Y) :- edge(X,Y)."));
    try testing.expectEqual(@as(c_int, 1), db.fixpoint_dirty); // the run is owed

    // The run happens inside dl_publish_if_needed and derives nothing new, so
    // the gate fires on the content: no version — and it stays that way.
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(c_int, 0), db.fixpoint_dirty); // it ran
    try testing.expectEqual(@as(u32, 1), db.snap_version); // and created nothing
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 1), db.snap_version);

    // A fact added in this process changes what the run derives: publish (v2),
    // and the derived head must be in the new snapshot for a fresh reader.
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &[2]u32{ 3, 4 }, 2));
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 2), db.snap_version); // it DID publish
    var got = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 3), snapshot_query_scan(dir, db.snap_version, &db.vcache, "path", null, 0, collectCb, &got));
    try testing.expectEqualSlices(u32, &[2]u32{ 3, 4 }, got.rows[2][0..2]);
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db)); // then quiet again
    try testing.expectEqual(@as(u32, 2), db.snap_version);

    // A rule set that derives NEW tuples must publish as well.  The closure
    // adds (1,3) and (2,4), which v2 does not hold — content differs, so the
    // gate declines and a version is created.
    try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "path(X,Y) :- edge(X,Y). path(X,Y) :- edge(X,Z), path(Z,Y)."));
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db));
    try testing.expectEqual(@as(u32, 3), db.snap_version); // it DID publish
    var got2 = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 6), snapshot_query_scan(dir, db.snap_version, &db.vcache, "path", null, 0, collectCb, &got2));
    try testing.expectEqual(@as(c_int, 0), dl_publish_if_needed(db)); // the closure is stable
    try testing.expectEqual(@as(u32, 3), db.snap_version);
}

test "zerocopy: a write after a view read promotes and works end to end" {
    // The promotion contract: read (view) then write must materialize the
    // relation, apply the write as before, and persist it — the view must not
    // be a read-only dead end.
    const dir = "/tmp/datalog_zig_zerocopy_promote";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
    }

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        const ia = findRel(db, "alpha");
        try testing.expect(ia >= 0);
        const rel = db.rels[@intCast(ia)].rel;

        // Read first: a view, not a parse.
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.view)), relation.rel_read_state(rel));

        // Write: promotes, and the pre-existing fact survives the promotion.
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 9, 9 }, 2));
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.owned)), relation.rel_read_state(rel));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 9, 9 }, 2));

        // A duplicate write through the promoted relation still reports 0.
        try testing.expectEqual(@as(c_int, 0), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
    }

    // The promoted relation was saved normally: both facts persist.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 9, 9 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "alpha", &[2]u32{ 9, 8 }, 2));
    }
}

// ─── publish gate: the cross-process (unmutated ⇒ live file) half ───────────

/// stat <dir>/<name>.dafsa and <dir>/snapshots/<version>/<name>.dafsa and
/// require the SAME inode — i.e. the publish hardlinked the live file into the
/// snapshot instead of re-serializing it.
fn expectSnapshotLinksLive(dir: [*:0]const u8, version: u32, name: [*:0]const u8) !void {
    var live: [4096:0]u8 = undefined;
    var snap: [4096:0]u8 = undefined;
    _ = snprintf(&live, 4096, "%s/%s.dafsa", dir, name);
    _ = snprintf(&snap, 4096, "%s/snapshots/%u/%s.dafsa", dir, version, name);
    var sl: Stat = undefined;
    var ss: Stat = undefined;
    try testing.expectEqual(@as(c_int, 0), stat(&live, &sl));
    try testing.expectEqual(@as(c_int, 0), stat(&snap, &ss));
    try testing.expectEqual(sl.st_dev, ss.st_dev);
    try testing.expectEqual(sl.st_ino, ss.st_ino);
}

/// stat the two paths and require DIFFERENT inodes (the publish wrote new
/// bytes instead of linking an existing file).
fn expectDifferentInode(a: [*:0]const u8, b: [*:0]const u8) !void {
    var sa: Stat = undefined;
    var sb: Stat = undefined;
    try testing.expectEqual(@as(c_int, 0), stat(a, &sa));
    try testing.expectEqual(@as(c_int, 0), stat(b, &sb));
    try testing.expect(sa.st_ino != sb.st_ino or sa.st_dev != sb.st_dev);
}

test "publish gate: a FRESH process links unmutated relations from their live files" {
    // The dominant publish cost at a large store: every invocation is a fresh
    // process, and saved_rev — a PER-PROCESS counter re-armed to the sentinel
    // at open — made the first publish of every process re-serialize the whole
    // store (MEASURED on the agent store: 26 relations, 61,878,035 bytes,
    // 1.44 s of rel_save) even when nothing had been written.  An unmutated
    // relation is .deferred/.view, so its content IS its live
    // <db>/<rel>.dafsa file: the publish must hardlink it, writing zero bytes.
    //
    // FAILS on the pre-gate tree: there the snapshot's file is a fresh
    // serialization — a different inode — for every relation.
    const dir = "/tmp/datalog_zig_pubgate_fresh";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // Session 1: build the store.  dl_close writes the live relation files.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "beta", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "beta", &[2]u32{ 4, 5 }, 2));
    }

    // Session 2: publish with nothing read and nothing written.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.deferred)), relation.rel_read_state(db.rels[@intCast(findRel(db, "alpha"))].rel));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        const v1 = db.snap_version;
        try testing.expect(v1 > 0);
        try expectSnapshotLinksLive(dir, v1, "alpha");
        try expectSnapshotLinksLive(dir, v1, "beta");
        // The snapshot's bytes ARE the live file's bytes (a hardlink, not a
        // copy): a fresh serialization would be answer-equivalent, so this
        // pins the stronger property the gate claims.
        var live: [4096:0]u8 = undefined;
        var snap: [4096:0]u8 = undefined;
        _ = snprintf(&live, 4096, "%s/alpha.dafsa", @as([*:0]const u8, dir));
        _ = snprintf(&snap, 4096, "%s/snapshots/%u/alpha.dafsa", @as([*:0]const u8, dir), v1);
        const lb = readFileAlloc(&live);
        defer c.free(@ptrCast(lb.ptr));
        const sb = readFileAlloc(&snap);
        defer c.free(@ptrCast(sb.ptr));
        try testing.expect(lb.len > 0);
        try testing.expectEqualSlices(u8, lb, sb);
    }

    // The linked snapshot answers the store's facts.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "beta", &[2]u32{ 4, 5 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "alpha", &[2]u32{ 1, 3 }, 2));
    }
}

test "publish gate: a MUTATED relation is re-serialized and the old snapshot keeps its bytes" {
    // The failure mode of the gate is a STALE SNAPSHOT: a hardlinked file that
    // should have been rewritten.  So: publish a relation while it is linked,
    // snapshot the published bytes, mutate the relation (which promotes it out
    // of .view — the gate must stop applying), publish again, and require
    //   * the new snapshot's file is a DIFFERENT inode,
    //   * it holds the new fact, and
    //   * the OLD snapshot's file still holds exactly the old bytes — the
    //     mutation must not have followed the hardlink into it.
    const dir = "/tmp/datalog_zig_pubgate_mutate";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
    }

    var v1: u32 = 0;
    var v2: u32 = 0;
    var v1_alpha: [4096:0]u8 = undefined;
    var v2_alpha: [4096:0]u8 = undefined;
    var live_alpha: [4096:0]u8 = undefined;
    _ = snprintf(&live_alpha, 4096, "%s/alpha.dafsa", @as([*:0]const u8, dir));

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v1 = db.snap_version;
        _ = snprintf(&v1_alpha, 4096, "%s/snapshots/%u/alpha.dafsa", @as([*:0]const u8, dir), v1);
        try expectSnapshotLinksLive(dir, v1, "alpha");

        // Keep the published bytes to prove they are never touched again.
        const before = readFileAlloc(&v1_alpha);
        defer c.free(@ptrCast(before.ptr));
        try testing.expect(before.len > 0);

        // READ it first (a view), then mutate: the write must promote the
        // relation to .owned, which is exactly what disables the link gate.
        const ia = findRel(db, "alpha");
        try testing.expect(ia >= 0);
        const rel = db.rels[@intCast(ia)].rel;
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.view)), relation.rel_read_state(rel));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 9, 9 }, 2));
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.owned)), relation.rel_read_state(rel));

        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v2 = db.snap_version;
        try testing.expect(v2 != v1);
        _ = snprintf(&v2_alpha, 4096, "%s/snapshots/%u/alpha.dafsa", @as([*:0]const u8, dir), v2);
        try expectDifferentInode(&v1_alpha, &v2_alpha);
        try expectDifferentInode(&live_alpha, &v2_alpha);

        // The old snapshot is immutable: the mutation did not follow the link.
        const after = readFileAlloc(&v1_alpha);
        defer c.free(@ptrCast(after.ptr));
        try testing.expectEqualSlices(u8, before, after);
    }

    // Both snapshots answer what they were published with; the live store has
    // both facts.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        var got1 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), snapshot_query_scan(dir, v1, &db.vcache, "alpha", null, 0, collectCb, &got1));
        try testing.expectEqualSlices(u32, &[2]u32{ 1, 2 }, got1.rows[0][0..2]);
        // Same cache, other version: the slot carries the version it was
        // opened at, so this is a MISS and re-opens from v2's dir.
        var got2 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 2), snapshot_query_scan(dir, v2, &db.vcache, "alpha", null, 0, collectCb, &got2));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 9, 9 }, 2));
    }
}

test "snapshot vcache: one relation at TWO versions in one process" {
    // A view maps ONE version's bytes, so a cache slot is only reusable for the
    // (relation, version) it was opened at.  Keyed on the name alone, the v2
    // read below is served v1's view — the second version's answer is silently
    // the first version's data.
    const dir = "/tmp/datalog_zig_vcache_version";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var v1: u32 = 0;
    var v2: u32 = 0;
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v1 = db.snap_version;
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 9, 9 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v2 = db.snap_version;
        try testing.expect(v2 != v1);
    }

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);

        var got1 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), snapshot_query_scan(dir, v1, &db.vcache, "alpha", null, 0, collectCb, &got1));
        try testing.expectEqualSlices(u32, &[2]u32{ 1, 2 }, got1.rows[0][0..2]);

        // The SAME cache, a different version — no invalidation in between.
        var got2 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 2), snapshot_query_scan(dir, v2, &db.vcache, "alpha", null, 0, collectCb, &got2));
        try testing.expectEqualSlices(u32, &[2]u32{ 1, 2 }, got2.rows[0][0..2]);
        try testing.expectEqualSlices(u32, &[2]u32{ 9, 9 }, got2.rows[1][0..2]);
    }
}

test "snapshot vcache: one relation at THREE versions in one process" {
    // Three versions live in the cache at once (8 slots), each opened at its
    // own version: every read must answer from ITS version, in any order.
    const dir = "/tmp/datalog_zig_vcache_three";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var v1: u32 = 0;
    var v2: u32 = 0;
    var v3: u32 = 0;
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v1 = db.snap_version;
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 9, 9 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v2 = db.snap_version;
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 5, 6 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v3 = db.snap_version;
    }

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);

        var g1 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), snapshot_query_scan(dir, v1, &db.vcache, "alpha", null, 0, collectCb, &g1));
        var g2 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 2), snapshot_query_scan(dir, v2, &db.vcache, "alpha", null, 0, collectCb, &g2));
        var g3 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 3), snapshot_query_scan(dir, v3, &db.vcache, "alpha", null, 0, collectCb, &g3));
        try testing.expectEqualSlices(u32, &[2]u32{ 1, 2 }, g3.rows[0][0..2]);
        try testing.expectEqualSlices(u32, &[2]u32{ 5, 6 }, g3.rows[1][0..2]);
        try testing.expectEqualSlices(u32, &[2]u32{ 9, 9 }, g3.rows[2][0..2]);

        // Back to the OLDEST version with all three slots warm: still v1's
        // single tuple (no eviction happened — 3 of 8 slots are used).
        var g1b = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), snapshot_query_scan(dir, v1, &db.vcache, "alpha", null, 0, collectCb, &g1b));
        try testing.expectEqualSlices(u32, &[2]u32{ 1, 2 }, g1b.rows[0][0..2]);
        var g2b = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 2), snapshot_query_scan(dir, v2, &db.vcache, "alpha", null, 0, collectCb, &g2b));
        try testing.expectEqualSlices(u32, &[2]u32{ 9, 9 }, g2b.rows[1][0..2]);

        // The cache really did hold one slot PER version.
        var seen: [3]u32 = .{ 0, 0, 0 };
        for (db.vcache) |slot| {
            if (slot.view == null) continue;
            if (slot.version == v1) seen[0] += 1;
            if (slot.version == v2) seen[1] += 1;
            if (slot.version == v3) seen[2] += 1;
        }
        try testing.expectEqual(@as(u32, 1), seen[0]);
        try testing.expectEqual(@as(u32, 1), seen[1]);
        try testing.expectEqual(@as(u32, 1), seen[2]);
    }
}

test "snapshot vcache: a PRUNED version is never served from a warm slot" {
    // Retention deletes old snapshot dirs.  A slot that maps a pruned version's
    // file must be closed, not served: the answer for a version that no longer
    // exists is the loud -1, never a cached tuple set.  (Publish's own
    // vcache_invalidate runs before the prune, so this pins the requirement
    // from the outside; vcache_drop_version covers a future prune caller.)
    const dir = "/tmp/datalog_zig_vcache_prune";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var v1: u32 = 0;
    var v2: u32 = 0;
    var v3: u32 = 0;
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v1 = db.snap_version;
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 9, 9 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        v2 = db.snap_version;

        // Warm the cache for BOTH existing versions, then prune down to one.
        var warm1 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), snapshot_query_scan(dir, v1, &db.vcache, "alpha", null, 0, collectCb, &warm1));
        var warm2 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 2), snapshot_query_scan(dir, v2, &db.vcache, "alpha", null, 0, collectCb, &warm2));

        try testing.expectEqual(@as(c_int, 0), dl_set_snapshot_retain(db, 1));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 5, 6 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v3 → prune v1, v2
        v3 = db.snap_version;

        // Pruned versions: loud failure, not a cached answer.
        var p1 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, -1), snapshot_query_scan(dir, v1, &db.vcache, "alpha", null, 0, collectCb, &p1));
        try testing.expectEqual(@as(usize, 0), p1.n);
        var p2 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, -1), snapshot_query_scan(dir, v2, &db.vcache, "alpha", null, 0, collectCb, &p2));
        try testing.expectEqual(@as(usize, 0), p2.n);

        // No slot may still claim a pruned version.
        for (db.vcache) |slot| {
            if (slot.view == null) continue;
            try testing.expect(slot.version != v1);
            try testing.expect(slot.version != v2);
        }

        // The surviving version still answers.
        var g3 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 3), snapshot_query_scan(dir, v3, &db.vcache, "alpha", null, 0, collectCb, &g3));
        try testing.expectEqualSlices(u32, &[2]u32{ 5, 6 }, g3.rows[1][0..2]);
    }
}

test "publish gate: WAL-recovered content is SERIALIZED, never linked from the live file" {
    // The one case where an unmutated-looking relation's content is NOT its
    // live file: records committed to the per-relation WAL but never
    // consolidated.  Such a relation opens .owned (replay), and linking the
    // stale live file would publish a snapshot that silently LOSES those
    // facts.  The gate must serialize.
    const dir = "/tmp/datalog_zig_pubgate_wal";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // Session 1: a fact, consolidated by the close sweep.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
    }

    // Session 2: a second fact, then ABANDON the handle (the crash shape): it
    // is durable ONLY in the WAL, so the live file still holds {1,2} alone.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 7, 8 }, 2));
        // NO dl_close.
    }

    var live_alpha: [4096:0]u8 = undefined;
    _ = snprintf(&live_alpha, 4096, "%s/alpha.dafsa", @as([*:0]const u8, dir));

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        const ia = findRel(db, "alpha");
        try testing.expect(ia >= 0);
        // Recovered EAGERLY: owned, so the gate's live-file branch cannot fire.
        try testing.expectEqual(@as(c_int, 1), relation.rel_is_loaded(db.rels[@intCast(ia)].rel));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 7, 8 }, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        const v1 = db.snap_version;
        var snap_alpha: [4096:0]u8 = undefined;
        _ = snprintf(&snap_alpha, 4096, "%s/snapshots/%u/alpha.dafsa", @as([*:0]const u8, dir), v1);
        try expectDifferentInode(&live_alpha, &snap_alpha);
        // The replayed fact IS in the snapshot (a stale link would drop it).
        var got = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 2), snapshot_query_scan(dir, v1, &db.vcache, "alpha", null, 0, collectCb, &got));
    }
}

test "zerocopy: an UNCHANGED view-backed relation is not rewritten at close" {
    // The view must not turn a read-only session into a write: a relation read
    // through a view stays non-dirty, so dl_close neither compacts nor saves
    // it (mtime unchanged) — the same no-spurious-write property lazyrel has.
    const dir = "/tmp/datalog_zig_zerocopy_nosave";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "alpha", 2));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "alpha", &[2]u32{ 1, 2 }, 2));
    }

    var alpha_path: [4096:0]u8 = undefined;
    _ = snprintf(&alpha_path, 4096, "%s/alpha.dafsa", @as([*:0]const u8, dir));
    const mt_before = testStatMtime(&alpha_path);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "alpha", &[2]u32{ 1, 2 }, 2));
        const ia = findRel(db, "alpha");
        try testing.expectEqual(@as(u8, @intFromEnum(relation.ReadState.view)), relation.rel_read_state(db.rels[@intCast(ia)].rel));
        try testing.expectEqual(@as(c_int, 0), relation.rel_is_dirty(db.rels[@intCast(ia)].rel));
    }

    try testing.expectEqual(mt_before, testStatMtime(&alpha_path));
}

test "dl query: rule load + compile + query" {
    const dir = "/tmp/datalog_zig_u11_query";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "edge", 2));
    const e1 = [2]u32{ 1, 2 };
    const e2 = [2]u32{ 2, 3 };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &e1, 2));
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &e2, 2));

    try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "path(X,Y) :- edge(X,Y). path(X,Y) :- edge(X,Z), path(Z,Y)."));
    try testing.expectEqual(@as(c_int, 0), dl_compile(db));

    var got = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 3), dl_query(db, "path", collectCb, &got));
    try testing.expectEqual(@as(usize, 3), got.n);
}

test "dl txn commit + rollback" {
    const dir = "/tmp/datalog_zig_u11_txn";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "t", 2));

    // rollback discards
    try testing.expectEqual(@as(c_int, 0), dl_txn_begin(db));
    const a = [2]u32{ 1, 1 };
    try testing.expectEqual(@as(c_int, 0), dl_txn_add_fact(db, "t", &a, 2));
    try testing.expectEqual(@as(c_int, 0), dl_txn_rollback(db));
    try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "t", &a, 2));

    // commit applies atomically
    try testing.expectEqual(@as(c_int, 0), dl_txn_begin(db));
    const b = [2]u32{ 2, 2 };
    try testing.expectEqual(@as(c_int, 0), dl_txn_add_fact(db, "t", &b, 2));
    try testing.expectEqual(@as(c_int, 0), dl_txn_commit(db));
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "t", &b, 2));

    // CAS round-trip
    try testing.expectEqual(@as(c_int, 0), dl_cas_revision(db, "ent", 0, 5));
    var rev: u32 = 0;
    try testing.expectEqual(@as(c_int, 0), dl_rev_get(db, "ent", &rev));
    try testing.expectEqual(@as(u32, 5), rev);
}

test "publish inherits symbols.array in O(1) unless the vocabulary changed" {
    const dir = "/tmp/datalog_zig_u3_symgate";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    var db_closed = false;
    defer if (!db_closed) dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
    const pairs = [_][2][]const u8{ .{ "alice", "nyc" }, .{ "bob", "la" } };
    for (pairs) |p| {
        const ids = [2]u32{ intern_str(db.ir, p[0].ptr), intern_str(db.ir, p[1].ptr) };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &ids, 2));
    }

    // v1: no previous snapshot -> full write of symbols.array.
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
    // v2: nothing interned since v1 -> the array is INHERITED (O(1)).
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));

    const P = struct {
        fn array(buf: *[512:0]u8, db_dir: [*:0]const u8, v: u32) [*:0]const u8 {
            return std.fmt.bufPrintZ(buf, "{s}/snapshots/{d}/symbols.array", .{ std.mem.span(db_dir), v }) catch unreachable;
        }
        fn sdir(buf: *[512:0]u8, db_dir: [*:0]const u8, v: u32) [*:0]const u8 {
            return std.fmt.bufPrintZ(buf, "{s}/snapshots/{d}", .{ std.mem.span(db_dir), v }) catch unreachable;
        }
    };

    var pa: [5][512:0]u8 = @splat(undefined);
    const a1 = P.array(&pa[0], dir, 1);
    const a2 = P.array(&pa[1], dir, 2);
    const a3 = P.array(&pa[2], dir, 3);
    const a4 = P.array(&pa[3], dir, 4);
    const a5 = P.array(&pa[4], dir, 5);

    // The gate's whole point: v2's array IS v1's bytes — proven by INODE
    // identity (never rewritten) and by byte equality.
    {
        var s1: Stat = undefined;
        var s2: Stat = undefined;
        try testing.expectEqual(@as(c_int, 0), stat(a1, &s1));
        try testing.expectEqual(@as(c_int, 0), stat(a2, &s2));
        try testing.expectEqual(s1.st_ino, s2.st_ino);
        try testing.expectEqual(s1.st_dev, s2.st_dev);
    }
    const bytes1 = readFileAlloc(a1);
    defer c.free(@ptrCast(bytes1.ptr));
    const bytes2 = readFileAlloc(a2);
    defer c.free(@ptrCast(bytes2.ptr));
    try testing.expectEqualSlices(u8, bytes1, bytes2);

    // Both manifests stamp the same (count, bytes, hash), and the stamp the
    // gate reads back from disk equals the live interner's.
    var ps1: [512:0]u8 = undefined;
    var ps2: [512:0]u8 = undefined;
    var stamp1: SymStampOnDisk = undefined;
    var stamp2: SymStampOnDisk = undefined;
    try testing.expect(readSymStamp(P.sdir(&ps1, dir, 1), &stamp1));
    try testing.expect(readSymStamp(P.sdir(&ps2, dir, 2), &stamp2));
    try testing.expectEqual(@as(u32, 4), stamp1.count); // alice nyc bob la
    try testing.expectEqual(stamp1.count, stamp2.count);
    try testing.expectEqual(stamp1.bytes, stamp2.bytes);
    try testing.expectEqual(@as(u64, bytes1.len), stamp1.bytes);
    try testing.expectEqual(stamp1.h0, stamp2.h0);
    try testing.expectEqual(stamp1.h1, stamp2.h1);
    const live = intern_mod.symStamp(db.ir);
    try testing.expectEqual(stamp1.h0, live.h0);
    try testing.expectEqual(stamp1.h1, live.h1);

    // Read-only resolution of the INHERITED snapshot's own array: every id
    // resolves to the string the live db holds.
    {
        const snap_ir = intern_load(null, a2) orelse return error.TestUnexpectedResult;
        defer intern_free(snap_ir);
        try testing.expectEqual(@as(u32, 4), intern_mod.liveSymBound(snap_ir) - 1);
        try testing.expectEqualStrings("alice", std.mem.span(intern_str_of(snap_ir, intern_str_find(db.ir, "alice")).?));
        try testing.expectEqualStrings("la", std.mem.span(intern_str_of(snap_ir, intern_str_find(db.ir, "la")).?));
        var i: u32 = 1;
        while (i <= 4) : (i += 1) {
            try testing.expectEqualStrings(
                std.mem.span(intern_str_of(db.ir, i).?),
                std.mem.span(intern_str_of(snap_ir, i).?),
            );
        }
    }

    // A TRUNCATED previous array must NOT be inherited even though its recorded
    // COUNT still matches (the count lives in the manifest; the size does not).
    {
        const s2 = readFileAlloc(a2);
        defer c.free(@ptrCast(s2.ptr));
        try testing.expect(s2.len > 8);
        const wf = c.fopen(a2, "w") orelse return error.TestUnexpectedResult;
        try testing.expectEqual(s2.len - 6, c.fwrite(s2.ptr, 1, s2.len - 6, wf));
        try testing.expectEqual(@as(c_int, 0), c.fclose(wf));
    }
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v3
    {
        // A fresh write of the SAME vocabulary: different inode, same bytes.
        var s2: Stat = undefined;
        var s3: Stat = undefined;
        try testing.expectEqual(@as(c_int, 0), stat(a2, &s2));
        try testing.expectEqual(@as(c_int, 0), stat(a3, &s3));
        try testing.expect(s2.st_ino != s3.st_ino);
        const bytes3 = readFileAlloc(a3);
        defer c.free(@ptrCast(bytes3.ptr));
        try testing.expectEqualSlices(u8, bytes1, bytes3);
        const snap_ir = intern_load(null, a3) orelse return error.TestUnexpectedResult;
        defer intern_free(snap_ir);
        try testing.expectEqual(@as(u32, 4), intern_mod.liveSymBound(snap_ir) - 1);
        try testing.expectEqual(@as(u32, 3), intern_str_find(snap_ir, "bob"));
    }

    // v4: a NEW symbol -> count/hash move -> full write, fresh inode, and the
    // new symbol resolves in the snapshot's array.
    const carol = intern_str(db.ir, "carol");
    const ids3 = [2]u32{ carol, intern_str(db.ir, "sf") };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &ids3, 2));
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
    {
        var s3: Stat = undefined;
        var s4: Stat = undefined;
        try testing.expectEqual(@as(c_int, 0), stat(a3, &s3));
        try testing.expectEqual(@as(c_int, 0), stat(a4, &s4));
        try testing.expect(s3.st_ino != s4.st_ino);
    }
    const bytes4 = readFileAlloc(a4);
    defer c.free(@ptrCast(bytes4.ptr));
    try testing.expect(bytes4.len > bytes1.len);
    try testing.expect(!std.mem.eql(u8, bytes1, bytes4));
    const bytes4_copy = c.malloc(bytes4.len) orelse return error.OutOfMemory;
    defer c.free(bytes4_copy);
    @memcpy(@as([*]u8, @ptrCast(bytes4_copy))[0..bytes4.len], bytes4);
    {
        const snap_ir = intern_load(null, a4) orelse return error.TestUnexpectedResult;
        defer intern_free(snap_ir);
        try testing.expectEqual(@as(u32, 6), intern_mod.liveSymBound(snap_ir) - 1);
        try testing.expectEqualStrings("carol", std.mem.span(intern_str_of(snap_ir, carol).?));
        try testing.expectEqualStrings("sf", std.mem.span(intern_str_of(snap_ir, 6).?));
        try testing.expectEqual(carol, intern_str_find(snap_ir, "carol"));
    }

    // THE FAILURE MODE THE STAMP EXISTS TO CATCH: a db-root symbols.array that
    // carries the right COUNT but different bytes (a restored/copied/edited
    // root array — the interner loads from the root, not from the snapshot).
    // A count-only gate would inherit the stale snapshot bytes here and publish
    // a snapshot in which every symbol id resolves to the WRONG string.
    {
        var pr: [512:0]u8 = undefined;
        const root_path = std.fmt.bufPrintZ(&pr, "{s}/symbols.array", .{dir}) catch unreachable;
        const raw = readFileAlloc(root_path);
        defer c.free(@ptrCast(raw.ptr));
        const at = std.mem.indexOf(u8, raw, "bob") orelse return error.TestUnexpectedResult;
        raw[at] = 'z'; // same length, different content: st_size unchanged
        const wf = c.fopen(root_path, "w") orelse return error.TestUnexpectedResult;
        try testing.expectEqual(raw.len, c.fwrite(raw.ptr, 1, raw.len, wf));
        try testing.expectEqual(@as(c_int, 0), c.fclose(wf));
    }
    dl_close(db);
    db_closed = true;
    {
        const db2 = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db2);
        try testing.expectEqual(@as(u32, 6), intern_mod.liveSymBound(db2.ir) - 1);
        try testing.expectEqualStrings("zob", std.mem.span(intern_str_of(db2.ir, 3).?));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db2)); // v5
        const bytes5 = readFileAlloc(a5);
        defer c.free(@ptrCast(bytes5.ptr));
        // NOT the stale v4 bytes: the gate fell back and wrote the live array.
        try testing.expect(!std.mem.eql(u8, @as([*]const u8, @ptrCast(bytes4_copy))[0..bytes4.len], bytes5));
        try testing.expectEqual(bytes4.len, bytes5.len);
        try testing.expect(std.mem.indexOf(u8, bytes5, "zob") != null);
        try testing.expect(std.mem.indexOf(u8, bytes5, "carol") != null);
        // ... and the snapshot resolves exactly what the live db holds.
        const snap_ir = intern_load(null, a5) orelse return error.TestUnexpectedResult;
        defer intern_free(snap_ir);
        var i: u32 = 1;
        while (i <= 6) : (i += 1) {
            try testing.expectEqualStrings(
                std.mem.span(intern_str_of(db2.ir, i).?),
                std.mem.span(intern_str_of(snap_ir, i).?),
            );
        }
    }
}

test "publish gate: a steady publish inherits again right after a full write" {
    // The alternating shape a real workload has (interning a symbol, publish,
    // publish, intern, publish ...): after a FULL write the stamp must be
    // caught up, so the publish that follows it inherits in O(1) instead of
    // re-writing the array or falling through to a fresh write.
    const dir = "/tmp/datalog_zig_u3_symgate_alt";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));

    var v: u32 = 0;
    while (v < 6) : (v += 1) {
        const odd = (v % 2) == 1;
        var a = [2]u32{ 0, 0 };
        if (odd) {
            // vocabulary change: a new symbol pair
            var buf: [32]u8 = undefined;
            const s1 = std.fmt.bufPrintZ(&buf, "k{d}", .{v}) catch unreachable;
            a = .{ intern_str(db.ir, s1.ptr), intern_str(db.ir, "const") };
        } else {
            // no vocabulary change: reuse the previous pair
            a = .{ intern_str(db.ir, "k1"), intern_str(db.ir, "const") };
        }
        _ = dl_add_fact(db, "e", &a, 2);
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // version v+1
    }
    // Version map (v = loop counter, version = v+1): v=0 interns k1+const ->
    // version 1 full write; v=1 and v=2 re-intern existing symbols -> versions
    // 2 and 3 inherit; v=3 interns k3 -> version 4 full write; v=4 steady ->
    // version 5 inherits; v=5 interns k5 -> version 6 full write.  So (1,2),
    // (2,3) and (4,5) must be the SAME file, and includes the case this test
    // exists for: the publish right after a FULL write inherits again.
    const P = struct {
        fn array(buf: *[512:0]u8, d: [*:0]const u8, ver: u32) [*:0]const u8 {
            return std.fmt.bufPrintZ(buf, "{s}/snapshots/{d}/symbols.array", .{ std.mem.span(d), ver }) catch unreachable;
        }
    };
    var pb: [2][512:0]u8 = @splat(undefined);
    inline for (.{ .{ 1, 2 }, .{ 2, 3 }, .{ 4, 5 } }) |pair| {
        var s1: Stat = undefined;
        var s2: Stat = undefined;
        const p1 = P.array(&pb[0], dir, pair[0]);
        const p2 = P.array(&pb[1], dir, pair[1]);
        try testing.expectEqual(@as(c_int, 0), stat(p1, &s1));
        try testing.expectEqual(@as(c_int, 0), stat(p2, &s2));
        try testing.expectEqual(s1.st_ino, s2.st_ino); // inherited, not rewritten
        const b1 = readFileAlloc(p1);
        defer c.free(@ptrCast(b1.ptr));
        const b2 = readFileAlloc(p2);
        defer c.free(@ptrCast(b2.ptr));
        try testing.expectEqualSlices(u8, b1, b2);
    }
    // ... while the vocabulary-changing publishes each produced their own file.
    var pv: [6][512:0]u8 = @splat(undefined);
    const a6 = P.array(&pv[0], dir, 6);
    const a5 = P.array(&pv[1], dir, 5);
    const b6 = readFileAlloc(a6);
    defer c.free(@ptrCast(b6.ptr));
    const b5 = readFileAlloc(a5);
    defer c.free(@ptrCast(b5.ptr));
    try testing.expect(!std.mem.eql(u8, b5, b6));
    // The snapshot resolves every live symbol.
    const snap_ir = intern_load(null, a6) orelse return error.TestUnexpectedResult;
    defer intern_free(snap_ir);
    const n = intern_mod.liveSymBound(db.ir);
    try testing.expectEqual(n, intern_mod.liveSymBound(snap_ir));
    var i: u32 = 1;
    while (i < n) : (i += 1) {
        try testing.expectEqualStrings(
            std.mem.span(intern_str_of(db.ir, i).?),
            std.mem.span(intern_str_of(snap_ir, i).?),
        );
    }
}

test "incremental interner save: new-string facts append and resolve after a reopen" {
    // The shape an agent store actually has: every content write interns new
    // strings, and every such write used to pay a whole-file symbols.array
    // rewrite (MEASURED 51 ms per fact on the 45 MB agent store).  The interner
    // save now APPENDS the ids added since the last save — same fsync, O(k).
    const dir = "/tmp/datalog_zig_u3_internappend";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);
    try testing.expectEqual(@as(c_int, 0), mkdir(dir, @as(posix.mode_t, 0o755)));

    // A vocabulary large enough for a whole-file save to be visible, written
    // straight to the db root (interning 40k symbols one at a time would be a
    // linear rev[] scan each).
    {
        var rootbuf: [512:0]u8 = undefined;
        const root = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;
        const f = c.fopen(root, "w") orelse return error.TestUnexpectedResult;
        var linebuf: [96]u8 = undefined;
        var i: u32 = 0;
        while (i < 40000) : (i += 1) {
            const line = std.fmt.bufPrint(&linebuf, "seed_{d}_padding_padding_padding_padding\n", .{i}) catch unreachable;
            try testing.expectEqual(line.len, c.fwrite(line.ptr, 1, line.len, f));
        }
        try testing.expectEqual(@as(c_int, 0), c.fclose(f));
    }

    var rootbuf: [512:0]u8 = undefined;
    const root = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;
    var st0: Stat = undefined;
    try testing.expectEqual(@as(c_int, 0), stat(root, &st0));
    const size0: u64 = @intCast(st0.st_size);

    var names: [3][32]u8 = undefined;
    var ids: [3]u32 = undefined;
    var added_bytes: u64 = 0;
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        var k: u32 = 0;
        while (k < 3) : (k += 1) {
            const s1 = std.fmt.bufPrintZ(&names[k], "newstr_{d}_alpha", .{k}) catch unreachable;
            var idbuf: [32]u8 = undefined;
            const s2 = std.fmt.bufPrintZ(&idbuf, "newstr_{d}_beta", .{k}) catch unreachable;
            const row = [2]u32{ intern_str(db.ir, s1.ptr), intern_str(db.ir, s2.ptr) };
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &row, 2));
            ids[k] = row[0];
            added_bytes += s1.len + 1 + s2.len + 1; // canonical: content + '\n'
        }
        dl_close(db);
    }

    // The three per-fact saves APPENDED: the root array is the same inode it
    // was, grown by exactly the six new lines.  (A whole-file save renames a
    // new inode in — that is what this asserts did NOT happen.)
    var st1: Stat = undefined;
    try testing.expectEqual(@as(c_int, 0), stat(root, &st1));
    try testing.expectEqual(st0.st_ino, st1.st_ino);
    try testing.expectEqual(st0.st_dev, st1.st_dev);
    try testing.expectEqual(size0 + added_bytes, @as(u64, @intCast(st1.st_size)));

    // A REOPEN resolves every id to the string it was assigned: the appended
    // lines are what a fresh process parses.
    {
        const db2 = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db2);
        var k: u32 = 0;
        while (k < 3) : (k += 1) {
            const got = dl_intern_str_of(db2, ids[k]) orelse return error.TestUnexpectedResult;
            try testing.expectEqualStrings(std.mem.span(@as([*:0]const u8, @ptrCast(&names[k]))), std.mem.span(got));
            // and the beta string of the same row is right after it
            const beta = dl_intern_str_of(db2, ids[k] + 1) orelse return error.TestUnexpectedResult;
            try testing.expect(std.mem.startsWith(u8, std.mem.span(beta), "newstr_"));
            try testing.expect(std.mem.endsWith(u8, std.mem.span(beta), "_beta"));
        }
        // The live stamp still describes the whole array: an unchanged publish
    }
}

test "publish: the interner save stays incremental across publishes" {
    // A publishing process touches TWO symbols.array paths — the staged
    // snapshot's and the db root's — so the append bookkeeping keeps one
    // durable-prefix record per path.  A publish must not disarm the root's
    // record, or every fact after a publish would fall back to a 44 MB rewrite.
    const dir = "/tmp/datalog_zig_u3_internappend_pub";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var rootbuf: [512:0]u8 = undefined;
    const root = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;
    var inos: [4]u64 = @splat(0);
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        var k: u32 = 0;
        while (k < 4) : (k += 1) {
            var nbuf: [32]u8 = undefined;
            const s = std.fmt.bufPrintZ(&nbuf, "pubstr_{d}", .{k}) catch unreachable;
            const row = [2]u32{ intern_str(db.ir, s.ptr), intern_str(db.ir, "constant") };
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &row, 2));
            var st: Stat = undefined;
            try testing.expectEqual(@as(c_int, 0), stat(root, &st));
            inos[k] = st.st_ino;
            // A vocabulary-changing publish (full snapshot write) in between.
            try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
        }
    }
    // Every add appended onto the SAME root inode, including the ones that
    // followed a publish — the staged snapshot write did not steal the record.
    try testing.expect(inos[1] == inos[0]);
    try testing.expect(inos[2] == inos[0]);
    try testing.expect(inos[3] == inos[0]);
}

test "dl_snapshot_relations enumerates a published manifest" {
    const dir = "/tmp/datalog_zig_u11_snaprels";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "edge", 2));
    const e1 = [2]u32{ 1, 2 };
    const e2 = [2]u32{ 2, 3 };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &e1, 2));
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &e2, 2));

    // a variadic relation: its '*' marker + per-variant lines must NOT emit
    try testing.expectEqual(@as(c_int, 0), dl_declare_relation_variadic(db, "svc"));
    const s1 = [1]u32{7};
    const s2 = [2]u32{ 7, 8 };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "svc", &s1, 1));
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "svc", &s2, 2));

    // a derived (IDB) head relation, materialized by compile
    try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "path(X,Y) :- edge(X,Y)."));
    try testing.expectEqual(@as(c_int, 0), dl_compile(db));

    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));

    const Rel = struct { name: [64]u8 = undefined, len: usize = 0, arity: u8 = 0, idb: c_int = 0 };
    const Ctx = struct {
        rels: [16]Rel = undefined,
        n: usize = 0,

        fn cb(name: [*c]const u8, arity: u8, idb: c_int, user: ?*anyopaque) callconv(.c) c_int {
            const self: *@This() = @ptrCast(@alignCast(user.?));
            const nm = std.mem.span(@as([*:0]const u8, @ptrCast(name)));
            @memcpy(self.rels[self.n].name[0..nm.len], nm);
            self.rels[self.n].len = nm.len;
            self.rels[self.n].arity = arity;
            self.rels[self.n].idb = idb;
            self.n += 1;
            return 0;
        }

        fn has(self: *const @This(), want: []const u8, arity: u8, idb: c_int) bool {
            for (self.rels[0..self.n]) |r| {
                if (std.mem.eql(u8, r.name[0..r.len], want) and r.arity == arity and r.idb == idb)
                    return true;
            }
            return false;
        }
    };

    var got = Ctx{};
    // edge + path only: 'svc' is variadic (star marker + svc.1/svc.2 skipped)
    try testing.expectEqual(@as(c_long, 2), dl_snapshot_relations(db, 1, Ctx.cb, &got));
    try testing.expectEqual(@as(usize, 2), got.n);
    try testing.expect(got.has("edge", 2, 0));
    try testing.expect(got.has("path", 2, 1));

    // error arms: version 0 / nonexistent version / null cb
    try testing.expectEqual(@as(c_long, -1), dl_snapshot_relations(db, 0, Ctx.cb, &got));
    try testing.expectEqual(@as(c_long, -1), dl_snapshot_relations(db, 99, Ctx.cb, &got));
    try testing.expectEqual(@as(c_long, -1), dl_snapshot_relations(db, 1, null, &got));
}

/// Overwrite a file with exactly `bytes` (tests only).  Builds the divergent
/// db-root symbols.array a restored/copied/edited store presents.
fn writeFileRaw(path: [*c]const u8, bytes: []const u8) bool {
    const wf = c.fopen(path, "w") orelse return false;
    const wrote = c.fwrite(bytes.ptr, 1, bytes.len, wf) == bytes.len;
    return c.fclose(wf) == 0 and wrote;
}

/// Number of symbols a symbols.array holds (one line per symbol).
fn arraySymbolCount(path: [*c]const u8) usize {
    const bytes = readFileAlloc(path);
    if (bytes.len == 0) return 0;
    defer c.free(@ptrCast(bytes.ptr));
    var n: usize = 0;
    for (bytes) |ch| {
        if (ch == '\n') n += 1;
    }
    return n;
}

test "version pattern: a symbol interned after the publish is not in that snapshot's answer" {
    // The VERSION route must answer from the v1 key set.  A symbol interned
    // AFTER v1 (with a row that matches the regex) belongs to no published
    // snapshot: it must not appear in the v1 answer, and it must appear once
    // v2 is published — which also exercises the per-version cache rebuild.
    const dir = "/tmp/datalog_zig_u11_pat_live";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
    const facts = [_][2][]const u8{ .{ "alice", "nyc" }, .{ "bob", "la" }, .{ "carol", "sf" } };
    for (facts) |f| {
        const ids = [2]u32{ intern_str(db.ir, f[0].ptr), intern_str(db.ir, f[1].ptr) };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &ids, 2));
    }
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1

    var snapbuf: [256]u8 = undefined;
    const sdir = std.fmt.bufPrintZ(&snapbuf, "{s}/snapshots/1", .{dir}) catch unreachable;
    var fbuf: [512]u8 = undefined;
    const fpath = std.fmt.bufPrintZ(&fbuf, "{s}/symbols.dafsa", .{sdir}) catch unreachable;
    var st: Stat = undefined;
    try testing.expect(stat(fpath, &st) != 0); // default publish writes no derived file

    const dfa = regexwalk.regex_compile(".*a.*");
    defer regexwalk.regex_dfa_free(dfa);
    try testing.expect(dfa != null and dfa.?.errmsg == null);

    // Unanchored: this walk's DFA accepts only at the NUL terminator, so `^`
    // and `$` never match (MEASURED: "zzz" -> 1 row, "^zzz$" -> 0 on the same
    // store; the pinned golden for `pattern edge "^[12]$"` is "(no results)").
    var v1 = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 2), dl_pattern(db, "e", 0, dfa, collectCb, &v1));

    // Interned AND given a row, but NOT published: matches the regex, at an id
    // the v1 snapshot never assigned.
    const zeta = intern_str(db.ir, "zeta");
    try testing.expect(zeta > 6);
    const zrow = [2]u32{ zeta, 2 };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &zrow, 2));

    // The row IS in the live relation (dl_add_fact returned 1) and must NOT be
    // in the v1 answer.  dl_query is not the live view here: with a snapshot
    // published it routes to the snapshot, so it reports v1's 3 rows either way.
    var v1_again = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 2), dl_pattern(db, "e", 0, dfa, collectCb, &v1_again));
    for (v1_again.rows[0..v1_again.n]) |r| try testing.expect(r[0] != zeta);

    // v2 publishes that symbol: the cache must follow the new version.
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v2
    var v2 = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 3), dl_pattern(db, "e", 0, dfa, collectCb, &v2));
    var seen_zeta = false;
    for (v2.rows[0..v2.n]) |r| {
        if (r[0] == zeta) seen_zeta = true;
    }
    try testing.expect(seen_zeta);
}

test "publish persists the db-root symbols.array, so a reopen cannot reuse an id" {
    // MEASURED BLOCKER (root cause): `intern -> publish -> add -> publish ->
    // close` left the db-root symbols.array SHORTER than the snapshot's, because
    // publish wrote only the snapshot copy and intern_save cleared the dirty
    // flag.  A reopen then restarted next_id below ids the snapshot had already
    // assigned — the same id handed to a different string.
    const dir = "/tmp/datalog_zig_u11_pat_root";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        const first = [2]u32{ intern_str(db.ir, "alice"), intern_str(db.ir, "nyc") };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &first, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1
        const zzz = intern_str(db.ir, "zzz"); // creates the lag
        try testing.expectEqual(@as(u32, 3), zzz);
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v2
        // The interner is clean after a publish, so this add used to skip the
        // root save entirely (dl_add_fact only saves when dirty).
        const zrow = [2]u32{ zzz, first[1] };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &zrow, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v3
    }
    var rootbuf: [512:0]u8 = undefined;
    const root_path = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;
    var snapbuf: [512:0]u8 = undefined;
    const v3_path = std.fmt.bufPrintZ(&snapbuf, "{s}/snapshots/3/symbols.array", .{dir}) catch unreachable;
    try testing.expectEqual(@as(usize, 3), arraySymbolCount(v3_path));
    // THE FIX: the root array covers every id the snapshot assigned.
    try testing.expectEqual(@as(usize, 3), arraySymbolCount(root_path));

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    try testing.expectEqual(@as(u32, 3), intern_str_find(db.ir, "zzz"));
    // ... so no id is handed out twice.
    try testing.expectEqual(@as(u32, 4), intern_str(db.ir, "qqq"));
}

test "version pattern: a lagging db-root array cannot drop a snapshot row" {
    // The blocker's READ half.  The root array is written here by hand to the
    // pre-fix shape (2 symbols) so this test isolates the read path from the
    // publish fix above: whatever the root array says, the v3 answer must be
    // the v3 key set.
    const dir = "/tmp/datalog_zig_u11_pat_lag";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var rootbuf: [512:0]u8 = undefined;
    const root_path = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        const first = [2]u32{ intern_str(db.ir, "alice"), intern_str(db.ir, "nyc") };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &first, 2));
        const zzz = intern_str(db.ir, "zzz");
        try testing.expectEqual(@as(u32, 3), zzz);
        const zrow = [2]u32{ zzz, first[1] };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &zrow, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1
    }
    // The pre-fix on-disk state: the root array stopped at 2 symbols while the
    // snapshot holds 3 (id 3 = "zzz").
    try testing.expect(writeFileRaw(root_path, "alice\nnyc\n"));

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    // The reuse is real: this id is FREE in the reopened interner and is
    // exactly what the live map hands out (the old fallback read it).
    try testing.expectEqual(@as(u32, 3), intern_str(db.ir, "qqq"));

    const dzzz = regexwalk.regex_compile("zzz");
    defer regexwalk.regex_dfa_free(dzzz);
    var got = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 1), dl_pattern(db, "e", 0, dzzz, collectCb, &got));
    try testing.expectEqual(@as(usize, 1), got.n);
    try testing.expectEqual(@as(u32, 3), got.rows[0][0]);
    try testing.expectEqual(@as(u32, 2), got.rows[0][1]);
}

test "version pattern: a same-count, different-bytes root array cannot keep a snapshot row" {
    // The state dl.zig's inherit-gate test constructs (a restored/copied/edited
    // root array).  "the live key set is a superset" is the wrong predicate
    // here: the COUNT is unchanged, but id 3 names another string in each map,
    // so a regex matching the live string KEEPS a row the snapshot does not
    // match, and one matching the snapshot string DROPS it.
    const dir = "/tmp/datalog_zig_u11_pat_diverge";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var rootbuf: [512:0]u8 = undefined;
    const root_path = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        const first = [2]u32{ intern_str(db.ir, "alice"), intern_str(db.ir, "nyc") };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &first, 2));
        const bob = intern_str(db.ir, "bob"); // id 3
        try testing.expectEqual(@as(u32, 3), bob);
        const brow = [2]u32{ bob, first[1] };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &brow, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
    }
    // same length, different bytes: id 3 means "zob" in the root from now on
    try testing.expect(writeFileRaw(root_path, "alice\nnyc\nzob\n"));

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    try testing.expectEqualStrings("zob", std.mem.span(intern_str_of(db.ir, 3).?));

    const dbob = regexwalk.regex_compile("bob"); // the SNAPSHOT's string
    defer regexwalk.regex_dfa_free(dbob);
    var got = CollectCbCtx{};
    // The snapshot's row (1,3) holds id 3, whose snapshot string is "bob":
    // the answer cannot be emptied by what the live root array says.
    try testing.expectEqual(@as(c_long, 1), dl_pattern(db, "e", 0, dbob, collectCb, &got));
    try testing.expectEqual(@as(usize, 1), got.n);

    const dzob = regexwalk.regex_compile("zob"); // the LIVE string
    defer regexwalk.regex_dfa_free(dzob);
    var wrong = CollectCbCtx{};
    // ... and it cannot be widened by it either.
    try testing.expectEqual(@as(c_long, 0), dl_pattern(db, "e", 0, dzob, collectCb, &wrong));
}

test "version read: reuses the LIVE interner only when the stamp proves it identical" {
    // THE PROOF, BOTH WAYS.  snapshotInterner borrows the live interner when the
    // snapshot's manifest stamp and the live content stamp agree on all of
    // (count, bytes, hash) — the duplicate 22.8 ms load `dl query` used to pay
    // on the agent store.  Arm 2 is the shape a COUNT-ONLY gate cannot see: a
    // db-ROOT symbols.array with the same count, the same byte length and
    // different bytes (a restored or hand-edited root — the case 4727cb2 and
    // df22e55 exist for).  Loosening the gate to the count makes arm 2 answer
    // with the live map: `bob` finds nothing and `zob` matches the snapshot's
    // row, exit 0, silently wrong.
    const dir = "/tmp/datalog_zig_u12_symreuse";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var rootbuf: [512:0]u8 = undefined;
    const root_path = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        const first = [2]u32{ intern_str(db.ir, "alice"), intern_str(db.ir, "nyc") };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &first, 2));
        const bob = intern_str(db.ir, "bob"); // id 3
        try testing.expectEqual(@as(u32, 3), bob);
        const brow = [2]u32{ bob, first[1] };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &brow, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1
    }

    // ARM 1 — HEALTHY STORE: root array and v1's array hold the same
    // vocabulary, so the version read must BORROW the live interner.  Without
    // this arm, arm 2 could pass for the wrong reason (a gate that never fires).
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        var sym: u32 = 99;
        try testing.expectEqual(@as(c_int, 0), dl_intern_str_version(db, 1, "bob", &sym));
        try testing.expectEqual(@as(u32, 3), sym);
        try testing.expect(db.snap_sym_ir == db.ir); // reused, not re-parsed
        try testing.expect(db.snap_sym_shared);
        // The pattern path shares it and still answers the snapshot's rows.
        const dbob = regexwalk.regex_compile("bob");
        defer regexwalk.regex_dfa_free(dbob);
        var got = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), dl_pattern(db, "e", 0, dbob, collectCb, &got));
        try testing.expectEqual(@as(usize, 1), got.n);
        try testing.expectEqual(@as(u32, 3), got.rows[0][0]);
        // dl_close must not free the borrowed interner twice (it is the live
        // one); reaching the end of this arm with no crash is that check.
    }

    // ARM 2 — SAME COUNT, SAME BYTE LENGTH, DIFFERENT BYTES.  Only the hash can
    // tell these apart: 3 symbols, 15 bytes, id 3 = "zob" in the root and "bob"
    // in the snapshot.
    try testing.expect(writeFileRaw(root_path, "alice\nnyc\nzob\n"));
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqualStrings("zob", std.mem.span(intern_str_of(db.ir, 3).?));
        var sym: u32 = 99;
        // The version read answers with the SNAPSHOT's id and the SNAPSHOT's
        // name…
        try testing.expectEqual(@as(c_int, 0), dl_intern_str_version(db, 1, "bob", &sym));
        try testing.expectEqual(@as(u32, 3), sym);
        const v3 = dl_intern_str_of_version(db, 1, 3) orelse return error.TestUnexpectedResult;
        try testing.expectEqualStrings("bob", std.mem.span(v3));
        // …because it did NOT borrow the live map.
        try testing.expect(db.snap_sym_ir != db.ir);
        try testing.expect(!db.snap_sym_shared);
        // And the rows agree with the snapshot's own vocabulary, both ways: the
        // snapshot's string matches, the live string does not.
        const dbob = regexwalk.regex_compile("bob");
        defer regexwalk.regex_dfa_free(dbob);
        var got = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), dl_pattern(db, "e", 0, dbob, collectCb, &got));
        try testing.expectEqual(@as(usize, 1), got.n);
        try testing.expectEqual(@as(u32, 3), got.rows[0][0]);
        const dzob = regexwalk.regex_compile("zob");
        defer regexwalk.regex_dfa_free(dzob);
        var wrong = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 0), dl_pattern(db, "e", 0, dzob, collectCb, &wrong));
    }
}

test "publish persists the db-root symbols.array (rule-interned literal path)" {
    // The second, more ordinary repro: a RULE with a string literal interns it
    // via the compiler and nothing saves the root array, so `publish -> close
    // -> reopen` used to restart next_id below the literal's id.
    const dir = "/tmp/datalog_zig_u11_pat_rules";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        const first = [2]u32{ intern_str(db.ir, "alice"), intern_str(db.ir, "nyc") };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &first, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "r", 2));
        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "r(X, \"const\") :- e(X, Y)."));
        const lit = intern_str_find(db.ir, "const");
        try testing.expectEqual(@as(u32, 3), lit);
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v2
    }
    var rootbuf: [512:0]u8 = undefined;
    const root_path = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;
    var snapbuf: [512:0]u8 = undefined;
    const v2_path = std.fmt.bufPrintZ(&snapbuf, "{s}/snapshots/2/symbols.array", .{dir}) catch unreachable;
    try testing.expectEqual(@as(usize, 3), arraySymbolCount(v2_path));
    try testing.expectEqual(@as(usize, 3), arraySymbolCount(root_path));

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    // No id reuse: the reopened interner still knows the literal.
    try testing.expectEqual(@as(u32, 3), intern_str_find(db.ir, "const"));
    try testing.expectEqual(@as(u32, 4), intern_str(db.ir, "fresh"));

    const d = regexwalk.regex_compile("const");
    defer regexwalk.regex_dfa_free(d);
    var got = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 1), dl_pattern(db, "r", 1, d, collectCb, &got));
    try testing.expectEqual(@as(usize, 1), got.n);
    try testing.expectEqual(@as(u32, 3), got.rows[0][1]);
}

test "DL_PUB_DERIVED: derived file, hardlink inheritance and half-link recovery" {
    // The opt-in path is the ONLY thing that can hand a pattern query a
    // snapshot-local symbol map, and it had no coverage at all.  The switch is
    // forced in-process (pubDerivedForce): the env var is read once per
    // process, so setenv from a test would race every other test.
    const P = struct {
        fn file(buf: *[512:0]u8, d: [*:0]const u8, v: u32, name: []const u8) [*:0]const u8 {
            return std.fmt.bufPrintZ(buf, "{s}/snapshots/{d}/{s}", .{ std.mem.span(d), v, name }) catch unreachable;
        }
    };
    const dfa = regexwalk.regex_compile(".*a.*");
    defer regexwalk.regex_dfa_free(dfa);

    // ─── (a) derived file written, then inherited by inode ────────────────
    {
        const dir = "/tmp/datalog_zig_u11_derived_on";
        rmrfTestDir(dir);
        defer rmrfTestDir(dir);

        pubDerivedForce(true);
        defer pubDerivedForce(false);

        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        const facts = [_][2][]const u8{ .{ "alice", "nyc" }, .{ "bob", "la" }, .{ "carol", "sf" } };
        for (facts) |f| {
            const ids = [2]u32{ intern_str(db.ir, f[0].ptr), intern_str(db.ir, f[1].ptr) };
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &ids, 2));
        }

        var pb: [4][512:0]u8 = @splat(undefined);
        const a1 = P.file(&pb[0], dir, 1, "symbols.array");
        const f1 = P.file(&pb[1], dir, 1, "symbols.dafsa");
        const a2 = P.file(&pb[2], dir, 2, "symbols.array");
        const f2 = P.file(&pb[3], dir, 2, "symbols.dafsa");

        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1: full write
        var st: Stat = undefined;
        try testing.expectEqual(@as(c_int, 0), stat(f1, &st));
        try testing.expect(st.st_size > 0);
        var file_arm = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 2), dl_pattern(db, "e", 0, dfa, collectCb, &file_arm));

        // No vocabulary change -> the staged array is byte-identical, so BOTH
        // files are hardlinked from v1 (the inherit branch).
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v2
        var s1: Stat = undefined;
        var s2: Stat = undefined;
        try testing.expectEqual(@as(c_int, 0), stat(a1, &s1));
        try testing.expectEqual(@as(c_int, 0), stat(a2, &s2));
        try testing.expectEqual(s1.st_ino, s2.st_ino);
        try testing.expectEqual(@as(c_int, 0), stat(f1, &s1));
        try testing.expectEqual(@as(c_int, 0), stat(f2, &s2));
        try testing.expectEqual(s1.st_ino, s2.st_ino);
        var file_arm2 = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 2), dl_pattern(db, "e", 0, dfa, collectCb, &file_arm2));
    }

    // ─── (b) half-link recovery ───────────────────────────────────────────
    // Previous snapshot has symbols.array but NO symbols.dafsa (the state every
    // default publish leaves): the branch unlinks the staged array, links the
    // previous one, then fails to link a derived file that is not there — and
    // must re-serialize BOTH rather than publish one half of the pair.
    {
        const dir = "/tmp/datalog_zig_u11_derived_half";
        rmrfTestDir(dir);
        defer rmrfTestDir(dir);

        pubDerivedForce(false);
        {
            const db = dl_open(dir) orelse return error.TestUnexpectedResult;
            defer dl_close(db);
            try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
            const ids = [2]u32{ intern_str(db.ir, "alice"), intern_str(db.ir, "nyc") };
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &ids, 2));
            try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1: array only
        }
        var pb: [6][512:0]u8 = @splat(undefined);
        const a1 = P.file(&pb[0], dir, 1, "symbols.array");
        const f1 = P.file(&pb[1], dir, 1, "symbols.dafsa");
        const a2 = P.file(&pb[2], dir, 2, "symbols.array");
        const f2 = P.file(&pb[3], dir, 2, "symbols.dafsa");
        var st: Stat = undefined;
        try testing.expect(stat(f1, &st) != 0); // default publish wrote none

        pubDerivedForce(true);
        defer pubDerivedForce(false);
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v2, same vocabulary

        var s1: Stat = undefined;
        var s2: Stat = undefined;
        try testing.expectEqual(@as(c_int, 0), stat(f2, &s1)); // the derived half exists
        try testing.expect(s1.st_size > 0);
        try testing.expectEqual(@as(c_int, 0), stat(a2, &s2));
        try testing.expectEqual(@as(c_int, 0), stat(a1, &s1));
        // The array was REWRITTEN (a fresh inode), not the half-linked one...
        try testing.expect(s2.st_ino != s1.st_ino);
        // ... with the same symbols v1 published.
        try testing.expectEqual(@as(usize, 2), arraySymbolCount(a2));
        try testing.expectEqual(@as(usize, 2), arraySymbolCount(a1));
        var got = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), dl_pattern(db, "e", 0, dfa, collectCb, &got));
    }
}

test "DL_PUB_DERIVED: a failing derived save aborts the publish with nothing half-published" {
    // ENOSPC / EACCES on the one optional publish product.  The failure is
    // injected at DL_FPOINT_DERIVED_SAVE (a full filesystem cannot be arranged
    // deterministically in a test) and hits exactly the code path that
    // dafsa_save/intern_save returning -1 does.
    const dir = "/tmp/datalog_zig_u11_derived_fail";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    pubDerivedForce(true);
    defer pubDerivedForce(false);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
    const ids = [2]u32{ intern_str(db.ir, "alice"), intern_str(db.ir, "nyc") };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &ids, 2));
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1

    dl_set_fault_hook(db, rejectDerivedSave, null);
    // A vocabulary change forces the full path (not the inherit branch).
    const extra = [2]u32{ intern_str(db.ir, "zeta"), ids[1] };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &extra, 2));
    try testing.expectEqual(@as(c_int, -1), dl_publish_snapshot(db));

    // Nothing half-published: CURRENT still names v1, no v2 dir, no staged dir.
    var cur: [512:0]u8 = undefined;
    const cur_path = std.fmt.bufPrintZ(&cur, "{s}/snapshots/CURRENT", .{dir}) catch unreachable;
    const cbytes = readFileAlloc(cur_path);
    defer c.free(@ptrCast(cbytes.ptr));
    try testing.expectEqualStrings("1\n", cbytes[0..2]);

    var pb: [3][512:0]u8 = @splat(undefined);
    const v2arr = std.fmt.bufPrintZ(&pb[0], "{s}/snapshots/2/symbols.array", .{dir}) catch unreachable;
    const tmpdir = std.fmt.bufPrintZ(&pb[1], "{s}/snapshots/2.tmp", .{dir}) catch unreachable;
    var st: Stat = undefined;
    try testing.expect(stat(v2arr, &st) != 0);
    try testing.expect(stat(tmpdir, &st) != 0);

    const dfa = regexwalk.regex_compile("alice");
    defer regexwalk.regex_dfa_free(dfa);
    var got = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, 1), dl_pattern(db, "e", 0, dfa, collectCb, &got));

    // Clear the hook: the same publish now succeeds and v2 appears.
    dl_set_fault_hook(db, null, null);
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));
    const v2bytes = readFileAlloc(v2arr);
    defer c.free(@ptrCast(v2bytes.ptr));
    try testing.expect(v2bytes.len > 0);
    const cbytes2 = readFileAlloc(cur_path);
    defer c.free(@ptrCast(cbytes2.ptr));
    try testing.expectEqualStrings("2\n", cbytes2[0..2]);
}

fn rejectDerivedSave(fp: c_int, user: ?*anyopaque) callconv(.c) c_int {
    _ = user;
    return if (fp == DL_FPOINT_DERIVED_SAVE) 1 else 0;
}

test "dl_open_ro never writes" {
    const dir = "/tmp/datalog_zig_u11_ro";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "edge", 2));
        const a = [2]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &a, 2));
    }

    var err: c_int = 0;
    const ro = dl_open_ro(dir, &err) orelse return error.TestUnexpectedResult;
    defer dl_close(ro);
    try testing.expectEqual(@as(c_int, 0), err);
    try testing.expectEqual(@as(c_int, 1), ro.read_only);

    const a = [2]u32{ 1, 2 };
    try testing.expectEqual(@as(c_int, 1), dl_lookup(ro, "edge", &a, 2));

    // write APIs are rejected on the RO handle
    try testing.expectEqual(@as(c_int, -1), dl_declare_relation(ro, "other", 2));
    const b = [2]u32{ 3, 3 };
    try testing.expectEqual(@as(c_int, -1), dl_add_fact(ro, "edge", &b, 2));
    try testing.expectEqual(@as(u32, 0), dl_intern_str(ro, "new-sym"));
}

// ─── dl_fuzzy_lookup: the scan vs the Levenshtein automaton walk ───────────

const FuzzyRec = struct { id: u32, dist: u32 };

const FuzzyRecCtx = struct {
    recs: [64]FuzzyRec = undefined,
    n: usize = 0,
};

fn fuzzyRecCb(sym_id: u32, name: [*c]const u8, name_len: usize, dist: u32, user: ?*anyopaque) callconv(.c) c_int {
    _ = name;
    _ = name_len;
    const self: *FuzzyRecCtx = @ptrCast(@alignCast(user.?));
    self.recs[self.n] = .{ .id = sym_id, .dist = dist };
    self.n += 1;
    return 0;
}

/// The walk reports ids only; the distance is recomputed from the name.
fn fuzzyWalkCb(sym_id: u32, user: ?*anyopaque) callconv(.c) c_int {
    const self: *FuzzyRecCtx = @ptrCast(@alignCast(user.?));
    self.recs[self.n] = .{ .id = sym_id, .dist = 0 };
    self.n += 1;
    return 0;
}

fn fuzzyRecLess(_: void, a: FuzzyRec, b: FuzzyRec) bool {
    return a.id < b.id;
}

/// Distance recorded for `name`, or null when it is not among the records.
fn fuzzyRecDist(ctx: *const FuzzyRecCtx, db: ?*DlDb, name: [*:0]const u8) ?u32 {
    const id = intern_str_find(db.?.ir, name);
    if (id == 0) return null;
    for (ctx.recs[0..ctx.n]) |r| {
        if (r.id == id) return r.dist;
    }
    return null;
}

test "dl_fuzzy_lookup: scan agrees with the lev automaton walk, id+dist for id+dist" {
    const dir = "/tmp/datalog_zig_u11_fuzzy";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    // Boundary corpus for needle "abcdef" (m = 6): m-1 and m+1 are distance 1,
    // m-2 and m+2 are distance 2, a transposition is 2, "abcdefghij" is 4 —
    // the first two are exactly the edges a LENGTH/DEPTH bound could miscut.
    const corpus = [_][*:0]const u8{
        "abcdef", "abcde", "abcdefg", "abcdefgh", "abcdefghij", "abcd",
        "abcdeg", "abxdef", "xabcdef", "abdcef",   "Abcdef",     "zqxjvw",
        "alice",  "alise", "alica",   "bob",       "a",          "ab",
        "abc",    "abcdefghijklmnop",
    };
    for (corpus) |s| _ = intern_str(db.ir, s);

    // The walk arm needs the forward DAFSA, which the scan deliberately never
    // builds — building it here is what lets the two mechanisms be compared.
    const fwd = intern_fwd(db.ir) orelse return error.TestUnexpectedResult;

    const needles = [_][*:0]const u8{ "abcdef", "abcde", "alice", "ab", "zqxjvw" };
    for (needles) |needle| {
        const m = strlen(needle);
        for ([_]u32{ 0, 1, 2 }) |k| {
            // ── arm A: the production scan ──
            var scan = FuzzyRecCtx{};
            const ns = dl_fuzzy_lookup(db, needle, k, fuzzyRecCb, &scan);
            try testing.expectEqual(@as(c_long, @intCast(scan.n)), ns);

            // ── arm B: the Levenshtein automaton product walk over the DAFSA ──
            var walk = FuzzyRecCtx{};
            const dfa = regexwalk.lev_compile(needle, k) orelse return error.TestUnexpectedResult;
            defer regexwalk.regex_dfa_free(dfa);
            try testing.expect(dfa.errmsg == null);
            _ = symbols_dfa_walk(fwd, dfa, fuzzyWalkCb, &walk);
            for (walk.recs[0..walk.n]) |*r| {
                const nm = intern_str_of(db.ir, r.id) orelse return error.TestUnexpectedResult;
                r.dist = regexwalk.lev_distance_cap(needle, m, nm, strlen(nm), k);
            }

            // Same matches, same distances — set equality, no order dependence.
            std.mem.sort(FuzzyRec, scan.recs[0..scan.n], {}, fuzzyRecLess);
            std.mem.sort(FuzzyRec, walk.recs[0..walk.n], {}, fuzzyRecLess);
            try testing.expectEqualSlices(FuzzyRec, walk.recs[0..walk.n], scan.recs[0..scan.n]);

            // …and the boundary cases really are in the answer.
            if (std.mem.eql(u8, std.mem.span(needle), "abcdef")) {
                try testing.expectEqual(@as(?u32, 0), fuzzyRecDist(&scan, db, "abcdef"));
                switch (k) {
                    0 => try testing.expectEqual(@as(?u32, null), fuzzyRecDist(&scan, db, "abcde")),
                    1 => {
                        try testing.expectEqual(@as(?u32, 1), fuzzyRecDist(&scan, db, "abcde")); // len m-1
                        try testing.expectEqual(@as(?u32, 1), fuzzyRecDist(&scan, db, "abcdefg")); // len m+1
                        try testing.expectEqual(@as(?u32, 1), fuzzyRecDist(&scan, db, "abcdeg"));
                        try testing.expectEqual(@as(?u32, 1), fuzzyRecDist(&scan, db, "Abcdef"));
                        try testing.expectEqual(@as(?u32, null), fuzzyRecDist(&scan, db, "abcdefgh"));
                        try testing.expectEqual(@as(?u32, null), fuzzyRecDist(&scan, db, "abcd")); // len m-2
                    },
                    else => {
                        try testing.expectEqual(@as(?u32, 2), fuzzyRecDist(&scan, db, "abcdefgh")); // len m+2
                        try testing.expectEqual(@as(?u32, 2), fuzzyRecDist(&scan, db, "abcd")); // len m-2
                        try testing.expectEqual(@as(?u32, 2), fuzzyRecDist(&scan, db, "abdcef")); // transposition
                        try testing.expectEqual(@as(?u32, 1), fuzzyRecDist(&scan, db, "abcde"));
                        try testing.expectEqual(@as(?u32, null), fuzzyRecDist(&scan, db, "abcdefghij"));
                    },
                }
            }
        }
    }

    // Refusals are the lev_compile contract, kept by the scan.
    const cb = fuzzyRecCb;
    try testing.expectEqual(@as(c_long, -1), dl_fuzzy_lookup(db, "alice", 3, cb, null));
    try testing.expectEqual(@as(c_long, -1), dl_fuzzy_lookup(null, "alice", 1, cb, null));
    try testing.expectEqual(@as(c_long, -1), dl_fuzzy_lookup(db, null, 1, cb, null));
    try testing.expectEqual(@as(c_long, -1), dl_fuzzy_lookup(db, "alice", 1, null, null));
    try testing.expectEqual(@as(c_long, -1), dl_fuzzy_lookup(db, "", 1, cb, null));
    {
        var long_needle: [600:0]u8 = undefined;
        @memset(long_needle[0..600], 'a');
        try testing.expectEqual(@as(c_long, -1), dl_fuzzy_lookup(db, &long_needle, 1, cb, null));
        long_needle[512] = 0; // the cap is inclusive: 512 is accepted
        var ctx2 = FuzzyRecCtx{};
        try testing.expectEqual(@as(c_long, 0), dl_fuzzy_lookup(db, &long_needle, 0, cb, &ctx2));
    }
}

test "dl_intern_str_version: lookup-only, explicit errors, and never the live map" {
    // The CONTRACT half of the version-interning entry point: it resolves in
    // the snapshot's own vocabulary, mints nothing, and treats a damaged
    // snapshot as an error rather than as an empty vocabulary (the shape that
    // would answer "no rows" for a term the snapshot does contain).
    const dir = "/tmp/datalog_zig_verintern_api";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var snapbuf: [512:0]u8 = undefined;
    const snap_arr = std.fmt.bufPrintZ(&snapbuf, "{s}/snapshots/1/symbols.array", .{dir}) catch unreachable;

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        try testing.expectEqual(@as(u32, 1), intern_str(db.ir, "blue"));
        try testing.expectEqual(@as(u32, 2), intern_str(db.ir, "nyc"));
        const row = [2]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &row, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1

        var sym: u32 = 99;
        // version 0 IS the live path: rejected, and *sym_out untouched.
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 0, "blue", &sym));
        try testing.expectEqual(@as(u32, 99), sym);
        // NULL arguments.
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(null, 1, "blue", &sym));
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, null, &sym));
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "blue", null));
        // A nonexistent version (no snapshot dir, so no vocabulary to read).
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 99, "blue", &sym));

        // The snapshot's id, in that snapshot's vocabulary.
        try testing.expectEqual(@as(c_int, 0), dl_intern_str_version(db, 1, "blue", &sym));
        try testing.expectEqual(@as(u32, 1), sym);

        // LOOKUP-ONLY: a term the snapshot lacks resolves to the not-found
        // sentinel, and NOTHING was minted in the live interner — so the next
        // live id is still 3.
        try testing.expectEqual(@as(c_int, 0), dl_intern_str_version(db, 1, "quiz", &sym));
        try testing.expectEqual(@as(u32, 0), sym);
        try testing.expectEqual(@as(u32, 0), intern_str_find(db.ir, "quiz"));
        try testing.expectEqual(@as(u32, 3), intern_str(db.ir, "quiz"));
        dl_close(db);
    }

    // A snapshot whose symbols.array is GONE is an explicit error — not an
    // absent term and not an empty vocabulary.
    try testing.expect(posix.unlink(snap_arr) == 0);
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        var sym: u32 = 99;
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "blue", &sym));
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "quiz", &sym));
        try testing.expectEqual(@as(u32, 99), sym);
    }
}

test "dl_intern_str_version: an UNREADABLE snapshot array is an error, not an empty vocabulary" {
    // intern_load answers an unopenable path with an EMPTY interner ("no file:
    // start empty"), so a permissions fault would otherwise read as "this
    // snapshot has no symbols" — every term absent, every query silently empty.
    // The entry point probes readability first and reports -1.
    const dir = "/tmp/datalog_zig_verintern_unreadable";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var rootbuf: [512:0]u8 = undefined;
    const root_path = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;
    var snapbuf: [512:0]u8 = undefined;
    const snap_arr = std.fmt.bufPrintZ(&snapbuf, "{s}/snapshots/1/symbols.array", .{dir}) catch unreachable;

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        try testing.expectEqual(@as(u32, 1), intern_str(db.ir, "blue"));
        const row = [2]u32{ 1, 1 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &row, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1
        dl_close(db);
    }
    // root-only copy of the array survives a later publish, so the reopen still
    // loads a live interner; the SNAPSHOT's array is the one made unreadable.
    try testing.expect(arraySymbolCount(root_path) == 1);
    try testing.expect(posix.chmod(snap_arr, 0) == 0);
    defer _ = posix.chmod(snap_arr, 0o644);
    // Root can read anything, so this arm can only discriminate unprivileged.
    if (posix.geteuid() == 0) return error.SkipZigTest;

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    var sym: u32 = 99;
    try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "blue", &sym));
    try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "zilch", &sym));
    try testing.expectEqual(@as(u32, 99), sym);
}

test "dl_intern_str_of_version: id -> name resolves in the SNAPSHOT's map, not the live one" {
    // The MIRROR direction of the version-intern contract.  A version read
    // returns THAT snapshot's sym ids (dl_pattern/dl_query/dl_query_bound filter
    // the snapshot's rows by them), so naming them through the live interner
    // prints the right row under ANOTHER STRING'S NAME with exit 0.  This is the
    // half no test covered: the store is intact, only the db-ROOT array diverges
    // from the snapshot's.
    const dir = "/tmp/datalog_zig_verintern_of";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var rootbuf: [512:0]u8 = undefined;
    const root_path = std.fmt.bufPrintZ(&rootbuf, "{s}/symbols.array", .{dir}) catch unreachable;

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        try testing.expectEqual(@as(u32, 1), intern_str(db.ir, "red"));
        try testing.expectEqual(@as(u32, 2), intern_str(db.ir, "blue"));
        const row = [2]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &row, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1
        dl_close(db);
    }
    // The db-ROOT array now names different strings at the same ids: id 1 is
    // `blue` and id 2 is `red`, while snapshot 1 says id 1 `red`, id 2 `blue`.
    try testing.expect(arraySymbolCount(root_path) == 2);
    try testing.expect(writeFileRaw(root_path, "blue\nred\n"));

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    const live1 = dl_intern_str_of(db, 1) orelse return error.TestUnexpectedResult;
    try testing.expectEqualStrings("blue", std.mem.span(live1));
    // …and the snapshot's own map still says `red` at that id.
    const v1 = dl_intern_str_of_version(db, 1, 1) orelse return error.TestUnexpectedResult;
    try testing.expectEqualStrings("red", std.mem.span(v1));
    const v2 = dl_intern_str_of_version(db, 1, 2) orelse return error.TestUnexpectedResult;
    try testing.expectEqualStrings("blue", std.mem.span(v2));
    // The two maps really disagree — otherwise this test proves nothing.
    try testing.expect(!std.mem.eql(u8, std.mem.span(live1), std.mem.span(v1)));

    // Refusals: the LIVE path (version 0), id 0, an id this snapshot never
    // assigned, a nonexistent version, and NULL db.  A caller printing ids
    // then renders the raw number instead of a name from another map.
    try testing.expect(dl_intern_str_of_version(db, 0, 1) == null);
    try testing.expect(dl_intern_str_of_version(db, 1, 0) == null);
    try testing.expect(dl_intern_str_of_version(db, 1, 99) == null);
    try testing.expect(dl_intern_str_of_version(db, 99, 1) == null);
    try testing.expect(dl_intern_str_of_version(null, 1, 1) == null);
}

test "dl_intern_str_version: a DAMAGED snapshot array is an error, not an empty vocabulary" {
    // PRESENT and READABLE is not the same as INTACT.  A truncated array parses
    // into a SHORT vocabulary (every term past the cut reads as "absent") and a
    // DIRECTORY in its place parses into an EMPTY one (open(2) accepts O_RDONLY
    // on a directory, getline then answers EISDIR) — either way a version query
    // answers "(no results)" with rc 0, which is the silent-wrong shape this
    // module exists to prevent.  snapshotInterner therefore checks the file
    // against the size/count the snapshot's OWN manifest recorded for it:
    // D:symbols:<count>:<bytes>:<h0>:<h1>, the same pair inheritSymbolsArray
    // uses to refuse a "truncated or padded snapshot file".
    const dir = "/tmp/datalog_zig_verintern_damaged";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    var snapbuf: [512:0]u8 = undefined;
    const snap_arr = std.fmt.bufPrintZ(&snapbuf, "{s}/snapshots/1/symbols.array", .{dir}) catch unreachable;
    var sdirbuf: [512:0]u8 = undefined;
    const sdir = std.fmt.bufPrintZ(&sdirbuf, "{s}/snapshots/1", .{dir}) catch unreachable;

    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 2));
        try testing.expectEqual(@as(u32, 1), intern_str(db.ir, "blue"));
        try testing.expectEqual(@as(u32, 2), intern_str(db.ir, "nyc"));
        const row = [2]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &row, 2));
        try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db)); // v1
        dl_close(db);
    }

    // HEALTHY CONTROL: both terms resolve in the snapshot's vocabulary…
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        var sym: u32 = 99;
        try testing.expectEqual(@as(c_int, 0), dl_intern_str_version(db, 1, "blue", &sym));
        try testing.expectEqual(@as(u32, 1), sym);
        try testing.expectEqual(@as(c_int, 0), dl_intern_str_version(db, 1, "nyc", &sym));
        try testing.expectEqual(@as(u32, 2), sym);
    }
    // …and the manifest really does carry the stamp the gate reads.  Without
    // this the gate would be skipped (legacy snapshot) and the damaged arms
    // below would pass for the wrong reason.
    var stamp: SymStampOnDisk = undefined;
    try testing.expect(readSymStamp(sdir, &stamp));
    try testing.expectEqual(@as(u32, 2), stamp.count);
    try testing.expectEqual(fileSize(snap_arr).?, stamp.bytes);

    // Save the intact bytes: the arms below damage the file in place.
    const intact = readFileAlloc(snap_arr);
    defer c.free(@ptrCast(intact.ptr));
    try testing.expect(intact.len != 0);

    // ARM 1 — TRUNCATION (the reviewer's measurement: an 8-line array on a
    // 20-symbol snapshot made every version search answer "(no results)").
    {
        const nl = std.mem.indexOfScalar(u8, intact, '\n') orelse return error.TestUnexpectedResult;
        try testing.expect(writeFileRaw(snap_arr, intact[0 .. nl + 1]));
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        var sym: u32 = 99;
        // The term PAST the cut (absent from the short parse)…
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "nyc", &sym));
        // …and the term still inside it: the file is DAMAGED, not a shorter
        // snapshot, so the whole vocabulary is refused.
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "blue", &sym));
        try testing.expectEqual(@as(u32, 99), sym);
    }

    // The gate tracks the FILE, not the open: restoring the bytes restores the
    // answers (and proves arm 1 failed because of the damage, not the reopen).
    try testing.expect(writeFileRaw(snap_arr, intact));
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        var sym: u32 = 99;
        try testing.expectEqual(@as(c_int, 0), dl_intern_str_version(db, 1, "nyc", &sym));
        try testing.expectEqual(@as(u32, 2), sym);
    }

    // ARM 2 — A DIRECTORY IN PLACE OF THE FILE (fopen succeeds, getline EISDIR,
    // intern_load yields an EMPTY interner: every term absent, silently).
    {
        try testing.expect(posix.unlink(snap_arr) == 0);
        try testing.expect(posix.mkdir(snap_arr, 0o755) == 0);
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        var sym: u32 = 99;
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "blue", &sym));
        try testing.expectEqual(@as(c_int, -1), dl_intern_str_version(db, 1, "nyc", &sym));
        try testing.expectEqual(@as(u32, 99), sym);
    }
    try testing.expect(posix.rmdir(snap_arr) == 0);
}
// ─── Cross-batch stratification battery ───────────────────────────────────
//
// The property: a programme's ANSWERS depend on its rules, not on how the
// caller split them across dl_load_rules calls.  Strata used to be computed
// per batch (compile_rules sees one batch), so two negatively-related heads
// loaded in separate batches landed in the SAME stratum and the non-recursive
// path's stable stratum sort (vm.zig ruleStratumOrder) fell back to batch
// order — a SILENT, order-dependent wrong answer.  e39e204 fixed the
// within-batch half of that; this battery is the cross-batch half.
//
// So every case runs the same rules three ways:
//   (i)   ONE dl_load_rules (the reference answer — also varied by source
//         order, since that must not matter either),
//   (ii)  each rule as its OWN batch, in EVERY permutation,
//   (iii) the two grouped splits 1|rest and rest|1,
// each under BOTH fact-insertion orders, and every run must produce the
// HAND-COMPUTED answer — cross-run agreement alone would pass a uniformly
// wrong evaluator.
//
// PRE-FIX FAILURE (MEASURED by appending this same block to the 94fd4f7 tree
// and running `zig build test` there): case 1 returns r={1,2,4} for the
// r-then-q split (and {2} for q-then-r), case 2 returns t={2} instead of
// {2,4}, case 5 (recursion) closes to 8 of 10, and case 7's unstratifiable
// union is accepted silently instead of refused.

const XFact = struct {
    rel: [*:0]const u8,
    arity: u8,
    cols: []const u32,
    /// Symbol NAMES the row values refer to, interned immediately before this
    /// fact's rows are added — the only way a pattern (`~` / `~~`) atom has
    /// anything to match, since a walk matches symbol names, not ids.  `names`
    /// is asserted to intern in order as 1..n, so a case's expected ids (and
    /// hence its hand-computed answer) cannot drift silently.
    names: ?[]const [*:0]const u8 = null,
};

const XRel = struct {
    rel: [*:0]const u8,
    arity: u8,
};

/// One evaluation: load `batches` (each entry is one dl_load_rules call) over
/// a fresh copy of `facts`, then assert `goal`'s answer EXACTLY — every
/// expected tuple present (dl_lookup) and no others (dl_count), plus the
/// streamed row count as an independent witness.
fn xRun(
    dbpath: [*:0]const u8,
    rels: []const XRel,
    facts: []const XFact,
    reverse_facts: bool,
    batches: []const [:0]const u8,
    goal: [*:0]const u8,
    arity: u8,
    expected_flat: []const u32,
) !void {
    rmrfTestDir(dbpath);
    const db = dl_open(dbpath) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    for (rels) |r| {
        const rc = dl_declare_relation(db, r.rel, r.arity);
        if (rc != 0) std.debug.print("xRun: declare_relation({s}/{d}) -> {d}\n", .{ r.rel, r.arity, rc });
        try testing.expectEqual(@as(c_int, 0), rc);
    }
    for (facts) |f| {
        if (f.names) |names| {
            for (names, 0..) |nm, k| {
                try testing.expectEqual(@as(u32, @intCast(k + 1)), dl_intern_str(db, @constCast(nm)));
            }
        }
        const ntup = f.cols.len / f.arity;
        var k: usize = 0;
        while (k < ntup) : (k += 1) {
            const t = if (reverse_facts) ntup - 1 - k else k;
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, f.rel, f.cols.ptr + t * f.arity, f.arity));
        }
    }
    for (batches) |src| try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, src.ptr));
    try testing.expectEqual(@as(c_int, 0), dl_compile(db));

    const n_exp: usize = expected_flat.len / arity;
    var got = CollectCbCtx{};
    try testing.expectEqual(@as(c_long, @intCast(n_exp)), dl_query(db, goal, collectCb, &got));
    try testing.expectEqual(@as(u64, @intCast(n_exp)), dl_count(db, goal));
    try testing.expectEqual(n_exp, got.n);
    var k: usize = 0;
    while (k < expected_flat.len) : (k += arity) {
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, goal, expected_flat.ptr + k, arity));
    }
}

fn xJoinInto(buf: []u8, rules: []const [*:0]const u8, perm: []const usize) [:0]const u8 {
    var len: usize = 0;
    for (perm, 0..) |pi, k| {
        if (k != 0) {
            buf[len] = '\n';
            len += 1;
        }
        const s = std.mem.span(rules[pi]);
        @memcpy(buf[len .. len + s.len], s);
        len += s.len;
    }
    buf[len] = 0;
    return buf[0..len :0];
}

/// Lexicographic next permutation; false when the sequence is exhausted.
fn xNextPerm(p: []usize) bool {
    if (p.len < 2) return false;
    var i = p.len - 1;
    while (i > 0 and p[i - 1] >= p[i]) : (i -= 1) {}
    if (i == 0) return false;
    var j = p.len - 1;
    while (p[j] <= p[i - 1]) : (j -= 1) {}
    std.mem.swap(usize, &p[i - 1], &p[j]);
    std.mem.reverse(usize, p[i..]);
    return true;
}

fn xBattery(
    dbpath: [*:0]const u8,
    rels: []const XRel,
    facts: []const XFact,
    rules: []const [*:0]const u8,
    goal: [*:0]const u8,
    arity: u8,
    expected_flat: []const u32,
) !void {
    var joinbuf: [4096]u8 = undefined;
    var g1buf: [2048]u8 = undefined;
    var g2buf: [2048]u8 = undefined;
    var batchbuf: [8][1024]u8 = undefined;
    var per: [8][:0]const u8 = undefined;
    var perm: [8]usize = undefined;
    for (0..rules.len) |k| perm[k] = k;

    var runs: usize = 0;
    while (true) {
        const order = perm[0..rules.len];

        const one = [_][:0]const u8{xJoinInto(&joinbuf, rules, order)};
        for ([_]bool{ false, true }) |rev| {
            try xRun(dbpath, rels, facts, rev, &one, goal, arity, expected_flat);
            runs += 1;
        }

        for (order, 0..) |pi, k| {
            const s = std.mem.span(rules[pi]);
            @memcpy(batchbuf[k][0..s.len], s);
            batchbuf[k][s.len] = 0;
            per[k] = batchbuf[k][0..s.len :0];
        }
        for ([_]bool{ false, true }) |rev| {
            try xRun(dbpath, rels, facts, rev, per[0..rules.len], goal, arity, expected_flat);
            runs += 1;
        }

        // Grouped splits — a different batching SHAPE, not just a different
        // order: EVERY cut position, so a 4-rule programme is also run as the
        // 2|2 grouping the two ends do not produce.  Only meaningful with 3+
        // rules (else it repeats (ii)).
        if (rules.len >= 3) {
            var ci: usize = 1;
            while (ci < rules.len) : (ci += 1) {
                const cut: usize = ci;
                const head = [_][:0]const u8{xJoinInto(&g1buf, rules, order[0..cut])};
                const tail = [_][:0]const u8{xJoinInto(&g2buf, rules, order[cut..])};
                const grouped = [2][:0]const u8{ head[0], tail[0] };
                for ([_]bool{ false, true }) |rev| {
                    try xRun(dbpath, rels, facts, rev, &grouped, goal, arity, expected_flat);
                    runs += 1;
                }
            }
        }

        if (!xNextPerm(order)) break;
    }
    // 2 fact orders x (1 single-batch + n per-rule + 2 groupings) is the
    // floor; a battery that ran one configuration would test nothing.
    try testing.expect(runs >= 8);
}

test "dl_load_rules: cross-batch stratified negation is ORDER-INDEPENDENT" {
    // ── Case 1: the reported cross-batch probe.  m = {(1,2),(2,3),(4,4)},
    // s empty, so q(X) :- m(X,Y), !s(Y).  gives q = {1,2,4} and
    // r(X) :- m(X,Y), !q(Y). survives only on Y=3: r = {2}.
    // (q must be DECLARED here: a body predicate that is neither declared nor
    // a head of some batch is "unknown predicate".)
    try xBattery(
        "/tmp/datalog_zig_xbatch_1",
        &[_]XRel{ .{ .rel = "m", .arity = 2 }, .{ .rel = "s", .arity = 1 }, .{ .rel = "q", .arity = 1 }, .{ .rel = "r", .arity = 1 } },
        &[_]XFact{.{ .rel = "m", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 4, 4 } }},
        &[_][*:0]const u8{
            "r(X) :- m(X,Y), !q(Y).",
            "q(X) :- m(X,Y), !s(Y).",
        },
        "r",
        1,
        &[_]u32{2},
    );
    // ... and the same split programme's q, so the case is not passing by
    // evaluating nothing.
    try xBattery(
        "/tmp/datalog_zig_xbatch_1q",
        &[_]XRel{ .{ .rel = "m", .arity = 2 }, .{ .rel = "s", .arity = 1 }, .{ .rel = "q", .arity = 1 }, .{ .rel = "r", .arity = 1 } },
        &[_]XFact{.{ .rel = "m", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 4, 4 } }},
        &[_][*:0]const u8{
            "r(X) :- m(X,Y), !q(Y).",
            "q(X) :- m(X,Y), !s(Y).",
        },
        "q",
        1,
        &[_]u32{ 1, 2, 4 },
    );

    // ── Case 2: a THREE-batch chain — t negates r, r negates q, q negates s.
    // One batch: q = {1,2,4}, r = {2}, t = {2,4}.  Splitting it used to put
    // every head in stratum 1 (each batch saw only its own negation edge).
    try xBattery(
        "/tmp/datalog_zig_xbatch_2",
        &[_]XRel{
            .{ .rel = "m", .arity = 2 },
            .{ .rel = "s", .arity = 1 },
            .{ .rel = "q", .arity = 1 },
            .{ .rel = "r", .arity = 1 },
            .{ .rel = "t", .arity = 1 },
        },
        &[_]XFact{.{ .rel = "m", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 4, 4 } }},
        &[_][*:0]const u8{
            "t(X) :- m(X,Y), !r(Y).",
            "r(X) :- m(X,Y), !q(Y).",
            "q(X) :- m(X,Y), !s(Y).",
        },
        "t",
        1,
        &[_]u32{ 2, 4 },
    );

    // ── Case 3 (e39e204's chained case, now split): node = {1,2,3,4},
    // m = {(1,2),(2,3),(3,1)}, banned = {3}
    //   r1 = node \ banned = {1,2,4};  r2 = X of m with r1(Y) = {2};
    //   r3 = node \ r2 = {1,3,4}.
    try xBattery(
        "/tmp/datalog_zig_xbatch_3",
        &[_]XRel{
            .{ .rel = "node", .arity = 1 },
            .{ .rel = "m", .arity = 2 },
            .{ .rel = "banned", .arity = 1 },
            .{ .rel = "r1", .arity = 1 },
            .{ .rel = "r2", .arity = 1 },
            .{ .rel = "r3", .arity = 1 },
        },
        &[_]XFact{
            .{ .rel = "node", .arity = 1, .cols = &[_]u32{ 1, 2, 3, 4 } },
            .{ .rel = "m", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 3, 1 } },
            .{ .rel = "banned", .arity = 1, .cols = &[_]u32{3} },
        },
        &[_][*:0]const u8{
            "r3(X) :- node(X), !r2(X).",
            "r2(X) :- m(X,Y), !r1(Y).",
            "r1(X) :- node(X), !banned(X).",
        },
        "r3",
        1,
        &[_]u32{ 1, 3, 4 },
    );

    // ── Case 4 (e39e204's recursive case, now split): negation ABOVE a
    // recursive stratum.  edge is the chain 1->2->3->4->5, so reach = {(i,j):
    // i<j} (10 tuples) and nonreach = every remaining pair, i.e. a >= b.
    try xBattery(
        "/tmp/datalog_zig_xbatch_4",
        &[_]XRel{
            .{ .rel = "edge", .arity = 2 },
            .{ .rel = "node", .arity = 1 },
            .{ .rel = "reach", .arity = 2 },
            .{ .rel = "nonreach", .arity = 2 },
        },
        &[_]XFact{
            .{ .rel = "edge", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 3, 4, 4, 5 } },
            .{ .rel = "node", .arity = 1, .cols = &[_]u32{ 1, 2, 3, 4, 5 } },
        },
        &[_][*:0]const u8{
            "nonreach(X,Y) :- node(X), node(Y), !reach(X,Y).",
            "reach(X,Y) :- edge(X,Y).",
            "reach(X,Y) :- reach(X,Z), reach(Z,Y).",
        },
        "nonreach",
        2,
        &[_]u32{
            1, 1,
            2, 1, 2, 2,
            3, 1, 3, 2, 3, 3,
            4, 1, 4, 2, 4, 3, 4, 4,
            5, 1, 5, 2, 5, 3, 5, 4, 5, 5,
        },
    );

    // ── Case 5 (e39e204's aggregate case, now split): an AGGREGATE over a
    // negated DERIVED relation.  m = {(1,2),(1,3),(1,4),(2,2)}, aged = {3}
    // so old = {3} and cnt(X,N) :- m(X,Y), !old(Y), N = count() gives
    // {(1,2),(2,1)}.
    try xBattery(
        "/tmp/datalog_zig_xbatch_5",
        &[_]XRel{
            .{ .rel = "m", .arity = 2 },
            .{ .rel = "aged", .arity = 1 },
            .{ .rel = "old", .arity = 1 },
            .{ .rel = "cnt", .arity = 2 },
            .{ .rel = "tot", .arity = 1 },
        },
        &[_]XFact{
            .{ .rel = "m", .arity = 2, .cols = &[_]u32{ 1, 2, 1, 3, 1, 4, 2, 2 } },
            .{ .rel = "aged", .arity = 1, .cols = &[_]u32{3} },
        },
        &[_][*:0]const u8{
            "cnt(X,N) :- m(X,Y), !old(Y), N = count().",
            "tot(N) :- m(X,Y), N = count().",
            "old(X) :- aged(X).",
        },
        "cnt",
        2,
        &[_]u32{ 1, 2, 2, 1 },
    );
}

test "dl_load_rules: cross-batch RECURSION closes to the single-batch answer" {
    // The base rule and the recursive rule in separate batches: the batch that
    // defines the base rule has no cycle for its head, so it compiled
    // is_recursive = 0 and the recursive driver's delta seeding then
    // UNDER-DERIVED (MEASURED pre-fix: 8 of the 10 pairs).  is_recursive is
    // re-derived from the accumulated rules, so both flags now match the
    // single-batch compile.  edge is the chain 1->2->3->4->5.
    try xBattery(
        "/tmp/datalog_zig_xbatch_6",
        &[_]XRel{ .{ .rel = "edge", .arity = 2 }, .{ .rel = "reach", .arity = 2 } },
        &[_]XFact{.{ .rel = "edge", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 3, 4, 4, 5 } }},
        &[_][*:0]const u8{
            "reach(X,Y) :- edge(X,Y).",
            "reach(X,Y) :- reach(X,Z), reach(Z,Y).",
        },
        "reach",
        2,
        &[_]u32{
            1, 2, 1, 3, 1, 4, 1, 5,
            2, 3, 2, 4, 2, 5,
            3, 4, 3, 5,
            4, 5,
        },
    );

    // Mutual recursion split across batches: two heads in one SCC, each batch
    // acyclic on its own.  p = {1,2,3} either way; no batch may drop a cycle
    // member from the fixpoint.
    try xBattery(
        "/tmp/datalog_zig_xbatch_7",
        &[_]XRel{
            .{ .rel = "seed", .arity = 1 },
            .{ .rel = "hop", .arity = 1 },
            .{ .rel = "p", .arity = 1 },
            .{ .rel = "q", .arity = 1 },
        },
        &[_]XFact{
            .{ .rel = "seed", .arity = 1, .cols = &[_]u32{ 1, 2, 3 } },
            .{ .rel = "hop", .arity = 1, .cols = &[_]u32{1} },
        },
        &[_][*:0]const u8{
            "p(X) :- seed(X).",
            "p(X) :- q(X).",
            "q(X) :- p(X), hop(X).",
        },
        "p",
        1,
        &[_]u32{ 1, 2, 3 },
    );
}

test "dl_load_rules: a rule reading a later batch's RECURSION is stratified above it" {
    // NO NEGATION — so this is not only a negation residual.  compute_strata
    // lifts a rule that reads a recursive relation from a different SCC into a
    // strictly higher stratum (the `out_recursive[from] and comp differs` bump
    // in compiler.zig), which a per-batch stratification cannot see.  p was
    // therefore in the SAME stratum as the recursion, ran inside the
    // non-recursive pass loop while reach held only its base tuples, and was
    // never re-fired: MEASURED pre-fix, p = {2} when p's batch came first and
    // {2,3,4} when it came last (the single batch answers {2,3,4}).
    //   edge = 1->2->3->4, start = {1}
    //   reach = {(1,2),(1,3),(1,4),(2,3),(2,4),(3,4)}
    //   p(Y) :- start(X), reach(X,Y)  ->  {2,3,4}
    try xBattery(
        "/tmp/datalog_zig_xbatch_8",
        &[_]XRel{
            .{ .rel = "edge", .arity = 2 },
            .{ .rel = "start", .arity = 1 },
            .{ .rel = "reach", .arity = 2 },
            .{ .rel = "p", .arity = 1 },
        },
        &[_]XFact{
            .{ .rel = "edge", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 3, 4 } },
            .{ .rel = "start", .arity = 1, .cols = &[_]u32{1} },
        },
        &[_][*:0]const u8{
            "p(Y) :- start(X), reach(X,Y).",
            "reach(X,Y) :- edge(X,Y).",
            "reach(X,Y) :- reach(X,Z), reach(Z,Y).",
        },
        "p",
        1,
        &[_]u32{ 2, 3, 4 },
    );
}

test "dl_load_rules: an unstratifiable UNION is REFUSED, not silently evaluated" {
    const dir = "/tmp/datalog_zig_xbatch_uns";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    // The single-batch programme is the negation cycle the compiler refuses.
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "m", 2));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "q", 1));
        try testing.expectEqual(@as(c_int, -1), dl_load_rules(db, "p(X) :- m(X,Y), !q(Y).  q(X) :- m(X,Y), !p(Y)."));
        const msg = compiler.compile_last_error(null);
        try testing.expect(msg != null);
        try testing.expect(std.mem.indexOf(u8, std.mem.span(msg.?), "unstratifiable") != null);
    }

    // Split, the same programme must be refused at the load that closes the
    // cycle — the first batch alone is perfectly stratifiable.
    rmrfTestDir(dir);
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "m", 2));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "q", 1));
        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "p(X) :- m(X,Y), !q(Y)."));
        try testing.expectEqual(@as(c_int, -1), dl_load_rules(db, "q(X) :- m(X,Y), !p(Y)."));
        const msg = compiler.compile_last_error(null);
        try testing.expect(msg != null);
        try testing.expect(std.mem.indexOf(u8, std.mem.span(msg.?), "unstratifiable") != null);
        // The refused batch loaded NOTHING: the first batch is still the whole
        // programme, and it still evaluates.
        const m1 = [2]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "m", &m1, 2));
        try testing.expectEqual(@as(c_int, 0), dl_compile(db));
        var got = CollectCbCtx{};
        try testing.expectEqual(@as(c_long, 1), dl_query(db, "p", collectCb, &got));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "p", &[1]u32{1}, 1));
    }
}

test "dl_load_rules: union recursion keeps the compiler's REJECTS" {
    // cnt is an aggregate rule in batch 1; batch 2 makes cnt recursive.  The
    // single-batch programme is refused ("aggregate in recursive rule"), so the
    // split one must be too — otherwise the union-recursion flag would route a
    // rule through the recursive driver the compiler never lets near it.
    const dir = "/tmp/datalog_zig_xbatch_agg";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "m", 2));
        try testing.expectEqual(@as(c_int, -1), dl_load_rules(db, "cnt(N) :- m(X,Y), N = count().  cnt(X) :- cnt(X)."));
        const msg = compiler.compile_last_error(null);
        try testing.expect(msg != null);
        try testing.expect(std.mem.indexOf(u8, std.mem.span(msg.?), "aggregate in recursive rule") != null);
    }
    rmrfTestDir(dir);
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "m", 2));
        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "cnt(N) :- m(X,Y), N = count()."));
        try testing.expectEqual(@as(c_int, -1), dl_load_rules(db, "cnt(X) :- cnt(X)."));
        const msg = compiler.compile_last_error(null);
        try testing.expect(msg != null);
        try testing.expect(std.mem.indexOf(u8, std.mem.span(msg.?), "aggregate in recursive rule") != null);
    }
}

test "dl_load_rules: a later batch evaluates the ACCUMULATED rules, re-derived" {
    // The contract the fix makes explicit: dl_load_rules ACCUMULATES (a query
    // runs every retained rule), and each evaluation RE-DERIVES from the EDB
    // (vmExecuteMode resets the head views), so no tuple derived under an
    // earlier, differently-stratified programme survives.
    const dir = "/tmp/datalog_zig_xbatch_acc";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "m", 2));
    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "s", 1));
    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "q", 1));
    const f = [_]u32{ 1, 2, 2, 3, 4, 4 };
    var i: usize = 0;
    while (i < 3) : (i += 1)
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "m", f[i * 2 ..].ptr, 2));

    // One rule in the db: q = {1,2,4} (s is empty).
    try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "q(X) :- m(X,Y), !s(Y)."));
    try testing.expectEqual(@as(c_int, 0), dl_compile(db));
    try testing.expectEqual(@as(u64, 3), dl_count(db, "q"));

    // A second batch: BOTH rules now participate, and r is {2} — not the
    // {1,2,4} a per-batch stratification would have produced.
    try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, "r(X) :- m(X,Y), !q(Y)."));
    try testing.expectEqual(@as(c_int, 0), dl_compile(db));
    try testing.expectEqual(@as(u64, 3), dl_count(db, "q"));
    try testing.expectEqual(@as(u64, 1), dl_count(db, "r"));
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "r", &[1]u32{2}, 1));

    // q's derived view is RE-DERIVED when a new base fact arrives: the new
    // (7,8) makes q = {1,2,4,7} and r gains 7 (r keeps 2 — 8 is not in q).
    const nf = [2]u32{ 7, 8 };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "m", &nf, 2));
    try testing.expectEqual(@as(c_int, 0), dl_compile(db));
    try testing.expectEqual(@as(u64, 4), dl_count(db, "q"));
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "q", &[1]u32{7}, 1));
    try testing.expectEqual(@as(u64, 2), dl_count(db, "r"));
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "r", &[1]u32{2}, 1));
    try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "r", &[1]u32{7}, 1));
}

/// Load `batches` in order and assert the LAST one is REFUSED as
/// unstratifiable, copying its diagnostic into `out` (so the caller can
/// compare batchings byte for byte) and returning it.  Also asserts the
/// refusal loaded nothing: the batches already accepted still compile, and
/// they still evaluate over one fact.
fn xUnstratified(
    dbpath: [*:0]const u8,
    rels: []const XRel,
    batches: []const [*:0]const u8,
    goal: [*:0]const u8,
    out: []u8,
) ![]const u8 {
    rmrfTestDir(dbpath);
    const db = dl_open(dbpath) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    for (rels) |r| try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, r.rel, r.arity));

    const last = batches.len - 1;
    for (batches[0..last]) |src| {
        // Every prefix must be STRATIFIABLE — otherwise the case would be
        // testing the wrong thing (a refusal at the wrong load).
        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, src));
    }
    try testing.expectEqual(@as(c_int, -1), dl_load_rules(db, batches[last]));

    const msg = compiler.compile_last_error(null) orelse return error.TestUnexpectedResult;
    const s = std.mem.span(msg);
    try testing.expect(std.mem.indexOf(u8, s, "unstratifiable") != null);
    try testing.expect(s.len <= out.len);
    @memcpy(out[0..s.len], s);

    const m1 = [2]u32{ 1, 2 };
    try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "m", &m1, 2));
    try testing.expectEqual(@as(c_int, 0), dl_compile(db));
    // The accepted prefix derives exactly one row over m = {(1,2)}; a refusal
    // at the FIRST load leaves no rules at all, so nothing is derived.
    try testing.expectEqual(@as(u64, if (last == 0) 0 else 1), dl_count(db, goal));
    return out[0..s.len];
}

test "dl_load_rules: SPLIT == SINGLE for every batching, order and grouping" {
    // THE PROPERTY, stated over the API rather than over one implementation:
    // for any rule set, loading it across 1, 2 or 3 dl_load_rules calls — in
    // any order and grouping — must answer exactly what loading it as ONE
    // batch answers.  xBattery runs every arrangement (the single batch in
    // every source order, each rule as its own batch in every permutation, and
    // the 1|rest / rest|1 grouped splits) under both fact orders and asserts
    // the HAND-COMPUTED answer, so agreement alone cannot pass a uniformly
    // wrong evaluator.
    //
    // Cases E/F/G/H/I are the OP_WALK / OP_MAT_JOIN half of the same property
    // (see each case's comment): a `~`/`~~` pattern atom over a recursive
    // relation was SILENTLY EMPTY (h = {}, and a shorter-than-correct closure)
    // until the walk handler took the driver's idb/delta substitution; a bushy
    // plan was already fed through its leaves and is pinned here so that
    // cannot regress quietly.
    //
    // ── A: the 8ba09a0 blocker — promotion into the recursive driver.
    // s is recursive ONLY in the union (batch A alone has no cycle for s), so
    // compiling batch A alone makes s a NON-LEADING, PARTIALLY-bound body atom
    // (Y bound by b, Z free) over a non-recursive relation: emit_nonleading_join
    // picks OP_HASH_JOIN for it (b has cardinality 0, so b leads the scan).
    // The union stratification then republishes is_recursive = 1 onto batch A's
    // RETAINED rule, routing it through the recursive driver — and that driver
    // substituted its idb/delta tuplesets for body atoms by OP FILTER (SCAN /
    // LOOKUP / LOOKUP_PERM only), so the OP_HASH_JOIN atom kept reading the
    // relation, which is EMPTY for the whole fixpoint (the idb is written back
    // only at the end).  The rule never fired: h silently EMPTY.
    //   g = {(7,2),(8,3)};  b, s and h derived
    //   h(Z) :- b(X,Y), s(Z,Y).   b(X,Y) :- g(X,Y).
    //   s(X,Y) :- g(X,Y).         s(X,Y) :- h(X), g(X,Y).
    // single batch: h = {7,8}.  MEASURED with this test on the 8ba09a0 tree
    // (2da328c): h = {} — empty, not wrong-but-plausible — for every arrangement
    // that compiles the h rule before the s cycle exists.  The same probe on
    // 94fd4f7, and on this tree with the OP_HASH_JOIN filters in vm.zig, gives
    // {7,8} everywhere.
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_hj",
        &[_]XRel{
            .{ .rel = "b", .arity = 2 },
            .{ .rel = "g", .arity = 2 },
            .{ .rel = "h", .arity = 1 },
            .{ .rel = "s", .arity = 2 },
        },
        &[_]XFact{.{ .rel = "g", .arity = 2, .cols = &[_]u32{ 7, 2, 8, 3 } }},
        &[_][*:0]const u8{
            "h(Z) :- b(X,Y), s(Z,Y).",
            "b(X,Y) :- g(X,Y).",
            "s(X,Y) :- g(X,Y).",
            "s(X,Y) :- h(X), g(X,Y).",
        },
        "h",
        1,
        &[_]u32{ 7, 8 },
    );

    // ... and the same programme's s, so case A cannot be passing on an empty
    // programme: s = {(7,2),(8,3)}.
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_hjs",
        &[_]XRel{
            .{ .rel = "b", .arity = 2 },
            .{ .rel = "g", .arity = 2 },
            .{ .rel = "h", .arity = 1 },
            .{ .rel = "s", .arity = 2 },
        },
        &[_]XFact{.{ .rel = "g", .arity = 2, .cols = &[_]u32{ 7, 2, 8, 3 } }},
        &[_][*:0]const u8{
            "h(Z) :- b(X,Y), s(Z,Y).",
            "b(X,Y) :- g(X,Y).",
            "s(X,Y) :- g(X,Y).",
            "s(X,Y) :- h(X), g(X,Y).",
        },
        "s",
        2,
        &[_]u32{ 7, 2, 8, 3 },
    );

    // ── B: cross-batch NEGATION (8ba09a0's own case-1 shape).  m =
    // {(1,2),(2,3),(4,4)}, s empty: q = {1,2,4} and r survives on Y=3 -> {2}.
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_neg",
        &[_]XRel{
            .{ .rel = "m", .arity = 2 },
            .{ .rel = "s", .arity = 1 },
            .{ .rel = "q", .arity = 1 },
            .{ .rel = "r", .arity = 1 },
        },
        &[_]XFact{.{ .rel = "m", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 4, 4 } }},
        &[_][*:0]const u8{
            "r(X) :- m(X,Y), !q(Y).",
            "q(X) :- m(X,Y), !s(Y).",
        },
        "r",
        1,
        &[_]u32{2},
    );

    // ── C: cross-batch RECURSION (base rule and recursive rule split, so no
    // single batch sees the cycle).  edge is 1->2->3->4->5: reach = the 10
    // pairs i<j.
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_rec",
        &[_]XRel{ .{ .rel = "edge", .arity = 2 }, .{ .rel = "reach", .arity = 2 } },
        &[_]XFact{.{ .rel = "edge", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 3, 4, 4, 5 } }},
        &[_][*:0]const u8{
            "reach(X,Y) :- edge(X,Y).",
            "reach(X,Y) :- reach(X,Z), reach(Z,Y).",
        },
        "reach",
        2,
        &[_]u32{
            1, 2, 1, 3, 1, 4, 1, 5,
            2, 3, 2, 4, 2, 5,
            3, 4, 3, 5,
            4, 5,
        },
    );

    // ── C2: MUTUAL recursion across batches — two heads in one SCC, neither
    // batch closing a cycle by itself.  p = {1,2,3} either way; a batch that
    // dropped a cycle member would show here as a short answer.
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_mrec",
        &[_]XRel{
            .{ .rel = "seed", .arity = 1 },
            .{ .rel = "hop", .arity = 1 },
            .{ .rel = "p", .arity = 1 },
            .{ .rel = "q", .arity = 1 },
        },
        &[_]XFact{
            .{ .rel = "seed", .arity = 1, .cols = &[_]u32{ 1, 2, 3 } },
            .{ .rel = "hop", .arity = 1, .cols = &[_]u32{1} },
        },
        &[_][*:0]const u8{
            "p(X) :- seed(X).",
            "p(X) :- q(X).",
            "q(X) :- p(X), hop(X).",
        },
        "p",
        1,
        &[_]u32{ 1, 2, 3 },
    );

    // ── E: the OP_WALK analogue of case A — a `~` PATTERN atom over a relation
    // that is recursive only in the UNION (the driver must substitute its
    // idb/delta for the walked relation AND still apply the sym-set filter).
    // g = {(red=1,7),(blue=2,8)}; the walk keeps only column 0 matching /r.*/.
    //   h(Z) :- b(X,Y), s(Z,Y)~'r.*'.   b(X,Y) :- g(X,Y).
    //   s(X,Y) :- g(X,Y).               s(X,Y) :- h(X), g(X,Y).
    // single batch: s = {(1,7),(2,8)}, walked -> {(1,7)}, join b on Y=7 -> h =
    // {1}.  MEASURED on b66b208 (pre-fix): h = {} in EVERY batching — the walk
    // read the RELATION, empty for the whole fixpoint.  NOT a promotion-only
    // bug: a `~` atom is emitted as OP_WALK whether or not its relation is
    // recursive at compile time, so the SINGLE-batch arm was silently wrong
    // too (it is the split that makes the failure visible as a difference).
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_walk",
        &[_]XRel{
            .{ .rel = "g", .arity = 2 },
            .{ .rel = "b", .arity = 2 },
            .{ .rel = "h", .arity = 1 },
            .{ .rel = "s", .arity = 2 },
        },
        &[_]XFact{.{ .rel = "g", .arity = 2, .cols = &[_]u32{ 1, 7, 2, 8 }, .names = &[_][*:0]const u8{ "red", "blue" } }},
        &[_][*:0]const u8{
            "h(Z) :- b(X,Y), s(Z,Y)~'r.*'.",
            "b(X,Y) :- g(X,Y).",
            "s(X,Y) :- g(X,Y).",
            "s(X,Y) :- h(X), g(X,Y).",
        },
        "h",
        1,
        &[_]u32{1},
    );

    // ── F: a DIRECTLY recursive rule whose recursive atom carries the pattern,
    // no promotion involved.  edge = 1->2->3->4 (names row1..row4), the walk
    // keeps only sources /row[12]/:
    //   reach(X,Y) :- edge(X,Y).
    //   reach(X,Y) :- reach(X,Z)~0 'row[12]', edge(Z,Y).
    // reach = the 6 pairs of {1..4} for sources 1 and 2.  MEASURED on b66b208:
    // {(1,2),(2,3),(3,4)} — the recursive walk matched nothing, so only the
    // base rule's 3 tuples survived (SILENTLY short, no error).
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_walkrec",
        &[_]XRel{ .{ .rel = "edge", .arity = 2 }, .{ .rel = "reach", .arity = 2 } },
        &[_]XFact{.{ .rel = "edge", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 3, 4 }, .names = &[_][*:0]const u8{ "row1", "row2", "row3", "row4" } }},
        &[_][*:0]const u8{
            "reach(X,Y) :- edge(X,Y).",
            "reach(X,Y) :- reach(X,Z)~0 'row[12]', edge(Z,Y).",
        },
        "reach",
        2,
        &[_]u32{
            1, 2, 1, 3, 1, 4,
            2, 3, 2, 4,
            3, 4,
        },
    );

    // ── G: a BUSHY plan (MAT_BEGIN/MAT_JOIN, 4 positive atoms) over a relation
    // that is recursive only in the UNION.  OP_MAT_JOIN is NOT a body-atom op
    // — its operands are pool buffers — so the driver feeds the plan's LEAVES,
    // which OP_MAT_BEGIN reaches by passing its override list into the
    // sub-execution.  g = {(1,7),(2,8)}, m = {1,2}, n = {(7,5),(8,6)}, k = {5}.
    //   q(Z) :- m(X), t(X,Y), n(Y,Z), k(Z).
    //   t(X,Y) :- g(X,Y).   t(X,Y) :- q(X), g(X,Y).
    // q = {5}.  MEASURED on b66b208: {5} in every batching — this shape was
    // already fed (every leaf is a SCAN/LOOKUP/HJOIN), so bushy plans were NOT
    // a silent-wrong path; the test pins that so a later op change cannot
    // break it quietly.
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_bushy",
        &[_]XRel{
            .{ .rel = "g", .arity = 2 },
            .{ .rel = "m", .arity = 1 },
            .{ .rel = "n", .arity = 2 },
            .{ .rel = "k", .arity = 1 },
            .{ .rel = "q", .arity = 1 },
            .{ .rel = "t", .arity = 2 },
        },
        &[_]XFact{
            .{ .rel = "g", .arity = 2, .cols = &[_]u32{ 1, 7, 2, 8 } },
            .{ .rel = "m", .arity = 1, .cols = &[_]u32{ 1, 2 } },
            .{ .rel = "n", .arity = 2, .cols = &[_]u32{ 7, 5, 8, 6 } },
            .{ .rel = "k", .arity = 1, .cols = &[_]u32{5} },
        },
        &[_][*:0]const u8{
            "q(Z) :- m(X), t(X,Y), n(Y,Z), k(Z).",
            "t(X,Y) :- g(X,Y).",
            "t(X,Y) :- q(X), g(X,Y).",
        },
        "q",
        1,
        &[_]u32{5},
    );

    // ── H: the two holes combined — a PATTERN atom as a LEAF inside a bushy
    // plan over a relation recursive only in the UNION.  k = {5,6}, so the
    // walk is what discriminates: t's walk keeps column 0 /r.*/ = {1}, which
    // pairs only with n(7,5) -> q = {5}; an unfed walk (or a dropped sym-set
    // filter) answers {5,6}.
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_bushywalk",
        &[_]XRel{
            .{ .rel = "g", .arity = 2 },
            .{ .rel = "m", .arity = 1 },
            .{ .rel = "n", .arity = 2 },
            .{ .rel = "k", .arity = 1 },
            .{ .rel = "q", .arity = 1 },
            .{ .rel = "t", .arity = 2 },
        },
        &[_]XFact{
            .{ .rel = "g", .arity = 2, .cols = &[_]u32{ 1, 7, 2, 8 }, .names = &[_][*:0]const u8{ "red", "blue" } },
            .{ .rel = "m", .arity = 1, .cols = &[_]u32{ 1, 2 } },
            .{ .rel = "n", .arity = 2, .cols = &[_]u32{ 7, 5, 8, 6 } },
            .{ .rel = "k", .arity = 1, .cols = &[_]u32{ 5, 6 } },
        },
        &[_][*:0]const u8{
            "q(Z) :- m(X), t(X,Y)~0 'r.*', n(Y,Z), k(Z).",
            "t(X,Y) :- g(X,Y).",
            "t(X,Y) :- q(X), g(X,Y).",
        },
        "q",
        1,
        &[_]u32{5},
    );

    // ── I: the FUZZY variant of F (`~~`, the same OP_WALK handler walking a
    // lev automaton), so the admitted-op path is pinned for both pattern
    // constructors: ~~1 'row1' matches row1 (distance 0) and row2 (distance 1),
    // so the closure is F's 6 pairs again.  MEASURED on b66b208: the 3 base
    // tuples only.
    try xBattery(
        "/tmp/datalog_zig_xbatch_prop_walkfuzzy",
        &[_]XRel{ .{ .rel = "edge", .arity = 2 }, .{ .rel = "reach", .arity = 2 } },
        &[_]XFact{.{ .rel = "edge", .arity = 2, .cols = &[_]u32{ 1, 2, 2, 3, 3, 4 }, .names = &[_][*:0]const u8{ "row1", "row2", "row3", "row4" } }},
        &[_][*:0]const u8{
            "reach(X,Y) :- edge(X,Y).",
            "reach(X,Y) :- reach(X,Z)~~1 'row1', edge(Z,Y).",
        },
        "reach",
        2,
        &[_]u32{
            1, 2, 1, 3, 1, 4,
            2, 3, 2, 4,
            3, 4,
        },
    );

    // ── D: an UNSTRATIFIABLE UNION is refused LOUDLY in every batching, with
    // the same diagnostic text, and the refusal loads nothing.  The single
    // batch is the negation cycle the compiler refuses, so no split of it may
    // answer instead.
    {
        const rels = [_]XRel{
            .{ .rel = "m", .arity = 2 },
            .{ .rel = "s", .arity = 1 },
            .{ .rel = "p", .arity = 1 },
            .{ .rel = "q", .arity = 1 },
            .{ .rel = "z", .arity = 1 },
        };
        const a = "p(X) :- m(X,Y), !q(Y).";
        const b = "q(X) :- m(X,Y), !p(X).";
        // ^ p and q negate each other: the cycle.
        const z = "z(X) :- m(X,Y), !s(Y)."; // stratifiable filler for 3 batches
        var refbuf: [512]u8 = undefined;
        var cmpbuf: [512]u8 = undefined;

        // Reference: the single batch, and its mirror image.
        const ref = try xUnstratified("/tmp/datalog_zig_xbatch_prop_uns", &rels, &[_][*:0]const u8{ a, b }, "p", &refbuf);
        const mirror = try xUnstratified("/tmp/datalog_zig_xbatch_prop_uns", &rels, &[_][*:0]const u8{ b, a }, "q", &cmpbuf);
        try testing.expectEqualStrings(ref, mirror);

        // 2 batches, both orders (the split makes the cycle appear only when
        // the second batch is unioned in).  Which head the accepted batch
        // leaves derivable follows the order, so the goal does too.
        {
            const ab = try xUnstratified("/tmp/datalog_zig_xbatch_prop_uns", &rels, &[_][*:0]const u8{ a, b }, "p", &cmpbuf);
            try testing.expectEqualStrings(ref, ab);
            const ba = try xUnstratified("/tmp/datalog_zig_xbatch_prop_uns", &rels, &[_][*:0]const u8{ b, a }, "q", &cmpbuf);
            try testing.expectEqualStrings(ref, ba);
        }

        // 3 batches, the cycle closed at the LAST load and the stratifiable
        // filler in each of the two earlier slots (it cannot follow the cycle:
        // a refusal there would not be at the last load).
        for ([_][3][*:0]const u8{
            .{ z, a, b },
            .{ a, z, b },
        }) |arms| {
            const got = try xUnstratified("/tmp/datalog_zig_xbatch_prop_uns", &rels, &arms, "z", &cmpbuf);
            try testing.expectEqualStrings(ref, got);
        }
    }
}

// ─── Plan parity for a split programme ────────────────────────────────────
//
// Whether a non-leading, partially-bound body atom is emitted as
// OP_LOOKUP_PERM or OP_HASH_JOIN is a COMPILE-TIME decision
// (compiler.zig emit_nonleading_join): a relation that is recursive AT COMPILE
// TIME forces a perm index, otherwise a cost path picks the op.  A batch is
// compiled against its OWN recursive set, so a rule from an earlier batch keeps
// the op its own batch compile chose even after a later batch's union
// stratification makes the relation recursive.  That is the residual 8ba09a0
// left open twice over: the answers agree (the recursive driver feeds both ops,
// vm.driverFeeds), the PLAN does not — and a plan that needs too many perm
// indices is a refusal the single-batch compile makes and the split one cannot.
// Both halves are measured below, on the ops the engine will actually run.

test "dl_load_rules: a union-recompiled perm over a POPULATED idb answers" {
    // The recompile happens at a LOAD, which can be long after the relation it
    // now needs a perm on was derived and filled.  A perm index declared then is
    // built from the relation's CURRENT tuples by the existing dirty-gating
    // (vm.zig permindex_build_dirty), not from the empty relation the earlier
    // compiles saw.  The recursion has to arrive at the LAST load or the batch
    // compile's own recursion carry already plans it (MEASURED: the reverse
    // order of the plan test recompiles, the forward order does not need to).
    //
    //   a(Y)     :- c(Y), bq(X,Y).    batch 1
    //   bq(X,Y)  :- e(X), d(Y).       batch 2  -> bq is POPULATED by a compile
    //   bq(X,Y)  :- e(X), a(Y).       batch 3  -> closes the SCC {a,bq}, the
    //                                              only recursion, so THIS load
    //                                              recompiles a's rule
    // c={1,2} e={1} d={1,2,3} -> bq={(1,1),(1,2),(1,3)} (3 rows), a={1,2}.
    // bq's 3 rows outnumber c's 2, so the re-planned body keeps c first and the
    // bq atom is the non-leading, non-prefix-bound join: the perm carve-out,
    // over a relation that already holds tuples.
    const dir = "/tmp/datalog_zig_xbatch_pop";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const b1 = "a(Y) :- c(Y), bq(X,Y).";
    const b2 = "bq(X,Y) :- e(X), d(Y).";
    const b3 = "bq(X,Y) :- e(X), a(Y).";

    var ops: [32]u8 = undefined;
    var n_ops: usize = 0;
    var perm_rel: c_int = -1;
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "c", 1));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "d", 1));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 1));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "a", 1));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "bq", 2));

        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, b1));
        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, b2));
        const c1 = [_]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "c", &c1[0], 1));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "c", &c1[1], 1));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &[_]u32{1}, 1));
        for ([_]u32{ 1, 2, 3 }) |v|
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "d", &v, 1));
        // Evaluate: bq is now populated (3 rows) and a is derived.
        try testing.expectEqual(@as(c_int, 0), dl_compile(db));
        try testing.expectEqual(@as(u64, 3), dl_count(db, "bq"));
        try testing.expectEqual(@as(u64, 2), dl_count(db, "a"));

        // Batch 3 closes the recursion -> the union recompile runs with bq
        // holding its 3 rows.
        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, b3));
        const ops_seq = xRuleOps(db, "a", &ops);
        n_ops = ops_seq.len;
        const ri = findRel(db, "bq");
        try testing.expect(ri >= 0);
        var i: c_int = 0;
        while (i < db.n_crules) : (i += 1) {
            const cr = db.crules.?[@intCast(i)] orelse continue;
            if (!std.mem.eql(u8, std.mem.span(cr.head_pred.?), "a")) continue;
            var w: c_int = 0;
            while (w < cr.n_instrs) : (w += 1) {
                const in = &cr.instrs.?[@intCast(w)];
                if (in.op == 11) perm_rel = @intCast(in.a);
            }
        }
        // The perm atom must be over `bq` — the POPULATED idb — or this case
        // would be testing a perm over an empty relation instead.
        try testing.expect(n_ops > 0);
        try testing.expectEqual(ri, perm_rel);
        var nb: [256]u8 = undefined;
        std.debug.print("  populated-idb recompile: a ops [{s}], perm atom over bq({d}), bq held 3 rows\n", .{ xOpNames(ops[0..n_ops], &nb), ri });

        try testing.expectEqual(@as(c_int, 0), dl_compile(db));
        try testing.expectEqual(@as(u64, 2), dl_count(db, "a"));
        try testing.expectEqual(@as(u64, 3), dl_count(db, "bq"));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "a", &[_]u32{2}, 1));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "bq", &[_]u32{ 1, 3 }, 2));
    }

    // The single-batch twin answers the same, whatever plan the different card
    // estimates give it.  (A fresh directory: the first block's facts are
    // durable, and a re-add would be a duplicate.)
    rmrfTestDir(dir);
    {
        const db = dl_open(dir) orelse return error.TestUnexpectedResult;
        defer dl_close(db);
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "c", 1));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "d", 1));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 1));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "a", 1));
        try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "bq", 2));
        const c1 = [_]u32{ 1, 2 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "c", &c1[0], 1));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "c", &c1[1], 1));
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &[_]u32{1}, 1));
        for ([_]u32{ 1, 2, 3 }) |v|
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "d", &v, 1));
        var j1: [256]u8 = undefined;
        var j2: [256]u8 = undefined;
        const p12 = xJoinInto(&j1, &[_][*:0]const u8{ b1, b2 }, &[_]usize{ 0, 1 });
        const one = xJoinInto(&j2, &[_][*:0]const u8{ p12, b3 }, &[_]usize{ 0, 1 });
        try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, one.ptr));
        try testing.expectEqual(@as(c_int, 0), dl_compile(db));
        try testing.expectEqual(@as(u64, 2), dl_count(db, "a"));
        try testing.expectEqual(@as(u64, 3), dl_count(db, "bq"));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "a", &[_]u32{2}, 1));
        try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "bq", &[_]u32{ 1, 3 }, 2));
    }
}

fn xOpName(op: u8) []const u8 {
    return switch (op) {
        0 => "HALT",
        1 => "SCAN",
        2 => "LOOKUP",
        3 => "EQ",
        4 => "EQ_CONST",
        5 => "PROJECT",
        7 => "NEG_CHECK",
        8 => "AGG_ACC",
        9 => "AGG_EMIT",
        10 => "WALK",
        11 => "LOOKUP_PERM",
        12 => "HASH_JOIN",
        18 => "MAT_BEGIN",
        19 => "MAT_JOIN",
        else => "?",
    };
}

fn xOpNames(ops: []const u8, buf: []u8) []const u8 {
    var n: usize = 0;
    for (ops, 0..) |op, k| {
        if (k != 0) {
            buf[n] = ',';
            n += 1;
        }
        const s = xOpName(op);
        @memcpy(buf[n..][0..s.len], s);
        n += s.len;
    }
    return buf[0..n];
}

/// The op sequence of the installed compiled rule whose head is `pred` — the
/// program the engine will actually run, not a description of it.
fn xRuleOps(db: *DlDb, pred: [*:0]const u8, buf: []u8) []const u8 {
    var i: c_int = 0;
    while (i < db.n_crules) : (i += 1) {
        const cr = db.crules.?[@intCast(i)] orelse continue;
        if (!std.mem.eql(u8, std.mem.span(cr.head_pred.?), std.mem.span(pred))) continue;
        const n: usize = @intCast(cr.n_instrs);
        for (cr.instrs.?[0..n], 0..) |ins, k| buf[k] = ins.op;
        return buf[0..n];
    }
    return buf[0..0];
}

const XPlan = struct {
    ops: [32]u8 = undefined,
    n_ops: usize = 0,
    perms: c_int = 0,
    count: u64 = 0,
};

/// Load `batches` into a fresh db, read the installed op sequence of the rule
/// headed `goal`, then add the facts and compile.  Rules are loaded BEFORE any
/// fact so every arrangement compiles against the same (empty) relations — the
/// plan is chosen from the compile-time card estimates.
fn xPlanRun(dbpath: [*:0]const u8, batches: []const [:0]const u8, goal: [*:0]const u8) !XPlan {
    rmrfTestDir(dbpath);
    const db = dl_open(dbpath) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "edge", 2));
    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "reach", 2));
    for (batches) |src| try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, src.ptr));

    var out = XPlan{};
    const ops = xRuleOps(db, goal, &out.ops);
    out.n_ops = ops.len;
    out.perms = dl_db_perm_count(db);

    // edge = the chain 1->2->3->4->5
    const f = [_]u32{ 1, 2, 2, 3, 3, 4, 4, 5 };
    var k: usize = 0;
    while (k < 4) : (k += 1)
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &f[k * 2], 2));

    try testing.expectEqual(@as(c_int, 0), dl_compile(db));
    out.count = dl_count(db, goal);
    return out;
}

test "dl_load_rules: split PLAN == single-batch PLAN (perm vs hash join)" {
    //   reach(X,Y) :- edge(X,Y).
    //   reach(X,Y) :- edge(X,Z), reach(Z,Y).
    //   r(X)       :- edge(X,Y), reach(W,Y).      <- r's second atom is the
    //                                               non-leading join over reach
    // As ONE batch, reach is recursive when r is compiled, so the carve-out in
    // emit_nonleading_join forces a perm index.  Split {reach rules} | {r}, r's
    // batch sees no rule for reach at all: reach is non-recursive, the cost
    // path finds no perm and the card estimate is 0 < g_perm_card_threshold,
    // so r was emitted as OP_HASH_JOIN.
    //
    // MEASURED PRE-FIX on 75846fb, ops read out of d.crules:
    //   single batch        -> SCAN,LOOKUP_PERM,PROJECT,HALT   1 perm declared
    //   split fwd  (reach|r) -> SCAN,HASH_JOIN,PROJECT,HALT     0 perms
    //   split rev  (r|reach) -> SCAN,HASH_JOIN,PROJECT,HALT     0 perms
    // with r = {1,2,3,4} in all three (the driver feeds both ops).
    const one = "reach(X,Y) :- edge(X,Y).\nreach(X,Y) :- edge(X,Z), reach(Z,Y).";
    const rd = "r(X) :- edge(X,Y), reach(W,Y).";
    var jbuf: [512]u8 = undefined;
    const single = xJoinInto(&jbuf, &[_][*:0]const u8{ one, rd }, &[_]usize{ 0, 1 });

    const p_single = try xPlanRun("/tmp/datalog_zig_xbatch_plan", &[_][:0]const u8{single}, "r");
    const p_fwd = try xPlanRun("/tmp/datalog_zig_xbatch_plan", &[_][:0]const u8{ one, rd }, "r");
    const p_rev = try xPlanRun("/tmp/datalog_zig_xbatch_plan", &[_][:0]const u8{ rd, one }, "r");

    var nb1: [256]u8 = undefined;
    var nb2: [256]u8 = undefined;
    var nb3: [256]u8 = undefined;
    std.debug.print("  plan: single [{s}] {d} perm(s) / split-fwd [{s}] {d} / split-rev [{s}] {d}\n", .{
        xOpNames(p_single.ops[0..p_single.n_ops], &nb1), p_single.perms,
        xOpNames(p_fwd.ops[0..p_fwd.n_ops], &nb2), p_fwd.perms,
        xOpNames(p_rev.ops[0..p_rev.n_ops], &nb3), p_rev.perms,
    });

    // The fixture must actually exercise the perm carve-out, or the parity
    // assertion below would be vacuous.
    try testing.expect(std.mem.indexOfScalar(u8, p_single.ops[0..p_single.n_ops], 11) != null);
    try testing.expectEqualSlices(u8, p_single.ops[0..p_single.n_ops], p_fwd.ops[0..p_fwd.n_ops]);
    try testing.expectEqualSlices(u8, p_single.ops[0..p_single.n_ops], p_rev.ops[0..p_rev.n_ops]);
    try testing.expectEqual(p_single.perms, p_fwd.perms);
    try testing.expectEqual(p_single.perms, p_rev.perms);

    // r ⊆ edge's first column = {1,2,3,4}, so count 4 pins the answer exactly:
    // every one of 1..4 has an outgoing edge into reach's second column.
    try testing.expectEqual(@as(u64, 4), p_single.count);
    try testing.expectEqual(@as(u64, 4), p_fwd.count);
    try testing.expectEqual(@as(u64, 4), p_rev.count);
}

/// `q(X0) :- b(X0,...,X7), t(<arg0>,...,<arg7>).` where the JOIN columns of the
/// t-atom are exactly the bits set in `mask` and every other column holds a
/// rule-unique variable.  One perm per mask: the perm of a non-leading join
/// atom is its join columns in column order followed by the rest in column
/// order, so the join-column SET is the whole of the choice.
///
/// `mask` must have bit 0 CLEAR: an atom whose bound columns form a leading
/// prefix (k > 0) takes emit_pos_atom's OP_LOOKUP path and needs no perm at all,
/// so a mask with bit 0 set would not exercise the carve-out.
fn xSubsetRule(buf: []u8, idx: usize, mask: u32) []const u8 {
    var n: usize = 0;
    const head = "q(X0) :- b(X0,X1,X2,X3,X4,X5,X6,X7), t(";
    @memcpy(buf[0..head.len], head);
    n = head.len;
    var j: usize = 0;
    while (j < 8) : (j += 1) {
        if (j != 0) {
            buf[n] = ',';
            n += 1;
        }
        var tmp: [16]u8 = undefined;
        const s = if (mask & (@as(u32, 1) << @intCast(j)) != 0)
            std.fmt.bufPrint(&tmp, "X{d}", .{j}) catch unreachable
        else
            std.fmt.bufPrint(&tmp, "z{d}_{d}", .{ idx, j }) catch unreachable;
        @memcpy(buf[n..][0..s.len], s);
        n += s.len;
    }
    buf[n] = ')';
    buf[n + 1] = '.';
    n += 2;
    return buf[0..n];
}

/// Load `batches` in order, assert every PREFIX is accepted (so the refusal is
/// at the load that closes the recursion, not an earlier one) and the LAST one
/// is REFUSED with `want` in its diagnostic — and that the refusal loaded
/// nothing: the accepted prefix still compiles, and the perm table is exactly
/// where it was (the refused programme's perm declarations are its own).
fn xRefused(dbpath: [*:0]const u8, rels: []const XRel, batches: []const [:0]const u8, want: []const u8, union_refusal: bool) !void {
    rmrfTestDir(dbpath);
    const db = dl_open(dbpath) orelse return error.TestUnexpectedResult;
    defer dl_close(db);
    for (rels) |r| try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, r.rel, r.arity));

    const last = batches.len - 1;
    for (batches[0..last]) |src| try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, src.ptr));

    const perms_before = dl_db_perm_count(db);
    try testing.expectEqual(@as(c_int, -1), dl_load_rules(db, batches[last].ptr));
    const msg = compiler.compile_last_error(null) orelse return error.TestUnexpectedResult;
    const s = std.mem.span(msg);
    std.debug.print("  refusal at load {d}: {s}\n", .{ last + 1, s });
    try testing.expect(std.mem.indexOf(u8, s, want) != null);
    // When the UNION recompile is what refuses, it gives its perm indices back:
    // nothing references them once the programme it compiled is freed, and the
    // resident programme's own indices are untouched.  When instead the BATCH
    // compile refuses (which it can, for the orders where the recursion is
    // already resident — compute_strata seeds recursion from the resident
    // rules), the indices it declared before failing are kept, exactly as
    // compile_rules has always behaved.
    if (union_refusal) try testing.expectEqual(perms_before, dl_db_perm_count(db));

    // The refused load added no rule: the accepted prefix is still the whole
    // programme and still compiles.
    try testing.expectEqual(@as(c_int, 0), dl_compile(db));
}

test "dl_load_rules: a split load is REFUSED where the single batch is (perm cap)" {
    // 70 rules whose non-leading join atom is over ONE arity-8 recursive
    // relation, one per DISTINCT join-column subset with column 0 unbound (a
    // bound leading prefix is an OP_LOOKUP, not a perm): 70 distinct
    // (rel, arity, perm) triples against the engine's 64-slot perm table.  As
    // one batch, `t` is recursive when all 70 rules are compiled, the carve-out
    // forces every perm, and the 65th is refused with "too many permutation
    // indices".
    //
    // MEASURED PRE-FIX on 75846fb: loading the same rules as {t rules} | {70
    // rules} was ACCEPTED — the second batch compiled against its own recursive
    // set, `t` was not recursive there, the cost path declared no perm at all
    // (card 0 < 4), so a programme that CANNOT be compiled as one batch ran
    // happily as two.
    const rec = "t(X0,X1,X2,X3,X4,X5,X6,X7) :- b(X0,X1,X2,X3,X4,X5,X6,X7).\n" ++
        "t(A0,A1,A2,A3,A4,A5,A6,A7) :- t(A0,A1,A2,A3,A4,A5,A6,A7).";
    const rels = [_]XRel{
        .{ .rel = "b", .arity = 8 },
        .{ .rel = "t", .arity = 8 },
        .{ .rel = "q", .arity = 1 },
    };

    var sub: [70][160]u8 = undefined;
    var h1: [8192]u8 = undefined;
    var h2: [8192]u8 = undefined;
    var all: [16384]u8 = undefined;
    var n1: usize = 0;
    var n2: usize = 0;
    var na: usize = 0;
    var k: usize = 0;
    while (k < 70) : (k += 1) {
        const s = xSubsetRule(&sub[k], k, @intCast((k + 1) * 2));
        const dst = if (k < 35) &h1 else &h2;
        const at = if (k < 35) &n1 else &n2;
        std.mem.copyForwards(u8, dst[at.*..][0..s.len], s);
        dst[at.* + s.len] = '\n';
        at.* += s.len + 1;
    }
    std.mem.copyForwards(u8, all[0..n1], h1[0..n1]);
    na = n1;
    std.mem.copyForwards(u8, all[na..][0..n2], h2[0..n2]);
    na += n2;
    h1[n1] = 0;
    h2[n2] = 0;
    all[na] = 0;
    const first: [:0]const u8 = h1[0..n1 :0];
    const second: [:0]const u8 = h2[0..n2 :0];
    const both: [:0]const u8 = all[0..na :0];

    var jrec: [512]u8 = undefined;
    var jone: [16384]u8 = undefined;
    const recz = xJoinInto(&jrec, &[_][*:0]const u8{rec}, &[_]usize{0});
    const onez = xJoinInto(&jone, &[_][*:0]const u8{recz, both}, &[_]usize{ 0, 1 });

    // Reference: the single batch is refused.
    try xRefused("/tmp/datalog_zig_xbatch_cap", &rels, &[_][:0]const u8{onez}, "too many permutation indices", false);

    // 2 batches, both orders: refused at the load that closes the recursion.
    // The reverse order is the one the per-batch compile cannot see — the 70
    // perm-needing rules are loaded while `t` has no rule at all, so nothing but
    // the union recompile can refuse them — MEASURED PRE-FIX: accepted.
    try xRefused("/tmp/datalog_zig_xbatch_cap", &rels, &[_][:0]const u8{ recz, both }, "too many permutation indices", false);
    try xRefused("/tmp/datalog_zig_xbatch_cap", &rels, &[_][:0]const u8{ both, recz }, "too many permutation indices", true);

    // 3 batches: every prefix under the cap, the last one over it.
    try xRefused("/tmp/datalog_zig_xbatch_cap", &rels, &[_][:0]const u8{ recz, first, second }, "too many permutation indices", false);
    try xRefused("/tmp/datalog_zig_xbatch_cap", &rels, &[_][:0]const u8{ first, recz, second }, "too many permutation indices", false);
}

test "dl_load_rules: split plan keeps the INCREMENTAL-MAINTENANCE class" {
    // The maintenance class the cascade picks at consolidation is read off the
    // ops (vm.zig vm_ivm_eligible): one OP_HASH_JOIN atom makes a programme
    // IVM-ineligible and it falls back to a full re-evaluate, while an
    // OP_LOOKUP_PERM atom over a recursive head in the SAME stratum is what the
    // fixpoint's perm shadow exists for and stays incremental.  So a plan
    // difference is a maintenance-class difference too, and the split
    // programme must land in the class its single-batch twin lands in.
    //
    //   a(Y)     :- c(Y), bq(X,Y).    <- bq is only partially bound and NOT
    //                                   prefix-bound (col 1 is the join), so it
    //                                   needs a perm; the SCC is {a,bq}
    //   bq(X,Y)  :- e(X), a(Y).
    //   bq(X,Y)  :- e(X), d(Y).
    // c={1,2} d={1} e={1}  ->  a={1}, bq={(1,1)}; then d(2) -> a={1,2},
    // bq={(1,1),(1,2)}.
    const one = "a(Y) :- c(Y), bq(X,Y).\nbq(X,Y) :- e(X), a(Y).\nbq(X,Y) :- e(X), d(Y).";
    const ba = "a(Y) :- c(Y), bq(X,Y).";
    const bb = "bq(X,Y) :- e(X), a(Y).\nbq(X,Y) :- e(X), d(Y).";

    const T = struct {
        fn run(dbpath: [*:0]const u8, batches: []const [:0]const u8, eligible: *c_int, a_count: *u64, bq_count: *u64, a_del: *u64, bq_del: *u64, ops_out: []u8, n_ops: *usize) !void {
            rmrfTestDir(dbpath);
            const db = dl_open(dbpath) orelse return error.TestUnexpectedResult;
            defer dl_close(db);
            try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "c", 1));
            try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "d", 1));
            try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "e", 1));
            try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "a", 1));
            try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "bq", 2));
            // Rules BEFORE the facts: the plan (and so the maintenance class)
            // is chosen at compile time from the card estimates, and every
            // batching must be compared against the same empty relations.
            for (batches) |src| try testing.expectEqual(@as(c_int, 0), dl_load_rules(db, src.ptr));

            const ops = xRuleOps(db, "a", ops_out);
            n_ops.* = ops.len;

            const f1 = [_]u32{ 1, 2 };
            const f2 = [_]u32{1};
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "c", &f1[0], 1));
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "c", &f1[1], 1));
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "d", &f2[0], 1));
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "e", &f2[0], 1));

            try testing.expectEqual(@as(c_int, 0), dl_compile(db));
            try testing.expectEqual(@as(u64, 1), dl_count(db, "a"));
            try testing.expectEqual(@as(u64, 1), dl_count(db, "bq"));
            eligible.* = vm_ivm_eligible(db);

            // A new d fact: bq grows, then a = c ∩ proj(bq) grows, so both end
            // up with two rows.  dl_consolidate runs the maintenance cascade
            // (where the class decides the path) and the counts below are read
            // WITHOUT re-evaluating.
            const d2 = [_]u32{2};
            try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "d", &d2[0], 1));
            try testing.expectEqual(@as(c_int, 0), dl_consolidate(db));
            a_count.* = dl_count(db, "a");
            bq_count.* = dl_count(db, "bq");
            try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "a", &[_]u32{2}, 1));
            try testing.expectEqual(@as(c_int, 1), dl_lookup(db, "bq", &[_]u32{ 1, 2 }, 2));

            // ... and back out again: the DELETE half of the same question
            // (del_pending + the DRed class, which a recursive programme never
            // takes — it falls back to the full re-evaluate).  Everything must
            // return to the pre-insert state.
            try testing.expectEqual(@as(c_int, 1), dl_delete_fact(db, "d", &d2[0], 1));
            try testing.expectEqual(@as(c_int, 0), dl_consolidate(db));
            a_del.* = dl_count(db, "a");
            bq_del.* = dl_count(db, "bq");
            try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "a", &[_]u32{2}, 1));
            try testing.expectEqual(@as(c_int, 0), dl_lookup(db, "bq", &[_]u32{ 1, 2 }, 2));
        }
    };

    var e_single: c_int = -1;
    var e_split: c_int = -1;
    var a_single: u64 = 0;
    var a_split: u64 = 0;
    var bq_single: u64 = 0;
    var bq_split: u64 = 0;
    var ad_single: u64 = 0;
    var ad_split: u64 = 0;
    var bd_single: u64 = 0;
    var bd_split: u64 = 0;
    var ops_single: [32]u8 = undefined;
    var ops_split: [32]u8 = undefined;
    var n_single: usize = 0;
    var n_split: usize = 0;
    try T.run("/tmp/datalog_zig_xbatch_ivm", &[_][:0]const u8{one}, &e_single, &a_single, &bq_single, &ad_single, &bd_single, &ops_single, &n_single);
    try T.run("/tmp/datalog_zig_xbatch_ivm", &[_][:0]const u8{ ba, bb }, &e_split, &a_split, &bq_split, &ad_split, &bd_split, &ops_split, &n_split);

    var nb1: [256]u8 = undefined;
    var nb2: [256]u8 = undefined;
    std.debug.print("  a-rule ops: single [{s}] / split [{s}]; ivm eligible single {d} split {d}; after insert+consolidate a {d}/{d} bq {d}/{d}\n", .{
        xOpNames(ops_single[0..n_single], &nb1),
        xOpNames(ops_split[0..n_split], &nb2),
        e_single,
        e_split,
        a_single,
        a_split,
        bq_single,
        bq_split,
    });
    try testing.expectEqualSlices(u8, ops_single[0..n_single], ops_split[0..n_split]);
    try testing.expectEqual(e_single, e_split);
    try testing.expectEqual(a_single, a_split);
    try testing.expectEqual(bq_single, bq_split);
    try testing.expectEqual(ad_single, ad_split);
    try testing.expectEqual(bd_single, bd_split);
    // The incremental cascade really ran on the split db: both relations grew,
    // and the delete put them back.
    try testing.expectEqual(@as(u64, 2), a_split);
    try testing.expectEqual(@as(u64, 2), bq_split);
    try testing.expectEqual(@as(u64, 1), ad_split);
    try testing.expectEqual(@as(u64, 1), bd_split);
}

// Frame-limit overflow (MAX_FRAMES): the caller-visible contract.  The same
// 9-atom body that the CLI surfaces as "publish failed" must return the
// distinct DL_E_FRAMES code (dl.h) to the C caller — not the bare -1 — with
// the loud diagnostic on stderr; the 8-atom control publishes cleanly.
test "publish surfaces DL_E_FRAMES when a rule body exhausts the frame limit" {
    const dir = "/tmp/datalog_zig_u11_frames";
    rmrfTestDir(dir);
    defer rmrfTestDir(dir);

    const db = dl_open(dir) orelse return error.TestUnexpectedResult;
    defer dl_close(db);

    try testing.expectEqual(@as(c_int, 0), dl_declare_relation(db, "edge", 2));
    // 12-node chain: 11 edges, so the 9th enumerating atom is reached.
    var v: u32 = 1;
    while (v < 12) : (v += 1) {
        const e = [2]u32{ v, v + 1 };
        try testing.expectEqual(@as(c_int, 1), dl_add_fact(db, "edge", &e, 2));
    }

    // Control: ONE atom under the limit — publishes 0.
    try testing.expectEqual(@as(c_int, 0), dl_load_rules(db,
        "h(X,Y):-edge(X,A),edge(A,B),edge(B,C),edge(C,D),edge(D,E),edge(E,F),edge(F,G),edge(G,Y)."));
    try testing.expectEqual(@as(c_int, 0), dl_publish_snapshot(db));

    // Overflow: DL_E_FRAMES (-2), per dl.h.
    try testing.expectEqual(@as(c_int, 0), dl_load_rules(db,
        "h2(X,Y):-edge(X,A),edge(A,B),edge(B,C),edge(C,D),edge(D,E),edge(E,F),edge(F,G),edge(G,H),edge(H,Y)."));
    try testing.expectEqual(@as(c_int, -2), dl_publish_snapshot(db));
    try testing.expectEqual(@as(c_int, -2), dl_compile(db));
}

// ─── Removed-tier ABI shims ────────────────────────────────────────────────
//
// The vector tier was REMOVED (67df9ae): it needs an embedding encoder, and the
// consumers that matter query with the words they wrote, so retrieval is lexical
// (dl_search_top).  Two corpus entry points are kept here ONLY so binaries built
// before the removal still LOAD: the deployed fx-agent-memory links them with
// BIND_NOW, so a missing symbol stops it starting at all.
//
// They do nothing but return -1 — a LOUD error to the caller, never a silent
// wrong answer — and they must be deleted as soon as no built consumer imports
// them (check with `nm -D --undefined-only <binary> | grep dl_vector_`).
// Signatures match the removed vector.h prototypes; the corpus/callback
// parameters are taken as opaque pointers because the engine no longer has the
// types, and a pointer argument's ABI does not depend on its pointee's type.

pub export fn dl_vector_search_corpus(
    db: ?*DlDb,
    corpus: ?*const anyopaque,
    q_int8: ?[*]const u32,
    k: c_int,
    cb: ?*const anyopaque,
    user: ?*anyopaque,
) c_long {
    _ = db;
    _ = corpus;
    _ = q_int8;
    _ = k;
    _ = cb;
    _ = user;
    return -1;
}

pub export fn dl_vector_rerank_corpus(
    db: ?*DlDb,
    corpus: ?*const anyopaque,
    q_int8: ?[*]const u32,
    cand_syms: ?[*]const u32,
    n_cand: c_int,
    k: c_int,
    cb: ?*const anyopaque,
    user: ?*anyopaque,
) c_long {
    _ = db;
    _ = corpus;
    _ = q_int8;
    _ = cand_syms;
    _ = n_cand;
    _ = k;
    _ = cb;
    _ = user;
    return -1;
}
