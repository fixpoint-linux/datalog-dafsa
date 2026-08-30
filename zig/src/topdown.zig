//! topdown.zig — port of src/topdown.c (top-down / QSQ demand-driven
//! evaluator, SLG-style worklist over subqueries).
//!
//! Runs the magic-sets ADORNED PROGRAM (magic_transform_adorn output) as an
//! SLG worklist over subqueries (variant + bound tuple) instead of as a
//! forward semi-naive fixpoint.  The "magic relation" for a variant is its
//! bound_set (all subquery bound tuples, arity nbound); the "adorned relation"
//! is its memo (all answers, full arity).  Discovery is driven by the MAGIC
//! rules (their heads are child bound tuples); answers are derived by the
//! ADORNED rules with the bound_set as the guard override.
//!
//! `td_ctx` is OPAQUE (topdown.h forward-declares it) → a native Zig struct.
//! `vm_override`, `compiled_rule`/`vm_instr` come from vm.zig/compiler.zig;
//! `tuple_set` from tupleset.zig; `dl_db`/`dl_tuple_cb`/`dl_db_get_perm` via
//! @cImport("dl_internal.h").  Runs the compiled rule body with
//! vm_exec_rule() (vm.zig).  The driver is ITERATIVE (monotone least-fixpoint
//! over the reachable subquery set — no C recursion, so an N=10000 chain
//! cannot overflow the stack).
//!
//! No data globals are read here: g_reorder/g_bushy are consumed by the
//! still-C dl.c before this unit runs (it compiles the program with both 0),
//! and the vm_* counters belong to vm.zig — so no GOT-indirect accessors are
//! needed in this unit.
//!
//! Oracle: src/topdown.c (never modified).

const std = @import("std");
const c = std.c;

const parser = @import("parser.zig");
const compiler = @import("compiler.zig");
const tupleset = @import("tupleset.zig");
const magic = @import("magic.zig");

// dl_internal.h pulls in dl.h/intern.h/relation.h/.../compiler.h/regexwalk.h —
// supplying dl_db, dl_tuple_cb, dl_db_get_perm (permindex.h), and the extern
// decls for every still-C function this unit calls.
const dx = @cImport({
    @cInclude("dl_internal.h");
});

const vm_instr = compiler.vm_instr;
const compiled_rule = compiler.compiled_rule;
const tuple_set = tupleset.tuple_set;
const DlTupleCb = dx.dl_tuple_cb;

// vm_exec_rule is vm.zig's `pub export fn`; declare it against OUR @cImport'd
// dl_db (each module's translate-c produces a distinct type for the same C
// struct).
extern "c" fn vm_exec_rule(
    db: ?*dx.dl_db,
    cr: ?*const compiler.compiled_rule,
    ov: ?[*]const vm_override,
    n_ov: c_int,
    dry: c_int,
    cb: DlTupleCb,
    user: ?*anyopaque,
) c_long;

// tuple_set ops are `export fn`s in tupleset.zig — reach them through raw
// extern bindings, exactly like vm.zig does.
extern "c" fn ts_init(ts: ?*tuple_set, arity: u8) c_int;
extern "c" fn ts_free(ts: ?*tuple_set) void;
extern "c" fn ts_add(ts: ?*tuple_set, cols: ?[*]const u32) c_int;
extern "c" fn ts_sort(ts: ?*tuple_set) void;

/// typedef struct { int body_idx; const tuple_set *ts; int perm_id; }
/// vm_override (vm.h:38-42) — byte-identical to vm.zig's definition.
pub const vm_override = extern struct {
    body_idx: c_int,
    ts: ?*const tuple_set,
    perm_id: c_int,
};

comptime {
    std.debug.assert(@sizeOf(vm_override) == 24);
    std.debug.assert(@offsetOf(vm_override, "body_idx") == 0);
    std.debug.assert(@offsetOf(vm_override, "ts") == 8);
    std.debug.assert(@offsetOf(vm_override, "perm_id") == 16);
}

// ─── Constants ──────────────────────────────────────────────────────────────

const TD_MAX_VARIANTS = 64; // == MAX_ADORN_VARIANTS in magic.zig
const TD_MAX_OV = 32; // guard + up to 31 IDB body atoms per rule

const OP_SCAN: u8 = 1;
const OP_LOOKUP: u8 = 2;
const OP_LOOKUP_PERM: u8 = 11;
const OP_HASH_JOIN: u8 = 12;
const OP_WALK: u8 = 10;

// ─── Per-variant state ──────────────────────────────────────────────────────

const td_variant = struct {
    adorned_name: [80]u8,
    magic_name: [86]u8,
    adorn: [9]u8, // e.g. "bf" — length == arity
    arity: u8, // full arity
    nbound: u8, // count of 'b'
    bound_pos: [8]u8, // full-arity positions of 'b' (0-based)

    adorned_rules: ?[*]c_int,
    n_adorned: c_int,
    cap_adorned: c_int,
    magic_rules: ?[*]c_int,
    n_magic: c_int,
    cap_magic: c_int,

    bound_set: tuple_set, // all subquery bound tuples (arity nbound)
    bound_new: tuple_set, // bound tuples not yet processed
    bound_late: tuple_set, // bounds discovered during Phase B (need late-join)
    memo: tuple_set, // all answers (full arity)
    delta: tuple_set, // answers not yet propagated
    memo_sorted: c_int,
};

// ─── Per-rule meta (indexed by crules index == prog.rules index) ────────────

const td_rule_meta = struct {
    is_magic: c_int, // 1 = head is magic_name (discovery rule)
    head_variant: c_int, // adorned rule: V;  magic rule: target Q
    guard_variant: c_int, // adorned rule: V;  magic rule: parent P
    has_idb_body: c_int, // any idb_map[j] != -1 for j >= 1
    nbody: c_int,
    idb_map: ?[*]c_int, // [nbody]: variant_id for IDB body atom j, or -1
    op_at: ?[*]c_int, // [nbody]: opcode of the relational atom at j
    perm_at: ?[*]c_int, // [nbody]: perm_id (OP_LOOKUP_PERM / OP_HASH_JOIN)
    rel_at: ?[*]c_int, // [nbody]: rel_id (for dl_db_get_perm)
};

const td_consumer = struct { rule: c_int, body: c_int };
const consumer_vec = struct { v: ?[*]td_consumer, n: c_int, cap: c_int };

const td_ctx = struct {
    edb: ?*dx.dl_db,
    crules: ?[*]?*compiled_rule,
    n_crules: c_int,

    variants: ?[*]td_variant,
    n_variants: c_int,
    rmeta: ?[*]td_rule_meta,
    consumers: ?[*]consumer_vec,

    bound_queue: ?[*]c_int,
    bound_head: usize,
    bound_tail: usize,
    bound_cap: usize,
    prop_queue: ?[*]c_int,
    prop_head: usize,
    prop_tail: usize,
    prop_cap: usize,
    bound_pending: ?[*]u8,
    prop_pending: ?[*]u8,
    in_phase_b: c_int, // 1 while draining the prop queue (Phase B)

    goal_variant: c_int,
};

// ─── Small helpers ──────────────────────────────────────────────────────────

fn snprintStr(buf: []u8, s: ?[*:0]const u8) void {
    const n: usize = if (s) |sp| std.mem.span(sp).len else 0;
    if (buf.len > 0) {
        const to_copy = @min(n, buf.len - 1);
        if (s) |sp| @memcpy(buf[0..to_copy], sp[0..to_copy]);
        buf[to_copy] = 0;
    }
}

fn intVecPush(v: *?[*]c_int, n: *c_int, cap: *c_int, val: c_int) c_int {
    if (n.* >= cap.*) {
        const nc: c_int = if (cap.* != 0) cap.* * 2 else 8;
        const mem = c.realloc(if (v.*) |p| p else null, @as(usize, @intCast(nc)) * @sizeOf(c_int));
        if (mem == null) return -1;
        v.* = @ptrCast(@alignCast(mem));
        cap.* = nc;
    }
    v.*.?[@intCast(n.*)] = val;
    n.* += 1;
    return 0;
}

fn consumerPush(cv: *consumer_vec, rule: c_int, body: c_int) c_int {
    if (cv.n >= cv.cap) {
        const nc: c_int = if (cv.cap != 0) cv.cap * 2 else 8;
        const mem = c.realloc(if (cv.v) |p| p else null, @as(usize, @intCast(nc)) * @sizeOf(td_consumer));
        if (mem == null) return -1;
        cv.v = @ptrCast(@alignCast(mem));
        cv.cap = nc;
    }
    cv.v.?[@intCast(cv.n)].rule = rule;
    cv.v.?[@intCast(cv.n)].body = body;
    cv.n += 1;
    return 0;
}

fn variantByAdornedName(cctx: *const td_ctx, name: [*:0]const u8) c_int {
    var i: c_int = 0;
    while (i < cctx.n_variants) : (i += 1) {
        if (std.mem.eql(u8, std.mem.span(@as([*:0]const u8, @ptrCast(&cctx.variants.?[@intCast(i)].adorned_name))), std.mem.span(name))) return i;
    }
    return -1;
}

fn variantByMagicName(cctx: *const td_ctx, name: [*:0]const u8) c_int {
    var i: c_int = 0;
    while (i < cctx.n_variants) : (i += 1) {
        if (std.mem.eql(u8, std.mem.span(@as([*:0]const u8, @ptrCast(&cctx.variants.?[@intCast(i)].magic_name))), std.mem.span(name))) return i;
    }
    return -1;
}

// ─── Build the variant table from prog.decls ────────────────────────────────

fn buildVariants(cctx: *td_ctx, prog: *const magic.magic_program) c_int {
    cctx.n_variants = @divTrunc(prog.n_decls, 2);
    if (cctx.n_variants <= 0 or cctx.n_variants > TD_MAX_VARIANTS) return -1;
    cctx.variants = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(cctx.n_variants)), @sizeOf(td_variant))));
    if (cctx.variants == null) return -1;

    var i: c_int = 0;
    while (i < cctx.n_variants) : (i += 1) {
        const v = &cctx.variants.?[@intCast(i)];
        const ad = &prog.decls.?[@as(usize, 2) * @as(usize, @intCast(i))];
        const mg = &prog.decls.?[@as(usize, 2) * @as(usize, @intCast(i)) + 1];

        snprintStr(v.adorned_name[0..], @as([*:0]const u8, @ptrCast(&ad.name)));
        snprintStr(v.magic_name[0..], @as([*:0]const u8, @ptrCast(&mg.name)));
        v.arity = ad.arity;
        v.nbound = mg.arity;

        // adorned_name is "<pred>__<adorn>"; the adorn is its last `arity`
        // chars (adorn chars are only 'b'/'f', so this is unambiguous).
        const alen = std.mem.len(@as([*:0]const u8, @ptrCast(&v.adorned_name)));
        if (alen < @as(usize, v.arity)) return -1;
        @memcpy(v.adorn[0..@as(usize, v.arity)], v.adorned_name[alen - @as(usize, v.arity) .. alen]);
        v.adorn[@intCast(v.arity)] = 0;

        var nb: u8 = 0;
        var j: c_int = 0;
        while (j < v.arity) : (j += 1) {
            if (v.adorn[@intCast(j)] == 'b') {
                v.bound_pos[@intCast(nb)] = @intCast(j);
                nb += 1;
            }
        }
        if (nb != v.nbound) return -1; // cross-check decls vs adorn

        if (ts_init(&v.bound_set, v.nbound) != 0) return -1;
        if (ts_init(&v.bound_new, v.nbound) != 0) return -1;
        if (ts_init(&v.bound_late, v.nbound) != 0) return -1;
        if (ts_init(&v.memo, v.arity) != 0) return -1;
        if (ts_init(&v.delta, v.arity) != 0) return -1;
        v.memo_sorted = 1;
    }
    return 0;
}

// ─── Build per-rule meta + variant rule lists ───────────────────────────────

fn appendRuleIdx(cctx: *td_ctx, variant: c_int, ri: c_int, adorned: c_int) c_int {
    const v = &cctx.variants.?[@intCast(variant)];
    if (adorned != 0)
        return intVecPush(&v.adorned_rules, &v.n_adorned, &v.cap_adorned, ri);
    return intVecPush(&v.magic_rules, &v.n_magic, &v.cap_magic, ri);
}

fn buildRuleMeta(cctx: *td_ctx, prog: *const magic.magic_program) c_int {
    cctx.rmeta = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(cctx.n_crules)), @sizeOf(td_rule_meta))));
    if (cctx.rmeta == null) return -1;

    var i: c_int = 0;
    while (i < cctx.n_crules) : (i += 1) {
        const r = prog.rules.?[@intCast(i)];
        const m = &cctx.rmeta.?[@intCast(i)];
        const nbody: c_int = if (r.?.nbody > 0) r.?.nbody else 1;

        m.nbody = r.?.nbody;
        m.idb_map = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(nbody)), @sizeOf(c_int))));
        m.op_at = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(nbody)), @sizeOf(c_int))));
        m.perm_at = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(nbody)), @sizeOf(c_int))));
        m.rel_at = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(nbody)), @sizeOf(c_int))));
        if (m.idb_map == null or m.op_at == null or m.perm_at == null or m.rel_at == null) return -1;

        const hv = variantByAdornedName(cctx, r.?.head.?.pred.?);
        if (hv >= 0) {
            // adorned rule: P^a :- magic_P^a(bound), body'.
            m.is_magic = 0;
            m.head_variant = hv;
            m.guard_variant = hv;
            if (appendRuleIdx(cctx, hv, i, 1) != 0) return -1;
        } else {
            const qv = variantByMagicName(cctx, r.?.head.?.pred.?);
            if (qv < 0) return -1; // unknown head predicate
            // magic rule: magic_Q^b :- magic_P^a(bound), prefix.
            m.is_magic = 1;
            m.head_variant = qv;
            if (r.?.nbody < 1) return -1;
            m.guard_variant = variantByMagicName(cctx, r.?.body.?[0].?.pred.?);
            if (m.guard_variant < 0) return -1;
            if (appendRuleIdx(cctx, m.guard_variant, i, 0) != 0) return -1;
        }

        m.has_idb_body = 0;
        var j: c_int = 0;
        while (j < r.?.nbody) : (j += 1) {
            m.idb_map.?[@intCast(j)] = -1;
            m.op_at.?[@intCast(j)] = -1;
            m.perm_at.?[@intCast(j)] = -1;
            m.rel_at.?[@intCast(j)] = -1;
            if (j == 0) continue; // magic guard (body 0) — handled specially

            m.idb_map.?[@intCast(j)] = variantByAdornedName(cctx, r.?.body.?[@intCast(j)].?.pred.?);
            if (m.idb_map.?[@intCast(j)] >= 0) m.has_idb_body = 1;

            // find the relational opcode emitted for this body atom
            const cr = cctx.crules.?[@intCast(i)];
            var k: c_int = 0;
            while (k < cr.?.n_instrs) : (k += 1) {
                const in = &cr.?.instrs.?[@intCast(k)];
                if (@as(c_int, in.body_idx) != j) continue;
                if (in.op == OP_SCAN or in.op == OP_LOOKUP or
                    in.op == OP_LOOKUP_PERM or in.op == OP_HASH_JOIN or
                    in.op == OP_WALK)
                {
                    m.op_at.?[@intCast(j)] = in.op;
                    m.rel_at.?[@intCast(j)] = @as(c_int, in.a);
                    // OP_HASH_JOIN's imm is now a PACKED permutation, not a
                    // perm_id, so it must never be read back as one.
                    m.perm_at.?[@intCast(j)] = if (in.op == OP_LOOKUP_PERM) @intCast(in.imm) else -1;
                    break;
                }
            }
        }
    }
    return 0;
}

// ─── Build reverse consumer lists ───────────────────────────────────────────

fn buildConsumers(cctx: *td_ctx) c_int {
    cctx.consumers = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(cctx.n_variants)), @sizeOf(consumer_vec))));
    if (cctx.consumers == null) return -1;
    var i: c_int = 0;
    while (i < cctx.n_crules) : (i += 1) {
        const m = &cctx.rmeta.?[@intCast(i)];
        var j: c_int = 1;
        while (j < m.nbody) : (j += 1) {
            const Q = m.idb_map.?[@intCast(j)];
            if (Q < 0) continue;
            if (consumerPush(&cctx.consumers.?[@intCast(Q)], i, j) != 0) return -1;
        }
    }
    return 0;
}

// ─── Queues (dedup via pending flags) ───────────────────────────────────────

fn enqueueBound(cctx: *td_ctx, V: c_int) c_int {
    if (cctx.bound_pending.?[@intCast(V)] != 0) return 0;
    if (cctx.bound_tail >= cctx.bound_cap) {
        const nc: usize = if (cctx.bound_cap != 0) cctx.bound_cap * 2 else 16;
        const mem = c.realloc(if (cctx.bound_queue) |p| p else null, nc * @sizeOf(c_int));
        if (mem == null) return -1;
        cctx.bound_queue = @ptrCast(@alignCast(mem));
        cctx.bound_cap = nc;
    }
    cctx.bound_queue.?[@intCast(cctx.bound_tail)] = V;
    cctx.bound_tail += 1;
    cctx.bound_pending.?[@intCast(V)] = 1;
    return 0;
}

fn enqueueProp(cctx: *td_ctx, V: c_int) c_int {
    if (cctx.prop_pending.?[@intCast(V)] != 0) return 0;
    if (cctx.prop_tail >= cctx.prop_cap) {
        const nc: usize = if (cctx.prop_cap != 0) cctx.prop_cap * 2 else 16;
        const mem = c.realloc(if (cctx.prop_queue) |p| p else null, nc * @sizeOf(c_int));
        if (mem == null) return -1;
        cctx.prop_queue = @ptrCast(@alignCast(mem));
        cctx.prop_cap = nc;
    }
    cctx.prop_queue.?[@intCast(cctx.prop_tail)] = V;
    cctx.prop_tail += 1;
    cctx.prop_pending.?[@intCast(V)] = 1;
    return 0;
}

// ─── Tuple collector + perm shadow ──────────────────────────────────────────

const collect_ctx = struct { ts: ?*tuple_set, err: c_int };

fn collectCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const cx: *collect_ctx = @ptrCast(@alignCast(user orelse return 1));
    if (cx.ts.?.arity == 0) {
        if (ts_init(cx.ts, arity) != 0) {
            cx.err = 1;
            return 1;
        }
    }
    if (ts_add(cx.ts, cols) < 0) {
        cx.err = 1;
        return 1;
    }
    return 0;
}

// Build the permuted shadow of `src` under `perm` (position p -> original
// column perm[p]), sorted in permuted order (what OP_LOOKUP_PERM's override
// path expects).  `out` must be zero-initialised.
fn buildPermShadow(src: *const tuple_set, perm: [*c]const u8, arity: u8, out: *tuple_set) c_int {
    if (src.count == 0) return 0; // leave out empty (arity 0)
    if (ts_init(out, arity) != 0) return -1;
    var row: [8]u32 = undefined;
    var i: c_long = 0;
    while (i < src.count) : (i += 1) {
        const r = src.data.? + @as(usize, @intCast(i)) * @as(usize, arity);
        var p: u8 = 0;
        while (p < arity) : (p += 1) {
            row[@intCast(p)] = r[@as(usize, perm[@intCast(p)])];
        }
        if (ts_add(out, &row) < 0) {
            ts_free(out);
            return -1;
        }
    }
    ts_sort(out);
    return 0;
}

// ─── Fire one compiled rule with overrides ──────────────────────────────────

fn fireRule(cctx: *td_ctx, ri: c_int, guard_ts: *const tuple_set, delta_body: c_int, delta_ts: ?*const tuple_set, out: *tuple_set) c_int {
    const m = &cctx.rmeta.?[@intCast(ri)];
    const cr = cctx.crules.?[@intCast(ri)];
    var ov: [TD_MAX_OV]vm_override = undefined;
    var shadows: [TD_MAX_OV]tuple_set = undefined;
    var shadow_used: [TD_MAX_OV]c_int = undefined;
    var n_ov: c_int = 0;

    if (m.nbody + 1 > TD_MAX_OV) return -1;

    @memset(shadow_used[0..], 0);
    var cx = std.mem.zeroes(collect_ctx);
    cx.ts = out;

    // body 0 = magic guard → OP_SCAN over the bound set
    ov[@intCast(n_ov)].body_idx = 0;
    ov[@intCast(n_ov)].ts = guard_ts;
    ov[@intCast(n_ov)].perm_id = -1;
    n_ov += 1;

    var j: c_int = 1;
    while (j < m.nbody) : (j += 1) {
        const Q = m.idb_map.?[@intCast(j)];
        if (Q < 0) continue;
        const memo_ptr: *const tuple_set = &cctx.variants.?[@intCast(Q)].memo;
        const src: *const tuple_set = if (delta_body == j) (delta_ts orelse memo_ptr) else memo_ptr;
        if (src.count == 0) continue; // empty → DAFSA read (empty rel)

        const op = m.op_at.?[@intCast(j)];
        if (op == OP_LOOKUP_PERM) {
            const perm = dx.dl_db_get_perm(cctx.edb.?, m.rel_at.?[@intCast(j)], m.perm_at.?[@intCast(j)]);
            if (perm == null or src.arity > 8) return -1;
            const sh = &shadows[@intCast(n_ov)];
            if (buildPermShadow(src, perm, src.arity, sh) != 0) {
                var t: c_int = 0;
                while (t < n_ov) : (t += 1) {
                    if (shadow_used[@intCast(t)] != 0) ts_free(&shadows[@intCast(t)]);
                }
                return -1;
            }
            shadow_used[@intCast(n_ov)] = 1;
            ov[@intCast(n_ov)].body_idx = j;
            ov[@intCast(n_ov)].ts = sh;
            ov[@intCast(n_ov)].perm_id = -1;
            n_ov += 1;
        } else {
            // OP_SCAN / OP_LOOKUP / OP_HASH_JOIN.  Only OP_LOOKUP reads via
            // ts_prefix and therefore needs the source SORTED.
            if (op == OP_LOOKUP and src == memo_ptr and cctx.variants.?[@intCast(Q)].memo_sorted == 0) {
                ts_sort(&cctx.variants.?[@intCast(Q)].memo);
                cctx.variants.?[@intCast(Q)].memo_sorted = 1;
            }
            ov[@intCast(n_ov)].body_idx = j;
            ov[@intCast(n_ov)].ts = src;
            ov[@intCast(n_ov)].perm_id = -1;
            n_ov += 1;
        }
    }

    const rc = vm_exec_rule(cctx.edb, cr, &ov, n_ov, 1, collectCb, &cx);

    var t: c_int = 0;
    while (t < n_ov) : (t += 1) {
        if (shadow_used[@intCast(t)] != 0) ts_free(&shadows[@intCast(t)]);
    }
    if (cx.err != 0 or rc < 0) return -1;
    return 0;
}

// ─── Memo / bound bookkeeping ───────────────────────────────────────────────

fn mergeInto(cctx: *td_ctx, V: c_int, out: *const tuple_set) c_int {
    const var_ = &cctx.variants.?[@intCast(V)];
    var added: c_int = 0;
    if (out.arity == 0 or out.count == 0) return 0;
    var i: c_long = 0;
    while (i < out.count) : (i += 1) {
        const t = out.data.? + @as(usize, @intCast(i)) * @as(usize, out.arity);
        const r = ts_add(&var_.memo, t);
        if (r < 0) return -1;
        if (r == 1) {
            added = 1;
            if (ts_add(&var_.delta, t) < 0) return -1;
        }
    }
    if (added != 0) var_.memo_sorted = 0;
    if (var_.delta.count > 0) {
        if (enqueueProp(cctx, V) != 0) return -1;
    }
    return 0;
}

fn addBound(cctx: *td_ctx, Q: c_int, t: [*]const u32) c_int {
    const var_ = &cctx.variants.?[@intCast(Q)];
    const r = ts_add(&var_.bound_set, t);
    if (r < 0) return -1;
    if (r == 1) {
        if (ts_add(&var_.bound_new, t) < 0) return -1;
        if (cctx.in_phase_b != 0 and ts_add(&var_.bound_late, t) < 0) return -1;
        if (enqueueBound(cctx, Q) != 0) return -1;
    }
    return 0;
}

// ─── Phase A: process new bounds (base answers + late join + discovery) ─────

fn initVariant(cctx: *td_ctx, V: c_int) c_int {
    const var_ = &cctx.variants.?[@intCast(V)];
    if (var_.bound_new.count == 0) return 0;

    var nb = var_.bound_new; // take ownership
    var_.bound_new = std.mem.zeroes(tuple_set);
    var_.bound_new.arity = var_.nbound; // keep arity: ts_add re-allocs data
    var nl = var_.bound_late; // take ownership (Phase-B bounds)
    var_.bound_late = std.mem.zeroes(tuple_set);
    var_.bound_late.arity = var_.nbound;

    // Answer: fire only the BASE adorned rules (no IDB body atoms) with
    // guard = new bounds.
    var a: c_int = 0;
    while (a < var_.n_adorned) : (a += 1) {
        const ri = var_.adorned_rules.?[@intCast(a)];
        if (cctx.rmeta.?[@intCast(ri)].has_idb_body != 0) continue;
        var out = std.mem.zeroes(tuple_set);
        if (fireRule(cctx, ri, &nb, -1, null, &out) != 0) {
            ts_free(&out);
            ts_free(&nl);
            ts_free(&nb);
            return -1;
        }
        if (mergeInto(cctx, V, &out) != 0) {
            ts_free(&out);
            ts_free(&nl);
            ts_free(&nb);
            return -1;
        }
        ts_free(&out);
    }

    // Late-join: bounds discovered during Phase B may have had their child
    // memos already populated (deltas consumed) — fire the recursive/
    // dependency rules for THOSE bounds with full child memos.
    if (nl.count > 0) {
        var a2: c_int = 0;
        while (a2 < var_.n_adorned) : (a2 += 1) {
            const ri = var_.adorned_rules.?[@intCast(a2)];
            if (cctx.rmeta.?[@intCast(ri)].has_idb_body == 0) continue;
            var out = std.mem.zeroes(tuple_set);
            if (fireRule(cctx, ri, &nl, -1, null, &out) != 0) {
                ts_free(&out);
                ts_free(&nl);
                ts_free(&nb);
                return -1;
            }
            if (mergeInto(cctx, V, &out) != 0) {
                ts_free(&out);
                ts_free(&nl);
                ts_free(&nb);
                return -1;
            }
            ts_free(&out);
        }
    }

    // Discovery: fire every magic rule (parent V) with guard = new bounds.
    var mi: c_int = 0;
    while (mi < var_.n_magic) : (mi += 1) {
        const ri = var_.magic_rules.?[@intCast(mi)];
        const Q = cctx.rmeta.?[@intCast(ri)].head_variant;
        var out = std.mem.zeroes(tuple_set);
        if (fireRule(cctx, ri, &nb, -1, null, &out) != 0) {
            ts_free(&out);
            ts_free(&nl); // (oracle omits this — leak on error path; freed for hygiene)
            ts_free(&nb);
            return -1;
        }
        var i: c_long = 0;
        while (i < out.count) : (i += 1) {
            const t = out.data.? + @as(usize, @intCast(i)) * @as(usize, out.arity);
            if (addBound(cctx, Q, t) != 0) {
                ts_free(&out);
                ts_free(&nl);
                ts_free(&nb);
                return -1;
            }
        }
        ts_free(&out);
    }

    ts_free(&nl);
    ts_free(&nb);
    return 0;
}

// ─── Phase B: semi-naive delta propagation ──────────────────────────────────

fn propagateDelta(cctx: *td_ctx, V: c_int) c_int {
    const var_ = &cctx.variants.?[@intCast(V)];
    if (var_.delta.count == 0) return 0;

    var cur = var_.delta; // take ownership
    var_.delta = std.mem.zeroes(tuple_set);
    var_.delta.arity = var_.arity; // keep arity: ts_add re-allocs data
    if (cur.arity > 0) ts_sort(&cur);

    const cv = &cctx.consumers.?[@intCast(V)];
    var k: c_int = 0;
    while (k < cv.n) : (k += 1) {
        const ri = cv.v.?[@intCast(k)].rule;
        const body = cv.v.?[@intCast(k)].body;
        const m = &cctx.rmeta.?[@intCast(ri)];
        const parent: c_int = if (m.is_magic != 0) m.guard_variant else m.head_variant;
        const pvar = &cctx.variants.?[@intCast(parent)];
        if (pvar.bound_set.count == 0) continue;

        var out = std.mem.zeroes(tuple_set);
        if (fireRule(cctx, ri, &pvar.bound_set, body, &cur, &out) != 0) {
            ts_free(&out);
            ts_free(&cur);
            return -1;
        }

        if (m.is_magic != 0) {
            const Q = m.head_variant;
            var i: c_long = 0;
            while (i < out.count) : (i += 1) {
                const t = out.data.? + @as(usize, @intCast(i)) * @as(usize, out.arity);
                if (addBound(cctx, Q, t) != 0) {
                    ts_free(&out);
                    ts_free(&cur);
                    return -1;
                }
            }
        } else {
            if (mergeInto(cctx, parent, &out) != 0) {
                ts_free(&out);
                ts_free(&cur);
                return -1;
            }
        }
        ts_free(&out);
    }

    ts_free(&cur);

    // self-recursion may have re-grown delta[V] during this pass
    if (var_.delta.count > 0) {
        if (enqueueProp(cctx, V) != 0) return -1;
    }
    return 0;
}

// ─── Driver loop ────────────────────────────────────────────────────────────

fn tdRun(cctx: *td_ctx) c_int {
    while (true) {
        // Phase A: drain the bound queue (discovery + base + late join).
        while (cctx.bound_head < cctx.bound_tail) {
            const V = cctx.bound_queue.?[@intCast(cctx.bound_head)];
            cctx.bound_head += 1;
            cctx.bound_pending.?[@intCast(V)] = 0;
            if (initVariant(cctx, V) != 0) return -1;
        }
        // Phase B: drain the prop queue (semi-naive delta propagation).
        while (cctx.prop_head < cctx.prop_tail) {
            const V = cctx.prop_queue.?[@intCast(cctx.prop_head)];
            cctx.prop_head += 1;
            cctx.prop_pending.?[@intCast(V)] = 0;
            cctx.in_phase_b = 1;
            if (propagateDelta(cctx, V) != 0) {
                cctx.in_phase_b = 0;
                return -1;
            }
            cctx.in_phase_b = 0;
        }
        if (cctx.bound_head >= cctx.bound_tail and cctx.prop_head >= cctx.prop_tail)
            break;
    }
    return 0;
}

// ─── Cleanup ────────────────────────────────────────────────────────────────

fn tdFree(cctx: *td_ctx) void {
    if (cctx.variants) |vars| {
        var i: c_int = 0;
        while (i < cctx.n_variants) : (i += 1) {
            const v = &vars[@intCast(i)];
            c.free(@ptrCast(v.adorned_rules));
            c.free(@ptrCast(v.magic_rules));
            ts_free(&v.bound_set);
            ts_free(&v.bound_new);
            ts_free(&v.bound_late);
            ts_free(&v.memo);
            ts_free(&v.delta);
        }
        c.free(@ptrCast(vars));
    }
    if (cctx.rmeta) |rm| {
        var i: c_int = 0;
        while (i < cctx.n_crules) : (i += 1) {
            c.free(@ptrCast(rm[@intCast(i)].idb_map));
            c.free(@ptrCast(rm[@intCast(i)].op_at));
            c.free(@ptrCast(rm[@intCast(i)].perm_at));
            c.free(@ptrCast(rm[@intCast(i)].rel_at));
        }
        c.free(@ptrCast(rm));
    }
    if (cctx.consumers) |cs| {
        var i: c_int = 0;
        while (i < cctx.n_variants) : (i += 1) c.free(@ptrCast(cs[@intCast(i)].v));
        c.free(@ptrCast(cs));
    }
    c.free(@ptrCast(cctx.bound_queue));
    c.free(@ptrCast(cctx.prop_queue));
    c.free(@ptrCast(cctx.bound_pending));
    c.free(@ptrCast(cctx.prop_pending));
}

// ─── Public entry ───────────────────────────────────────────────────────────

pub export fn td_eval(
    edb: ?*dx.dl_db,
    prog: ?*const magic.magic_program,
    crules: ?[*]?*compiled_rule,
    n_crules: c_int,
    goal_variant_id: c_int,
    bound: ?[*]const u32,
    cb: DlTupleCb,
    user: ?*anyopaque,
) c_long {
    var cctx = std.mem.zeroes(td_ctx);
    var result: c_long = -1;

    cctx.edb = edb;
    cctx.crules = crules;
    cctx.n_crules = n_crules;
    cctx.goal_variant = goal_variant_id;

    defer tdFree(&cctx);

    if (prog == null) return result;
    if (goal_variant_id < 0 or goal_variant_id >= @divTrunc(prog.?.n_decls, 2)) return result;
    if (bound == null or cb == null) return result;

    if (buildVariants(&cctx, prog.?) != 0) return result;
    if (buildRuleMeta(&cctx, prog.?) != 0) return result;
    if (buildConsumers(&cctx) != 0) return result;

    cctx.bound_pending = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(cctx.n_variants)), 1)));
    cctx.prop_pending = @ptrCast(@alignCast(c.calloc(@as(usize, @intCast(cctx.n_variants)), 1)));
    if (cctx.bound_pending == null or cctx.prop_pending == null) return result;

    // seed the goal subquery
    if (addBound(&cctx, goal_variant_id, bound.?) != 0) return result;

    if (tdRun(&cctx) != 0) return result;

    // stream the goal variant's memo (all its tuples match the single seed
    // bound; the per-position check is a defensive no-op)
    const gv = &cctx.variants.?[@intCast(goal_variant_id)];
    if (gv.memo_sorted == 0) {
        ts_sort(&gv.memo);
        gv.memo_sorted = 1;
    }
    result = 0;
    if (gv.memo.arity > 0) {
        var i: c_long = 0;
        while (i < gv.memo.count) : (i += 1) {
            const t = gv.memo.data.? + @as(usize, @intCast(i)) * @as(usize, gv.memo.arity);
            var ok: c_int = 1;
            var p: u8 = 0;
            while (p < gv.nbound) : (p += 1) {
                if (t[@as(usize, gv.bound_pos[@intCast(p)])] != bound.?[@intCast(p)]) {
                    ok = 0;
                    break;
                }
            }
            if (ok == 0) continue;
            if (cb.?(t, gv.memo.arity, user) != 0) break;
            result += 1;
        }
    }
    return result;
}

// ─── Tests ──────────────────────────────────────────────────────────────────

const testing = std.testing;

extern "c" fn system(cmd: [*c]const u8) c_int;

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
        const rc = dx.dl_add_fact(db, rel, cols.ptr + i * ar, ar);
        if (rc != 1) @panic("dl_add_fact failed");
    }
}

fn testCollectCb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ts: *tuple_set = @ptrCast(@alignCast(user orelse return 1));
    if (ts.arity == 0) {
        if (ts_init(ts, arity) != 0) return 1;
    }
    if (ts_add(ts, cols) < 0) return 1;
    return 0;
}

test "td_eval: cyclic TC matches the full-fixpoint magic oracle" {
    const path = "/tmp/datalog_zig_u10_topdown";
    const db = testOpenDb(path);
    defer testCloseDb(db, path);

    _ = dx.dl_declare_relation(db, "edge", 2);
    testAddFacts(db, "edge", &.{ 1, 2, 2, 3, 3, 4, 4, 5, 5, 1 }, 2, 5);
    _ = dx.dl_load_rules(db, "tc(X,Y):-edge(X,Y).\ntc(X,Y):-edge(X,Z),tc(Z,Y).\n");

    var a = std.mem.zeroes(tuple_set);
    var b = std.mem.zeroes(tuple_set);
    const vals = [1]u32{1};
    const rc1 = dx.dl_query_topdown_adorn(db, "tc", "bf", @ptrCast(&vals), 1, testCollectCb, &a);
    const rc2 = dx.dl_query_magic_adorn(db, "tc", "bf", @ptrCast(&vals), 1, testCollectCb, &b);
    try testing.expect(rc1 > 0);
    try testing.expectEqual(rc2, rc1);
    ts_sort(&a);
    ts_sort(&b);
    try testing.expectEqual(@as(c_long, a.count), b.count);
    if (a.count > 0) {
        const n = @as(usize, @intCast(a.count)) * @as(usize, a.arity);
        try testing.expect(std.mem.eql(u32, a.data.?[0..n], b.data.?[0..n]));
    }
    ts_free(&a);
    ts_free(&b);
}
