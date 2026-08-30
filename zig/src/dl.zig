//! dl.zig — port of src/dl.c (the dl_db integration layer / public API glue).
//!
//! This is the facade that ties every ported module together: lifecycle
//! (dl_open/open2/open_ro/close), relation declare (fixed + variadic), CSV
//! fact loading, incremental add/delete with per-relation WAL, the CAS
//! revision API + transaction buffer, interner/term-store access, the query
//! family (lookup/prefix/rank/select/range_count + _perm/_bound/count), rule
//! loading + compilation, snapshot publish (with the IVM/DRed/aggregate
//! incremental-maintenance dispatch cascade), magic-sets / top-down bound
//! queries, permutation-index management, fault hooks, graph traversal,
//! node observations and the regex pattern query.
//!
//! Strangler-hybrid ABI: `struct dl_db` is OPAQUE in dl.h but its
//! AUTHORITATIVE layout lives in src/dl_internal.h (still C, still
//! @cImport'd by the retained dl_cli.c / analyze.c).  dl.zig owns the
//! AUTHORITATIVE Zig definition as a native extern struct that MUST stay
//! byte-for-byte identical to that header — comptime-gated against
//! @cImport("dl_internal.h")'s translate-c layout via @sizeOf/@offsetOf.
//! The still-C consumers and every ported module (vm.zig/permindex.zig/
//! snapshot.zig/iter.zig/topdown.zig/compiler.zig, each with its OWN
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
const magic = @import("magic.zig");

// dl_internal.h pulls in dl.h/intern.h/relation.h/vrelation.h/snapshot.h/
// permindex.h/termstore.h/compiler.h/parser.h/regexwalk.h — the reference
// layout of dl_db/rel_entry/txn/txn_op/view_cache_slot/perm_index_entry and
// the opaque relation/vrelation/interner/termstore/tuple_set types.
const dx = @cImport({
    @cInclude("dl_internal.h");
});

// dafsa_internal.h: struct dafsa/dafsa_view + dafsa_save/dafsa_view_open/
// dafsa_view_close (the vendored DAFSA engine stays C in the hybrid .so).
const dc = @cImport({
    @cInclude("dafsa_internal.h");
});

// POSIX filesystem/lock primitives (stat/lstat/fstatat/mkdir/open/fcntl/
// flock/opendir/readdir/dirfd/rename/unlink/rmdir) with _GNU_SOURCE so
// glibc exposes the full declarations (stat/lstat/fstatat/dirfd/S_ISDIR).
const posix = @cImport({
    @cDefine("_GNU_SOURCE", "1");
    @cInclude("sys/stat.h");
    @cInclude("fcntl.h");
    @cInclude("unistd.h");
    @cInclude("dirent.h");
    @cInclude("stdio.h");
});

// ─── libc decls not in std.c (precedent: vm.zig / snapshot.zig) ────────────
extern "c" fn strdup(s: [*c]const u8) ?[*:0]u8;
extern "c" fn strcmp(a: [*c]const u8, b: [*c]const u8) c_int;
extern "c" fn strlen(s: [*c]const u8) usize;
extern "c" fn strncmp(a: [*c]const u8, b: [*c]const u8, n: usize) c_int;
extern "c" fn strstr(haystack: [*c]const u8, needle: [*c]const u8) [*c]u8;
extern "c" fn strtoul(nptr: [*c]const u8, endptr: [*c][*c]u8, base: c_int) c_ulong;
extern "c" fn snprintf(buf: [*c]u8, size: usize, fmt: [*c]const u8, ...) c_int;
extern "c" fn fprintf(stream: *c.FILE, fmt: [*c]const u8, ...) c_int;
extern "c" fn getline(lineptr: *?[*]u8, n: *usize, stream: *c.FILE) isize;
extern "c" fn fputs(s: [*:0]const u8, stream: *c.FILE) c_int;

// ─── Constants (mirror src/dl_internal.h / dl.h / compiler.h / txnwal.h) ───

const MAX_RELS: usize = 64;
const MAX_VAR_ARITY: u8 = 8;
const MAX_PERMS: usize = 64;
const DL_VIEW_CACHE_SZ: usize = 8;

const RELK_FIXED: u8 = 0;
const RELK_VARIADIC: u8 = 1;

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

// compiler.h opcodes consulted by db_has_list_builtin / db_has_range_builtin.
const OP_LIST_CONS: u8 = 20;
const OP_LIST_CAR: u8 = 21;
const OP_LIST_CDR: u8 = 22;
const OP_LIST_APPEND: u8 = 23;
const OP_LIST_MEMBER: u8 = 24;
const OP_RANGE: u8 = 25;

/// All-zero `owned` collision-head map — the magic/topdown eval clones never
/// deep-copy a borrowed head (fresh heads are appended), so this shared const
/// is the always-false owned[] passed to evalDbFreeOwned.
const ZERO_OWNED = [_]u8{0} ** MAX_RELS;

// ─── Callback types (mirror dl.h typedefs) ─────────────────────────────────

/// dl_tuple_cb == rel_enum_cb — identical C signature, shared type identity
/// with relation.zig so rel_prefix/vrel_prefix calls need no cast.
const DlTupleCb = relation.RelEnumCb;
const DlTraverseCb = ?*const fn (node_sym: u32, depth: u8, user: ?*anyopaque) callconv(.c) c_int;
const DlStrCb = ?*const fn (s: [*c]const u8, user: ?*anyopaque) callconv(.c) c_int;
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
};

/// view_cache_slot (snapshot.h) — rel_name[64] + view* + used.
pub const ViewCacheSlot = extern struct {
    rel_name: [64]u8,
    view: ?*anyopaque, // dafsa_view*
    used: c_int,
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
};

// Comptime gate: our extern layouts must be byte-identical to the C header.
comptime {
    std.debug.assert(@sizeOf(RelEntry) == @sizeOf(dx.rel_entry));
    std.debug.assert(@offsetOf(RelEntry, "rel") == @offsetOf(dx.rel_entry, "rel"));
    std.debug.assert(@sizeOf(TxnOp) == @sizeOf(dx.txn_op));
    std.debug.assert(@offsetOf(TxnOp, "cols") == @offsetOf(dx.txn_op, "cols"));
    std.debug.assert(@sizeOf(Txn) == @sizeOf(dx.txn));
    std.debug.assert(@sizeOf(ViewCacheSlot) == @sizeOf(dx.view_cache_slot));
    std.debug.assert(@sizeOf(PermIndexEntry) == @sizeOf(dx.perm_index_entry));

    std.debug.assert(@sizeOf(DlDb) == @sizeOf(dx.dl_db));
    std.debug.assert(@offsetOf(DlDb, "rels") == @offsetOf(dx.dl_db, "rels"));
    std.debug.assert(@offsetOf(DlDb, "nrels") == @offsetOf(dx.dl_db, "nrels"));
    std.debug.assert(@offsetOf(DlDb, "lock_fd") == @offsetOf(dx.dl_db, "lock_fd"));
    std.debug.assert(@offsetOf(DlDb, "read_only") == @offsetOf(dx.dl_db, "read_only"));
    std.debug.assert(@offsetOf(DlDb, "snap_version") == @offsetOf(dx.dl_db, "snap_version"));
    std.debug.assert(@offsetOf(DlDb, "vcache") == @offsetOf(dx.dl_db, "vcache"));
    std.debug.assert(@offsetOf(DlDb, "perms") == @offsetOf(dx.dl_db, "perms"));
    std.debug.assert(@offsetOf(DlDb, "ast_rules") == @offsetOf(dx.dl_db, "ast_rules"));
    std.debug.assert(@offsetOf(DlDb, "delta_pending") == @offsetOf(dx.dl_db, "delta_pending"));
    std.debug.assert(@offsetOf(DlDb, "del_pending") == @offsetOf(dx.dl_db, "del_pending"));
}

// ─── Ported-module extern bindings (callees whose db type is a private ────
// ─── @cImport'd dl_db, or whose module-private types are opaque) ────────────

// tupleset.zig (export fn, not pub) — reached via raw extern like relation.zig.
extern "c" fn ts_init(ts: ?*tupleset.tuple_set, arity: u8) c_int;
extern "c" fn ts_free(ts: ?*tupleset.tuple_set) void;
extern "c" fn ts_contains(ts: ?*const tupleset.tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_add(ts: ?*tupleset.tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_sort(ts: ?*tupleset.tuple_set) void;

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

// compiler.zig — compile_rules takes compiler.zig's private dx.dl_db.
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

// topdown.zig.
extern "c" fn td_eval(edb: ?*DlDb, prog: ?*const magic.magic_program, crules: ?[*]?*compiler.compiled_rule, n_crules: c_int, goal_variant_id: c_int, bound: ?[*]const u32, cb: DlTupleCb, user: ?*anyopaque) c_long;

// snapshot.zig.
extern "c" fn snapshot_read_current(db_dir: [*c]const u8) u32;
extern "c" fn vcache_invalidate(vcache: [*c]ViewCacheSlot) void;
extern "c" fn view_open_cached(vcache: [*c]ViewCacheSlot, rel_name: [*c]const u8, sdir: [*c]const u8) ?*anyopaque;
extern "c" fn manifest_find_rel_ex(sdir: [*c]const u8, rel_name: [*c]const u8, arity_out: [*c]u8, variadic_out: [*c]c_int) c_int;
extern "c" fn manifest_find_variants(sdir: [*c]const u8, rel_name: [*c]const u8, present: [*]u8) void;
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
extern "c" fn symbols_dfa_walk_view(v: ?*const anyopaque, dfa: ?*const regexwalk.regex_dfa, cb: regexwalk.SymWalkCb, user: ?*anyopaque) c_long;

// util.zig.
extern "c" fn atomic_write_str(path: [*c]const u8, content: [*c]const u8) c_int;
extern "c" fn fsync_dir_path(dirpath: [*c]const u8) c_int;

// ─── Data-global GOT accessors (U9 copy-relocation fix) ───────────────────
// dl.c reads/writes vm_nomaterialize/vm_export_relid/vm_export_ts (magic skip-
// materialize hook) and g_reorder/g_bushy (top-down compile toggles).  C exes
// take R_X86_64_COPY on these data globals, so shared-lib accesses must go
// through GOTPCREL — the same thing -fPIC refs in dl.c do.
const use_got_refs = builtin.output_mode == .Lib and builtin.mode != .Debug and
    builtin.cpu.arch == .x86_64 and builtin.os.tag == .linux;

inline fn gotDataRef(comptime name: []const u8, comptime T: type) *T {
    if (use_got_refs) {
        return asm ("movq " ++ name ++ "@GOTPCREL(%rip), %[p]"
            : [p] "={rax}" (-> *T)
        );
    }
    return @extern(*T, .{ .name = name });
}

inline fn vmNomaterializeRef() *c_int {
    return gotDataRef("vm_nomaterialize", c_int);
}
inline fn vmExportRelidRef() *c_int {
    return gotDataRef("vm_export_relid", c_int);
}
inline fn vmExportTsRef() *?*tupleset.tuple_set {
    return gotDataRef("vm_export_ts", ?*tupleset.tuple_set);
}
inline fn gReorderRef() *c_int {
    return gotDataRef("g_reorder", c_int);
}
inline fn gBushyRef() *c_int {
    return gotDataRef("g_bushy", c_int);
}

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

// ─── Internal helpers ──────────────────────────────────────────────────────

fn findRel(db: *const DlDb, name: [*c]const u8) c_int {
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (strEq(db.rels[i].name, name)) return @intCast(i);
    }
    return -1;
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
    var st: posix.struct_stat = undefined;
    var i: usize = 0;
    while (i < 3) : (i += 1) {
        const p = makeVpath(db, name, a, suffixes[i]);
        const exists = if (p) |pp| posix.stat(pp, &st) == 0 else false;
        if (p) |pp| c.free(@ptrCast(pp));
        if (exists) return true;
    }
    return false;
}

/// A variadic variant's rule-head-ness is derived from FILE EXISTENCE.
fn variantIsIdbOnDisk(db: *const DlDb, name: [*c]const u8, a: u8) bool {
    var st: posix.struct_stat = undefined;
    const p = makeVpath(db, name, a, ".base.dafsa");
    const exists = if (p) |pp| posix.stat(pp, &st) == 0 else false;
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

// ─── Lifecycle ─────────────────────────────────────────────────────────────

pub export fn dl_open(dir: ?[*:0]const u8) ?*DlDb {
    return dl_open2(dir, null);
}

/// RO existence probe: 1 if <dir>/<name> exists, 0 if it does not, -1 if
/// the path cannot be formed (over-long dir).
fn roProbeExists(dir: [*c]const u8, name: [*c]const u8) c_int {
    var path: [4096:0]u8 = undefined;
    var st: posix.struct_stat = undefined;
    const n = snprintf(&path, 4096, "%s/%s", dir, name);
    if (n < 0 or n >= 4096) return -1;
    return if (posix.stat(&path, &st) == 0) 1 else 0;
}

/// Shared body of dl_open2 (read-write) and dl_open_ro.
fn dlOpenCommon(dir: ?[*:0]const u8, err_out: ?*c_int, ro: c_int) ?*DlDb {
    if (dir == null) {
        if (err_out) |e| e.* = -1;
        return null;
    }
    const dirn: [*c]const u8 = dir.?;

    if (ro != 0) {
        var st: posix.struct_stat = undefined;
        if (posix.stat(dirn, &st) != 0 or !posix.S_ISDIR(st.st_mode)) {
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
        _ = posix.mkdir(dirn, @as(posix.mode_t, 0o755));
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

    {
        var fl: posix.struct_flock = undefined;
        fl.l_type = if (ro != 0) posix.F_RDLCK else posix.F_WRLCK;
        fl.l_whence = posix.SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0; // whole file
        fl.l_pid = 0;

        if (posix.fcntl(lfd, posix.F_SETLK, &fl) != 0) {
            _ = posix.close(lfd);
            if (err_out) |e| e.* = DL_E_LOCKED;
            return null;
        }
    }

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
                    if (strEq(rest + c2 + 1, "idb")) is_idb = 1;
                }
                if (buf[0] != 0 and (is_variadic or (arity >= 1 and arity <= 8))) {
                    _ = snprintf(&rel_names[n_meta], 256, "%s", buf);
                    rel_arities[n_meta] = if (is_variadic) 0 else @intCast(arity);
                    rel_idb[n_meta] = is_idb;
                    n_meta += 1;
                }
            }
            if (line) |lp| c.free(@ptrCast(lp));
            _ = c.fclose(rff);

            var mi: usize = 0;
            while (mi < n_meta) : (mi += 1) {
                _ = dlDeclareRelationKind(db, @ptrCast(&rel_names[mi]), rel_arities[mi], rel_idb[mi], 1);
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
    if (d.txn) |t| {
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
fn writeRelsTxt(db: *DlDb) void {
    const rels_path = makePath(db, "rels", ".txt") orelse return;

    var total: usize = 1; // trailing NUL
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        total += strLen(db.rels[i].name) + 16;
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
            n = snprintf(buf + p, total - p, "%s:*:%s\n",
                db.rels[i].name,
                if (vrelation.vrel_any_idb(db.rels[i].vrel) != 0) "idb" else "edb");
        } else {
            n = snprintf(buf + p, total - p, "%s:%d:%s\n",
                db.rels[i].name,
                @as(c_int, relation.rel_arity(db.rels[i].rel)),
                if (relation.rel_is_idb(db.rels[i].rel) != 0) "idb" else "edb");
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
        (if (arity == 0) db.rels[ridx].vrel == null else db.rels[ridx].rel == null)) {
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
                        if (have[@intCast(aa)] != 0) ts_free(&ts[@intCast(aa)]);
                    }
                    return rc;
                }
                cols[@intCast(i)] = sym;
            }
        }
        if (i < nf) continue; // row marked for skip

        if (have[@intCast(nf)] == 0) {
            const vr = vrelation.vrel_variant_or_null(e.vrel, @intCast(nf));
            if (ts_init(&ts[@intCast(nf)], @intCast(nf)) != 0) break;
            have[@intCast(nf)] = 1;
            if (vr != null and relation.rel_prefix_base(vr, null, 0, relation.ts_sink_cb, &ts[@intCast(nf)]) < 0) break;
        }
        {
            const trc = ts_add(&ts[@intCast(nf)], &cols);
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
                    if (have[@intCast(aa)] != 0) ts_free(&ts[@intCast(aa)]);
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
                    if (have[@intCast(aa)] != 0) ts_free(&ts[@intCast(aa)]);
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

        ts_sort(&ts[@intCast(a)]);
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
        if (have[@intCast(a)] != 0) ts_free(&ts[@intCast(a)]);
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
        if (ts_init(ts_p, arity) != 0) {
            c.free(m);
            return -1;
        }
        db.delta_pending[@intCast(rel_id)] = ts_p;
    }
    const rc = ts_add(ts_p, cols);
    return if (rc < 0) -1 else 0;
}

/// IVM Slice 3: append a -delta for rel_id to db->del_pending (lazy-init).
fn ivmCaptureDelete(db: *DlDb, rel_id: c_int, cols: [*c]const u32, arity: u8) c_int {
    var ts_p = db.del_pending[@intCast(rel_id)];
    if (ts_p == null) {
        const m = c.malloc(@sizeOf(tupleset.tuple_set)) orelse return -1;
        ts_p = @ptrCast(@alignCast(m));
        if (ts_init(ts_p, arity) != 0) {
            c.free(m);
            return -1;
        }
        db.del_pending[@intCast(rel_id)] = ts_p;
    }
    const rc = ts_add(ts_p, cols);
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
    if (ts_init(&ts, arity) != 0) {
        _ = c.fclose(f);
        return -1;
    }
    if (ts_init(&delta, arity) != 0) {
        ts_free(&ts);
        _ = c.fclose(f);
        return -1;
    }

    // Union the relation's pre-existing BASE into ts.
    if (relation.rel_prefix_base(d.rels[@intCast(idx)].rel, null, 0, relation.ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        ts_free(&delta);
        _ = c.fclose(f);
        return -1;
    }

    var line: ?[*]u8 = null;
    var linecap: usize = 0;
    var loaded: c_int = 0;
    var delta_failed: c_int = 0;
    var n_new: c_int = 0;

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
            } else {
                const sym = intern_str(d.ir, fields[@intCast(i)]);
                if (sym == 0) {
                    ts_free(&ts);
                    ts_free(&delta);
                    _ = c.fclose(f);
                    if (line) |lp| c.free(@ptrCast(lp));
                    return -1;
                }
                cols[@intCast(i)] = sym;
            }
        }

        {
            const rc = ts_add(&ts, &cols);
            if (rc < 0) {
                ts_free(&ts);
                ts_free(&delta);
                _ = c.fclose(f);
                if (line) |lp| c.free(@ptrCast(lp));
                return -1;
            }
            if (rc == 1) {
                n_new += 1;
                if (delta_failed == 0 and ts_add(&delta, &cols) < 0)
                    delta_failed = 1; // OOM: fall back to full re-eval
            }
        }
    }

    if (line) |lp| c.free(@ptrCast(lp));
    _ = c.fclose(f);

    ts_sort(&ts);
    if (relation.rel_build_base_from_tupleset(d.rels[@intCast(idx)].rel, &ts) != 0) {
        ts_free(&ts);
        ts_free(&delta);
        return -1;
    }

    loaded = @intCast(ts.count);
    ts_free(&ts);

    // M7: save interner BEFORE relation save.
    {
        const fwd_path = makePath(d, "symbols", ".dafsa");
        const rev_path = makePath(d, "symbols", ".array");
        if (fwd_path != null and rev_path != null) {
            if (intern_save(d.ir, fwd_path.?, rev_path.?) != 0) {
                if (fwd_path) |x| c.free(@ptrCast(x));
                if (rev_path) |x| c.free(@ptrCast(x));
                ts_free(&delta);
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
                ts_free(&delta);
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
        ts_free(&delta);

        d.fixpoint_dirty = 1;

        // M6: mark permutation indices of this relation dirty
        var pi: c_int = 0;
        while (pi < d.n_perms) : (pi += 1) {
            if (d.perms[@intCast(pi)].rel_id == idx)
                d.perms[@intCast(pi)].dirty = 1;
        }
    } else {
        ts_free(&delta);
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

    // 2. Duplicate check against the variant BASE.
    if (relation.rel_exact_base(vr, cols) != 0)
        return 0;

    // 3. WAL-append ADD + sync.
    if (relation.rel_wal_append_add(vr, &key, @intCast(key_len)) != 0)
        return -1;

    // 4. In-memory add to the variant BASE.
    const rc = relation.rel_add_base(vr, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(db, @intCast(idx));
        db.full_reeval_pending = 1;
    }

    // 5. Compaction threshold (per variant).
    {
        const wal_sz = relation.rel_wal_size(vr);
        const dafsa_sz = relation.rel_dafsa_size(vr);
        if (dafsa_sz > 0 and wal_sz > dafsa_sz / 4) {
            const path = if (relation.rel_is_idb(vr) != 0)
                makeVpath(db, rel_name, arity, ".base.dafsa")
            else
                makeVpath(db, rel_name, arity, ".dafsa");
            if (path != null) {
                _ = relation.rel_compact(vr, path.?);
                c.free(@ptrCast(path.?));
            }
        }
    }

    db.fixpoint_dirty = 1;
    return 1; // added
}

/// CAS Slice 2: apply a fact add to the in-memory BASE + IVM delta capture.
fn addFactApply(db: *DlDb, idx: c_int, cols: [*c]const u32, arity: u8) c_int {
    const rc = relation.rel_add_base(db.rels[@intCast(idx)].rel, cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(db, idx);
        if (ivmRelIsHead(db, idx)) {
            db.full_reeval_pending = 1;
        } else if (ivmCaptureDelta(db, idx, cols, arity) != 0) {
            db.full_reeval_pending = 1;
        }
    }

    // Compaction threshold.
    {
        const wal_sz = relation.rel_wal_size(db.rels[@intCast(idx)].rel);
        const dafsa_sz = relation.rel_dafsa_size(db.rels[@intCast(idx)].rel);
        if (dafsa_sz > 0 and wal_sz > dafsa_sz / 4) {
            const path = if (relation.rel_is_idb(db.rels[@intCast(idx)].rel) != 0)
                makePath(db, db.rels[@intCast(idx)].name, ".base.dafsa")
            else
                makePath(db, db.rels[@intCast(idx)].name, ".dafsa");
            if (path != null) {
                _ = relation.rel_compact(db.rels[@intCast(idx)].rel, path.?);
                c.free(@ptrCast(path.?));
            }
        }
    }

    db.fixpoint_dirty = 1;
    return if (rc == 1) 1 else 0;
}

/// CAS Slice 2: apply a fact delete to the in-memory BASE + IVM -delta.
fn deleteFactApply(db: *DlDb, idx: c_int, cols: [*c]const u32, arity: u8) c_int {
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

    // 2. Duplicate check against BASE.
    if (relation.rel_exact_base(d.rels[@intCast(idx)].rel, cols) != 0)
        return 0;

    // 3. WAL-append ADD + sync.
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

    // 1. Absent check against the variant BASE.
    if (relation.rel_exact_base(vr, cols) == 0)
        return 0;

    // 2. WAL-append DEL + sync.
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

    // 1. Absent check against BASE.
    if (relation.rel_exact_base(d.rels[@intCast(idx)].rel, cols) == 0)
        return 0;

    // 2. WAL-append DEL + sync.
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

    // 2. ADD the new row.
    new_cols[0] = entity_sym;
    new_cols[1] = new_value;
    if (encodeFactKey(&key, &key_len, &new_cols, 2) != 0) return -1;
    if (relation.rel_wal_append_add(rel, &key, @intCast(key_len)) != 0) return -1;
    rc = relation.rel_add_base(rel, &new_cols);
    if (rc < 0) return -1;
    if (rc == 1) {
        permindex_mark_dirty(d, rel_id);
        if (ivmCaptureDelta(d, rel_id, &new_cols, 2) != 0)
            d.full_reeval_pending = 1;
    }

    // 3. Compaction threshold.
    {
        const wal_sz = relation.rel_wal_size(rel);
        const dafsa_sz = relation.rel_dafsa_size(rel);
        if (dafsa_sz > 0 and wal_sz > dafsa_sz / 4) {
            const path = makePath(d, "rev", ".dafsa");
            if (path != null) {
                _ = relation.rel_compact(rel, path.?);
                c.free(@ptrCast(path.?));
            }
        }
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

pub export fn dl_txn_begin(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1; // reject nested transactions
    const mem = c.calloc(1, @sizeOf(Txn)) orelse return -1;
    d.txn = @ptrCast(@alignCast(mem));
    return 0;
}

pub export fn dl_txn_rollback(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.txn == null) return -1;
    txnDiscard(d);
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
                    txnDiscard(d);
                    return -1;
                }
                if (cur != t.ops.?[i].expected) {
                    txnDiscard(d);
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
            txnDiscard(d);
            return -1;
        }
        if (intern_save(d.ir, fwd_path.?, rev_path.?) != 0) {
            c.free(@ptrCast(fwd_path.?));
            c.free(@ptrCast(rev_path.?));
            txnDiscard(d);
            return -1;
        }
        c.free(@ptrCast(fwd_path.?));
        c.free(@ptrCast(rev_path.?));
    }
    if (term_is_dirty(d.terms) != 0) {
        const tpath = makePath(d, "terms", ".bin") orelse {
            txnDiscard(d);
            return -1;
        };
        if (term_save(d.terms, tpath) != 0) {
            c.free(@ptrCast(tpath));
            txnDiscard(d);
            return -1;
        }
        c.free(@ptrCast(tpath));
    }

    // (d) Append one record per buffered op + a COMMIT marker, then fsync.
    {
        const w = txnwal.txnwal_open_rw(d.dir.?) orelse {
            txnDiscard(d);
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
                    txnDiscard(d);
                    return -1;
                }
                _ = encodeFactKey(&key, &key_len, &add_cols, 2);
                if (txnwal.txnwal_append_record(w, "rev", 3, TXNWAL_OP_ADD, &key, @intCast(key_len)) != 0) {
                    txnwal.txnwal_close(w);
                    txnDiscard(d);
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
                    txnDiscard(d);
                    return -1;
                }
            }
        }
        if (d.fault_hook) |hook| {
            if (hook(DL_FPOINT_TXN_BEFORE_MARKER, d.fault_user) != 0) {
                txnwal.txnwal_close(w);
                txnDiscard(d);
                return -1;
            }
        }
        if (txnwal.txnwal_append_commit(w) != 0) {
            txnwal.txnwal_close(w);
            txnDiscard(d);
            return -1;
        }
        if (txnwal.txnwal_sync(w) != 0) {
            txnwal.txnwal_close(w);
            txnDiscard(d);
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
                    txnDiscard(d);
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

pub export fn dl_intern_str_of(db: ?*DlDb, sym_id: u32) ?[*:0]const u8 {
    const d = db orelse return null;
    if (d.ir == null) return null;
    return intern_str_of(d.ir, sym_id);
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
    return view_open_cached(@constCast(&d.vcache), rel_name, &sdir);
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

/// M8: AST deep-copy — retain rules for the magic-sets transform.

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

    // Dhall schema hook (S2/S3).
    if (d.schema != null) {
        var errbuf: [256]u8 = undefined;
        if (dl_typecheck_rules(d.schema, @ptrCast(rules), n_rules, null, &errbuf, errbuf.len) != 0) {
            freeRulesAndParser(rules, n_rules, p);
            return -1;
        }
    }

    var new_crules: ?[*]?*compiler.compiled_rule = null;
    var n_compiled: c_int = 0;
    if (compile_rules(d, rules, n_rules, &new_crules, &n_compiled) != 0) {
        freeRulesAndParser(rules, n_rules, p);
        return -1;
    }

    // M8: retain a DEEP copy of the rule AST for the magic-sets transform.
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
            @memcpy(na_rules[@intCast(d.n_ast_rules) .. @intCast(d.n_ast_rules + n_rules)], cloned_rules[0..@intCast(n_rules)]);
            c.free(cloned);
            d.ast_rules = na_rules;
            d.n_ast_rules += n_rules;
        }
    }

    // Append compiled rules to db's list.
    {
        const new_total = d.n_crules + n_compiled;
        const merged = c.realloc(@ptrCast(d.crules), @as(usize, @intCast(new_total)) * @sizeOf(?*compiler.compiled_rule)) orelse {
            var i: c_int = 0;
            while (i < n_compiled) : (i += 1) compiler.compiled_rule_free(new_crules.?[@intCast(i)]);
            c.free(@ptrCast(new_crules));
            freeRulesAndParser(rules, n_rules, p);
            return -1;
        };
        const merged_rules: [*]?*compiler.compiled_rule = @ptrCast(@alignCast(merged));
        @memcpy(merged_rules[@intCast(d.n_crules) .. @intCast(new_total)], new_crules.?[0..@intCast(n_compiled)]);
        c.free(@ptrCast(new_crules));
        d.crules = merged_rules;
        d.n_crules = new_total;
    }

    // IVM Slice 1: the rule set changed.
    d.full_reeval_pending = 1;
    d.fixpoint_dirty = 1;

    freeRulesAndParser(rules, n_rules, p);
    return 0;
}

pub export fn dl_compile(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    if (d.read_only != 0) return -1;
    if (d.n_crules == 0) return 0;

    if (vm_execute(d, d.crules, d.n_crules) != 0)
        return -1;

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
        if (vm_execute(d, d.crules, d.n_crules) != 0)
            return -1;
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
    if (ts_init(&ts, ar) != 0) {
        relation.rel_free(dst);
        return null;
    }
    if (relation.rel_prefix(s, null, 0, relation.ts_sink_cb, &ts) < 0) {
        ts_free(&ts);
        relation.rel_free(dst);
        return null;
    }
    ts_sort(&ts);
    if (relation.rel_build_from_tupleset(dst, &ts) != 0) {
        ts_free(&ts);
        relation.rel_free(dst);
        return null;
    }
    ts_free(&ts);
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
            while (i < n_crules) : (i += 1) compiler.compiled_rule_free(crules.?[@intCast(i)]);
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

    if (vm_execute(&edb, crules, n_crules) != 0) {
        dlErr("dl_query_rules_ro: evaluation failed\n", .{});
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

// ─── M8: magic-sets bound query (scoped re-eval, clone-and-scope) ───────────

/// Clone-and-scope: build an eval-only dl_db that shallow-aliases EDB rels.
fn evalDbClone(src: *const DlDb, out: *DlDb) void {
    out.* = std.mem.zeroes(DlDb);
    out.dir = null;
    out.ir = src.ir;
    out.terms = src.terms;
    out.lock_fd = -1;
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

/// Is goal_rel a rule head (i.e. IDB) in the retained AST?
fn astHasHead(db: *const DlDb, pred: [*c]const u8) bool {
    var i: c_int = 0;
    while (i < db.n_ast_rules) : (i += 1) {
        const r = db.ast_rules.?[@intCast(i)] orelse continue;
        if (r.head != null and r.head.?.pred != null and
            strEq(r.head.?.pred, pred))
            return true;
    }
    return false;
}

/// Adapter turning a dl_tuple_cb into a per-position adornment filter.
const MagicFilterCtx = struct {
    user_cb: DlTupleCb,
    user: ?*anyopaque,
    arity: u8,
    adorn: [9]u8,
    vals: [8]u32,
    matched: c_long,
};

fn magicFilterCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *MagicFilterCtx = @ptrCast(@alignCast(user.?));
    _ = arity;
    var i: u8 = 0;
    var b: u8 = 0;
    while (i < ctx.arity) : (i += 1) {
        if (ctx.adorn[i] == 'b') {
            if (cols.?[i] != ctx.vals[b]) return 0;
            b += 1;
        }
    }
    ctx.matched += 1;
    return ctx.user_cb.?(cols, ctx.arity, ctx.user);
}

pub export fn dl_query_magic(db: ?*DlDb, goal_rel: [*c]const u8, leading: ?[*]const u32, k: u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (goal_rel == null or cb == null) return -1;

    if (db_has_variadic(d) != 0) {
        dlErr("dl_query_magic: rejected: program contains a variadic relation (outside the magic-sets class)\n", .{});
        return -1;
    }
    if (db_has_list_builtin(d) != 0 or db_has_range_builtin(d) != 0) {
        dlErr("dl_query_magic: rejected: program uses a list builtin (outside the magic-sets class)\n", .{});
        return -1;
    }

    const goal_idx = findRel(d, goal_rel);
    if (goal_idx < 0) return -1;
    const goal_arity = relation.rel_arity(d.rels[@intCast(goal_idx)].rel);
    if (k > goal_arity) return -1;

    if (k == 0)
        return dl_query(d, goal_rel, cb, user);

    var adorn: [9]u8 = undefined;
    var i: u8 = 0;
    while (i < k) : (i += 1) adorn[i] = 'b';
    while (i < goal_arity) : (i += 1) adorn[i] = 'f';
    adorn[goal_arity] = 0;

    return dl_query_magic_adorn(d, goal_rel, @ptrCast(&adorn), leading, k, cb, user);
}

pub export fn dl_query_magic_adorn(db: ?*DlDb, goal_rel: [*c]const u8, adorn: [*c]const u8, vals: ?[*]const u32, nvals: u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (goal_rel == null or adorn == null or cb == null) return -1;

    const goal_idx = findRel(d, goal_rel);
    if (goal_idx < 0) return -1;
    const goal_arity = relation.rel_arity(d.rels[@intCast(goal_idx)].rel);

    if (db_has_variadic(d) != 0) {
        dlErr("dl_query_magic: rejected: program contains a variadic relation (outside the magic-sets class)\n", .{});
        return -1;
    }
    if (db_has_list_builtin(d) != 0 or db_has_range_builtin(d) != 0) {
        dlErr("dl_query_magic: rejected: program uses a list builtin (outside the magic-sets class)\n", .{});
        return -1;
    }

    const alen = strLen(adorn);
    if (alen != goal_arity) {
        dlErr("dl_query_magic: adornment length {d} != goal arity {d}\n", .{ alen, goal_arity });
        return -1;
    }
    var nb: c_int = 0;
    {
        var xi: usize = 0;
        while (xi < alen) : (xi += 1) {
            if (adorn[xi] != 'b' and adorn[xi] != 'f') {
                dlErr("dl_query_magic: bad adornment char '{c}'\n", .{adorn[xi]});
                return -1;
            }
            if (adorn[xi] == 'b') nb += 1;
        }
    }
    if (nb != @as(c_int, nvals)) {
        dlErr("dl_query_magic: nvals={d} != count_b(adorn)={d}\n", .{ nvals, nb });
        return -1;
    }

    if (nvals == 0)
        return dl_query(d, goal_rel, cb, user);

    // EDB goal → direct full-scan + per-position filter.
    if (!astHasHead(d, goal_rel)) {
        var ctx = std.mem.zeroes(MagicFilterCtx);
        ctx.user_cb = cb;
        ctx.user = user;
        ctx.arity = goal_arity;
        @memcpy(ctx.adorn[0..alen], adorn[0..alen]);
        ctx.adorn[alen] = 0;
        @memcpy(ctx.vals[0..nvals], vals.?[0..nvals]);
        if (relation.rel_prefix(d.rels[@intCast(goal_idx)].rel, null, 0, magicFilterCb, &ctx) < 0)
            return -1;
        return ctx.matched;
    }

    if (d.n_ast_rules <= 0) return -1;

    // Negation/aggregate soundness: materialize src first if dirty.
    if (d.fixpoint_dirty != 0) {
        var ri: c_int = 0;
        while (ri < d.n_ast_rules) : (ri += 1) {
            const r = d.ast_rules.?[@intCast(ri)] orelse continue;
            if (r.has_negation != 0 or r.has_aggregate != 0) {
                if (dl_compile(d) != 0) return -1;
                break;
            }
        }
    }

    var edb: DlDb = undefined;
    var prog: magic.magic_program = std.mem.zeroes(magic.magic_program);
    var magic_crules: ?[*]?*compiler.compiled_rule = null;
    var n_magic: c_int = 0;
    var reject: [256]u8 = undefined;
    var result: c_long = -1;

    evalDbClone(d, &edb);
    const n_aliased = edb.nrels;

    if (magic.magic_transform_adorn(@ptrCast(d.ast_rules), d.n_ast_rules, goal_rel, goal_arity, adorn, vals, nvals, d.nrels, d.ir, &prog, &reject, reject.len) != 0) {
        dlErr("dl_query_magic: rejected: {s}\n", .{@as([*c]const u8, @ptrCast(&reject))});
        evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
        return -1;
    }

    {
        var dd: c_int = 0;
        while (dd < prog.n_decls) : (dd += 1) {
            if (evalDbDeclareInmem(&edb, @ptrCast(&prog.decls.?[@intCast(dd)].name), prog.decls.?[@intCast(dd)].arity) != 0) {
                dlErr("dl_query_magic: cannot pre-declare '{s}'\n", .{@as([*c]const u8, @ptrCast(&prog.decls.?[@intCast(dd)].name))});
                magic.magic_program_free(&prog);
                evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
                return -1;
            }
        }
    }

    // Seed the magic goal relation with the bound values.
    {
        var magic_goal: [64:0]u8 = undefined;
        if (snprintf(&magic_goal, 64, "magic_%s", @as([*c]const u8, @ptrCast(&prog.adorned_goal))) >= 64) {
            dlErr("dl_query_magic: magic goal name too long\n", .{});
            magic.magic_program_free(&prog);
            evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
            return -1;
        }
        const m_idx = findRel(&edb, &magic_goal);
        if (m_idx < 0 or relation.rel_arity(edb.rels[@intCast(m_idx)].rel) != nvals) {
            dlErr("dl_query_magic: internal: magic goal '{s}' missing/arity-mismatch\n", .{@as([*c]const u8, @ptrCast(&magic_goal))});
            magic.magic_program_free(&prog);
            evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
            return -1;
        }
        _ = relation.rel_add(edb.rels[@intCast(m_idx)].rel, vals);
    }

    if (compile_rules(&edb, prog.rules, prog.n_rules, &magic_crules, &n_magic) != 0) {
        dlErr("dl_query_magic: compile of adorned program failed\n", .{});
        magic.magic_program_free(&prog);
        evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
        return -1;
    }

    if (edb.dir != null or edb.nrels != n_aliased + @as(usize, @intCast(prog.n_decls))) {
        dlErr("dl_query_magic: internal error: compile_rules grew the eval clone's relation table (missed head relation)\n", .{});
        magic.magic_program_free(&prog);
        evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
        return -1;
    }

    {
        const a_idx = findRel(&edb, @ptrCast(&prog.adorned_goal));
        if (a_idx < 0) {
            dlErr("dl_query_magic: internal: adorned goal '{s}' missing\n", .{@as([*c]const u8, @ptrCast(&prog.adorned_goal))});
            magic.magic_program_free(&prog);
            evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
            return -1;
        }

        var goal_ts: tupleset.tuple_set = std.mem.zeroes(tupleset.tuple_set);
        vmNomaterializeRef().* = 1;
        vmExportRelidRef().* = a_idx;
        vmExportTsRef().* = &goal_ts;

        const exec_rc = vm_execute(&edb, magic_crules, n_magic);

        vmNomaterializeRef().* = 0;
        vmExportRelidRef().* = -1;
        vmExportTsRef().* = null;

        if (exec_rc != 0) {
            dlErr("dl_query_magic: scoped fixpoint failed\n", .{});
            ts_free(&goal_ts);
            magic.magic_program_free(&prog);
            evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
            return -1;
        }

        var ctx = std.mem.zeroes(MagicFilterCtx);
        ctx.user_cb = cb;
        ctx.user = user;
        ctx.arity = goal_arity;
        @memcpy(ctx.adorn[0..alen], adorn[0..alen]);
        ctx.adorn[alen] = 0;
        @memcpy(ctx.vals[0..nvals], vals.?[0..nvals]);

        if (goal_ts.arity > 0) {
            var ci: c_long = 0;
            while (ci < goal_ts.count) : (ci += 1) {
                const tt = goal_ts.data.? + @as(usize, @intCast(ci)) * goal_ts.arity;
                if (magicFilterCb(tt, goal_ts.arity, &ctx) != 0) break;
            }
            result = ctx.matched;
            ts_free(&goal_ts);
        } else {
            if (relation.rel_prefix(edb.rels[@intCast(a_idx)].rel, null, 0, magicFilterCb, &ctx) < 0)
                result = -1
            else
                result = ctx.matched;
        }
    }

    magic.magic_program_free(&prog);
    {
        var i: c_int = 0;
        while (i < n_magic) : (i += 1) compiler.compiled_rule_free(magic_crules.?[@intCast(i)]);
        if (magic_crules) |x| c.free(@ptrCast(x));
    }
    evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
    return result;
}

// ─── top-down / QSQ bound query ─────────────────────────────────────────────

const TcBfsCtx = struct {
    db: *DlDb,
    edge_idx: c_int,
    seed: u32,
    cb: DlTupleCb,
    user: ?*anyopaque,
    visited: ?*tupleset.tuple_set,
    queue: ?[*]u32,
    qcap: c_long,
    qtail: c_long,
    count: c_long,
    stop: c_int,
};

fn tcBfsEdgeCb(cols: ?[*]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *TcBfsCtx = @ptrCast(@alignCast(user.?));
    _ = arity;
    const w = cols.?[1];
    var emit: [2]u32 = undefined;

    if (ts_contains(ctx.visited, @ptrCast(&w)) != 0) return 0; // already reached

    emit[0] = ctx.seed;
    emit[1] = w;
    ctx.count += 1;
    if (ctx.cb.?( &emit, 2, ctx.user) != 0) {
        ctx.stop = 1;
        return 1;
    }

    if (ts_add(ctx.visited, @ptrCast(&w)) != 1) {
        ctx.stop = 1;
        return 1;
    }
    if (ctx.qtail >= ctx.qcap) {
        const nc: c_long = if (ctx.qcap != 0) ctx.qcap * 2 else 64;
        const nq = c.realloc(@ptrCast(ctx.queue), @as(usize, @intCast(nc)) * @sizeOf(u32)) orelse {
            ctx.stop = 1;
            return 1;
        };
        ctx.queue = @ptrCast(@alignCast(nq));
        ctx.qcap = nc;
    }
    ctx.queue.?[@intCast(ctx.qtail)] = w;
    ctx.qtail += 1;
    return 0;
}

/// Detect the exact canonical left-recursive transitive-closure shape
/// tc(X,Y) :- edge(X,Y).  tc(X,Y) :- edge(X,Z), tc(Z,Y).  queried as tc(seed,?).
fn tcRecognizeBf(db: *DlDb, goal_rel: [*c]const u8, adorn: [*c]const u8, vals: ?[*]const u32, nvals: u8, cb: DlTupleCb, user: ?*anyopaque, matched: *c_int) c_long {
    matched.* = 0;

    if (db.n_ast_rules != 2) return 0;
    {
        var i: c_int = 0;
        while (i < 2) : (i += 1) {
            const r = db.ast_rules.?[@intCast(i)] orelse return 0;
            if (r.head == null or r.head.?.pred == null) return 0;
        }
    }

    const h = db.ast_rules.?[0].?.head;
    {
        var i: c_int = 0;
        while (i < 2) : (i += 1) {
            const r = db.ast_rules.?[@intCast(i)].?;
            if (!strEq(r.head.?.pred, goal_rel)) return 0;
            if (r.head.?.nargs != 2) return 0;
            if (r.head.?.args.?[0].?.kind != parser.TOK_VAR or
                r.head.?.args.?[1].?.kind != parser.TOK_VAR) return 0;
        }
    }

    if (adorn == null or adorn[0] != 'b' or adorn[1] != 'f' or
        adorn[2] != 0 or nvals != 1 or vals == null) return 0;

    var B: ?*const parser.rule = null;
    var R: ?*const parser.rule = null;
    {
        var i: c_int = 0;
        while (i < 2) : (i += 1) {
            const r = db.ast_rules.?[@intCast(i)].?;
            if (r.has_negation != 0 or r.has_aggregate != 0) return 0;
            if (r.nbody == 1 and B == null) B = r
            else if (r.nbody == 2 and R == null) R = r
            else return 0;
        }
    }
    if (B == null or R == null) return 0;

    const X = h.?.args.?[0].?.text orelse return 0;
    const Y = h.?.args.?[1].?.text orelse return 0;

    var edge_pred: [*c]const u8 = undefined;
    var edge_idx: c_int = undefined;
    // E. Base rule: single pure-EDB edge atom edge(X,Y).
    {
        const a = B.?.body.?[0].?;
        if (a.pred == null or strEq(a.pred, h.?.pred)) return 0;
        if (a.nargs != 2) return 0;
        if (a.args.?[0].?.kind != parser.TOK_VAR or a.args.?[1].?.kind != parser.TOK_VAR) return 0;
        if (a.negated != 0 or a.aggregate != 0 or a.arith != null or a.pattern != null) return 0;
        if (a.args.?[0].?.text == null or a.args.?[1].?.text == null) return 0;
        if (!strEq(a.args.?[0].?.text, X)) return 0;
        if (!strEq(a.args.?[1].?.text, Y)) return 0;
        edge_pred = a.pred;
        edge_idx = findRel(db, edge_pred);
        if (edge_idx < 0) return 0;
        if (db.rels[@intCast(edge_idx)].kind != RELK_FIXED) return 0;
        if (relation.rel_arity(db.rels[@intCast(edge_idx)].rel) != 2) return 0;
        if (astHasHead(db, edge_pred)) return 0;
    }

    // F. Recursive rule: edge(X,Z), tc(Z,Y) with Z fresh, tc the LAST atom.
    var Z: [*c]const u8 = undefined;
    {
        const a0 = R.?.body.?[0].?;
        const a1 = R.?.body.?[1].?;

        if (!strEq(a0.pred, edge_pred)) return 0;
        if (a0.nargs != 2) return 0;
        if (a0.args.?[0].?.kind != parser.TOK_VAR or a0.args.?[1].?.kind != parser.TOK_VAR) return 0;
        if (a0.negated != 0 or a0.aggregate != 0 or a0.arith != null or a0.pattern != null) return 0;
        if (a0.args.?[0].?.text == null or a0.args.?[1].?.text == null) return 0;
        if (!strEq(a0.args.?[0].?.text, X)) return 0;
        Z = a0.args.?[1].?.text;
        if (strEq(Z, X) or strEq(Z, Y)) return 0; // Z fresh

        if (a1.pred == null or !strEq(a1.pred, h.?.pred)) return 0;
        if (a1.nargs != 2) return 0;
        if (a1.args.?[0].?.kind != parser.TOK_VAR or a1.args.?[1].?.kind != parser.TOK_VAR) return 0;
        if (a1.negated != 0 or a1.aggregate != 0 or a1.arith != null or a1.pattern != null) return 0;
        if (a1.args.?[0].?.text == null or a1.args.?[1].?.text == null) return 0;
        if (!strEq(a1.args.?[0].?.text, Z)) return 0;
        if (!strEq(a1.args.?[1].?.text, Y)) return 0;
    }

    // G. Every argument token in every atom of both rules is TOK_VAR.
    {
        var ri: c_int = 0;
        while (ri < 2) : (ri += 1) {
            const r = if (ri == 0) B else R;
            var ai: c_int = 0;
            while (ai < r.?.nbody) : (ai += 1) {
                const a = r.?.body.?[@intCast(ai)].?;
                var k: c_int = 0;
                while (k < a.nargs) : (k += 1)
                    if (a.args.?[@intCast(k)].?.kind != parser.TOK_VAR) return 0;
            }
        }
    }

    // Fires: single-source BFS reachability from seed.
    matched.* = 1;
    {
        var qhead: c_long = 0;
        var visited: tupleset.tuple_set = undefined;

        if (ts_init(&visited, 1) != 0) return -1;
        var ctx = TcBfsCtx{
            .db = db,
            .edge_idx = edge_idx,
            .seed = vals.?[0],
            .cb = cb,
            .user = user,
            .visited = &visited,
            .queue = null,
            .qcap = 64,
            .qtail = 0,
            .count = 0,
            .stop = 0,
        };
        ctx.queue = @ptrCast(@alignCast(c.malloc(@as(usize, @intCast(ctx.qcap)) * @sizeOf(u32)) orelse {
            ts_free(&visited);
            return -1;
        }));

        ctx.queue.?[@intCast(ctx.qtail)] = ctx.seed; // NOT pre-visited
        ctx.qtail += 1;

        while (qhead < ctx.qtail) {
            const u = ctx.queue.?[@intCast(qhead)];
            qhead += 1;
            if (ctx.stop != 0) break;
            const n = relation.rel_prefix(db.rels[@intCast(edge_idx)].rel, @ptrCast(&u), 1, tcBfsEdgeCb, &ctx);
            if (n < 0) {
                c.free(@ptrCast(ctx.queue));
                ts_free(&visited);
                return -1;
            }
        }
        c.free(@ptrCast(ctx.queue));
        ts_free(&visited);
        return ctx.count;
    }
}

pub export fn dl_query_topdown(db: ?*DlDb, goal_rel: [*c]const u8, leading: ?[*]const u32, k: u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (goal_rel == null or cb == null) return -1;

    if (db_has_variadic(d) != 0) {
        dlErr("dl_query_topdown: rejected: program contains a variadic relation (outside the magic-sets class)\n", .{});
        return -1;
    }
    if (db_has_list_builtin(d) != 0 or db_has_range_builtin(d) != 0) {
        dlErr("dl_query_topdown: rejected: program uses a list builtin (outside the magic-sets class)\n", .{});
        return -1;
    }

    const goal_idx = findRel(d, goal_rel);
    if (goal_idx < 0) return -1;
    const goal_arity = relation.rel_arity(d.rels[@intCast(goal_idx)].rel);
    if (k > goal_arity) return -1;
    if (k == 0)
        return dl_query(d, goal_rel, cb, user);

    var adorn: [9]u8 = undefined;
    var i: u8 = 0;
    while (i < k) : (i += 1) adorn[i] = 'b';
    while (i < goal_arity) : (i += 1) adorn[i] = 'f';
    adorn[goal_arity] = 0;

    return dl_query_topdown_adorn(d, goal_rel, @ptrCast(&adorn), leading, k, cb, user);
}

pub export fn dl_query_topdown_adorn(db: ?*DlDb, goal_rel: [*c]const u8, adorn: [*c]const u8, vals: ?[*]const u32, nvals: u8, cb: DlTupleCb, user: ?*anyopaque) c_long {
    const d = db orelse return -1;
    if (goal_rel == null or adorn == null or cb == null) return -1;

    const goal_idx = findRel(d, goal_rel);
    if (goal_idx < 0) return -1;
    const goal_arity = relation.rel_arity(d.rels[@intCast(goal_idx)].rel);

    if (db_has_variadic(d) != 0) {
        dlErr("dl_query_topdown: rejected: program contains a variadic relation (outside the magic-sets class)\n", .{});
        return -1;
    }
    if (db_has_list_builtin(d) != 0 or db_has_range_builtin(d) != 0) {
        dlErr("dl_query_topdown: rejected: program uses a list builtin (outside the magic-sets class)\n", .{});
        return -1;
    }

    const alen = strLen(adorn);
    if (alen != goal_arity) {
        dlErr("dl_query_topdown: adornment length {d} != goal arity {d}\n", .{ alen, goal_arity });
        return -1;
    }
    var nb: c_int = 0;
    {
        var xi: usize = 0;
        while (xi < alen) : (xi += 1) {
            if (adorn[xi] != 'b' and adorn[xi] != 'f') {
                dlErr("dl_query_topdown: bad adornment char '{c}'\n", .{adorn[xi]});
                return -1;
            }
            if (adorn[xi] == 'b') nb += 1;
        }
    }
    if (nb != @as(c_int, nvals)) {
        dlErr("dl_query_topdown: nvals={d} != count_b(adorn)={d}\n", .{ nvals, nb });
        return -1;
    }

    if (nvals == 0)
        return dl_query(d, goal_rel, cb, user);

    // EDB goal → direct full-scan + per-position filter.
    if (!astHasHead(d, goal_rel)) {
        var ctx = std.mem.zeroes(MagicFilterCtx);
        ctx.user_cb = cb;
        ctx.user = user;
        ctx.arity = goal_arity;
        @memcpy(ctx.adorn[0..alen], adorn[0..alen]);
        ctx.adorn[alen] = 0;
        @memcpy(ctx.vals[0..nvals], vals.?[0..nvals]);
        if (relation.rel_prefix(d.rels[@intCast(goal_idx)].rel, null, 0, magicFilterCb, &ctx) < 0)
            return -1;
        return ctx.matched;
    }

    if (d.n_ast_rules <= 0) return -1;

    // Fast path: canonical left-recursive TC shape (bf adorn).
    {
        var matched: c_int = 0;
        const rc = tcRecognizeBf(d, goal_rel, adorn, vals, nvals, cb, user, &matched);
        if (matched != 0)
            return rc;
    }

    // Negation/aggregate soundness.
    if (d.fixpoint_dirty != 0) {
        var ri: c_int = 0;
        while (ri < d.n_ast_rules) : (ri += 1) {
            const r = d.ast_rules.?[@intCast(ri)] orelse continue;
            if (r.has_negation != 0 or r.has_aggregate != 0) {
                if (dl_compile(d) != 0) return -1;
                break;
            }
        }
    }

    var edb: DlDb = undefined;
    var prog: magic.magic_program = std.mem.zeroes(magic.magic_program);
    var td_crules: ?[*]?*compiler.compiled_rule = null;
    var n_td: c_int = 0;
    var reject: [256]u8 = undefined;
    var result: c_long = -1;
    var goal_variant_id: c_int = -1;

    evalDbClone(d, &edb);
    const n_aliased = edb.nrels;

    if (magic.magic_transform_adorn(@ptrCast(d.ast_rules), d.n_ast_rules, goal_rel, goal_arity, adorn, vals, nvals, d.nrels, d.ir, &prog, &reject, reject.len) != 0) {
        dlErr("dl_query_topdown: rejected: {s}\n", .{@as([*c]const u8, @ptrCast(&reject))});
        evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
        return -1;
    }

    {
        var dd: c_int = 0;
        while (dd < prog.n_decls) : (dd += 1) {
            if (evalDbDeclareInmem(&edb, @ptrCast(&prog.decls.?[@intCast(dd)].name), prog.decls.?[@intCast(dd)].arity) != 0) {
                dlErr("dl_query_topdown: cannot pre-declare '{s}'\n", .{@as([*c]const u8, @ptrCast(&prog.decls.?[@intCast(dd)].name))});
                magic.magic_program_free(&prog);
                evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
                return -1;
            }
        }
    }

    // Compile with g_reorder=0 / g_bushy=0 so body_idx == AST body position.
    {
        const saved_reorder = gReorderRef().*;
        const saved_bushy = gBushyRef().*;
        gReorderRef().* = 0;
        gBushyRef().* = 0;
        const compile_rc = compile_rules(&edb, prog.rules, prog.n_rules, &td_crules, &n_td);
        gReorderRef().* = saved_reorder;
        gBushyRef().* = saved_bushy;
        if (compile_rc != 0) {
            dlErr("dl_query_topdown: compile of adorned program failed\n", .{});
            magic.magic_program_free(&prog);
            evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
            return -1;
        }
    }

    // Build the permutation indices the compiler declared (OP_LOOKUP_PERM).
    if (permindex_build_dirty(&edb) != 0) {
        dlErr("dl_query_topdown: perm index build failed\n", .{});
        magic.magic_program_free(&prog);
        {
            var i: c_int = 0;
            while (i < n_td) : (i += 1) compiler.compiled_rule_free(td_crules.?[@intCast(i)]);
            if (td_crules) |x| c.free(@ptrCast(x));
        }
        evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
        return -1;
    }

    if (edb.dir != null or edb.nrels != n_aliased + @as(usize, @intCast(prog.n_decls))) {
        dlErr("dl_query_topdown: internal error: compile_rules grew the eval clone's relation table\n", .{});
        magic.magic_program_free(&prog);
        {
            var i: c_int = 0;
            while (i < n_td) : (i += 1) compiler.compiled_rule_free(td_crules.?[@intCast(i)]);
            if (td_crules) |x| c.free(@ptrCast(x));
        }
        evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
        return -1;
    }

    // Locate the goal variant (adorned_goal among the decl pairs).
    {
        var dd: c_int = 0;
        while (dd < prog.n_decls) : (dd += 2) {
            if (strEq(@ptrCast(&prog.decls.?[@intCast(dd)].name), @ptrCast(&prog.adorned_goal))) {
                goal_variant_id = @divTrunc(dd, 2);
                break;
            }
        }
    }
    if (goal_variant_id < 0) {
        dlErr("dl_query_topdown: internal: adorned goal '{s}' missing\n", .{@as([*c]const u8, @ptrCast(&prog.adorned_goal))});
        magic.magic_program_free(&prog);
        {
            var i: c_int = 0;
            while (i < n_td) : (i += 1) compiler.compiled_rule_free(td_crules.?[@intCast(i)]);
            if (td_crules) |x| c.free(@ptrCast(x));
        }
        evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
        return -1;
    }

    result = td_eval(&edb, &prog, td_crules, n_td, goal_variant_id, vals, cb, user);

    magic.magic_program_free(&prog);
    {
        var i: c_int = 0;
        while (i < n_td) : (i += 1) compiler.compiled_rule_free(td_crules.?[@intCast(i)]);
        if (td_crules) |x| c.free(@ptrCast(x));
    }
    evalDbFreeOwned(&edb, n_aliased, &ZERO_OWNED);
    return result;
}

// ─── M4: snapshot publish ───────────────────────────────────────────────────

/// Best-effort recursive removal of a file or directory.
fn rmRf(path: [*c]const u8) void {
    const d = posix.opendir(path);
    if (d != null) {
        var e = posix.readdir(d);
        while (e != null) : (e = posix.readdir(d)) {
            var child: [4096:0]u8 = undefined;
            var st: posix.struct_stat = undefined;
            if (strEq(@ptrCast(&e.*.d_name), ".") or strEq(@ptrCast(&e.*.d_name), ".."))
                continue;
            _ = snprintf(&child, 4096, "%s/%s", path, @as([*c]const u8, @ptrCast(&e.*.d_name)));
            if (posix.lstat(&child, &st) == 0 and posix.S_ISDIR(st.st_mode))
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
        var st: posix.struct_stat = undefined;
        const name: [*c]const u8 = @ptrCast(&e.*.d_name);

        if (strEq(name, ".") or strEq(name, "..")) continue;
        if (name[0] < '0' or name[0] > '9') continue;

        var endp: [*c]u8 = null;
        const v = strtoul(name, @ptrCast(&endp), 10);
        if (endp[0] != 0) continue; // not a clean all-digit parse
        if (v < 1 or v > 0xFFFFFFFF) continue;

        // Only real version DIRECTORIES are enumerated.
        if (posix.fstatat(posix.dirfd(d), name, &st, 0) != 0 or !posix.S_ISDIR(st.st_mode))
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
    }
    if (vers) |x| c.free(@ptrCast(x));
}

pub export fn dl_publish_snapshot(db: ?*DlDb) c_int {
    const d = db orelse return -1;
    var snapshots_dir: [4096:0]u8 = undefined;
    var tmp_dir: [4096:0]u8 = undefined;
    var new_dir: [4096:0]u8 = undefined;
    var current_path: [4096:0]u8 = undefined;
    var buf: [4096:0]u8 = undefined;
    var renamed: c_int = 0;

    if (d.read_only != 0) return -1;
    if (d.txn != null) return -1;

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
            if (vm_execute(d, d.crules, d.n_crules) != 0)
                return -1;
            vm_clear_deltas(d);
            vm_clear_deletes(d);
        } else if (has_agg != 0) {
            if (vm_agg_eligible(d) != 0) {
                if (vm_agg_maintain(d) != 0) {
                    d.full_reeval_pending = 1;
                    return -1;
                }
            } else {
                if (vm_execute(d, d.crules, d.n_crules) != 0)
                    return -1;
                vm_clear_deltas(d);
                vm_clear_deletes(d);
            }
        } else if (has_del != 0 and vm_dred_eligible(d) == 0) {
            if (vm_execute(d, d.crules, d.n_crules) != 0)
                return -1;
            vm_clear_deltas(d);
            vm_clear_deletes(d);
        } else if (has_ins != 0 and vm_ivm_eligible(d) == 0 and vm_dred_eligible(d) == 0) {
            if (vm_execute(d, d.crules, d.n_crules) != 0)
                return -1;
            vm_clear_deltas(d);
            vm_clear_deletes(d);
        } else if (has_del != 0 or (has_ins != 0 and vm_ivm_eligible(d) == 0)) {
            if (vm_dred_delete(d) != 0) {
                d.full_reeval_pending = 1;
                return -1;
            }
        } else if (vm_ivm_eligible(d) == 0) {
            if (vm_execute(d, d.crules, d.n_crules) != 0)
                return -1;
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

    // 2. Determine new version
    const new_version = d.snap_version +% 1;

    // 3. Ensure snapshots directory exists
    _ = snprintf(&snapshots_dir, 4096, "%s/snapshots", d.dir.?);
    _ = posix.mkdir(&snapshots_dir, @as(posix.mode_t, 0o755));

    // 4. Build into <db>/snapshots/<new_v>.tmp/
    _ = snprintf(&tmp_dir, 4096, "%s/snapshots/%u.tmp", d.dir.?, new_version);
    rmRf(&tmp_dir);
    if (posix.mkdir(&tmp_dir, @as(posix.mode_t, 0o755)) != 0)
        return -1;

    // 4a. Save interner
    {
        var fwd: [4096:0]u8 = undefined;
        var rev: [4096:0]u8 = undefined;
        _ = snprintf(&fwd, 4096, "%s/symbols.dafsa", &tmp_dir);
        _ = snprintf(&rev, 4096, "%s/symbols.array", &tmp_dir);
        if (intern_save(d.ir, &fwd, &rev) != 0)
            return dlPublishFail(&tmp_dir, &new_dir, renamed);
        if (dc.dafsa_save(@ptrCast(@alignCast(intern_fwd(d.ir))), &fwd) != 0)
            return dlPublishFail(&tmp_dir, &new_dir, renamed);
    }

    // 4a'. Save the list term store.
    {
        var tpath: [4096:0]u8 = undefined;
        _ = snprintf(&tpath, 4096, "%s/terms.bin", &tmp_dir);
        if (term_save(d.terms, &tpath) != 0)
            return dlPublishFail(&tmp_dir, &new_dir, renamed);
    }

    // 4b. Save each relation + write manifest.
    {
        var manifest_path: [4096:0]u8 = undefined;
        _ = snprintf(&manifest_path, 4096, "%s/manifest.txt", &tmp_dir);
        const mf = c.fopen(&manifest_path, "w") orelse return dlPublishFail(&tmp_dir, &new_dir, renamed);

        _ = fprintf(mf, "# Datalog-DAFSA snapshot version %u\n", new_version);

        var i: usize = 0;
        while (i < d.nrels) : (i += 1) {
            var rel_path: [4096:0]u8 = undefined;

            if (d.rels[i].kind == RELK_VARIADIC) {
                var a: u8 = 0;
                _ = fprintf(mf, "%s:*:%s\n", d.rels[i].name,
                    if (vrelation.vrel_any_idb(d.rels[i].vrel) != 0) "idb" else "edb");
                a = 1;
                while (a <= MAX_VAR_ARITY) : (a += 1) {
                    var vname: [384:0]u8 = undefined;
                    const vr = vrelation.vrel_variant_or_null(d.rels[i].vrel, a);
                    if (vr == null) continue;
                    if (snprintf(&vname, 384, "%s.%d", d.rels[i].name, @as(c_int, a)) >= 384) {
                        _ = c.fclose(mf);
                        return dlPublishFail(&tmp_dir, &new_dir, renamed);
                    }
                    _ = fprintf(mf, "%s:%d:%s\n", @as([*c]const u8, @ptrCast(&vname)), @as(c_int, a),
                        if (relation.rel_is_idb(vr) != 0) "idb" else "edb");
                    _ = snprintf(&rel_path, 4096, "%s/%s.dafsa", &tmp_dir, @as([*c]const u8, @ptrCast(&vname)));
                    if (relation.rel_save(vr, &rel_path) != 0) {
                        _ = c.fclose(mf);
                        return dlPublishFail(&tmp_dir, &new_dir, renamed);
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
            _ = fprintf(mf, "%s:%d:%s\n", d.rels[i].name, @as(c_int, arity),
                if (relation.rel_is_idb(d.rels[i].rel) != 0) "idb" else "edb");

            _ = snprintf(&rel_path, 4096, "%s/%s.dafsa", &tmp_dir, d.rels[i].name);
            if (relation.rel_save(d.rels[i].rel, &rel_path) != 0) {
                _ = c.fclose(mf);
                return dlPublishFail(&tmp_dir, &new_dir, renamed);
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
                if (relation.rel_save(pe.pidx_rel, &pi_path) != 0) {
                    _ = c.fclose(mf);
                    return dlPublishFail(&tmp_dir, &new_dir, renamed);
                }
            }
        }

        if (c.fclose(mf) != 0) return dlPublishFail(&tmp_dir, &new_dir, renamed);
    }

    // 4c. fsync the tmp dir
    if (fsync_dir_path(&tmp_dir) != 0) return dlPublishFail(&tmp_dir, &new_dir, renamed);

    // 5. Atomic rename: .tmp → final dir
    _ = snprintf(&new_dir, 4096, "%s/snapshots/%u", d.dir.?, new_version);
    rmRf(&new_dir);
    if (posix.rename(&tmp_dir, &new_dir) != 0) return dlPublishFail(&tmp_dir, &new_dir, renamed);
    renamed = 1;
    if (fsync_dir_path(&snapshots_dir) != 0) return dlPublishFail(&tmp_dir, &new_dir, renamed);

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

    // 7. Invalidate cache + update snap_version
    vcache_invalidate(&d.vcache);
    d.snap_version = new_version;

    // 8. Opt-in retention
    if (d.snapshot_retain > 0)
        pruneSnapshots(d);

    return 0;
}

/// Best-effort publish failure cleanup: rm_rf(tmp) and, if renamed, rm_rf(new).
fn dlPublishFail(tmp_dir: [*c]const u8, new_dir: [*c]const u8, renamed: c_int) c_int {
    rmRf(tmp_dir);
    if (renamed != 0) rmRf(new_dir);
    return -1;
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

        var sym_path: [16384:0]u8 = undefined;
        _ = snprintf(&sym_path, 16384, "%s/symbols.dafsa", &sdir);
        const v = dc.dafsa_view_open(&sym_path) orelse return -1;
        var set: regexwalk.sym_set = std.mem.zeroes(regexwalk.sym_set);
        if (regexwalk.symset_init(&set) != 0) {
            dc.dafsa_view_close(v);
            return -1;
        }
        const ns = symbols_dfa_walk_view(@ptrCast(v), dfa, symsetAddCb, &set);
        dc.dafsa_view_close(v);
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
                const vv = view_open_cached(@constCast(&d.vcache), @ptrCast(&vname), &sdir) orelse {
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

        const vv = view_open_cached(@constCast(&d.vcache), rel_name, &sdir) orelse {
            regexwalk.symset_free(&set);
            return -1;
        };
        const n = view_filter_col(vv, arity, col, &set, cb, user);
        regexwalk.symset_free(&set);
        return n;
    }

    // In-memory path
    const idx = findRel(d, rel_name);
    if (idx < 0) return -1;

    var set: regexwalk.sym_set = std.mem.zeroes(regexwalk.sym_set);
    if (regexwalk.symset_init(&set) != 0) return -1;
    const ns = symbols_dfa_walk(@ptrCast(intern_fwd(d.ir)), dfa, symsetAddCb, &set);
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
