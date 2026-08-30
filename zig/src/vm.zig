//! vm.zig — port of src/vm.c (bytecode VM interpreter + in-memory fixpoint,
//! IVM/DRed/aggregate incremental cascade).
//!
//! Architecture (unchanged from the oracle):
//!   - exec_range() is the flat binding-table bytecode interpreter (26 opcodes).
//!   - eval_stratum_recursive() runs the semi-naive fixpoint over in-memory
//!     tuple_sets (idb/delta/next_delta per recursive head), bulk-building each
//!     IDB DAFSA ONCE at stratum end.
//!   - vm_execute() stratifies and dispatches; vm_propagate_deltas /
//!     vm_dred_delete / vm_agg_maintain are the incremental-maintenance cascade.
//!
//! Strangler-hybrid ABI: `vm_instr`/`compiled_rule`/`var_info` are the
//! byte-identical extern structs in compiler.zig; `vm_override` is defined
//! here (still dereferenced by src/topdown.c and the C tests).  `struct dl_db`/
//! `rel_entry`/`perm_index_entry` and every C function (`rel_*`, `term_*`,
//! `intern_*`, `dl_iter_*`, `permindex_*`, `symset_*`, `symbols_dfa_walk`,
//! `db_rel_at_arity_*`, `db_has_*`) are consumed through @cImport
//! ("dl_internal.h"), NOT redefined.  `tuple_set` is tupleset.zig's extern
//! struct (the only type that must cross the opaque<->native boundary).
//!
//! Oracle: src/vm.c (never modified).  All allocation goes through libc
//! (malloc/realloc/calloc/free) like the C oracle; wrapping arithmetic uses
//! `*%`/`+%`/`-%` where the C unsigned-wraps.

const std = @import("std");
const builtin = @import("builtin");
const c = std.c;

const compiler = @import("compiler.zig");
const tupleset = @import("tupleset.zig");
const regexwalk = @import("regexwalk.zig");
const parser = @import("parser.zig");

// Byte-identical extern structs owned by compiler.zig (per the brief, the
// canonical ABI types for the bytecode the still-C dl.c/topdown.c share).
const vm_instr = compiler.vm_instr;
const compiled_rule = compiler.compiled_rule;
const var_info = compiler.var_info;

// dl_internal.h pulls in dl.h/intern.h/relation.h/vrelation.h/snapshot.h/
// permindex.h/termstore.h/compiler.h/regexwalk.h — supplying dl_db, rel_entry,
// perm_index_entry, RELK_FIXED/RELK_VARIADIC, dl_tuple_cb/rel_enum_cb, and the
// extern decls for every still-C / already-ported function the VM calls.
const dx = @cImport({
    @cInclude("dl_internal.h");
});

// std.c does not re-export the C string functions the M9 string ops need.
extern "c" fn strcmp(a: [*c]const u8, b: [*c]const u8) c_int;
extern "c" fn strlen(s: [*c]const u8) usize;
extern "c" fn strncmp(a: [*c]const u8, b: [*c]const u8, n: usize) c_int;
extern "c" fn strstr(a: [*c]const u8, b: [*c]const u8) [*c]u8;

// tuple_set ops are `export fn`s in tupleset.zig (private to that module) —
// reach them through raw extern bindings, exactly like relation.zig does.
extern "c" fn ts_init(ts: ?*tupleset.tuple_set, arity: u8) c_int;
extern "c" fn ts_free(ts: ?*tupleset.tuple_set) void;
extern "c" fn ts_contains(ts: ?*const tupleset.tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_add(ts: ?*tupleset.tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_prefix(ts: ?*const tupleset.tuple_set, p: ?[*]const u32, k: u8, first_idx_out: ?[*]c_long) c_long;
extern "c" fn ts_sort(ts: ?*tupleset.tuple_set) void;
extern "c" fn ts_reset(ts: ?*tupleset.tuple_set) void;

// compile_rules is compiler.zig's `pub export fn`; declare it against OUR
// @cImport'd dl_db (each module's translate-c produces a distinct type for
// the same C struct, so it cannot be called through the imported module).
extern "c" fn compile_rules(db: ?*dx.dl_db, rules: ?[*]?*parser.rule, n_rules: c_int, out_rules: ?*?[*]?*compiled_rule, out_n: ?*c_int) c_int;

// ─── Constants (mirror src/compiler.h / src/dl_internal.h) ────────────────

const MAX_VARS = 64;
const MAX_ARITY = 8;
const MAX_RELS = 64;
const MAX_FRAMES = 8;
const FIXPOINT_ERROR_BOUND = 10000000;
const UNBOUND: u32 = 0xFFFFFFFF;

const OP_HALT: u8 = 0;
const OP_SCAN: u8 = 1;
const OP_LOOKUP: u8 = 2;
const OP_EQ: u8 = 3;
const OP_EQ_CONST: u8 = 4;
const OP_PROJECT: u8 = 5;
const OP_OPEN_REL: u8 = 6;
const OP_NEG_CHECK: u8 = 7;
const OP_AGG_ACC: u8 = 8;
const OP_AGG_EMIT: u8 = 9;
const OP_WALK: u8 = 10;
const OP_LOOKUP_PERM: u8 = 11;
const OP_HASH_JOIN: u8 = 12;
const OP_CMP: u8 = 13;
const OP_ARITH: u8 = 14;
const OP_STR_FILTER: u8 = 15;
const OP_STR_LEN: u8 = 16;
const OP_STR_BIND: u8 = 17;
const OP_MAT_BEGIN: u8 = 18;
const OP_MAT_JOIN: u8 = 19;
const OP_LIST_CONS: u8 = 20;
const OP_LIST_CAR: u8 = 21;
const OP_LIST_CDR: u8 = 22;
const OP_LIST_APPEND: u8 = 23;
const OP_LIST_MEMBER: u8 = 24;
const OP_RANGE: u8 = 25;

// ─── Public C-ABI types ───────────────────────────────────────────────────

/// dl_tuple_cb / rel_enum_cb — identical C signature (cols, arity, user).
const DlTupleCb = dx.dl_tuple_cb;

/// typedef struct { int body_idx; const tuple_set *ts; int perm_id; }
/// vm_override (vm.h:38-42) — still dereferenced by src/topdown.c and tests.
pub const vm_override = extern struct {
    body_idx: c_int,
    ts: ?*const tupleset.tuple_set,
    perm_id: c_int,
};

comptime {
    // LP64 layout: int(4) @0, pad, pointer(8) @8, int(4) @16, pad -> 24.
    std.debug.assert(@sizeOf(vm_override) == 24);
    std.debug.assert(@offsetOf(vm_override, "body_idx") == 0);
    std.debug.assert(@offsetOf(vm_override, "ts") == 8);
    std.debug.assert(@offsetOf(vm_override, "perm_id") == 16);
}

// ─── Data globals (test-observable; MUST be writable export var D/B symbols) ─

// Magic-sets skip-materialize hook (dl.c points the export-ts hook at a stack slot).
export var vm_nomaterialize: c_int = 0;
export var vm_export_relid: c_int = -1;
export var vm_export_ts: ?*tupleset.tuple_set = null;

export var vm_dred_runs: c_int = 0;
export var vm_agg_runs: c_int = 0;
export var vm_propagate_runs: c_int = 0;
export var vm_range_yields: c_long = 0;

// C exes take R_X86_64_COPY on these data globals: they get their own .bss
// copy and the loader only redirects GOT-based references, so a direct
// reference binds to this .so's local storage and never sees the exe's copy
// (exe-set counters read 0, exe-set toggles are ignored). In shared-lib
// builds every access goes through the accessors below, which load the symbol
// address via GOTPCREL — the same thing -fPIC refs in dl.c do. Test/exe
// builds bind direct (one storage domain); Debug shared-lib builds take the
// @extern fallback (the self-hosted backend emits true extern refs, and its
// inline-asm parser rejects GOTPCREL templates).
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
inline fn vmDredRunsRef() *c_int {
    return gotDataRef("vm_dred_runs", c_int);
}
inline fn vmAggRunsRef() *c_int {
    return gotDataRef("vm_agg_runs", c_int);
}
inline fn vmPropagateRunsRef() *c_int {
    return gotDataRef("vm_propagate_runs", c_int);
}
inline fn vmRangeYieldsRef() *c_long {
    return gotDataRef("vm_range_yields", c_long);
}

// ─── db helpers (mirror vm.c statics db_rel/db_find) ──────────────────────

fn dbRel(db: *const dx.dl_db, idx: c_int) ?*dx.relation {
    if (idx < 0 or @as(usize, @intCast(idx)) >= db.nrels) return null;
    return db.rels[@intCast(idx)].rel;
}

fn tsFromPending(p: ?*dx.struct_tuple_set) ?*tupleset.tuple_set {
    if (p == null) return null;
    return @ptrCast(@alignCast(p.?));
}

fn dbFind(db: *const dx.dl_db, name: [*c]const u8) c_int {
    var i: usize = 0;
    while (i < db.nrels) : (i += 1) {
        if (strcmp(db.rels[i].name, name) == 0) return @intCast(i);
    }
    return -1;
}

// ─── Tuple buffer (for frame materialization) ─────────────────────────────

const TupleBuf = struct {
    data: ?[*]u32,
    count: c_long,
    cap: c_long,
    arity: u8,
};

fn tbufCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const tb: *TupleBuf = @ptrCast(@alignCast(user orelse return 1));
    if (tb.arity == 0) tb.arity = arity;
    if (tb.count >= tb.cap) {
        const nc: c_long = if (tb.cap != 0) tb.cap *% 2 else 1024;
        const old: ?*anyopaque = if (tb.data) |d| @ptrCast(d) else null;
        const mem = c.realloc(old, @as(usize, @intCast(nc)) *
            @as(usize, tb.arity) * @sizeOf(u32)) orelse return 1;
        tb.data = @ptrCast(@alignCast(mem));
        tb.cap = nc;
    }
    const dst = tb.data.? + @as(usize, @intCast(tb.count)) * @as(usize, tb.arity);
    @memcpy(dst[0..tb.arity], cols[0..tb.arity]);
    tb.count +%= 1;
    return 0;
}

fn tbufFree(tb: *TupleBuf) void {
    if (tb.data) |d| c.free(@ptrCast(d));
    tb.* = std.mem.zeroes(TupleBuf);
}

// ─── Bindings ──────────────────────────────────────────────────────────────

const Bindings = struct {
    vals: [MAX_VARS]u32,
    valid: [MAX_VARS]u8,
};

fn bInit(b: *Bindings) void {
    @memset(b.valid[0..], 0);
}

fn bTry(b: *Bindings, s: u8, v: u32) c_int {
    if (s >= MAX_VARS) return 0;
    if (b.valid[s] != 0) return if (b.vals[s] == v) 1 else 0;
    b.vals[s] = v;
    b.valid[s] = 1;
    return 1;
}

fn bGet(b: *const Bindings, s: u8) u32 {
    return if (s < MAX_VARS and b.valid[s] != 0) b.vals[s] else UNBOUND;
}

fn bOk(b: *const Bindings, s: u8) c_int {
    return if (s < MAX_VARS and b.valid[s] != 0) 1 else 0;
}

fn bSave(dst: *Bindings, src: *const Bindings) void {
    @memcpy(dst.vals[0..], src.vals[0..]);
    @memcpy(dst.valid[0..], src.valid[0..]);
}

const bLoad = bSave;

// ─── Aggregate accumulator (M3) ───────────────────────────────────────────

const AggBucket = struct {
    key: ?[*]u32,
    count: u32,
    sum: u32,
    min: u32,
    max: u32,
    valid: c_int,
};

const AggAccum = struct {
    buckets: ?[*]AggBucket,
    n_buckets: c_int,
    cap: c_int,
    n_key: u8,
    op: u8,
    src_slot: u8,
    res_slot: u8,
    group_slots: [8]u8,
    head_rel_id: u8,
    head_arity: u8,
    head_slots: [8]u8,
    dry: c_int,
    cb: DlTupleCb,
    user: ?*anyopaque,
};

fn aggHash(key: [*]const u32, n: c_int) u64 {
    var h: u64 = 14695981039346656037;
    var i: c_int = 0;
    while (i < n) : (i += 1) {
        const v = key[@intCast(i)];
        h ^= v & 0xFF;
        h *%= 1099511628211;
        h ^= (v >> 8) & 0xFF;
        h *%= 1099511628211;
        h ^= (v >> 16) & 0xFF;
        h *%= 1099511628211;
        h ^= (v >> 24) & 0xFF;
        h *%= 1099511628211;
    }
    return h;
}

fn aggInit(ac: *AggAccum) c_int {
    ac.* = std.mem.zeroes(AggAccum);
    ac.cap = 16;
    const mem = c.calloc(@as(usize, @intCast(ac.cap)), @sizeOf(AggBucket)) orelse return -1;
    ac.buckets = @ptrCast(@alignCast(mem));
    return 0;
}

fn aggGrow(ac: *AggAccum) c_int {
    const new_cap: c_int = ac.cap *% 2;
    const mem = c.calloc(@as(usize, @intCast(new_cap)), @sizeOf(AggBucket)) orelse return -1;
    const nb: [*]AggBucket = @ptrCast(@alignCast(mem));
    var i: c_int = 0;
    while (i < ac.cap) : (i += 1) {
        const ob = &ac.buckets.?[@intCast(i)];
        if (ob.key == null) continue;
        var idx: usize = @intCast(aggHash(ob.key.?, ac.n_key) & (@as(u64, @intCast(new_cap)) - 1));
        while (nb[idx].key != null)
            idx = (idx +% 1) & (@as(usize, @intCast(new_cap)) - 1);
        nb[idx] = ob.*;
    }
    c.free(@ptrCast(ac.buckets.?));
    ac.buckets = nb;
    ac.cap = new_cap;
    return 0;
}

fn aggFindOrCreate(ac: *AggAccum, key: [*]const u32) ?*AggBucket {
    if (ac.n_buckets +% 1 > ac.cap - (ac.cap >> 2)) {
        if (aggGrow(ac) != 0) return null;
    }
    var idx: usize = @intCast(aggHash(key, ac.n_key) & (@as(u64, @intCast(ac.cap)) - 1));
    while (ac.buckets.?[idx].key != null) {
        if (std.mem.eql(u32, ac.buckets.?[idx].key.?[0..@intCast(ac.n_key)], key[0..@intCast(ac.n_key)]))
            return &ac.buckets.?[idx];
        idx = (idx +% 1) & (@as(usize, @intCast(ac.cap)) - 1);
    }
    const b = &ac.buckets.?[idx];
    {
        const ksz: usize = @as(usize, @intCast(ac.n_key)) * @sizeOf(u32);
        const mem = c.malloc(if (ksz < 1) 1 else ksz) orelse return null;
        b.key = @ptrCast(@alignCast(mem));
    }
    if (ac.n_key > 0)
        @memcpy(b.key.?[0..ac.n_key], key[0..ac.n_key]);
    b.count = 0;
    b.sum = 0;
    b.min = 0;
    b.max = 0;
    b.valid = 0;
    ac.n_buckets +%= 1;
    return b;
}

fn aggFree(ac: *AggAccum) void {
    if (ac.buckets == null) return;
    var i: c_int = 0;
    while (i < ac.cap) : (i += 1) {
        if (ac.buckets.?[@intCast(i)].key) |k| c.free(@ptrCast(k));
    }
    c.free(@ptrCast(ac.buckets.?));
    ac.* = std.mem.zeroes(AggAccum);
}

// ─── Frame ─────────────────────────────────────────────────────────────────

const VmFrame = struct {
    ip: c_int,
    op: u8,
    tuples: TupleBuf,
    idx: c_long,
    saved: Bindings,
    perm: [*c]const u8,
    perm_storage: [8]u8,
    it: ?*dx.dl_iter,
    last: u32,
    started: u8,
    lo: u32,
    hi: u32,
};

// ─── Override lookup ───────────────────────────────────────────────────────

fn findOv(body_idx: c_int, ov: ?[*]const vm_override, n_ov: c_int) ?*const tupleset.tuple_set {
    if (ov == null) return null;
    var i: c_int = 0;
    while (i < n_ov) : (i += 1) {
        if (ov.?[@intCast(i)].body_idx == body_idx) return ov.?[@intCast(i)].ts;
    }
    return null;
}

// ─── Row binding (perm-aware) ──────────────────────────────────────────────

fn bindRowPerm(in: *const vm_instr, row: [*c]const u32, sc: c_int, ar: c_int, perm: [*c]const u8, b: *Bindings) c_int {
    var pp: c_int = sc;
    while (pp < ar) : (pp += 1) {
        const oc: c_int = if (perm != null) @intCast(perm[@intCast(pp)]) else pp;
        const s: u8 = in.slots[@intCast(oc)];
        if (s == 0xFF) continue;
        if (bTry(b, s, row[@intCast(pp)]) == 0) return 0;
    }
    return 1;
}

fn seekValidPerm(f: *VmFrame, in: *const vm_instr, sc: c_int, ar: c_int, perm: [*c]const u8, b: *Bindings) c_int {
    while (f.idx < f.tuples.count) {
        bLoad(b, &f.saved);
        if (bindRowPerm(in, f.tuples.data.? + @as(usize, @intCast(f.idx)) * @as(usize, f.tuples.arity), sc, ar, perm, b) != 0)
            return 1;
        f.idx +%= 1;
    }
    return 0;
}

fn seekValid(f: *VmFrame, in: *const vm_instr, sc: c_int, ar: c_int, b: *Bindings) c_int {
    return seekValidPerm(f, in, sc, ar, null, b);
}

fn rangeResume(f: *VmFrame, fi: *const vm_instr, b: *Bindings) c_int {
    var cols: [8]u32 = undefined;
    while (true) {
        const rc = dx.dl_iter_next(f.it, &cols);
        if (rc == 1) {
            const v = cols[0];
            if (v < f.lo) continue;
            if (v >= f.hi) return 0;
            if (f.started != 0 and v == f.last) continue;
            f.last = v;
            f.started = 1;
            vmRangeYieldsRef().* +%= 1;
            bLoad(b, &f.saved);
            _ = bTry(b, fi.slots[0], v);
            return 1;
        }
        return 0; // iterator exhausted (rc==0) or depth overflow (rc<0)
    }
}

fn backtrack(frames: []VmFrame, sp: *c_int, b: *Bindings, p: [*]const vm_instr, ip: *c_int) c_int {
    while (sp.* > 0) {
        const f = &frames[@intCast(sp.* - 1)];
        if (f.op == OP_RANGE) {
            const fi = &p[@intCast(f.ip)];
            if (rangeResume(f, fi, b) != 0) {
                ip.* = f.ip + 1;
                return 1;
            }
            dx.dl_iter_close(f.it);
            f.it = null;
            tbufFree(&f.tuples);
            sp.* -= 1;
            continue;
        }
        f.idx +%= 1;
        {
            const fi = &p[@intCast(f.ip)];
            const sc: c_int = if (f.op == OP_LOOKUP or f.op == OP_LOOKUP_PERM) @intCast(fi.b) else 0;
            var found: c_int = 0;
            if (f.perm != null) {
                found = seekValidPerm(f, fi, sc, @intCast(f.tuples.arity), f.perm, b);
            } else {
                found = seekValid(f, fi, sc, @intCast(f.tuples.arity), b);
            }
            if (found != 0) {
                ip.* = f.ip + 1;
                return 1;
            }
        }
        tbufFree(&f.tuples);
        sp.* -= 1;
    }
    return 0;
}

// ─── BUSHY (v2): buffer hash join helpers ──────────────────────────────────

fn matCaptureCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ts: *tupleset.tuple_set = @ptrCast(@alignCast(user orelse return -1));
    return if (ts_add(ts, cols) < 0) 1 else 0;
}

fn matHash(k: [*c]const u32, cc: c_int) u64 {
    var h: u64 = 14695981039346656037;
    var i: c_int = 0;
    while (i < cc) : (i += 1) {
        const v = k[@intCast(i)];
        h ^= v & 0xFF;
        h *%= 1099511628211;
        h ^= (v >> 8) & 0xFF;
        h *%= 1099511628211;
        h ^= (v >> 16) & 0xFF;
        h *%= 1099511628211;
        h ^= (v >> 24) & 0xFF;
        h *%= 1099511628211;
    }
    return h;
}

fn matJoinBuild(l: *const tupleset.tuple_set, r: *const tupleset.tuple_set, cc: c_int, tb: *TupleBuf) c_int {
    const nL: c_int = @intCast(l.arity);
    const nR: c_int = @intCast(r.arity);
    const nLp: c_int = nL - cc;
    const nRp: c_int = nR - cc;
    const out_ar: c_int = cc + nLp + nRp;
    var i: c_long = 0;
    var ri: c_long = 0;
    var hcap: usize = 1;
    while (hcap < @as(usize, @intCast(r.count)) *% 2 +% 1) hcap <<= 1;
    const bmem = c.calloc(hcap, @sizeOf(c_long)) orelse return -1;
    const bucket: [*]c_long = @ptrCast(@alignCast(bmem));
    const nxt_n: c_long = if (r.count > 0) r.count else 1;
    const nmem = c.malloc(@as(usize, @intCast(nxt_n)) * @sizeOf(c_long)) orelse {
        c.free(@ptrCast(bucket));
        return -1;
    };
    const nxt: [*]c_long = @ptrCast(@alignCast(nmem));

    while (i < r.count) : (i += 1) {
        const row = r.data.? + @as(usize, @intCast(i)) * @as(usize, @intCast(nR));
        const slot: usize = @intCast(matHash(row, cc) & (@as(u64, @intCast(hcap)) - 1));
        nxt[@intCast(i)] = bucket[slot] -% 1;
        bucket[slot] = i +% 1;
    }

    tb.arity = @intCast(out_ar);
    i = 0;
    while (i < l.count) : (i += 1) {
        const lrow = l.data.? + @as(usize, @intCast(i)) * @as(usize, @intCast(nL));
        const slot: usize = @intCast(matHash(lrow, cc) & (@as(u64, @intCast(hcap)) - 1));
        ri = bucket[slot] -% 1;
        while (ri >= 0) {
            const rrow = r.data.? + @as(usize, @intCast(ri)) * @as(usize, @intCast(nR));
            var k: c_int = 0;
            var match: c_int = 1;
            while (k < cc) : (k += 1) {
                if (lrow[@intCast(k)] != rrow[@intCast(k)]) {
                    match = 0;
                    break;
                }
            }
            if (match != 0) {
                if (tb.count >= tb.cap) {
                    const nc: c_long = if (tb.cap != 0) tb.cap *% 2 else 1024;
                    const old: ?*anyopaque = if (tb.data) |d| @ptrCast(d) else null;
                    const nd = c.realloc(old, @as(usize, @intCast(nc)) *
                        @as(usize, @intCast(out_ar)) * @sizeOf(u32)) orelse {
                        c.free(@ptrCast(bucket));
                        c.free(@ptrCast(nxt));
                        return -1;
                    };
                    tb.data = @ptrCast(@alignCast(nd));
                    tb.cap = nc;
                }
                const o = tb.data.? + @as(usize, @intCast(tb.count)) * @as(usize, @intCast(out_ar));
                k = 0;
                while (k < cc) : (k += 1) o[@intCast(k)] = lrow[@intCast(k)];
                k = 0;
                while (k < nLp) : (k += 1) o[@intCast(cc + k)] = lrow[@intCast(cc + k)];
                k = 0;
                while (k < nRp) : (k += 1) o[@intCast(cc + nLp + k)] = rrow[@intCast(cc + k)];
                tb.count +%= 1;
            }
            ri = nxt[@intCast(ri)];
        }
    }
    c.free(@ptrCast(bucket));
    c.free(@ptrCast(nxt));
    return 0;
}

// ─── OP_WALK symbol-walk callback ──────────────────────────────────────────

fn symsetAddCb(sym_id: u32, user: ?*anyopaque) callconv(.c) c_int {
    const set: *dx.sym_set = @ptrCast(@alignCast(user orelse return -1));
    return dx.symset_add(set, sym_id);
}

// ─── Bytecode interpreter ──────────────────────────────────────────────────

fn execRange(
    db: *dx.dl_db,
    cr: *const compiler.compiled_rule,
    start: c_int,
    end: c_int,
    ov: ?[*]const vm_override,
    n_ov: c_int,
    dry: c_int,
    cb: DlTupleCb,
    user: ?*anyopaque,
    capture: DlTupleCb,
    capture_user: ?*anyopaque,
    pool: ?[*]tupleset.tuple_set,
    n_pool: c_int,
) c_long {
    const p: [*]const vm_instr = cr.instrs.?;
    var b: Bindings = undefined;
    var frames: [MAX_FRAMES]VmFrame = undefined;
    var sp: c_int = 0;
    var ip: c_int = start;
    var rc: c_long = 0;

    var acc: AggAccum = std.mem.zeroes(AggAccum);
    var agg_acc_ip: c_int = -1;
    var agg_emit_ip: c_int = -1;
    var acc_init: c_int = 0;
    {
        var k: c_int = start;
        while (k < end) : (k += 1) {
            if (p[@intCast(k)].op == OP_AGG_ACC) agg_acc_ip = k else if (p[@intCast(k)].op == OP_AGG_EMIT) agg_emit_ip = k;
        }
        if (agg_acc_ip >= 0 and agg_emit_ip >= 0) {
            const ai = &p[@intCast(agg_acc_ip)];
            const ei = &p[@intCast(agg_emit_ip)];
            if (aggInit(&acc) != 0) return -1;
            acc.n_key = ai.a;
            acc.op = ai.b;
            acc.res_slot = ai.c;
            k = 0;
            while (k < acc.n_key) : (k += 1) acc.group_slots[@intCast(k)] = ai.slots[@intCast(k)];
            acc.src_slot = ai.slots[@intCast(acc.n_key)];
            acc.head_rel_id = ei.a;
            acc.head_arity = ei.b;
            k = 0;
            while (k < acc.head_arity) : (k += 1) acc.head_slots[@intCast(k)] = ei.slots[@intCast(k)];
            acc.dry = dry;
            acc.cb = cb;
            acc.user = user;
            acc_init = 1;
        }
    }

    bInit(&b);
    @memset(std.mem.asBytes(&frames), 0);

    while (ip < end) {
        const in: *const vm_instr = &p[@intCast(ip)];

        switch (in.op) {
            OP_SCAN => blk: {
                if (sp >= MAX_FRAMES) return -1;
                const f = &frames[@intCast(sp)];
                f.ip = ip;
                f.op = OP_SCAN;
                f.idx = 0;
                f.tuples = std.mem.zeroes(TupleBuf);

                const ov_ts = findOv(@intCast(in.body_idx), ov, n_ov);

                if (ov_ts != null and ov_ts.?.count > 0) {
                    f.tuples.arity = ov_ts.?.arity;
                    f.tuples.count = ov_ts.?.count;
                    f.tuples.cap = ov_ts.?.count;
                    const mem = c.malloc(@as(usize, @intCast(ov_ts.?.count)) *
                        @as(usize, ov_ts.?.arity) * @sizeOf(u32)) orelse return -1;
                    f.tuples.data = @ptrCast(@alignCast(mem));
                    @memcpy(f.tuples.data.?[0 .. @as(usize, @intCast(ov_ts.?.count)) * @as(usize, ov_ts.?.arity)], ov_ts.?.data.?[0 .. @as(usize, @intCast(ov_ts.?.count)) * @as(usize, ov_ts.?.arity)]);
                } else {
                    const r = dx.db_rel_at_arity_ro(db, in.a, in.b);
                    if (r == null) {
                        ip = end;
                        break :blk;
                    }
                    f.tuples.arity = in.b;
                    _ = dx.rel_prefix(r, null, 0, tbufCb, &f.tuples);
                }

                if (f.tuples.count == 0) {
                    tbufFree(&f.tuples);
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }

                bSave(&f.saved, &b);
                if (seekValid(f, in, 0, @intCast(f.tuples.arity), &b) == 0) {
                    tbufFree(&f.tuples);
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                sp += 1;
                ip += 1;
            },

            OP_WALK => blk: {
                if (sp >= MAX_FRAMES) return -1;
                const f = &frames[@intCast(sp)];
                f.ip = ip;
                f.op = OP_SCAN;
                f.idx = 0;
                f.tuples = std.mem.zeroes(TupleBuf);

                const pat_idx: c_int = @intCast(in.imm);
                if (pat_idx < 0 or pat_idx >= cr.n_patterns) {
                    ip = end;
                    break :blk;
                }
                const dfa = cr.patterns.?[@intCast(pat_idx)];

                const r = dx.db_rel_at_arity_ro(db, in.a, in.b);
                if (r == null) {
                    ip = end;
                    break :blk;
                }
                f.tuples.arity = in.b;

                var set: dx.sym_set = undefined;
                if (dx.symset_init(&set) != 0) {
                    ip = end;
                    break :blk;
                }
                const ns = dx.symbols_dfa_walk(dx.intern_fwd(db.ir), @ptrCast(@alignCast(dfa)), symsetAddCb, &set);
                if (ns < 0) {
                    dx.symset_free(&set);
                    ip = end;
                    break :blk;
                }
                _ = dx.rel_filter_col(r, in.c, &set, tbufCb, &f.tuples);
                dx.symset_free(&set);

                if (f.tuples.count == 0) {
                    tbufFree(&f.tuples);
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }

                bSave(&f.saved, &b);
                if (seekValid(f, in, 0, @intCast(f.tuples.arity), &b) == 0) {
                    tbufFree(&f.tuples);
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                sp += 1;
                ip += 1;
            },

            OP_LOOKUP => blk: {
                if (sp >= MAX_FRAMES) return -1;
                const f = &frames[@intCast(sp)];
                f.ip = ip;
                f.op = OP_LOOKUP;
                f.idx = 0;
                f.tuples = std.mem.zeroes(TupleBuf);

                const k: c_int = @intCast(in.b);
                var pref: [8]u32 = undefined;
                var pk: c_int = 0;
                while (pk < k) : (pk += 1)
                    pref[@intCast(pk)] = bGet(&b, in.slots[@intCast(pk)]);

                const ov_ts = findOv(@intCast(in.body_idx), ov, n_ov);

                if (ov_ts != null and ov_ts.?.count > 0) {
                    var first: [1]c_long = undefined;
                    const cnt = ts_prefix(ov_ts, &pref, @intCast(k), &first);
                    if (cnt == 0) {
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    f.tuples.arity = ov_ts.?.arity;
                    f.tuples.count = cnt;
                    f.tuples.cap = cnt;
                    const mem = c.malloc(@as(usize, @intCast(cnt)) * @as(usize, ov_ts.?.arity) * @sizeOf(u32)) orelse return -1;
                    f.tuples.data = @ptrCast(@alignCast(mem));
                    @memcpy(f.tuples.data.?[0 .. @as(usize, @intCast(cnt)) * @as(usize, ov_ts.?.arity)], ov_ts.?.data.?[@as(usize, @intCast(first[0])) * @as(usize, ov_ts.?.arity) ..][0 .. @as(usize, @intCast(cnt)) * @as(usize, ov_ts.?.arity)]);
                } else {
                    const r = dx.db_rel_at_arity_ro(db, in.a, in.c);
                    if (r == null) {
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    f.tuples.arity = in.c;
                    _ = dx.rel_prefix(r, &pref, @intCast(k), tbufCb, &f.tuples);
                    if (f.tuples.count == 0) {
                        tbufFree(&f.tuples);
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                }

                bSave(&f.saved, &b);
                if (seekValid(f, in, k, @intCast(f.tuples.arity), &b) == 0) {
                    tbufFree(&f.tuples);
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                sp += 1;
                ip += 1;
            },

            OP_LOOKUP_PERM => blk: {
                if (sp >= MAX_FRAMES) return -1;

                const perm_id: c_int = @intCast(in.imm);
                const rel_id: c_int = @intCast(in.a);
                const k: c_int = @intCast(in.b);
                const ar: c_int = @intCast(in.c);
                const perm_arr = dx.dl_db_get_perm(db, rel_id, perm_id);
                if (perm_arr == null) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }

                const f = &frames[@intCast(sp)];
                f.ip = ip;
                f.op = OP_LOOKUP_PERM;
                f.idx = 0;
                f.perm = perm_arr;
                f.tuples = std.mem.zeroes(TupleBuf);

                var pref: [8]u32 = undefined;
                var pk: c_int = 0;
                while (pk < k) : (pk += 1) {
                    const oc: c_int = @intCast(perm_arr[@intCast(pk)]);
                    pref[@intCast(pk)] = bGet(&b, in.slots[@intCast(oc)]);
                }

                const ov_ts = findOv(@intCast(in.body_idx), ov, n_ov);

                if (ov_ts != null and ov_ts.?.count > 0) {
                    var first: [1]c_long = undefined;
                    const cnt = ts_prefix(ov_ts, &pref, @intCast(k), &first);
                    if (cnt == 0) {
                        f.perm = null;
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    f.tuples.arity = ov_ts.?.arity;
                    f.tuples.count = cnt;
                    f.tuples.cap = cnt;
                    const mem = c.malloc(@as(usize, @intCast(cnt)) * @as(usize, ov_ts.?.arity) * @sizeOf(u32)) orelse return -1;
                    f.tuples.data = @ptrCast(@alignCast(mem));
                    @memcpy(f.tuples.data.?[0 .. @as(usize, @intCast(cnt)) * @as(usize, ov_ts.?.arity)], ov_ts.?.data.?[@as(usize, @intCast(first[0])) * @as(usize, ov_ts.?.arity) ..][0 .. @as(usize, @intCast(cnt)) * @as(usize, ov_ts.?.arity)]);
                } else {
                    const pr = dx.dl_db_get_perm_rel(db, rel_id, perm_id);
                    if (pr == null) {
                        f.perm = null;
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    f.tuples.arity = @intCast(ar);
                    _ = dx.rel_prefix(pr, &pref, @intCast(k), tbufCb, &f.tuples);
                    if (f.tuples.count == 0) {
                        tbufFree(&f.tuples);
                        f.perm = null;
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                }

                bSave(&f.saved, &b);
                if (seekValidPerm(f, in, k, ar, perm_arr, &b) == 0) {
                    tbufFree(&f.tuples);
                    f.perm = null;
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                sp += 1;
                ip += 1;
            },

            OP_HASH_JOIN => blk: {
                if (sp >= MAX_FRAMES) return -1;

                const rel_id: c_int = @intCast(in.a);
                const k: c_int = @intCast(in.b);
                const ar: c_int = @intCast(in.c);

                const f = &frames[@intCast(sp)];
                f.ip = ip;
                f.op = OP_HASH_JOIN;
                f.idx = 0;
                f.perm = null;
                f.tuples = std.mem.zeroes(TupleBuf);

                const ov_ts = findOv(@intCast(in.body_idx), ov, n_ov);

                var all_ts: tupleset.tuple_set = undefined;
                if (ts_init(&all_ts, @intCast(ar)) != 0) return -1;

                if (ov_ts != null and ov_ts.?.count > 0) {
                    var ci: c_long = 0;
                    while (ci < ov_ts.?.count) : (ci += 1) {
                        _ = ts_add(&all_ts, ov_ts.?.data.? + @as(usize, @intCast(ci)) * @as(usize, ov_ts.?.arity));
                    }
                } else {
                    const r = dx.db_rel_at_arity_ro(db, rel_id, @intCast(ar));
                    if (r == null) {
                        ts_free(&all_ts);
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    _ = dx.rel_prefix(r, null, 0, dx.ts_sink_cb, &all_ts);
                    ts_sort(&all_ts);
                }

                if (all_ts.count == 0) {
                    ts_free(&all_ts);
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }

                var pj: c_int = 0;
                while (pj < ar) : (pj += 1) {
                    const shamt: u5 = @intCast(3 * @as(u32, @intCast(pj)));
                    f.perm_storage[@intCast(pj)] = @intCast((in.imm >> shamt) & 7);
                }
                const perm_arr: [*c]const u8 = &f.perm_storage;

                {
                    var match_ts: tupleset.tuple_set = undefined;
                    if (ts_init(&match_ts, @intCast(ar)) != 0) {
                        ts_free(&all_ts);
                        return -1;
                    }

                    var ci: c_long = 0;
                    while (ci < all_ts.count) : (ci += 1) {
                        const row = all_ts.data.? + @as(usize, @intCast(ci)) * @as(usize, all_ts.arity);
                        var match: c_int = 1;
                        var pk2: c_int = 0;
                        while (pk2 < k) : (pk2 += 1) {
                            const oc: c_int = @intCast(perm_arr[@intCast(pk2)]);
                            const bound_val = bGet(&b, in.slots[@intCast(oc)]);
                            if (bound_val != row[@intCast(oc)]) {
                                match = 0;
                                break;
                            }
                        }
                        if (match != 0) {
                            var prow: [8]u32 = undefined;
                            var jj: c_int = 0;
                            while (jj < ar) : (jj += 1)
                                prow[@intCast(jj)] = row[@intCast(perm_arr[@intCast(jj)])];
                            _ = ts_add(&match_ts, &prow);
                        }
                    }

                    if (match_ts.count == 0) {
                        ts_free(&match_ts);
                        ts_free(&all_ts);
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }

                    ts_sort(&match_ts);

                    f.tuples.arity = @intCast(ar);
                    f.tuples.count = match_ts.count;
                    f.tuples.cap = match_ts.count;
                    const mem = c.malloc(@as(usize, @intCast(match_ts.count)) * @as(usize, @intCast(ar)) * @sizeOf(u32)) orelse {
                        ts_free(&match_ts);
                        ts_free(&all_ts);
                        return -1;
                    };
                    f.tuples.data = @ptrCast(@alignCast(mem));
                    @memcpy(f.tuples.data.?[0 .. @as(usize, @intCast(match_ts.count)) * @as(usize, @intCast(ar))], match_ts.data.?[0 .. @as(usize, @intCast(match_ts.count)) * @as(usize, @intCast(ar))]);
                    ts_free(&match_ts);
                }

                ts_free(&all_ts);

                bSave(&f.saved, &b);
                f.perm = perm_arr;

                if (seekValidPerm(f, in, k, ar, perm_arr, &b) == 0) {
                    tbufFree(&f.tuples);
                    f.perm = null;
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                sp += 1;
                ip += 1;
            },

            OP_MAT_BEGIN => blk: {
                const buf: c_int = @intCast(in.a);
                const end_ip: c_int = @intCast(in.imm);
                const ar: c_int = @intCast(in.b);
                if (pool == null or buf < 0 or buf >= n_pool) {
                    ip = end;
                    break :blk;
                }
                if (end_ip <= ip + 1 or end_ip > end) {
                    ip = end;
                    break :blk;
                }
                const ts = &pool.?[@intCast(buf)];
                if (ts.arity == 0) {
                    if (ts_init(ts, @intCast(ar)) != 0) return -1;
                }
                const sub = execRange(db, cr, ip + 1, end_ip, ov, n_ov, dry, cb, user, matCaptureCb, ts, pool, n_pool);
                if (sub < 0) return -1;
                ip = end_ip;
            },

            OP_MAT_JOIN => blk: {
                const Lb: c_int = @intCast(in.a);
                const Rb: c_int = @intCast(in.b);
                const cc: c_int = @intCast(in.c);
                if (pool == null or Lb >= n_pool or Rb >= n_pool) {
                    ip = end;
                    break :blk;
                }
                const L = &pool.?[@intCast(Lb)];
                const R = &pool.?[@intCast(Rb)];
                if (L.arity == 0 or R.arity == 0) {
                    ip = end;
                    break :blk;
                }
                const nLp: c_int = @as(c_int, L.arity) - cc;
                const nRp: c_int = @as(c_int, R.arity) - cc;
                const out_ar: c_int = cc + nLp + nRp;
                if (nLp < 0 or nRp < 0 or out_ar > MAX_ARITY) {
                    ip = end;
                    break :blk;
                }

                if (sp >= MAX_FRAMES) return -1;
                const f = &frames[@intCast(sp)];
                f.ip = ip;
                f.op = OP_MAT_JOIN;
                f.idx = 0;
                f.perm = null;
                f.tuples = std.mem.zeroes(TupleBuf);

                if (matJoinBuild(L, R, cc, &f.tuples) != 0) return -1;

                if (f.tuples.count == 0) {
                    tbufFree(&f.tuples);
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                bSave(&f.saved, &b);
                if (seekValid(f, in, 0, out_ar, &b) == 0) {
                    tbufFree(&f.tuples);
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                sp += 1;
                ip += 1;
            },

            OP_NEG_CHECK => blk: {
                const r = dx.db_rel_at_arity_ro(db, in.a, in.b);
                if (r == null) {
                    ip += 1;
                    break :blk;
                }
                var cols: [8]u32 = undefined;
                const ar: c_int = @intCast(in.b);
                var all: c_int = 1;
                var j: c_int = 0;
                while (j < ar) : (j += 1) {
                    const s: u8 = in.slots[@intCast(j)];
                    if (bOk(&b, s) == 0) {
                        all = 0;
                        break;
                    }
                    cols[@intCast(j)] = bGet(&b, s);
                }
                if (all == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                if (dx.rel_exact(r, &cols) != 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_EQ => blk: {
                const va: u8 = in.a;
                const vb: u8 = in.b;
                const av = bGet(&b, va);
                const bv = bGet(&b, vb);
                const ab = bOk(&b, va);
                const bb = bOk(&b, vb);

                if (ab != 0 and bb != 0) {
                    if (av != bv) {
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                } else if (ab != 0 and bb == 0) {
                    _ = bTry(&b, vb, av);
                } else if (ab == 0 and bb != 0) {
                    _ = bTry(&b, va, bv);
                }
                ip += 1;
            },

            OP_EQ_CONST => blk: {
                const s: u8 = in.a;
                const cv: u32 = in.imm;
                if (bOk(&b, s) != 0) {
                    if (bGet(&b, s) != cv) {
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                } else {
                    _ = bTry(&b, s, cv);
                }
                ip += 1;
            },

            OP_CMP => blk: {
                const av = bGet(&b, in.a);
                const bv = bGet(&b, in.b);
                var pass: c_int = 0;
                switch (in.imm) {
                    0 => {
                        pass = if (av < bv) 1 else 0;
                    },
                    1 => {
                        pass = if (av <= bv) 1 else 0;
                    },
                    2 => {
                        pass = if (av > bv) 1 else 0;
                    },
                    3 => {
                        pass = if (av >= bv) 1 else 0;
                    },
                    4 => {
                        pass = if (av != bv) 1 else 0;
                    },
                    else => {
                        pass = 0;
                    },
                }
                if (pass == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_ARITH => blk: {
                const av = bGet(&b, in.a);
                const bv = bGet(&b, in.b);
                var r: u32 = 0;
                var ok: c_int = 1;
                switch (in.imm) {
                    0 => {
                        r = av +% bv;
                    },
                    1 => {
                        r = av -% bv;
                    },
                    2 => {
                        r = av *% bv;
                    },
                    3 => {
                        if (bv == 0) ok = 0 else r = av / bv;
                    },
                    4 => {
                        if (bv == 0) ok = 0 else r = av % bv;
                    },
                    else => {
                        ok = 0;
                    },
                }
                if (ok == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                if (bTry(&b, in.c, r) == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_STR_FILTER => blk: {
                const as = dx.intern_str_of(db.ir, bGet(&b, in.a));
                const bs = dx.intern_str_of(db.ir, bGet(&b, in.b));
                var pass: c_int = 0;
                if (as != null and bs != null) {
                    const al = strlen(as);
                    const bl = strlen(bs);
                    switch (in.imm) {
                        0 => {
                            pass = if (bl <= al and strncmp(as, bs, bl) == 0) 1 else 0;
                        },
                        1 => {
                            pass = if (bl <= al and strcmp(as + al - bl, bs) == 0) 1 else 0;
                        },
                        2 => {
                            pass = if (strstr(as, bs) != null) 1 else 0;
                        },
                        else => {
                            pass = 0;
                        },
                    }
                }
                if (pass == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_STR_LEN => blk: {
                const v = bGet(&b, in.a);
                if (dx.term_is_list(db.terms, v) != 0) {
                    const len = dx.term_length(db.terms, v);
                    if (bTry(&b, in.c, len) == 0) {
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    ip += 1;
                    break :blk;
                }
                const s = dx.intern_str_of(db.ir, v);
                if (s == null) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                {
                    const len: u32 = @intCast(strlen(s));
                    if (bTry(&b, in.c, len) == 0) {
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                }
                ip += 1;
            },

            OP_STR_BIND => blk: {
                var sid: u32 = 0;
                if (in.imm == 0) {
                    const as = dx.intern_str_of(db.ir, bGet(&b, in.a));
                    const bs = dx.intern_str_of(db.ir, bGet(&b, in.b));
                    if (as != null and bs != null) {
                        const al = strlen(as);
                        const bl = strlen(bs);
                        if (al + bl <= 4096) {
                            const mem = c.malloc(al + bl + 1);
                            if (mem != null) {
                                const buf: [*]u8 = @ptrCast(@alignCast(mem));
                                @memcpy(buf[0..al], as[0..al]);
                                @memcpy(buf[al..][0..bl], bs[0..bl]);
                                buf[al + bl] = 0;
                                sid = dx.intern_str(db.ir, buf);
                                c.free(@ptrCast(buf));
                            }
                        }
                    }
                } else if (in.imm == 1 or in.imm == 2) {
                    const as = dx.intern_str_of(db.ir, bGet(&b, in.a));
                    if (as != null) {
                        const al = strlen(as);
                        if (al <= 4096) {
                            const mem = c.malloc(al + 1);
                            if (mem != null) {
                                const buf: [*]u8 = @ptrCast(@alignCast(mem));
                                var j: usize = 0;
                                while (j < al) : (j += 1) {
                                    var ch: u8 = as[j];
                                    if (in.imm == 1) {
                                        if (ch >= 'A' and ch <= 'Z')
                                            ch += ('a' - 'A');
                                    } else {
                                        if (ch >= 'a' and ch <= 'z')
                                            ch -= ('a' - 'A');
                                    }
                                    buf[j] = ch;
                                }
                                buf[al] = 0;
                                sid = dx.intern_str(db.ir, buf);
                                c.free(@ptrCast(buf));
                            }
                        }
                    }
                }
                if (sid == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                if (bTry(&b, in.c, sid) == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_LIST_CONS => blk: {
                const h = dx.term_cons(db.terms, bGet(&b, in.b), bGet(&b, in.c));
                if (h == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                if (bTry(&b, in.a, h) == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_LIST_CAR => blk: {
                const opv = bGet(&b, in.a);
                if (dx.term_is_list(db.terms, opv) == 0 or opv == dx.TERM_NIL) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                if (bTry(&b, in.c, dx.term_car(db.terms, opv)) == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_LIST_CDR => blk: {
                const opv = bGet(&b, in.a);
                if (dx.term_is_list(db.terms, opv) == 0 or opv == dx.TERM_NIL) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                if (bTry(&b, in.c, dx.term_cdr(db.terms, opv)) == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_LIST_APPEND => blk: {
                const r = dx.term_append(db.terms, bGet(&b, in.a), bGet(&b, in.b));
                if (r == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                if (bTry(&b, in.c, r) == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                ip += 1;
            },

            OP_LIST_MEMBER => blk: {
                const lv = bGet(&b, in.a);
                if (bOk(&b, in.a) == 0 or dx.term_is_list(db.terms, lv) == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                if (bOk(&b, in.b) != 0) {
                    const xv = bGet(&b, in.b);
                    var cur = lv;
                    var found: c_int = 0;
                    while (dx.term_is_list(db.terms, cur) != 0 and cur != dx.TERM_NIL) {
                        if (dx.term_car(db.terms, cur) == xv) {
                            found = 1;
                            break;
                        }
                        cur = dx.term_cdr(db.terms, cur);
                    }
                    if (found == 0) {
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    ip += 1;
                    break :blk;
                }
                if (sp >= MAX_FRAMES) return -1;
                {
                    const f = &frames[@intCast(sp)];
                    var cur = lv;
                    var cnt: c_long = 0;
                    f.ip = ip;
                    f.op = OP_LIST_MEMBER;
                    f.idx = 0;
                    f.perm = null;
                    f.tuples = std.mem.zeroes(TupleBuf);
                    f.tuples.arity = 1;
                    f.tuples.cap = @intCast(dx.term_length(db.terms, lv));
                    if (f.tuples.cap < 1) f.tuples.cap = 1;
                    const mem = c.malloc(@as(usize, @intCast(f.tuples.cap)) * @sizeOf(u32)) orelse return -1;
                    f.tuples.data = @ptrCast(@alignCast(mem));
                    while (dx.term_is_list(db.terms, cur) != 0 and cur != dx.TERM_NIL) {
                        if (cnt >= f.tuples.cap) {
                            const nc: c_long = f.tuples.cap *% 2;
                            const old: ?*anyopaque = @ptrCast(f.tuples.data.?);
                            const nd = c.realloc(old, @as(usize, @intCast(nc)) * @sizeOf(u32)) orelse return -1;
                            f.tuples.data = @ptrCast(@alignCast(nd));
                            f.tuples.cap = nc;
                        }
                        f.tuples.data.?[@intCast(cnt)] = dx.term_car(db.terms, cur);
                        cnt +%= 1;
                        cur = dx.term_cdr(db.terms, cur);
                    }
                    f.tuples.count = cnt;
                    if (f.tuples.count == 0) {
                        tbufFree(&f.tuples);
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    bSave(&f.saved, &b);
                    if (seekValid(f, in, 0, 1, &b) == 0) {
                        tbufFree(&f.tuples);
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    sp += 1;
                    ip += 1;
                }
            },

            OP_RANGE => blk: {
                const r = if (@as(usize, in.imm) < db.nrels and db.rels[@intCast(in.imm)].kind == dx.RELK_FIXED)
                    db.rels[@intCast(in.imm)].rel
                else
                    null;
                if (r == null) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                const lo = bGet(&b, in.a);
                const hi = bGet(&b, in.b);
                if (bOk(&b, in.c) != 0) {
                    const xv = bGet(&b, in.c);
                    if (xv < lo or xv >= hi or dx.rel_has_col0(r, xv) == 0) {
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    ip += 1;
                    break :blk;
                }
                if (sp >= MAX_FRAMES) return -1;
                {
                    const f = &frames[@intCast(sp)];
                    f.ip = ip;
                    f.op = OP_RANGE;
                    f.idx = 0;
                    f.perm = null;
                    f.tuples = std.mem.zeroes(TupleBuf);
                    f.it = dx.dl_iter_open_live(r, null, 0);
                    if (f.it == null) return -1;
                    f.lo = lo;
                    f.hi = hi;
                    f.last = 0;
                    f.started = 0;
                    bSave(&f.saved, &b);
                    if (rangeResume(f, in, &b) == 0) {
                        dx.dl_iter_close(f.it);
                        f.it = null;
                        tbufFree(&f.tuples);
                        if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                        break :blk;
                    }
                    sp += 1;
                    ip += 1;
                }
            },

            OP_PROJECT => blk: {
                if (capture != null) {
                    var cols: [8]u32 = undefined;
                    var j: c_int = 0;
                    while (j < in.b) : (j += 1) cols[@intCast(j)] = bGet(&b, in.slots[@intCast(j)]);
                    if (capture.?(&cols, in.b, capture_user) != 0) {
                        ip = end;
                        break :blk;
                    }
                    rc +%= 1;
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                const r = dx.db_rel_at_arity_rw(db, in.a, in.b);
                if (r == null) {
                    ip = end;
                    break :blk;
                }
                var cols: [8]u32 = undefined;
                var j: c_int = 0;
                while (j < in.b) : (j += 1) {
                    const s: u8 = in.slots[@intCast(j)];
                    if (s == 0xFF) {
                        ip = end;
                        break;
                    }
                    cols[@intCast(j)] = bGet(&b, s);
                }
                if (ip >= end) break :blk;

                if (dry != 0) {
                    rc +%= 1;
                    if (cb != null and cb.?(cols[0..].ptr, in.b, user) != 0) {
                        ip = end;
                        break :blk;
                    }
                } else {
                    const rr = dx.rel_add(r, cols[0..].ptr);
                    if (rr < 0) {
                        ip = end;
                        break :blk;
                    }
                    if (rr == 1) {
                        rc +%= 1;
                        if (cb != null and cb.?(cols[0..].ptr, in.b, user) != 0) {
                            ip = end;
                            break :blk;
                        }
                    }
                }
                if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
            },

            OP_AGG_ACC => blk: {
                var key: [8]u32 = undefined;
                var g: c_int = 0;
                var all_bound: c_int = 1;
                while (g < acc.n_key) : (g += 1) {
                    if (bOk(&b, acc.group_slots[@intCast(g)]) == 0) {
                        all_bound = 0;
                        break;
                    }
                    key[@intCast(g)] = bGet(&b, acc.group_slots[@intCast(g)]);
                }
                if (all_bound == 0) {
                    if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
                    break :blk;
                }
                const bk = aggFindOrCreate(&acc, &key) orelse {
                    ip = end;
                    break :blk;
                };
                bk.valid = 1;
                bk.count +%= 1;
                if (acc.op == 1) {
                    bk.sum +%= bGet(&b, acc.src_slot);
                } else if (acc.op == 2) {
                    const v = bGet(&b, acc.src_slot);
                    if (bk.count == 1) bk.min = v else if (v < bk.min) bk.min = v;
                } else if (acc.op == 3) {
                    const v = bGet(&b, acc.src_slot);
                    if (bk.count == 1) bk.max = v else if (v > bk.max) bk.max = v;
                }
                if (backtrack(frames[0..], &sp, &b, p, &ip) == 0) ip = end;
            },

            OP_AGG_EMIT => {
                ip += 1;
            },

            OP_HALT => {
                ip = end;
            },

            else => {
                std.debug.print("vm: bad op {d}\n", .{in.op});
                ip = end;
            },
        }
    }

    if (acc_init != 0 and acc.n_buckets > 0) {
        var bi2: c_int = 0;
        while (bi2 < acc.cap) : (bi2 += 1) {
            const bk = &acc.buckets.?[@intCast(bi2)];
            if (bk.key == null) continue;
            var agg_val: u32 = 0;
            switch (acc.op) {
                1 => agg_val = bk.sum,
                2 => agg_val = bk.min,
                3 => agg_val = bk.max,
                else => agg_val = bk.count,
            }
            var cols: [8]u32 = undefined;
            var jj: c_int = 0;
            var ok: c_int = 1;
            while (jj < acc.head_arity) : (jj += 1) {
                const hs: u8 = acc.head_slots[@intCast(jj)];
                if (hs == acc.res_slot) {
                    cols[@intCast(jj)] = agg_val;
                } else {
                    var g2: c_int = 0;
                    var found: c_int = 0;
                    while (g2 < acc.n_key) : (g2 += 1) {
                        if (acc.group_slots[@intCast(g2)] == hs) {
                            cols[@intCast(jj)] = bk.key.?[@intCast(g2)];
                            found = 1;
                            break;
                        }
                    }
                    if (found == 0) {
                        ok = 0;
                        break;
                    }
                }
            }
            if (ok == 0) continue;
            if (acc.dry != 0) {
                rc +%= 1;
                if (acc.cb != null and acc.cb.?(cols[0..].ptr, acc.head_arity, acc.user) != 0) break;
            } else {
                const r = dx.db_rel_at_arity_rw(db, acc.head_rel_id, acc.head_arity);
                if (r != null) {
                    const rr = dx.rel_add(r, cols[0..].ptr);
                    if (rr < 0) break;
                    if (rr == 1) {
                        rc +%= 1;
                        if (acc.cb != null and acc.cb.?(cols[0..].ptr, acc.head_arity, acc.user) != 0) break;
                    }
                }
            }
        }
    }
    if (acc_init != 0) aggFree(&acc);

    while (sp > 0) {
        const f = &frames[@intCast(sp - 1)];
        if (f.it != null) {
            dx.dl_iter_close(f.it);
            f.it = null;
        }
        tbufFree(&f.tuples);
        sp -= 1;
    }

    return rc;
}

// ─── Top-level rule entry ──────────────────────────────────────────────────

fn execRule(
    db: *dx.dl_db,
    cr: *const compiler.compiled_rule,
    ov: ?[*]const vm_override,
    n_ov: c_int,
    dry: c_int,
    cb: DlTupleCb,
    user: ?*anyopaque,
) c_long {
    const p = cr.instrs.?;
    const n_instrs = cr.n_instrs;
    var n_mat: c_int = 0;
    var k: c_int = 0;
    while (k < n_instrs) : (k += 1) {
        if (p[@intCast(k)].op == OP_MAT_BEGIN) n_mat += 1;
    }

    var pool: ?[*]tupleset.tuple_set = null;
    if (n_mat > 0) {
        const mem = c.calloc(@as(usize, @intCast(n_mat)), @sizeOf(tupleset.tuple_set)) orelse return -1;
        pool = @ptrCast(@alignCast(mem));
    }

    const rc = execRange(db, cr, 0, n_instrs, ov, n_ov, dry, cb, user, null, null, pool, n_mat);

    if (pool) |pl| {
        k = 0;
        while (k < n_mat) : (k += 1) {
            if (pl[@intCast(k)].arity != 0) ts_free(&pl[@intCast(k)]);
        }
        c.free(@ptrCast(pl));
    }
    return rc;
}

/// long vm_exec_rule(...) — thin wrapper over exec_rule (top-down QSQ).
pub export fn vm_exec_rule(
    db: ?*dx.dl_db,
    cr: ?*const compiler.compiled_rule,
    ov: ?[*]const vm_override,
    n_ov: c_int,
    dry: c_int,
    cb: DlTupleCb,
    user: ?*anyopaque,
) c_long {
    const d = db orelse return -1;
    const r = cr orelse return -1;
    return execRule(d, r, ov, n_ov, dry, cb, user);
}

// ─── Candidate collector callback ──────────────────────────────────────────

const CandCtx = struct {
    ts: *tupleset.tuple_set,
};

fn candCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const ctx: *CandCtx = @ptrCast(@alignCast(user orelse return -1));
    return if (ts_add(ctx.ts, cols) < 0) -1 else 0;
}

// ─── Non-recursive evaluation (M1 path) ────────────────────────────────────

fn evalNonrecursive(db: *dx.dl_db, rules: [*]?*compiler.compiled_rule, n: c_int) c_int {
    var pass: c_int = 0;
    while (pass <= n) : (pass += 1) {
        var changed: c_int = 0;
        var i: c_int = 0;
        while (i < n) : (i += 1) {
            const m = execRule(db, rules[@intCast(i)].?, null, 0, 0, null, null);
            if (m < 0) return -1;
            if (m > 0) changed = 1;
            if (m > 0) {
                dx.permindex_mark_dirty(db, @intCast(rules[@intCast(i)].?.head_rel_id));
                if (dx.permindex_build_dirty(db) != 0) return -1;
            }
        }
        if (changed == 0) break;
    }
    return 0;
}

// ─── Recursive stratum evaluation (semi-naive fixpoint) ────────────────────

const RdT = struct {
    rel_id: c_int,
    idb: tupleset.tuple_set,
    delta: tupleset.tuple_set,
    next_delta: tupleset.tuple_set,
    arity: u8,
};

fn rdFreeTs(rd: [*]RdT, nr: c_int) void {
    var i: c_int = 0;
    while (i < nr) : (i += 1) {
        ts_free(&rd[@intCast(i)].idb);
        ts_free(&rd[@intCast(i)].delta);
        ts_free(&rd[@intCast(i)].next_delta);
    }
}

fn permFreeAll(nr: c_int, perm_count: [*]c_int, perm_ids: [*]?[*]c_int, perm_cap: [*]c_int, idb_perm_shadows: [*]?[*]tupleset.tuple_set) void {
    var rdi: c_int = 0;
    while (rdi < nr) : (rdi += 1) {
        var pi: c_int = 0;
        while (pi < perm_count[@intCast(rdi)]) : (pi += 1) {
            ts_free(&idb_perm_shadows[@intCast(rdi)].?[@intCast(pi)]);
        }
        if (perm_ids[@intCast(rdi)]) |p| c.free(@ptrCast(p));
        if (idb_perm_shadows[@intCast(rdi)]) |s| c.free(@ptrCast(s));
    }
    c.free(@ptrCast(perm_count));
    c.free(@ptrCast(perm_ids));
    c.free(@ptrCast(perm_cap));
    c.free(@ptrCast(idb_perm_shadows));
}

fn rebuildPermShadows(db: *dx.dl_db, nr: c_int, rd: [*]RdT, perm_count: [*]c_int, perm_ids: [*]?[*]c_int, idb_perm_shadows: [*]?[*]tupleset.tuple_set) void {
    var rdi: c_int = 0;
    while (rdi < nr) : (rdi += 1) {
        var pi: c_int = 0;
        while (pi < perm_count[@intCast(rdi)]) : (pi += 1) {
            const db_pi = perm_ids[@intCast(rdi)].?[@intCast(pi)];
            const perm_arr = db.perms[@intCast(db_pi)].perm;
            const ar = db.perms[@intCast(db_pi)].arity;
            const shadow = &idb_perm_shadows[@intCast(rdi)].?[@intCast(pi)];
            ts_reset(shadow);
            var ci: c_long = 0;
            while (ci < rd[@intCast(rdi)].idb.count) : (ci += 1) {
                const row = rd[@intCast(rdi)].idb.data.? + @as(usize, @intCast(ci)) * @as(usize, rd[@intCast(rdi)].idb.arity);
                var prow: [8]u32 = undefined;
                var j: c_int = 0;
                while (j < ar) : (j += 1)
                    prow[@intCast(j)] = row[perm_arr[@intCast(j)]];
                _ = ts_add(shadow, &prow);
            }
            ts_sort(shadow);
        }
    }
}

fn evalStratumRecursive(db: *dx.dl_db, rules: [*]?*compiler.compiled_rule, n: c_int, ivm: c_int) c_int {
    var i: c_int = 0;
    var ri: c_int = 0;
    var rp: [64]c_int = undefined;
    var nr: c_int = 0;
    var rp_idx: [64]c_int = undefined;

    {
        var seen: [64]c_int = [_]c_int{0} ** 64;
        i = 0;
        while (i < n) : (i += 1) {
            const cr = rules[@intCast(i)].?;
            if (cr.is_recursive == 0) continue;
            const rid: c_int = @intCast(cr.head_rel_id);
            if (rid < 64 and seen[@intCast(rid)] == 0) {
                seen[@intCast(rid)] = 1;
                rp[@intCast(nr)] = rid;
                nr += 1;
            }
        }
        i = 0;
        while (i < 64) : (i += 1) rp_idx[@intCast(i)] = -1;
        i = 0;
        while (i < nr) : (i += 1) rp_idx[@intCast(rp[@intCast(i)])] = i;
    }

    if (nr == 0)
        return evalNonrecursive(db, rules, n);

    var need_idb_sort: c_int = 0;
    {
        i = 0;
        while (i < n) : (i += 1) {
            const cr = rules[@intCast(i)].?;
            if (cr.is_recursive == 0) continue;
            var n_rec: c_int = 0;
            var n_lookup: c_int = 0;
            var ii: c_int = 0;
            while (ii < cr.n_instrs) : (ii += 1) {
                const in = &cr.instrs.?[@intCast(ii)];
                if (in.op != OP_LOOKUP and in.op != OP_SCAN and in.op != OP_LOOKUP_PERM) continue;
                if (rp_idx[@intCast(in.a)] < 0) continue;
                n_rec += 1;
                if (in.op == OP_LOOKUP) n_lookup += 1;
            }
            if (n_rec >= 2 and n_lookup >= 1) {
                need_idb_sort = 1;
                break;
            }
        }
    }

    const rmem = c.calloc(@as(usize, @intCast(nr)), @sizeOf(RdT)) orelse return -1;
    const rd: [*]RdT = @ptrCast(@alignCast(rmem));

    i = 0;
    while (i < nr) : (i += 1) {
        const rc = dbRel(db, rp[@intCast(i)]);
        const ar: u8 = if (rc != null) dx.rel_arity(rc) else 0;
        rd[@intCast(i)].rel_id = rp[@intCast(i)];
        rd[@intCast(i)].arity = ar;
        if (ts_init(&rd[@intCast(i)].idb, ar) != 0 or
            ts_init(&rd[@intCast(i)].delta, ar) != 0 or
            ts_init(&rd[@intCast(i)].next_delta, ar) != 0)
        {
            ri = 0;
            while (ri <= i) : (ri += 1) {
                ts_free(&rd[@intCast(ri)].idb);
                ts_free(&rd[@intCast(ri)].delta);
                ts_free(&rd[@intCast(ri)].next_delta);
            }
            c.free(@ptrCast(rd));
            return -1;
        }
    }

    const pcm = c.calloc(@as(usize, @intCast(nr)), @sizeOf(c_int)) orelse {
        rdFreeTs(rd, nr);
        c.free(@ptrCast(rd));
        return -1;
    };
    const perm_count: [*]c_int = @ptrCast(@alignCast(pcm));
    const pim = c.calloc(@as(usize, @intCast(nr)), @sizeOf(?[*]c_int)) orelse {
        c.free(@ptrCast(perm_count));
        rdFreeTs(rd, nr);
        c.free(@ptrCast(rd));
        return -1;
    };
    const perm_ids: [*]?[*]c_int = @ptrCast(@alignCast(pim));
    const pcm2 = c.calloc(@as(usize, @intCast(nr)), @sizeOf(c_int)) orelse {
        c.free(@ptrCast(perm_count));
        c.free(@ptrCast(perm_ids));
        rdFreeTs(rd, nr);
        c.free(@ptrCast(rd));
        return -1;
    };
    const perm_cap: [*]c_int = @ptrCast(@alignCast(pcm2));
    const ism = c.calloc(@as(usize, @intCast(nr)), @sizeOf(?[*]tupleset.tuple_set)) orelse {
        c.free(@ptrCast(perm_count));
        c.free(@ptrCast(perm_ids));
        c.free(@ptrCast(perm_cap));
        rdFreeTs(rd, nr);
        c.free(@ptrCast(rd));
        return -1;
    };
    const idb_perm_shadows: [*]?[*]tupleset.tuple_set = @ptrCast(@alignCast(ism));

    {
        var pi: c_int = 0;
        while (pi < db.n_perms) : (pi += 1) {
            const p_rel_id = db.perms[@intCast(pi)].rel_id;
            const rdi = rp_idx[@intCast(p_rel_id)];
            if (rdi < 0) continue;
            const pc = perm_count[@intCast(rdi)];
            if (pc >= perm_cap[@intCast(rdi)]) {
                const nc: c_int = if (perm_cap[@intCast(rdi)] != 0) perm_cap[@intCast(rdi)] *% 2 else 2;
                const old_i: ?*anyopaque = if (perm_ids[@intCast(rdi)]) |p| @ptrCast(p) else null;
                const ni = c.realloc(old_i, @as(usize, @intCast(nc)) * @sizeOf(c_int)) orelse {
                    permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                    rdFreeTs(rd, nr);
                    c.free(@ptrCast(rd));
                    return -1;
                };
                const old_s: ?*anyopaque = if (idb_perm_shadows[@intCast(rdi)]) |s| @ptrCast(s) else null;
                const ns = c.realloc(old_s, @as(usize, @intCast(nc)) * @sizeOf(tupleset.tuple_set)) orelse {
                    c.free(@ptrCast(rd));
                    permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                    rdFreeTs(rd, nr);
                    c.free(@ptrCast(rd));
                    return -1;
                };
                perm_ids[@intCast(rdi)] = @ptrCast(@alignCast(ni));
                idb_perm_shadows[@intCast(rdi)] = @ptrCast(@alignCast(ns));
                perm_cap[@intCast(rdi)] = nc;
            }
            perm_ids[@intCast(rdi)].?[@intCast(pc)] = pi;
            if (ts_init(&idb_perm_shadows[@intCast(rdi)].?[@intCast(pc)], db.perms[@intCast(pi)].arity) != 0) {
                permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                rdFreeTs(rd, nr);
                c.free(@ptrCast(rd));
                return -1;
            }
            perm_count[@intCast(rdi)] += 1;
        }
    }

    if (ivm != 0) {
        i = 0;
        while (i < nr) : (i += 1) {
            const rel = dbRel(db, rd[@intCast(i)].rel_id);
            if (rel != null)
                _ = dx.rel_prefix(rel, null, 0, dx.ts_sink_cb, &rd[@intCast(i)].idb);
        }
    }

    {
        var pass: c_int = 0;
        while (pass <= n) : (pass += 1) {
            var changed_nr: c_int = 0;
            i = 0;
            while (i < n) : (i += 1) {
                const cr = rules[@intCast(i)].?;
                if (cr.is_recursive != 0) continue;
                const m = execRule(db, cr, null, 0, 0, null, null);
                if (m < 0) {
                    permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                    rdFreeTs(rd, nr);
                    c.free(@ptrCast(rd));
                    return -1;
                }
                if (m > 0) {
                    changed_nr = 1;
                    dx.permindex_mark_dirty(db, @intCast(cr.head_rel_id));
                    if (dx.permindex_build_dirty(db) != 0) {
                        permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                        rdFreeTs(rd, nr);
                        c.free(@ptrCast(rd));
                        return -1;
                    }
                }
            }
            if (changed_nr == 0) break;
        }
    }

    i = 0;
    while (i < n) : (i += 1) {
        const cr = rules[@intCast(i)].?;
        if (cr.is_recursive == 0) {
            const m = execRule(db, cr, null, 0, 0, null, null);
            if (m < 0) {
                permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                rdFreeTs(rd, nr);
                c.free(@ptrCast(rd));
                return -1;
            }
            dx.permindex_mark_dirty(db, @intCast(cr.head_rel_id));
            if (dx.permindex_build_dirty(db) != 0) {
                permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                rdFreeTs(rd, nr);
                c.free(@ptrCast(rd));
                return -1;
            }
        } else if (ivm == 0) {
            const hri: c_int = @intCast(cr.head_rel_id);
            const hdi = rp_idx[@intCast(hri)];

            var seed: tupleset.tuple_set = undefined;
            if (ts_init(&seed, rd[@intCast(hdi)].arity) != 0) {
                permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                rdFreeTs(rd, nr);
                c.free(@ptrCast(rd));
                return -1;
            }

            var ctx = CandCtx{ .ts = &seed };

            const m = execRule(db, cr, null, 0, 1, candCb, &ctx);
            if (m < 0) {
                ts_free(&seed);
                permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                rdFreeTs(rd, nr);
                c.free(@ptrCast(rd));
                return -1;
            }

            var ci: c_long = 0;
            while (ci < seed.count) : (ci += 1) {
                const t = seed.data.? + @as(usize, @intCast(ci)) * @as(usize, seed.arity);
                _ = ts_add(&rd[@intCast(hdi)].idb, t);
                _ = ts_add(&rd[@intCast(hdi)].delta, t);
            }
            ts_free(&seed);
        } else {
            const hri: c_int = @intCast(cr.head_rel_id);
            const hdi = rp_idx[@intCast(hri)];
            var ii: c_int = 0;
            while (ii < cr.n_instrs) : (ii += 1) {
                const in = &cr.instrs.?[@intCast(ii)];
                if (in.op != OP_SCAN and in.op != OP_LOOKUP) continue;
                const br: c_int = @intCast(in.a);
                if (br < 0 or br >= MAX_RELS) continue;
                if (rp_idx[@intCast(br)] >= 0) continue;
                if (br >= @as(c_int, @intCast(db.nrels))) continue;
                const dp = db.delta_pending[@intCast(br)];
                if (dp == null) continue;
                if (tsFromPending(dp).?.count == 0) continue;

                var ov: [1]vm_override = undefined;
                ov[0].body_idx = @intCast(in.body_idx);
                ov[0].ts = tsFromPending(dp);
                ov[0].perm_id = -1;

                var cand: tupleset.tuple_set = undefined;
                if (ts_init(&cand, rd[@intCast(hdi)].arity) != 0) {
                    permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                    rdFreeTs(rd, nr);
                    c.free(@ptrCast(rd));
                    return -1;
                }
                var ctx = CandCtx{ .ts = &cand };

                const n_out = execRule(db, cr, &ov, 1, 1, candCb, &ctx);
                if (n_out < 0) {
                    ts_free(&cand);
                    permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                    rdFreeTs(rd, nr);
                    c.free(@ptrCast(rd));
                    return -1;
                }

                var ci: c_long = 0;
                while (ci < cand.count) : (ci += 1) {
                    const t = cand.data.? + @as(usize, @intCast(ci)) * @as(usize, cand.arity);
                    if (ts_contains(&rd[@intCast(hdi)].idb, t) == 0) {
                        _ = ts_add(&rd[@intCast(hdi)].idb, t);
                        _ = ts_add(&rd[@intCast(hdi)].delta, t);
                    }
                }
                ts_free(&cand);
            }
        }
    }

    i = 0;
    while (i < nr) : (i += 1) {
        ts_sort(&rd[@intCast(i)].idb);
        ts_sort(&rd[@intCast(i)].delta);
    }

    rebuildPermShadows(db, nr, rd, perm_count, perm_ids, idb_perm_shadows);

    {
        var any: c_int = 0;
        i = 0;
        while (i < nr) : (i += 1) {
            if (rd[@intCast(i)].delta.count > 0) {
                any = 1;
                break;
            }
        }
        if (any == 0) {
            permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
            rdFreeTs(rd, nr);
            c.free(@ptrCast(rd));
            return 0;
        }
    }

    {
        var iter: c_int = 0;
        while (true) : (iter += 1) {
            if (iter >= FIXPOINT_ERROR_BOUND) {
                std.debug.print("vm: fixpoint not converged after {d} iterations\n", .{iter});
                permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                rdFreeTs(rd, nr);
                c.free(@ptrCast(rd));
                return -1;
            }

            rebuildPermShadows(db, nr, rd, perm_count, perm_ids, idb_perm_shadows);

            if (need_idb_sort != 0) {
                i = 0;
                while (i < nr) : (i += 1) ts_sort(&rd[@intCast(i)].idb);
            }

            i = 0;
            while (i < nr) : (i += 1)
                ts_reset(&rd[@intCast(i)].next_delta);

            ri = 0;
            while (ri < n) : (ri += 1) {
                const cr = rules[@intCast(ri)].?;
                if (cr.is_recursive == 0) continue;

                const hri: c_int = @intCast(cr.head_rel_id);
                const hdi = rp_idx[@intCast(hri)];
                const prog = cr.instrs.?;
                const ni_instrs = cr.n_instrs;
                var ii: c_int = 0;
                while (ii < ni_instrs) : (ii += 1) {
                    const in = &prog[@intCast(ii)];
                    if (in.op != OP_SCAN and in.op != OP_LOOKUP and in.op != OP_LOOKUP_PERM) continue;

                    const br: c_int = @intCast(in.a);
                    const bdi = rp_idx[@intCast(br)];
                    if (bdi < 0) continue;
                    if (rd[@intCast(bdi)].delta.count == 0) continue;

                    var overrides: [16]vm_override = undefined;
                    var n_ov: c_int = 0;
                    var ji: c_int = 0;
                    while (ji < ni_instrs) : (ji += 1) {
                        const jin = &prog[@intCast(ji)];
                        if (jin.op != OP_SCAN and jin.op != OP_LOOKUP and jin.op != OP_LOOKUP_PERM) continue;
                        const jbr: c_int = @intCast(jin.a);
                        const jbdi = rp_idx[@intCast(jbr)];
                        if (jbdi < 0) continue;

                        const is_delta: c_int = if (@as(c_int, @intCast(jin.body_idx)) == @as(c_int, @intCast(in.body_idx)) and jbr == br) 1 else 0;

                        if (jin.op == OP_LOOKUP_PERM) {
                            const jperm_id: c_int = @intCast(jin.imm);
                            var pk: c_int = 0;
                            var shadow_ts: ?*const tupleset.tuple_set = null;
                            while (pk < perm_count[@intCast(jbdi)]) : (pk += 1) {
                                if (perm_ids[@intCast(jbdi)].?[@intCast(pk)] == jperm_id) {
                                    shadow_ts = &idb_perm_shadows[@intCast(jbdi)].?[@intCast(pk)];
                                    break;
                                }
                            }
                            overrides[@intCast(n_ov)].body_idx = @intCast(jin.body_idx);
                            overrides[@intCast(n_ov)].ts = if (shadow_ts) |s| s else &rd[@intCast(jbdi)].idb;
                            overrides[@intCast(n_ov)].perm_id = jperm_id;
                        } else if (is_delta != 0) {
                            overrides[@intCast(n_ov)].body_idx = @intCast(jin.body_idx);
                            overrides[@intCast(n_ov)].ts = &rd[@intCast(bdi)].delta;
                            overrides[@intCast(n_ov)].perm_id = -1;
                        } else {
                            overrides[@intCast(n_ov)].body_idx = @intCast(jin.body_idx);
                            overrides[@intCast(n_ov)].ts = &rd[@intCast(jbdi)].idb;
                            overrides[@intCast(n_ov)].perm_id = -1;
                        }
                        n_ov += 1;
                    }

                    var cand: tupleset.tuple_set = undefined;
                    if (ts_init(&cand, rd[@intCast(hdi)].arity) != 0) {
                        permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                        rdFreeTs(rd, nr);
                        c.free(@ptrCast(rd));
                        return -1;
                    }

                    var ctx = CandCtx{ .ts = &cand };

                    const n_out = execRule(db, cr, &overrides, n_ov, 1, candCb, &ctx);
                    if (n_out < 0) {
                        ts_free(&cand);
                        permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
                        rdFreeTs(rd, nr);
                        c.free(@ptrCast(rd));
                        return -1;
                    }

                    var ci: c_long = 0;
                    while (ci < cand.count) : (ci += 1) {
                        const t = cand.data.? + @as(usize, @intCast(ci)) * @as(usize, cand.arity);
                        if (ts_contains(&rd[@intCast(hdi)].idb, t) == 0) {
                            _ = ts_add(&rd[@intCast(hdi)].next_delta, t);
                        }
                    }

                    ts_free(&cand);
                }
            }

            {
                var empty: c_int = 1;
                i = 0;
                while (i < nr) : (i += 1) {
                    if (rd[@intCast(i)].next_delta.count > 0) {
                        empty = 0;
                        break;
                    }
                }
                if (empty != 0) break;
            }

            i = 0;
            while (i < nr) : (i += 1) {
                if (rd[@intCast(i)].next_delta.count > 0) {
                    ts_sort(&rd[@intCast(i)].next_delta);
                    {
                        var ci: c_long = 0;
                        while (ci < rd[@intCast(i)].next_delta.count) : (ci += 1) {
                            _ = ts_add(&rd[@intCast(i)].idb,
                                rd[@intCast(i)].next_delta.data.? + @as(usize, @intCast(ci)) * @as(usize, rd[@intCast(i)].arity));
                        }
                    }
                    const tmp = rd[@intCast(i)].delta;
                    rd[@intCast(i)].delta = rd[@intCast(i)].next_delta;
                    rd[@intCast(i)].next_delta = tmp;
                    ts_reset(&rd[@intCast(i)].next_delta);
                } else {
                    ts_free(&rd[@intCast(i)].delta);
                    rd[@intCast(i)].delta = std.mem.zeroes(tupleset.tuple_set);
                    rd[@intCast(i)].delta.arity = rd[@intCast(i)].arity;
                }
            }
        }
    }

    i = 0;
    while (i < nr) : (i += 1) {
        const rel = dbRel(db, rd[@intCast(i)].rel_id);
        if (rel == null) continue;

        _ = dx.rel_prefix(rel, null, 0, dx.ts_sink_cb, &rd[@intCast(i)].idb);
        ts_sort(&rd[@intCast(i)].idb);
        if (vmNomaterializeRef().* != 0 and rd[@intCast(i)].rel_id == vmExportRelidRef().*) {
            vmExportTsRef().*.?.* = rd[@intCast(i)].idb;
            rd[@intCast(i)].idb = std.mem.zeroes(tupleset.tuple_set);
            continue;
        }
        if (dx.rel_build_from_tupleset(rel, @ptrCast(@alignCast(&rd[@intCast(i)].idb))) != 0) {
            permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
            rdFreeTs(rd, nr);
            c.free(@ptrCast(rd));
            return -1;
        }
    }

    {
        var mi: c_int = 0;
        while (mi < nr) : (mi += 1)
            dx.permindex_mark_dirty(db, rd[@intCast(mi)].rel_id);
        if (dx.permindex_build_dirty(db) != 0) {
            permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);
            rdFreeTs(rd, nr);
            c.free(@ptrCast(rd));
            return -1;
        }
    }

    rdFreeTs(rd, nr);
    c.free(@ptrCast(rd));
    permFreeAll(nr, perm_count, perm_ids, perm_cap, idb_perm_shadows);

    return 0;
}

// ─── vm_execute ────────────────────────────────────────────────────────────

fn vmExecuteMode(db: *dx.dl_db, rules: [*]?*compiler.compiled_rule, n_rules: c_int, ivm: c_int) c_int {
    var i: c_int = 0;
    var s: c_int = 0;
    var max_stratum: c_int = 0;

    if (n_rules <= 0) return 0;

    if (ivm == 0) {
        var seen: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
        i = 0;
        while (i < n_rules) : (i += 1) {
            const hid: u8 = rules[@intCast(i)].?.head_rel_id;
            if (hid >= MAX_RELS) return -1;
            if (seen[hid] != 0) continue;
            seen[hid] = 1;
            if (db.rels[hid].kind == dx.RELK_VARIADIC) {
                if (dx.vrel_reset_views(db.rels[hid].vrel) != 0) return -1;
                dx.permindex_mark_dirty(db, hid);
                continue;
            }
            const r = dbRel(db, hid);
            if (r == null) return -1;
            if (dx.rel_reset_view(r) != 0) return -1;
            dx.permindex_mark_dirty(db, hid);
        }
    }

    if (dx.permindex_build_dirty(db) != 0) return -1;

    max_stratum = 0;
    {
        var any_recursive: c_int = 0;
        i = 0;
        while (i < n_rules) : (i += 1) {
            if (rules[@intCast(i)].?.stratum > max_stratum)
                max_stratum = rules[@intCast(i)].?.stratum;
            if (rules[@intCast(i)].?.is_recursive != 0)
                any_recursive = 1;
        }
        if (any_recursive == 0)
            return evalNonrecursive(db, rules, n_rules);
    }

    s = 0;
    while (s <= max_stratum) : (s += 1) {
        var sr: [256]c_int = undefined;
        var sc: c_int = 0;
        var srec: c_int = 0;
        i = 0;
        while (i < n_rules) : (i += 1) {
            if (rules[@intCast(i)].?.stratum == s) {
                if (sc < 256) {
                    sr[@intCast(sc)] = i;
                    sc += 1;
                }
                if (rules[@intCast(i)].?.is_recursive != 0) srec = 1;
            }
        }

        if (sc == 0) continue;

        if (srec == 0) {
            var strat_rules: [256]?*compiler.compiled_rule = undefined;
            i = 0;
            while (i < sc) : (i += 1)
                strat_rules[@intCast(i)] = rules[@intCast(sr[@intCast(i)])];
            if (evalNonrecursive(db, strat_rules[0..].ptr, sc) != 0)
                return -1;
        } else {
            var strat_rules: [256]?*compiler.compiled_rule = undefined;
            i = 0;
            while (i < sc) : (i += 1)
                strat_rules[@intCast(i)] = rules[@intCast(sr[@intCast(i)])];
            if (evalStratumRecursive(db, strat_rules[0..].ptr, sc, ivm) != 0)
                return -1;
        }
    }

    return 0;
}

/// int vm_execute(dl_db*, compiled_rule**, int)
pub export fn vm_execute(db: ?*dx.dl_db, rules: ?[*]?*compiler.compiled_rule, n_rules: c_int) c_int {
    const d = db orelse return -1;
    const r = rules orelse return -1;
    return vmExecuteMode(d, r, n_rules, 0);
}

/// int vm_execute_ivm(dl_db*)
pub export fn vm_execute_ivm(db: ?*dx.dl_db) c_int {
    const d = db orelse return -1;
    var i: c_int = 0;
    while (i < MAX_RELS) : (i += 1) {
        const dp = d.delta_pending[@intCast(i)];
        if (dp != null and tsFromPending(dp).?.count > 0)
            ts_sort(tsFromPending(dp));
    }
    const cr = d.crules orelse return -1;
    return vmExecuteMode(d, @ptrCast(cr), d.n_crules, 1);
}

// ─── vm_query ──────────────────────────────────────────────────────────────

/// long vm_query(...)
pub export fn vm_query(
    db: ?*dx.dl_db,
    rules: ?[*]?*compiler.compiled_rule,
    n_rules: c_int,
    goal_rel: [*c]const u8,
    cb: DlTupleCb,
    user: ?*anyopaque,
) c_long {
    const d = db orelse return -1;
    const r = rules orelse return -1;
    if (vmExecuteMode(d, r, n_rules, 0) != 0) return -1;

    const gri = dbFind(d, goal_rel);
    if (gri >= 0 and cb != null) {
        if (d.rels[@intCast(gri)].kind == dx.RELK_VARIADIC)
            return dx.vrel_prefix(d.rels[@intCast(gri)].vrel, null, 0, cb, user);
        const rel = d.rels[@intCast(gri)].rel;
        if (rel != null) {
            const nn = dx.rel_prefix(rel, null, 0, cb, user);
            if (nn < 0) return -1;
            return nn;
        }
    }
    return 0;
}

// ─── IVM Slice 1/2: insert-only incremental maintenance ────────────────────

/// int vm_ivm_eligible(dl_db*)
pub export fn vm_ivm_eligible(db: ?*dx.dl_db) c_int {
    const d = db orelse return 0;
    if (dx.db_has_variadic(d) != 0 or dx.db_has_list_builtin(d) != 0 or dx.db_has_range_builtin(d) != 0) return 0;

    var is_rule_head: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
    var is_rec_head: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
    var rec_stratum: [MAX_RELS]c_int = [_]c_int{-1} ** MAX_RELS;

    var i: c_int = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        const hid: u8 = cr.head_rel_id;
        if (hid >= MAX_RELS) continue;
        is_rule_head[hid] = 1;
        if (cr.is_recursive != 0) {
            is_rec_head[hid] = 1;
            rec_stratum[hid] = cr.stratum;
        }
    }

    i = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        if (cr.has_aggregate != 0) return 0;
        var k: c_int = 0;
        while (k < cr.n_instrs) : (k += 1) {
            const op: u8 = cr.instrs.?[@intCast(k)].op;
            if (op == OP_NEG_CHECK) return 0;
            if (op == OP_WALK) return 0;
            if (op == OP_LOOKUP_PERM) return 0;
            if (op == OP_HASH_JOIN) return 0;
            if (op == OP_MAT_BEGIN) return 0;
            if (op == OP_MAT_JOIN) return 0;

            if (cr.is_recursive == 0) continue;
            if (op != OP_SCAN and op != OP_LOOKUP) continue;
            {
                const R: c_int = @intCast(cr.instrs.?[@intCast(k)].a);
                if (R < 0 or R >= MAX_RELS) continue;
                if (is_rule_head[@intCast(R)] == 0) continue;
                if (is_rec_head[@intCast(R)] != 0 and rec_stratum[@intCast(R)] == cr.stratum) continue;
                return 0;
            }
        }
    }
    return 1;
}

/// int vm_has_recursive(dl_db*)
pub export fn vm_has_recursive(db: ?*dx.dl_db) c_int {
    const d = db orelse return 0;
    var i: c_int = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        if (cr.is_recursive != 0) return 1;
    }
    return 0;
}

/// void vm_clear_deltas(dl_db*)
pub export fn vm_clear_deltas(db: ?*dx.dl_db) void {
    const d = db orelse return;
    var i: c_int = 0;
    while (i < MAX_RELS) : (i += 1) {
        const dp = d.delta_pending[@intCast(i)];
        if (dp != null) {
            const tsp: *tupleset.tuple_set = @ptrCast(@alignCast(dp));
            ts_free(tsp);
            c.free(@ptrCast(tsp));
            d.delta_pending[@intCast(i)] = null;
        }
    }
}

const IvmWork = struct {
    rel_id: c_int,
    ts: ?*tupleset.tuple_set,
};

const IvmCapture = struct {
    ts: ?*tupleset.tuple_set,
    err: c_int,
};

fn ivmCaptureCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const cap: *IvmCapture = @ptrCast(@alignCast(user orelse return 1));
    if (cap.err != 0) return 1;
    if (ts_add(cap.ts, cols) < 0) {
        cap.err = -1;
        return 1;
    }
    return 0;
}

/// int vm_propagate_deltas(dl_db*)
pub export fn vm_propagate_deltas(db: ?*dx.dl_db) c_int {
    var queue: ?[*]IvmWork = null;
    var qcap: usize = 0;
    var qn: usize = 0;
    var qh: usize = 0;
    var rc: c_int = -1;

    const d = db orelse return -1;

    vmPropagateRunsRef().* +%= 1;

    fail: {
        var i: usize = 0;
        while (i < d.nrels) : (i += 1) {
            const dp = d.delta_pending[i];
            if (dp == null) continue;
            d.delta_pending[i] = null;
            const tsp: *tupleset.tuple_set = @ptrCast(@alignCast(dp));
            if (tsp.count == 0) {
                ts_free(tsp);
                c.free(@ptrCast(tsp));
                continue;
            }
            ts_sort(tsp);
            if (qn == qcap) {
                const nc: usize = if (qcap != 0) qcap *% 2 else 16;
                const old: ?*anyopaque = if (queue) |q| @ptrCast(q) else null;
                const nq = c.realloc(old, nc * @sizeOf(IvmWork)) orelse {
                    ts_free(tsp);
                    c.free(@ptrCast(tsp));
                    break :fail;
                };
                queue = @ptrCast(@alignCast(nq));
                qcap = nc;
            }
            queue.?[qn].rel_id = @intCast(i);
            queue.?[qn].ts = tsp;
            qn += 1;
        }

        while (qh < qn) {
            const w = queue.?[qh];
            qh += 1;
            var ri: c_int = 0;
            while (ri < d.n_crules) : (ri += 1) {
                const cc = d.crules[@intCast(ri)];
                if (cc == null) continue;
                const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
                var k: c_int = 0;

                if (cr.has_aggregate != 0) continue;

                while (k < cr.n_instrs) : (k += 1) {
                    const in = &cr.instrs.?[@intCast(k)];
                    const op: u8 = in.op;
                    if (op != OP_SCAN and op != OP_LOOKUP) continue;
                    if (@as(c_int, @intCast(in.a)) != w.rel_id) continue;

                    const hrel = dbRel(d, @intCast(cr.head_rel_id));
                    if (hrel == null) break :fail;
                    const harity = dx.rel_arity(hrel);

                    const head_ts = c.malloc(@sizeOf(tupleset.tuple_set)) orelse break :fail;
                    const hts: *tupleset.tuple_set = @ptrCast(@alignCast(head_ts));
                    if (ts_init(hts, harity) != 0) {
                        c.free(head_ts);
                        break :fail;
                    }

                    var ov: [1]vm_override = undefined;
                    ov[0].body_idx = @intCast(in.body_idx);
                    ov[0].ts = w.ts;
                    ov[0].perm_id = -1;

                    var cap = IvmCapture{ .ts = hts, .err = 0 };

                    const m = execRule(d, cr, &ov, 1, 0, ivmCaptureCb, &cap);
                    if (m < 0 or cap.err != 0) {
                        ts_free(hts);
                        c.free(@ptrCast(hts));
                        break :fail;
                    }

                    if (hts.count > 0) {
                        dx.permindex_mark_dirty(d, @intCast(cr.head_rel_id));
                        if (dx.permindex_build_dirty(d) != 0) {
                            ts_free(hts);
                            c.free(@ptrCast(hts));
                            break :fail;
                        }
                        ts_sort(hts);

                        if (qn == qcap) {
                            const nc: usize = if (qcap != 0) qcap *% 2 else 16;
                            const old: ?*anyopaque = if (queue) |q| @ptrCast(q) else null;
                            const nq = c.realloc(old, nc * @sizeOf(IvmWork)) orelse {
                                ts_free(hts);
                                c.free(@ptrCast(hts));
                                break :fail;
                            };
                            queue = @ptrCast(@alignCast(nq));
                            qcap = nc;
                        }
                        queue.?[qn].rel_id = @intCast(cr.head_rel_id);
                        queue.?[qn].ts = hts;
                        qn += 1;
                    } else {
                        ts_free(hts);
                        c.free(@ptrCast(hts));
                    }
                }
            }

            ts_free(w.ts.?);
            c.free(@ptrCast(w.ts.?));
        }

        rc = 0;
    }

    while (qh < qn) {
        ts_free(queue.?[qh].ts.?);
        c.free(@ptrCast(queue.?[qh].ts.?));
        qh += 1;
    }
    if (queue) |q| c.free(@ptrCast(q));
    vm_clear_deltas(d);
    return rc;
}

// ─── IVM Slice 3: DRed deletion ────────────────────────────────────────────

/// int vm_dred_eligible(dl_db*)
pub export fn vm_dred_eligible(db: ?*dx.dl_db) c_int {
    const d = db orelse return 0;
    if (dx.db_has_variadic(d) != 0 or dx.db_has_list_builtin(d) != 0 or dx.db_has_range_builtin(d) != 0) return 0;
    var i: c_int = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        if (cr.is_recursive != 0) return 0;
        if (cr.has_aggregate != 0) return 0;
        var k: c_int = 0;
        while (k < cr.n_instrs) : (k += 1) {
            const op: u8 = cr.instrs.?[@intCast(k)].op;
            if (op == OP_WALK) return 0;
            if (op == OP_LOOKUP_PERM) return 0;
            if (op == OP_HASH_JOIN) return 0;
            if (op == OP_MAT_BEGIN) return 0;
            if (op == OP_MAT_JOIN) return 0;
        }
    }
    return 1;
}

/// void vm_clear_deletes(dl_db*)
pub export fn vm_clear_deletes(db: ?*dx.dl_db) void {
    const d = db orelse return;
    var i: c_int = 0;
    while (i < MAX_RELS) : (i += 1) {
        const dp = d.del_pending[@intCast(i)];
        if (dp != null) {
            const tsp: *tupleset.tuple_set = @ptrCast(@alignCast(dp));
            ts_free(tsp);
            c.free(@ptrCast(tsp));
            d.del_pending[@intCast(i)] = null;
        }
    }
}

fn dredRuleReads(cr: *const compiler.compiled_rule, rel_id: c_int) c_int {
    var k: c_int = 0;
    while (k < cr.n_instrs) : (k += 1) {
        const in = &cr.instrs.?[@intCast(k)];
        switch (in.op) {
            OP_SCAN, OP_LOOKUP, OP_NEG_CHECK, OP_WALK, OP_LOOKUP_PERM, OP_HASH_JOIN => {
                if (@as(c_int, @intCast(in.a)) == rel_id) return 1;
            },
            else => {},
        }
    }
    return 0;
}

/// int vm_dred_delete(dl_db*)
pub export fn vm_dred_delete(db: ?*dx.dl_db) c_int {
    var over: [MAX_RELS]?*tupleset.tuple_set = [_]?*tupleset.tuple_set{null} ** MAX_RELS;
    var full_reset: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
    var changed: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
    var affected: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
    var is_head: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
    var has_negation: u8 = 0;
    var max_stratum: c_int = 0;
    var any_change: c_int = 0;
    var rc: c_int = -1;
    var i: c_int = 0;

    const d = db orelse return -1;
    vmDredRunsRef().* +%= 1;

    out: {
    i = 0;
    while (i < MAX_RELS) : (i += 1) {
        const dp = d.delta_pending[@intCast(i)];
        if (dp != null and tsFromPending(dp).?.count > 0) {
            ts_sort(tsFromPending(dp));
            if (i < @as(c_int, @intCast(d.nrels))) changed[@intCast(i)] = 1;
            any_change = 1;
        }
        const dp2 = d.del_pending[@intCast(i)];
        if (dp2 != null and tsFromPending(dp2).?.count > 0) {
            if (i < @as(c_int, @intCast(d.nrels))) changed[@intCast(i)] = 1;
            any_change = 1;
        }
    }

    i = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        const hid: c_int = @intCast(cr.head_rel_id);
        if (cr.has_aggregate != 0) continue;
        if (hid < MAX_RELS) is_head[@intCast(hid)] = 1;
        if (cr.stratum > max_stratum) max_stratum = cr.stratum;
        var k: c_int = 0;
        while (k < cr.n_instrs) : (k += 1) {
            if (cr.instrs.?[@intCast(k)].op == OP_NEG_CHECK) {
                has_negation = 1;
                break;
            }
        }
    }

    i = 0;
    while (i < @as(c_int, @intCast(d.nrels))) : (i += 1) {
        const dp = d.del_pending[@intCast(i)];
        if (dp == null) continue;
        const dtsp: *tupleset.tuple_set = @ptrCast(@alignCast(dp));
        if (dtsp.count == 0) continue;
        const mem = c.malloc(@sizeOf(tupleset.tuple_set)) orelse break :out;
        const ts: *tupleset.tuple_set = @ptrCast(@alignCast(mem));
        if (ts_init(ts, dx.rel_arity(d.rels[@intCast(i)].rel)) != 0) {
            c.free(mem);
            over[@intCast(i)] = null;
            break :out;
        }
        var ci: c_long = 0;
        while (ci < dtsp.count) : (ci += 1) {
            _ = ts_add(ts, dtsp.data.? + @as(usize, @intCast(ci)) * @as(usize, dtsp.arity));
        }
        ts_sort(ts);
        over[@intCast(i)] = ts;
    }

    if (has_negation != 0 and any_change != 0) {
        i = 0;
        while (i < d.n_crules) : (i += 1) {
            const cc = d.crules[@intCast(i)];
            if (cc == null) continue;
            const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
            const hid: c_int = @intCast(cr.head_rel_id);
            if (cr.has_aggregate != 0) continue;
            if (hid >= MAX_RELS) continue;
            var k: c_int = 0;
            while (k < cr.n_instrs) : (k += 1) {
                if (cr.instrs.?[@intCast(k)].op == OP_NEG_CHECK) {
                    full_reset[@intCast(hid)] = 1;
                    break;
                }
            }
        }
        {
            var again: c_int = 1;
            while (again != 0) {
                again = 0;
                i = 0;
                while (i < d.n_crules) : (i += 1) {
                    const cc = d.crules[@intCast(i)];
                    if (cc == null) continue;
                    const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
                    const hid: c_int = @intCast(cr.head_rel_id);
                    if (cr.has_aggregate != 0) continue;
                    if (hid >= MAX_RELS or full_reset[@intCast(hid)] != 0) continue;
                    var k: c_int = 0;
                    while (k < cr.n_instrs) : (k += 1) {
                        const in = &cr.instrs.?[@intCast(k)];
                        switch (in.op) {
                            OP_SCAN, OP_LOOKUP, OP_NEG_CHECK, OP_WALK, OP_LOOKUP_PERM, OP_HASH_JOIN => {
                                if (in.a < MAX_RELS and full_reset[@intCast(in.a)] != 0) {
                                    full_reset[@intCast(hid)] = 1;
                                    again = 1;
                                }
                            },
                            else => {},
                        }
                        if (full_reset[@intCast(hid)] != 0) break;
                    }
                }
            }
        }
    }

    var s: c_int = 0;
    while (s <= max_stratum) : (s += 1) {
        var again: c_int = 1;
        while (again != 0) {
            again = 0;
            i = 0;
            while (i < d.n_crules) : (i += 1) {
                const cc = d.crules[@intCast(i)];
                if (cc == null) continue;
                const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
                const hid: c_int = @intCast(cr.head_rel_id);
                if (cr.has_aggregate != 0) continue;
                if (cr.stratum != s) continue;
                if (hid >= MAX_RELS or full_reset[@intCast(hid)] != 0) continue;
                var k: c_int = 0;
                while (k < cr.n_instrs) : (k += 1) {
                    const in = &cr.instrs.?[@intCast(k)];
                    if (in.op != OP_SCAN and in.op != OP_LOOKUP) continue;
                    const br: c_int = @intCast(in.a);
                    if (br < 0 or br >= MAX_RELS) continue;
                    const ob = over[@intCast(br)];
                    if (ob == null or ob.?.count == 0) continue;
                    if (full_reset[@intCast(br)] != 0) continue;

                    var ov: [1]vm_override = undefined;
                    ov[0].body_idx = @intCast(in.body_idx);
                    ov[0].ts = ob;
                    ov[0].perm_id = -1;
                    var cand: tupleset.tuple_set = undefined;
                    if (ts_init(&cand, dx.rel_arity(d.rels[@intCast(hid)].rel)) != 0)
                        break :out;
                    var ctx = CandCtx{ .ts = &cand };
                    const m = execRule(d, cr, &ov, 1, 1, candCb, &ctx);
                    if (m < 0) {
                        ts_free(&cand);
                        break :out;
                    }
                    var ci: c_long = 0;
                    while (ci < cand.count) : (ci += 1) {
                        const t = cand.data.? + @as(usize, @intCast(ci)) * @as(usize, cand.arity);
                        if (over[@intCast(hid)] == null) {
                            const mem = c.malloc(@sizeOf(tupleset.tuple_set)) orelse {
                                ts_free(&cand);
                                break :out;
                            };
                            const hts: *tupleset.tuple_set = @ptrCast(@alignCast(mem));
                            if (ts_init(hts, cand.arity) != 0) {
                                c.free(mem);
                                over[@intCast(hid)] = null;
                                ts_free(&cand);
                                break :out;
                            }
                            over[@intCast(hid)] = hts;
                        }
                        const ar = ts_add(over[@intCast(hid)], t);
                        if (ar < 0) {
                            ts_free(&cand);
                            break :out;
                        }
                        if (ar == 1) again = 1;
                    }
                    ts_free(&cand);
                    if (over[@intCast(hid)] != null and over[@intCast(hid)].?.count > 1)
                        ts_sort(over[@intCast(hid)]);
                }
            }
        }
        i = 0;
        while (i < MAX_RELS) : (i += 1) {
            if (over[@intCast(i)] != null and over[@intCast(i)].?.count > 0)
                ts_sort(over[@intCast(i)]);
        }
    }

    i = 0;
    while (i < @as(c_int, @intCast(d.nrels))) : (i += 1) {
        const ob = over[@intCast(i)];
        if (ob == null or ob.?.count == 0) continue;
        if (full_reset[@intCast(i)] != 0) continue;
        const rel = d.rels[@intCast(i)].rel;
        var ci: c_long = 0;
        var dirtied: c_int = 0;
        while (ci < ob.?.count) : (ci += 1) {
            const t = ob.?.data.? + @as(usize, @intCast(ci)) * @as(usize, ob.?.arity);
            if (dx.rel_exact_base(rel, t) != 0) continue;
            if (dx.rel_delete(rel, t) == 1) dirtied = 1;
        }
        if (dirtied != 0) dx.permindex_mark_dirty(d, i);
    }

    i = 0;
    while (i < @as(c_int, @intCast(d.nrels))) : (i += 1) {
        if (full_reset[@intCast(i)] == 0 or is_head[@intCast(i)] == 0) continue;
        if (dx.rel_reset_view(d.rels[@intCast(i)].rel) != 0) break :out;
        dx.permindex_mark_dirty(d, i);
    }
    if (dx.permindex_build_dirty(d) != 0) break :out;

    {
        var queue: [MAX_RELS]c_int = undefined;
        var qt: c_int = 0;
        var qh2: c_int = 0;
        i = 0;
        while (i < MAX_RELS) : (i += 1) {
            if (changed[@intCast(i)] != 0 or full_reset[@intCast(i)] != 0 or
                (over[@intCast(i)] != null and over[@intCast(i)].?.count > 0))
            {
                affected[@intCast(i)] = 1;
                if (qt < MAX_RELS) {
                    queue[@intCast(qt)] = i;
                    qt += 1;
                }
            }
        }
        while (qh2 < qt) {
            const r = queue[@intCast(qh2)];
            qh2 += 1;
            i = 0;
            while (i < d.n_crules) : (i += 1) {
                const cc = d.crules[@intCast(i)];
                if (cc == null) continue;
                const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
                const hid: c_int = @intCast(cr.head_rel_id);
                if (cr.has_aggregate != 0) continue;
                if (hid >= MAX_RELS or affected[@intCast(hid)] != 0) continue;
                if (dredRuleReads(cr, r) != 0) {
                    affected[@intCast(hid)] = 1;
                    if (qt < MAX_RELS) {
                        queue[@intCast(qt)] = hid;
                        qt += 1;
                    }
                }
            }
        }
    }

    s = 0;
    while (s <= max_stratum) : (s += 1) {
        var srules: [256]?*compiler.compiled_rule = undefined;
        var sc: c_int = 0;
        i = 0;
        while (i < d.n_crules) : (i += 1) {
            const cc = d.crules[@intCast(i)];
            if (cc == null) continue;
            const cr: *compiler.compiled_rule = @ptrCast(@alignCast(cc));
            if (cr.has_aggregate != 0) continue;
            if (cr.stratum != s) continue;
            if (cr.head_rel_id < MAX_RELS and affected[@intCast(cr.head_rel_id)] == 0) continue;
            if (sc < 256) {
                srules[@intCast(sc)] = cr;
                sc += 1;
            }
        }
        if (sc == 0) continue;
        if (evalNonrecursive(d, srules[0..].ptr, sc) != 0) break :out;
    }

        rc = 0;
    }

    i = 0;
    while (i < MAX_RELS) : (i += 1) {
        if (over[@intCast(i)]) |ts| {
            ts_free(ts);
            c.free(@ptrCast(ts));
        }
    }
    vm_clear_deltas(d);
    vm_clear_deletes(d);
    return rc;
}

// ─── IVM Slice 4: aggregates under change ──────────────────────────────────

const AggScan = struct {
    op: u8,
    src_col: u8,
    count: u32,
    sum: u32,
    min: u32,
    max: u32,
};

fn aggScanCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = arity;
    const st: *AggScan = @ptrCast(@alignCast(user orelse return -1));
    st.count +%= 1;
    if (st.op == 1) {
        st.sum +%= cols[st.src_col];
    } else if (st.op == 2) {
        const v = cols[st.src_col];
        if (st.count == 1) st.min = v else if (v < st.min) st.min = v;
    } else if (st.op == 3) {
        const v = cols[st.src_col];
        if (st.count == 1) st.max = v else if (v > st.max) st.max = v;
    }
    return 0;
}

const AggOld = struct {
    cols: [8]u32,
    found: c_int,
};

fn aggOldCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const oh: *AggOld = @ptrCast(@alignCast(user orelse return -1));
    if (oh.found == 0 and arity >= 1 and arity <= 8) {
        @memcpy(oh.cols[0..arity], cols[0..arity]);
        oh.found = 1;
    }
    return 0;
}

fn aggCountCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = cols;
    _ = arity;
    const n: *c_long = @ptrCast(@alignCast(user orelse return -1));
    n.* +%= 1;
    return 0;
}

fn relBaseEmpty(db: *dx.dl_db, rel_id: c_int) c_int {
    const r = dbRel(db, rel_id);
    var n: c_long = 0;
    if (r == null) return -1;
    if (dx.rel_prefix_base(r, null, 0, aggCountCb, &n) < 0) return -1;
    return if (n == 0) 1 else 0;
}

fn aggRuleTractable(db: *dx.dl_db, cr: *const compiler.compiled_rule, is_head: [*]const u8) c_int {
    var anchor: c_int = 0;
    var anchor_arity: c_int = 0;
    var n_key: c_int = 0;
    var op: c_int = 0;
    var res_slot: c_int = 0;
    var head_arity: c_int = 0;
    var g: c_int = 0;
    var cc: c_int = 0;

    if (cr.n_instrs != 4) return 0;
    if (cr.instrs.?[0].op != OP_SCAN or
        cr.instrs.?[1].op != OP_AGG_ACC or
        cr.instrs.?[2].op != OP_AGG_EMIT or
        cr.instrs.?[3].op != OP_HALT) return 0;

    const scan = &cr.instrs.?[0];
    const acc = &cr.instrs.?[1];
    const emit = &cr.instrs.?[2];

    anchor = @intCast(scan.a);
    anchor_arity = @intCast(scan.b);
    n_key = @intCast(acc.a);
    op = @intCast(acc.b);
    res_slot = @intCast(acc.c);
    head_arity = @intCast(emit.b);

    if (anchor < 0 or anchor >= MAX_RELS) return 0;
    if (anchor >= @as(c_int, @intCast(db.nrels))) return 0;
    if (is_head[@intCast(anchor)] != 0) return 0;

    cc = 0;
    while (cc < anchor_arity) : (cc += 1) {
        const sc: u8 = scan.slots[@intCast(cc)];
        if (sc == 0xFF) continue;
        g = cc + 1;
        while (g < anchor_arity) : (g += 1) {
            if (scan.slots[@intCast(g)] == sc) return 0;
        }
    }

    if (n_key > anchor_arity) return 0;
    g = 0;
    while (g < n_key) : (g += 1) {
        if (scan.slots[@intCast(g)] != acc.slots[@intCast(g)]) return 0;
    }

    if (op != 0) {
        const src_slot: c_int = @intCast(acc.slots[@intCast(n_key)]);
        cc = 0;
        while (cc < anchor_arity) : (cc += 1) {
            if (@as(c_int, @intCast(scan.slots[@intCast(cc)])) == src_slot) break;
        }
        if (cc == anchor_arity) return 0;
    }

    if (head_arity != n_key + 1) return 0;
    g = 0;
    while (g < n_key) : (g += 1) {
        if (emit.slots[@intCast(g)] != acc.slots[@intCast(g)]) return 0;
    }
    if (emit.slots[@intCast(n_key)] != @as(u8, @intCast(res_slot))) return 0;

    if (relBaseEmpty(db, @intCast(emit.a)) != 1) return 0;

    return 1;
}

/// int vm_agg_eligible(dl_db*)
pub export fn vm_agg_eligible(db: ?*dx.dl_db) c_int {
    var is_head: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
    var agg_head: [MAX_RELS]u8 = [_]u8{0} ** MAX_RELS;
    var i: c_int = 0;
    var k: c_int = 0;
    var has_agg: c_int = 0;

    const d = db orelse return 0;
    if (dx.db_has_variadic(d) != 0 or dx.db_has_list_builtin(d) != 0 or dx.db_has_range_builtin(d) != 0) return 0;

    i = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        if (cr.is_recursive != 0) return 0;
        if (cr.has_aggregate != 0) has_agg = 1;
        if (cr.head_rel_id < MAX_RELS) is_head[cr.head_rel_id] = 1;
    }
    if (has_agg == 0) return 0;

    i = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        if (cr.has_aggregate != 0 and cr.head_rel_id < MAX_RELS)
            agg_head[cr.head_rel_id] = 1;
    }

    {
        var head_count: [MAX_RELS]c_int = [_]c_int{0} ** MAX_RELS;
        i = 0;
        while (i < d.n_crules) : (i += 1) {
            const cc = d.crules[@intCast(i)];
            if (cc == null) continue;
            const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
            if (cr.head_rel_id < MAX_RELS) head_count[cr.head_rel_id] += 1;
        }
        i = 0;
        while (i < MAX_RELS) : (i += 1) {
            if (agg_head[@intCast(i)] != 0 and head_count[@intCast(i)] > 1) return 0;
        }
    }

    i = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        k = 0;
        while (k < cr.n_instrs) : (k += 1) {
            const op: u8 = cr.instrs.?[@intCast(k)].op;
            switch (op) {
                OP_SCAN, OP_LOOKUP, OP_NEG_CHECK, OP_WALK, OP_LOOKUP_PERM, OP_HASH_JOIN => {
                    if (cr.instrs.?[@intCast(k)].a < MAX_RELS and
                        agg_head[cr.instrs.?[@intCast(k)].a] != 0) return 0;
                },
                else => {},
            }
        }
    }

    i = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        if (cr.has_aggregate != 0) continue;
        k = 0;
        while (k < cr.n_instrs) : (k += 1) {
            const op: u8 = cr.instrs.?[@intCast(k)].op;
            if (op == OP_WALK or op == OP_LOOKUP_PERM or op == OP_HASH_JOIN) return 0;
            if (op == OP_MAT_BEGIN or op == OP_MAT_JOIN) return 0;
        }
    }

    i = 0;
    while (i < d.n_crules) : (i += 1) {
        const cc = d.crules[@intCast(i)];
        if (cc == null) continue;
        const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
        if (cr.has_aggregate == 0) continue;
        if (aggRuleTractable(d, cr, &is_head) == 0) return 0;
    }

    return 1;
}

fn aggUpdateGroup(db: *dx.dl_db, cr: *const compiler.compiled_rule, group_key: [*c]const u32) c_int {
    const scan = &cr.instrs.?[0];
    const acc = &cr.instrs.?[1];
    const emit = &cr.instrs.?[2];
    const anchor: c_int = @intCast(scan.a);
    const anchor_arity: c_int = @intCast(scan.b);
    const n_key: c_int = @intCast(acc.a);
    const op: c_int = @intCast(acc.b);
    const head: c_int = @intCast(emit.a);
    const arel = dbRel(db, anchor);
    const hrel = dbRel(db, head);
    var st: AggScan = std.mem.zeroes(AggScan);
    var oh: AggOld = std.mem.zeroes(AggOld);
    var cc: c_int = 0;
    var src_col: c_int = 0;

    if (arel == null or hrel == null) return -1;

    if (op != 0) {
        const src_slot: c_int = @intCast(acc.slots[@intCast(n_key)]);
        src_col = -1;
        cc = 0;
        while (cc < anchor_arity) : (cc += 1) {
            if (@as(c_int, @intCast(scan.slots[@intCast(cc)])) == src_slot) {
                src_col = cc;
                break;
            }
        }
        if (src_col < 0) return -1;
    }

    st.op = @intCast(op);
    st.src_col = @intCast(src_col);

    if (dx.rel_prefix(arel, group_key, @intCast(n_key), aggScanCb, &st) < 0)
        return -1;

    if (dx.rel_prefix(hrel, group_key, @intCast(n_key), aggOldCb, &oh) < 0)
        return -1;

    if (st.count == 0) {
        if (oh.found != 0 and dx.rel_delete(hrel, &oh.cols) < 0) return -1;
        return 0;
    }

    {
        var agg_val: u32 = 0;
        var new_tuple: [8]u32 = undefined;
        var gi: c_int = 0;
        switch (op) {
            1 => agg_val = st.sum,
            2 => agg_val = st.min,
            3 => agg_val = st.max,
            else => agg_val = st.count,
        }
        gi = 0;
        while (gi < n_key) : (gi += 1) new_tuple[@intCast(gi)] = group_key[@intCast(gi)];
        new_tuple[@intCast(n_key)] = agg_val;

        if (dx.rel_exact(hrel, &new_tuple) != 0) return 0;
        if (oh.found != 0 and dx.rel_delete(hrel, &oh.cols) < 0) return -1;
        if (dx.rel_add(hrel, &new_tuple) < 0) return -1;
    }
    return 0;
}

fn aggMaintainRule(db: *dx.dl_db, cr: *const compiler.compiled_rule) c_int {
    const scan = &cr.instrs.?[0];
    const acc = &cr.instrs.?[1];
    const anchor: c_int = @intCast(scan.a);
    const n_key: c_int = @intCast(acc.a);
    var groups: tupleset.tuple_set = undefined;
    var gi: c_long = 0;
    var which: c_int = 0;

    if (n_key == 0) {
        const has_delta = db.delta_pending[@intCast(anchor)] != null and
            tsFromPending(db.delta_pending[@intCast(anchor)]).?.count > 0;
        const has_del = db.del_pending[@intCast(anchor)] != null and
            tsFromPending(db.del_pending[@intCast(anchor)]).?.count > 0;
        if (!has_delta and !has_del) return 0;
        return aggUpdateGroup(db, cr, null);
    }

    if (ts_init(&groups, @intCast(n_key)) != 0) return -1;

    which = 0;
    while (which < 2) : (which += 1) {
        const dp = if (which == 0) db.delta_pending[@intCast(anchor)] else db.del_pending[@intCast(anchor)];
        if (dp == null) continue;
        const tsp: *tupleset.tuple_set = @ptrCast(@alignCast(dp));
        if (tsp.count == 0) continue;
        gi = 0;
        while (gi < tsp.count) : (gi += 1) {
            const t = tsp.data.? + @as(usize, @intCast(gi)) * @as(usize, tsp.arity);
            if (ts_add(&groups, t) < 0) {
                ts_free(&groups);
                return -1;
            }
        }
    }

    if (groups.count == 0) {
        ts_free(&groups);
        return 0;
    }
    ts_sort(&groups);

    gi = 0;
    while (gi < groups.count) : (gi += 1) {
        const g = groups.data.? + @as(usize, @intCast(gi)) * @as(usize, groups.arity);
        if (aggUpdateGroup(db, cr, g) != 0) {
            ts_free(&groups);
            return -1;
        }
    }
    ts_free(&groups);
    return 0;
}

/// int vm_agg_maintain(dl_db*)
pub export fn vm_agg_maintain(db: ?*dx.dl_db) c_int {
    var ri: c_int = 0;
    var i: c_int = 0;
    var has_ins: c_int = 0;
    var has_del: c_int = 0;
    var has_nonagg: c_int = 0;
    var has_neg: c_int = 0;

    const d = db orelse return -1;
    vmAggRunsRef().* +%= 1;

    fail: {
        ri = 0;
        while (ri < d.n_crules) : (ri += 1) {
            const cc = d.crules[@intCast(ri)];
            if (cc == null) continue;
            const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
            if (cr.has_aggregate == 0) continue;
            if (aggMaintainRule(d, cr) != 0) break :fail;
        }

        ri = 0;
        while (ri < d.n_crules) : (ri += 1) {
            const cc = d.crules[@intCast(ri)];
            if (cc == null) continue;
            const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
            if (cr.has_aggregate == 0) continue;
            dx.permindex_mark_dirty(d, @intCast(cr.head_rel_id));
        }
        if (dx.permindex_build_dirty(d) != 0) break :fail;

        i = 0;
        while (i < MAX_RELS) : (i += 1) {
            if (d.delta_pending[@intCast(i)] != null and tsFromPending(d.delta_pending[@intCast(i)]).?.count > 0) has_ins = 1;
            if (d.del_pending[@intCast(i)] != null and tsFromPending(d.del_pending[@intCast(i)]).?.count > 0) has_del = 1;
        }
        ri = 0;
        while (ri < d.n_crules) : (ri += 1) {
            const cc = d.crules[@intCast(ri)];
            if (cc == null) continue;
            const cr: *const compiler.compiled_rule = @ptrCast(@alignCast(cc));
            var k: c_int = 0;
            if (cr.has_aggregate != 0) continue;
            has_nonagg = 1;
            while (k < cr.n_instrs) : (k += 1) {
                if (cr.instrs.?[@intCast(k)].op == OP_NEG_CHECK) {
                    has_neg = 1;
                    break;
                }
            }
        }

        if (has_nonagg == 0 or (has_ins == 0 and has_del == 0)) {
            vm_clear_deltas(d);
            vm_clear_deletes(d);
            return 0;
        }
        if (has_del != 0 or (has_ins != 0 and has_neg != 0)) {
            if (vm_dred_delete(d) != 0) break :fail;
        } else if (has_ins != 0) {
            if (vm_propagate_deltas(d) != 0) break :fail;
        }
        return 0;
    }

    vm_clear_deltas(d);
    vm_clear_deletes(d);
    return -1;
}

// ─── Tests ─────────────────────────────────────────────────────────────────

const testing = std.testing;

extern "c" fn parse_create(source: ?[*:0]const u8) ?*anyopaque;
extern "c" fn parse_rules(p: ?*anyopaque, n_rules: ?*c_int) ?[*]?*parser.rule;
extern "c" fn parse_free(p: ?*anyopaque) void;
extern "c" fn rule_free(r: ?*parser.rule) void;
extern "c" fn system(cmd: [*c]const u8) c_int;

const ParsedRules = struct {
    p: ?*anyopaque,
    rules: ?[*]?*parser.rule,
    n: c_int,
};

fn testParse(src: [*:0]const u8) ?ParsedRules {
    const p = parse_create(src) orelse return null;
    var n: c_int = 0;
    const rules = parse_rules(p, &n) orelse {
        parse_free(p);
        return null;
    };
    return .{ .p = p, .rules = rules, .n = n };
}

fn testParseFree(pd: ParsedRules) void {
    var i: c_int = 0;
    while (i < pd.n) : (i += 1) rule_free(pd.rules.?[@intCast(i)]);
    c.free(@ptrCast(pd.rules));
    parse_free(pd.p);
}

fn testRmRf(path: [*:0]const u8) void {
    var buf: [256]u8 = undefined;
    const cmd = std.fmt.bufPrintZ(&buf, "rm -rf {s}", .{std.mem.span(path)}) catch return;
    _ = system(cmd.ptr);
}

fn testOpenDb(path: [*:0]const u8) *dx.dl_db {
    testRmRf(path);
    return dx.dl_open(path) orelse @panic("dl_open failed");
}

fn testCloseDb(db: *dx.dl_db, path: [*:0]const u8) void {
    dx.dl_close(db);
    testRmRf(path);
}

fn testAddFacts(db: *dx.dl_db, rel: [*:0]const u8, cols: []const u32, ar: u8, n: usize) void {
    var i: usize = 0;
    while (i < n) : (i += 1) {
        if (dx.dl_add_fact(db, rel, cols.ptr + i * ar, ar) != 1) @panic("dl_add_fact failed");
    }
}

fn testCompile(db: *dx.dl_db, src: [*:0]const u8, n_out: *c_int) ?[*]?*compiler.compiled_rule {
    const pd = testParse(src) orelse return null;
    defer testParseFree(pd);
    var arr: ?[*]?*compiler.compiled_rule = null;
    const rc = compile_rules(db, pd.rules, pd.n, &arr, n_out);
    if (rc != 0) return null;
    return arr;
}

fn testFreeCompiled(arr: ?[*]?*compiler.compiled_rule, n: c_int) void {
    var i: c_int = 0;
    while (i < n) : (i += 1) compiler.compiled_rule_free(arr.?[@intCast(i)]);
    c.free(@ptrCast(arr));
}

fn testCountCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    _ = cols;
    _ = arity;
    const n: *c_long = @ptrCast(@alignCast(user orelse return 1));
    n.* += 1;
    return 0;
}

test "vm_execute: non-recursive program materializes derived facts" {
    const path = "/tmp/datalog_zig_u9_vm_basic";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 2));
    testAddFacts(db, "edge", &.{ 1, 2, 2, 3 }, 2, 2);

    var n: c_int = 0;
    const arr = testCompile(db, "p(X,Y):-edge(X,Y).\n", &n) orelse return error.TestUnexpectedResult;
    defer testFreeCompiled(arr, n);
    try testing.expectEqual(@as(c_int, 1), n);

    try testing.expectEqual(@as(c_int, 0), vm_execute(db, arr, n));

    const t1 = [2]u32{ 1, 2 };
    const t2 = [2]u32{ 2, 3 };
    const t3 = [2]u32{ 3, 4 };
    try testing.expectEqual(@as(c_int, 1), dx.dl_lookup(db, "p", &t1, 2));
    try testing.expectEqual(@as(c_int, 1), dx.dl_lookup(db, "p", &t2, 2));
    try testing.expectEqual(@as(c_int, 0), dx.dl_lookup(db, "p", &t3, 2));
}

test "vm_execute: recursive transitive closure (need_idb_sort trap shape)" {
    const path = "/tmp/datalog_zig_u9_vm_recursive";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 2));
    testAddFacts(db, "edge", &.{ 1, 2, 2, 3, 3, 4, 4, 5 }, 2, 4);

    // The second rule has TWO recursive body atoms (reach(X,Z), reach(Z,Y)),
    // both compiled to plain OP_LOOKUP — exactly the need_idb_sort shape the
    // fixpoint must re-sort idb for each iteration (else ts_prefix silently
    // misses tuples and the closure is incomplete).
    var n: c_int = 0;
    const arr = testCompile(db,
        "reach(X,Y):-edge(X,Y).\nreach(X,Y):-reach(X,Z),reach(Z,Y).\n", &n) orelse return error.TestUnexpectedResult;
    defer testFreeCompiled(arr, n);
    try testing.expectEqual(@as(c_int, 2), n);

    try testing.expectEqual(@as(c_int, 0), vm_execute(db, arr, n));

    const pairs = [_][2]u32{
        .{ 1, 2 }, .{ 2, 3 }, .{ 3, 4 }, .{ 4, 5 },
        .{ 1, 3 }, .{ 2, 4 }, .{ 3, 5 },
        .{ 1, 4 }, .{ 2, 5 },
        .{ 1, 5 },
    };
    for (pairs) |t| {
        try testing.expectEqual(@as(c_int, 1), dx.dl_lookup(db, "reach", &t, 2));
    }
    const absent = [2]u32{ 5, 1 };
    try testing.expectEqual(@as(c_int, 0), dx.dl_lookup(db, "reach", &absent, 2));
}

test "vm_query: streams goal tuples via callback" {
    const path = "/tmp/datalog_zig_u9_vm_query";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 2));
    testAddFacts(db, "edge", &.{ 1, 2, 2, 3 }, 2, 2);

    var n: c_int = 0;
    const arr = testCompile(db, "p(X,Y):-edge(X,Y).\n", &n) orelse return error.TestUnexpectedResult;
    defer testFreeCompiled(arr, n);

    var count: c_long = 0;
    const rc = vm_query(db, arr, n, "p", testCountCb, &count);
    try testing.expectEqual(@as(c_long, 2), rc);
    try testing.expectEqual(@as(c_long, 2), count);
}

test "vm_ivm_eligible: eligible join vs ineligible negation" {
    const path = "/tmp/datalog_zig_u9_vm_ivm";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "edge", 2));
    try testing.expectEqual(@as(c_int, 0), dx.dl_declare_relation(db, "node", 1));
    testAddFacts(db, "edge", &.{ 1, 2 }, 2, 1);
    testAddFacts(db, "node", &.{1}, 1, 1);

    // Eligible: a single non-recursive join (SCAN/LOOKUP only).
    try testing.expectEqual(@as(c_int, 0), dx.dl_load_rules(db, "p(X,Y):-edge(X,Y).\n"));
    try testing.expectEqual(@as(c_int, 1), vm_ivm_eligible(db));
    try testing.expectEqual(@as(c_int, 0), vm_has_recursive(db));

    // Ineligible: negation forces the full-fixpoint path.
    try testing.expectEqual(@as(c_int, 0), dx.dl_load_rules(db, "q(X):-node(X),!edge(X,X).\n"));
    try testing.expectEqual(@as(c_int, 0), vm_ivm_eligible(db));
}

test "vm counters: propagate/dred/agg runs increment (observable paths taken)" {
    const path = "/tmp/datalog_zig_u9_vm_counters";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    try testing.expectEqual(@as(c_int, 0), vmDredRunsRef().*);
    try testing.expectEqual(@as(c_int, 0), vmAggRunsRef().*);
    try testing.expectEqual(@as(c_int, 0), vmPropagateRunsRef().*);
    try testing.expectEqual(@as(c_long, 0), vmRangeYieldsRef().*);

    try testing.expectEqual(@as(c_int, 0), vm_propagate_deltas(db));
    try testing.expectEqual(@as(c_int, 1), vmPropagateRunsRef().*);

    try testing.expectEqual(@as(c_int, 0), vm_dred_delete(db));
    try testing.expectEqual(@as(c_int, 1), vmDredRunsRef().*);

    try testing.expectEqual(@as(c_int, 0), vm_agg_maintain(db));
    try testing.expectEqual(@as(c_int, 1), vmAggRunsRef().*);
}
