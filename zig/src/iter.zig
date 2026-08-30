//! iter.zig — port of src/iter.c (pull-based sorted iterator + merge-join).
//!
//! Replaces the callback fire-and-forget enumeration (rel_prefix's
//! prefix_dfs / view_prefix's view_enum_dfs) with a RESUMABLE cursor.
//!
//! Strangler-hybrid ABI: `struct dl_iter` is OPAQUE in dl.h (typedef fwd),
//! so the implementation is a native Zig struct — C callers only ever hold
//! `dl_iter *`.  `dl_db`/`rel_entry` are EXPOSED still-C structs consumed
//! via @cImport("dl_internal.h"); dafsa/dafsa_view internals via
//! @cImport("dafsa_internal.h") (same translate-c workarounds as
//! relation.zig: local trans_arr_c).  relation.zig provides rel_arity/
//! rel_dafsa; snapshot.zig provides manifest_find_rel_ex.
//!
//! Oracle: src/iter.c (never modified).

const std = @import("std");
const c = std.c;

const relation = @import("relation.zig");
const snapshot = @import("snapshot.zig");

// dl_internal.h: dl_db, rel_entry, RELK_VARIADIC.
const dx = @cImport({
    @cInclude("dl_internal.h");
});

// dafsa_internal.h: struct dafsa/State/Edge, trans_find, dafsa_view,
// view_trans_find, view_edge_next, dafsa_view_open/close.
const dc = @cImport({
    @cInclude("dafsa_internal.h");
});

extern "c" fn snprintf(buf: [*c]u8, size: usize, fmt: [*c]const u8, ...) c_int;

const MAX_ARITY: usize = 8;
const MAX_KEY_LEN: usize = MAX_ARITY * 4 + 1; // 33
const DL_ITER_MAX_FRAMES: usize = MAX_KEY_LEN + 1; // 34

const DL_ITER_LIVE: u8 = 1;
const DL_ITER_VIEW: u8 = 2;

/// One DFS frame. `state` = DAFSA state entered at this level; the resume
/// position within that state's outgoing edges is next_edge (LIVE) or cursor
/// (VIEW); the unused one is ignored per `kind`.
const Frame = struct {
    state: u32,
    next_edge: u32, // LIVE only
    cursor: [*c]const u8, // VIEW only
};

/// struct dl_iter — opaque to C; native Zig layout.
pub const DlIter = struct {
    kind: u8, // DL_ITER_LIVE | DL_ITER_VIEW
    arity: u8,
    k: u8, // bound leading-column count
    exhausted: u8, // 1 once past the last tuple
    nframes: u8, // frames in use (>=1 while positioned)

    leading: [MAX_ARITY]u32,

    // Explicit DFS stack. buf[i] = sym on edge frames[i]->frames[i+1]. At a
    // final state depth = nframes-1 = (arity-k)*4 + 1: first (arity-k)*4
    // bytes of buf are the remaining columns (u32BE), buf[depth-1] is the
    // trailing \0.
    frames: [DL_ITER_MAX_FRAMES]Frame,
    buf: [MAX_KEY_LEN]u8,

    d: ?*const dc.dafsa, // LIVE: rel->d (borrowed; db owns it)
    v: ?*dc.dafsa_view, // VIEW: owned here; closed in dl_iter_close
};

// ─── helpers ──────────────────────────────────────────────────────────────

fn strEq(a: [*c]const u8, b: [*c]const u8) bool {
    var i: usize = 0;
    while (a[i] != 0 and a[i] == b[i]) i += 1;
    return a[i] == b[i];
}

/// Local relation lookup duplicating dl.c's static find_rel (keeps dl.c
/// untouched).
fn iter_find_rel(db: [*c]const dx.dl_db, name: [*c]const u8) c_int {
    // NB: unwrap to a single-item pointer before field+subscript —
    // `p.*.rels[i]` on a [*c] misparses (subscript swallowed by the field).
    if (db == null) return -1;
    const d: *const dx.dl_db = @ptrCast(db);
    var i: usize = 0;
    while (i < d.nrels) : (i += 1) {
        if (strEq(d.rels[i].name, name))
            return @intCast(i);
    }
    return -1;
}

/// Read `arity` u32 columns from a big-endian buffer (mirrors relation.c).
fn read_cols_be(cols: [*]u32, buf: [*]const u8, arity: u8) void {
    var i: u8 = 0;
    while (i < arity) : (i += 1) {
        cols[i] = (@as(u32, buf[4 * @as(usize, i)]) << 24) |
            (@as(u32, buf[4 * @as(usize, i) + 1]) << 16) |
            (@as(u32, buf[4 * @as(usize, i) + 2]) << 8) |
            (@as(u32, buf[4 * @as(usize, i) + 3]));
    }
}

/// trans_arr_c from dafsa_internal.h, reimplemented locally (same
/// translate-c workaround as relation.zig):
///   s->trans_heap ? s->trans_heap->edges : s->trans
fn transArrC(s: [*c]const dc.State) [*c]const dc.Edge {
    const heap = s.*.trans_heap;
    if (heap != null) {
        return @ptrCast(@alignCast(&heap.*._edges));
    }
    return @ptrCast(&s.*.trans);
}

/// Walk the k*4-byte prefix of it->leading from the DAFSA root, returning the
/// state index representing that bound, or -1 if the prefix is absent.
fn iter_walk_prefix(it: *const DlIter) c_int {
    var current: c_uint = undefined;

    if (it.kind == DL_ITER_LIVE) {
        current = it.d.?.initial;
        var i: u8 = 0;
        while (i < it.k) : (i += 1) {
            const v = it.leading[i];
            const col_be = [4]u8{
                @truncate(v >> 24),
                @truncate(v >> 16),
                @truncate(v >> 8),
                @truncate(v),
            };
            var b: usize = 0;
            while (b < 4) : (b += 1) {
                const tr = dc.trans_find(&it.d.?.states[current], col_be[b]);
                if (tr < 0) return -1;
                current = (transArrC(&it.d.?.states[current]) + @as(usize, @intCast(tr))).*.target;
            }
        }
    } else {
        current = it.v.?.initial;
        var i: u8 = 0;
        while (i < it.k) : (i += 1) {
            const v = it.leading[i];
            const col_be = [4]u8{
                @truncate(v >> 24),
                @truncate(v >> 16),
                @truncate(v >> 8),
                @truncate(v),
            };
            var b: usize = 0;
            while (b < 4) : (b += 1) {
                var target: c_uint = undefined;
                if (dc.view_trans_find(it.v, current, col_be[b], &target) != 0)
                    return -1;
                current = target;
            }
        }
    }
    return @intCast(current);
}

/// Reset the DFS stack to start from `root` (a DAFSA state, or -1 for an
/// absent prefix -> a VALID EMPTY iterator).
fn iter_reset_stack(it: *DlIter, root: c_int) void {
    it.exhausted = 0;
    if (root < 0) {
        it.exhausted = 1;
        it.nframes = 0;
        return;
    }
    it.nframes = 1;
    it.frames[0].state = @intCast(root);
    if (it.kind == DL_ITER_LIVE) {
        it.frames[0].next_edge = 0;
    } else {
        it.frames[0].cursor = it.v.?.csr + @as(usize, @intCast(it.v.?.state_off[@intCast(root)]));
    }
}

/// 1 iff the frame's state is a final (accepting) state.
fn iter_is_final(it: *const DlIter, f: *const Frame) bool {
    if (it.kind == DL_ITER_LIVE)
        return it.d.?.states[f.state].is_final != 0;
    return (it.v.?.final_bits[f.state / 8] &
        (@as(u8, 1) << @intCast(f.state % 8))) != 0;
}

// ─── public API ───────────────────────────────────────────────────────────

/// dl_iter *dl_iter_open(dl_db *db, const char *rel,
///                       const uint32_t *leading, uint8_t k)
pub export fn dl_iter_open(db: ?*dx.dl_db, rel: [*c]const u8, leading: ?[*]const u32, k: u8) ?*DlIter {
    const d0 = db orelse return null;
    if (d0.dir == null or rel == null) return null;
    const mem = c.calloc(1, @sizeOf(DlIter)) orelse return null;
    const it: *DlIter = @ptrCast(@alignCast(mem));
    it.* = std.mem.zeroes(DlIter);

    if (d0.snap_version > 0) {
        // Snapshot path: OWN the mmap view (NOT view_open_cached — the vcache
        // is LRU-evicted and fully invalidated by dl_publish_snapshot, which
        // would dangle a long-lived cursor across iter_next calls).
        var path: [8192]u8 = undefined;
        var arity: u8 = 0;
        var variadic: c_int = 0;
        _ = snprintf(&path, path.len, "%s/snapshots/%u", d0.dir, d0.snap_version);
        if (snapshot.manifest_find_rel_ex(&path, rel, &arity, &variadic) == 0) {
            c.free(mem);
            return null; // unknown rel in snapshot
        }
        if (variadic != 0) {
            c.free(mem);
            return null; // variadic: rejected
        }
        if (k > arity) {
            c.free(mem);
            return null;
        }
        if (k > 0 and leading == null) {
            c.free(mem);
            return null;
        }
        _ = snprintf(&path, path.len, "%s/snapshots/%u/%s.dafsa", d0.dir, d0.snap_version, rel);
        it.v = dc.dafsa_view_open(&path);
        if (it.v == null) {
            c.free(mem);
            return null;
        }
        it.kind = DL_ITER_VIEW;
        it.arity = arity;
    } else {
        const idx = iter_find_rel(d0, rel);
        if (idx < 0) {
            c.free(mem);
            return null; // unknown rel
        }
        const e = &d0.rels[@intCast(idx)];
        if (e.*.kind == dx.RELK_VARIADIC) {
            c.free(mem);
            return null; // rejected
        }
        if (k > e.*.arity) {
            c.free(mem);
            return null;
        }
        if (k > 0 and leading == null) {
            c.free(mem);
            return null;
        }
        // e->rel is the @cImport's opaque relation*; rel_dafsa (relation.zig)
        // takes the native struct — cast across the namespaces, then cast the
        // returned dafsa pointer back into this module's @cImport namespace.
        const d_c = relation.rel_dafsa(@ptrCast(@alignCast(e.*.rel)));
        it.d = @ptrCast(d_c); // borrow rel->d (VIEW)
        if (it.d == null) {
            c.free(mem);
            return null;
        }
        it.kind = DL_ITER_LIVE;
        it.arity = e.*.arity;
    }

    it.k = k;
    if (k > 0) {
        var i: u8 = 0;
        while (i < k) : (i += 1) it.leading[i] = leading.?[i];
    }
    iter_reset_stack(it, iter_walk_prefix(it));
    return it;
}

/// LIVE-mode open over an already-resolved relation.  Borrows rel->d and
/// NEVER routes to the snapshot view.  The VM's OP_RANGE must read LIVE:
/// vm_execute materializes rel->d in place even when snap_version > 0
/// (re-publish), and reading the mmap'd snapshot of a PREVIOUS version would
/// silently mis-evaluate.  Mirrors dl_iter_open's LIVE branch.  Returns NULL
/// on NULL rel / empty dafsa / k > arity / k > 0 && !leading / OOM.
pub export fn dl_iter_open_live(rel: ?*relation.Relation, leading: ?[*]const u32, k: u8) ?*DlIter {
    if (rel == null) return null;
    const arity = relation.rel_arity(rel);
    if (k > arity) return null;
    if (k > 0 and leading == null) return null;

    const mem = c.calloc(1, @sizeOf(DlIter)) orelse return null;
    const it: *DlIter = @ptrCast(@alignCast(mem));
    it.* = std.mem.zeroes(DlIter);

    it.kind = DL_ITER_LIVE;
    it.arity = arity;
    const d_c = relation.rel_dafsa(rel); // borrow rel->d (db owns it)
    it.d = @ptrCast(d_c);
    if (it.d == null) {
        c.free(mem);
        return null;
    }

    it.k = k;
    if (k > 0) {
        var i: u8 = 0;
        while (i < k) : (i += 1) it.leading[i] = leading.?[i];
    }
    iter_reset_stack(it, iter_walk_prefix(it));
    return it;
}

/// int dl_iter_seek(dl_iter *it, const uint32_t *leading, uint8_t k)
pub export fn dl_iter_seek(it: ?*DlIter, leading: ?[*]const u32, k: u8) c_int {
    const itr = it orelse return -1;
    if (k > itr.arity) return -1;
    if (k > 0 and leading == null) return -1;

    itr.k = k;
    if (k > 0) {
        var i: u8 = 0;
        while (i < k) : (i += 1) itr.leading[i] = leading.?[i];
    }
    iter_reset_stack(itr, iter_walk_prefix(itr));
    return 0;
}

/// int dl_iter_next(dl_iter *it, uint32_t *cols_out) — emit-then-backtrack
/// DFS: one tuple per call, ascending key order.
pub export fn dl_iter_next(it: ?*DlIter, cols_out: ?[*]u32) c_int {
    const itr = it orelse return -1;
    if (cols_out == null) return -1;
    if (itr.exhausted != 0) return 0;

    const n_rem: u8 = itr.arity - itr.k;

    while (true) {
        const top = &itr.frames[itr.nframes - 1];

        if (iter_is_final(itr, top)) {
            // Emit: leading cols ++ remaining cols decoded from buf, then
            // backtrack (pop the final frame).
            if (itr.k > 0) {
                var i: u8 = 0;
                while (i < itr.k) : (i += 1) cols_out.?[i] = itr.leading[i];
            }
            if (n_rem > 0) read_cols_be(cols_out.? + itr.k, &itr.buf, n_rem);
            itr.nframes -= 1;
            if (itr.nframes == 0) itr.exhausted = 1;
            return 1;
        }

        if (itr.nframes >= DL_ITER_MAX_FRAMES) return -1; // depth overflow

        // Descend the next outgoing edge (in sorted-symbol order).
        var sym: u8 = undefined;
        var tgt: c_uint = undefined;

        if (itr.kind == DL_ITER_LIVE) {
            const s = &itr.d.?.states[top.state];
            if (top.next_edge >= s.*.ntrans) {
                // no more edges: backtrack
                itr.nframes -= 1;
                if (itr.nframes == 0) {
                    itr.exhausted = 1;
                    return 0;
                }
                continue;
            }
            const e = transArrC(s) + top.next_edge;
            top.next_edge += 1;
            sym = e.*.sym;
            tgt = e.*.target;
        } else {
            if (dc.view_edge_next(itr.v, top.state, &top.cursor, &sym, &tgt) != 0) {
                // no more edges: backtrack
                itr.nframes -= 1;
                if (itr.nframes == 0) {
                    itr.exhausted = 1;
                    return 0;
                }
                continue;
            }
        }

        itr.buf[itr.nframes - 1] = sym; // buf[i]=sym on frames[i]->[i+1]
        itr.frames[itr.nframes].state = @intCast(tgt);
        if (itr.kind == DL_ITER_LIVE) {
            itr.frames[itr.nframes].next_edge = 0;
        } else {
            itr.frames[itr.nframes].cursor = itr.v.?.csr + @as(usize, @intCast(itr.v.?.state_off[@intCast(tgt)]));
        }
        itr.nframes += 1;
    }
}

/// uint8_t dl_iter_arity(const dl_iter *it)
pub export fn dl_iter_arity(it: ?*const DlIter) u8 {
    const itr = it orelse return 0;
    return itr.arity;
}

/// void dl_iter_close(dl_iter *it)
pub export fn dl_iter_close(it: ?*DlIter) void {
    const itr = it orelse return;
    if (itr.kind == DL_ITER_VIEW and itr.v != null)
        dc.dafsa_view_close(itr.v);
    c.free(itr);
}

// ─── merge-join ───────────────────────────────────────────────────────────

fn mj_keys_equal(a: [*]const u32, b: [*]const u32, j: u8) bool {
    var i: u8 = 0;
    while (i < j) : (i += 1) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

/// long dl_merge_join(dl_iter *l, dl_iter *r, uint8_t jcols,
///                    dl_join_cb cb, void *user)
pub export fn dl_merge_join(l: ?*DlIter, r: ?*DlIter, jcols: u8, cb: dx.dl_join_cb, user: ?*anyopaque) c_long {
    var lt: [MAX_ARITY]u32 = undefined;
    var rt: [MAX_ARITY]u32 = undefined;
    var rbuf: ?[*]u32 = null;
    var rcap: usize = 0;
    var rcnt: usize = 0;
    var emitted: c_long = 0;

    const li0 = l orelse return -1;
    const ri0 = r orelse return -1;
    if (cb == null) return -1;
    const la = li0.arity;
    const ra = ri0.arity;
    if (jcols == 0 or jcols > la or jcols > ra) return -1;

    var lok = dl_iter_next(l, &lt);
    var rok = dl_iter_next(r, &rt);
    if (lok < 0 or rok < 0) {
        c.free(@ptrCast(rbuf));
        return -1;
    }

    while (lok != 0 and rok != 0) {
        var cmp: c_int = 0;
        var j: u8 = 0;
        while (j < jcols) : (j += 1) {
            if (lt[j] < rt[j]) {
                cmp = -1;
                break;
            }
            if (lt[j] > rt[j]) {
                cmp = 1;
                break;
            }
        }

        if (cmp < 0) {
            lok = dl_iter_next(l, &lt);
            if (lok < 0) {
                c.free(@ptrCast(rbuf));
                return -1;
            }
            continue;
        }
        if (cmp > 0) {
            rok = dl_iter_next(r, &rt);
            if (rok < 0) {
                c.free(@ptrCast(rbuf));
                return -1;
            }
            continue;
        }

        // Keys equal: buffer the whole right run, then cross it with the left
        // run.  Both runs advance in sorted order so output stays sorted.
        rcnt = 0;
        while (true) {
            if (rcnt == rcap) {
                const nc = if (rcap != 0) rcap * 2 else 64;
                const nb = c.realloc(if (rbuf) |rb| rb else null, nc * @as(usize, ra) * @sizeOf(u32));
                if (nb == null) {
                    c.free(@ptrCast(rbuf));
                    return -1;
                }
                rbuf = @ptrCast(@alignCast(nb));
                rcap = nc;
            }
            const dst = rbuf.? + rcnt * @as(usize, ra);
            var m: u8 = 0;
            while (m < ra) : (m += 1) dst[m] = rt[m];
            rcnt += 1;
            rok = dl_iter_next(r, &rt);
            if (rok < 0) {
                c.free(@ptrCast(rbuf));
                return -1;
            }
            if (!(rok != 0 and mj_keys_equal(&rt, rbuf.?, jcols))) break;
        }

        while (true) {
            var i: usize = 0;
            while (i < rcnt) : (i += 1) {
                emitted += 1;
                if (cb.?(&lt, la, rbuf.? + i * @as(usize, ra), ra, user) != 0) {
                    c.free(@ptrCast(rbuf));
                    return emitted; // early stop
                }
            }
            lok = dl_iter_next(l, &lt);
            if (lok < 0) {
                c.free(@ptrCast(rbuf));
                return -1;
            }
            if (!(lok != 0 and mj_keys_equal(&lt, rbuf.?, jcols))) break;
        }
    }

    // Documented contract: both iterators are left exhausted.
    while (lok != 0) {
        lok = dl_iter_next(l, &lt);
        if (lok < 0) {
            c.free(@ptrCast(rbuf));
            return -1;
        }
    }
    while (rok != 0) {
        rok = dl_iter_next(r, &rt);
        if (rok < 0) {
            c.free(@ptrCast(rbuf));
            return -1;
        }
    }

    c.free(@ptrCast(rbuf));
    return emitted;
}

// ─── tests ────────────────────────────────────────────────────────────────

const testing = std.testing;

const MjCtx = struct {
    pairs: [64][2][2]u32 = undefined,
    n: usize = 0,
    stop_after: c_int = 0, // 0 = never stop early

    fn cb(lcols: [*c]const u32, la: u8, rcols: [*c]const u32, ra: u8, user: ?*anyopaque) callconv(.c) c_int {
        _ = la;
        _ = ra;
        const self: *MjCtx = @ptrCast(@alignCast(user.?));
        self.pairs[self.n][0][0] = lcols[0];
        self.pairs[self.n][0][1] = lcols[1];
        self.pairs[self.n][1][0] = rcols[0];
        self.pairs[self.n][1][1] = rcols[1];
        self.n += 1;
        if (self.stop_after != 0 and self.n == @as(usize, @intCast(self.stop_after))) return 1;
        return 0;
    }
};

/// {(1,2),(1,3),(2,4)} — sorted, duplicate-free base for iterator tests.
fn makeTestRel() ?*relation.Relation {
    const rel = relation.rel_create(2) orelse return null;
    const t1 = [2]u32{ 1, 2 };
    const t2 = [2]u32{ 1, 3 };
    const t3 = [2]u32{ 2, 4 };
    if (relation.rel_add(rel, &t1) < 0) return null;
    if (relation.rel_add(rel, &t2) < 0) return null;
    if (relation.rel_add(rel, &t3) < 0) return null;
    return rel;
}

test "dl_iter_open_live open/seek/next/arity/close" {
    const rel = makeTestRel() orelse return error.TestUnexpectedResult;

    // full enumeration
    const it = dl_iter_open_live(rel, null, 0) orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(u8, 2), dl_iter_arity(it));
    var out: [2]u32 = undefined;
    try testing.expectEqual(@as(c_int, 1), dl_iter_next(it, &out));
    try testing.expectEqual(@as(u32, 1), out[0]);
    try testing.expectEqual(@as(u32, 2), out[1]);
    try testing.expectEqual(@as(c_int, 1), dl_iter_next(it, &out));
    try testing.expectEqual(@as(u32, 3), out[1]);
    try testing.expectEqual(@as(c_int, 1), dl_iter_next(it, &out));
    try testing.expectEqual(@as(u32, 2), out[0]);
    try testing.expectEqual(@as(u32, 4), out[1]);
    try testing.expectEqual(@as(c_int, 0), dl_iter_next(it, &out)); // exhausted
    try testing.expectEqual(@as(c_int, 0), dl_iter_next(it, &out)); // stays exhausted
    dl_iter_close(it);

    // re-seek onto an existing prefix mid-iterator
    const it2 = dl_iter_open_live(rel, null, 0) orelse return error.TestUnexpectedResult;
    defer dl_iter_close(it2);
    _ = dl_iter_next(it2, &out);
    const lead1 = [1]u32{2};
    try testing.expectEqual(@as(c_int, 0), dl_iter_seek(it2, &lead1, 1));
    try testing.expectEqual(@as(c_int, 1), dl_iter_next(it2, &out));
    try testing.expectEqual(@as(u32, 4), out[1]);
    try testing.expectEqual(@as(c_int, 0), dl_iter_next(it2, &out));

    // seek to an ABSENT prefix: valid empty iterator
    const lead9 = [1]u32{9};
    try testing.expectEqual(@as(c_int, 0), dl_iter_seek(it2, &lead9, 1));
    try testing.expectEqual(@as(c_int, 0), dl_iter_next(it2, &out));

    // bound open k=1 leading=[1] -> (1,2),(1,3)
    const leadb = [1]u32{1};
    const it3 = dl_iter_open_live(rel, &leadb, 1) orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(c_int, 1), dl_iter_next(it3, &out));
    try testing.expectEqual(@as(u32, 1), out[0]); // leading col echoed
    try testing.expectEqual(@as(u32, 2), out[1]);
    try testing.expectEqual(@as(c_int, 1), dl_iter_next(it3, &out));
    try testing.expectEqual(@as(u32, 3), out[1]);
    try testing.expectEqual(@as(c_int, 0), dl_iter_next(it3, &out));
    dl_iter_close(it3);

    // error paths
    try testing.expect(dl_iter_open_live(null, null, 0) == null);
    try testing.expect(dl_iter_open_live(rel, null, 3) == null); // k > arity
    try testing.expect(dl_iter_open_live(rel, null, 1) == null); // k > 0 && !leading
    try testing.expectEqual(@as(c_int, -1), dl_iter_seek(null, null, 0));
    try testing.expectEqual(@as(u8, 0), dl_iter_arity(null));
    dl_iter_close(null); // NULL-safe
}

test "dl_iter_open live branch over a dl_db" {
    var db = std.mem.zeroes(dx.dl_db);
    var dir_buf = "/tmp/datalog_zig_u7_iterdir".*;
    var name_buf = "edge".*;

    const rel = makeTestRel() orelse return error.TestUnexpectedResult;
    db.dir = @ptrCast(&dir_buf);
    db.rels[0].name = @ptrCast(&name_buf);
    db.rels[0].kind = dx.RELK_FIXED;
    db.rels[0].arity = 2;
    db.rels[0].rel = @ptrCast(rel);
    db.nrels = 1;
    db.snap_version = 0; // LIVE path

    // unknown relation
    var other_buf = "nope".*;
    try testing.expect(dl_iter_open(&db, @ptrCast(&other_buf), null, 0) == null);

    // variadic rejected (a second entry with a different name, so the fixed
    // "edge" at index 0 stays the first match for its own lookup)
    var vary_buf = "edgy".*;
    db.rels[1].name = @ptrCast(&vary_buf);
    db.rels[1].kind = dx.RELK_VARIADIC;
    db.rels[1].arity = 0;
    db.rels[1].vrel = null;
    db.rels[1].rel = null;
    db.nrels = 2;
    try testing.expect(dl_iter_open(&db, @ptrCast(&vary_buf), null, 0) == null);

    // fixed relation: full walk (index 0 wins the "edge" lookup)
    const it = dl_iter_open(&db, @ptrCast(&name_buf), null, 0) orelse return error.TestUnexpectedResult;
    defer dl_iter_close(it);
    try testing.expectEqual(@as(u8, 2), dl_iter_arity(it));
    var out: [2]u32 = undefined;
    try testing.expectEqual(@as(c_int, 1), dl_iter_next(it, &out));
    try testing.expectEqual(@as(u32, 1), out[0]);
    try testing.expectEqual(@as(u32, 2), out[1]);

    // k > arity rejected
    try testing.expect(dl_iter_open(&db, @ptrCast(&name_buf), null, 3) == null);

    // NULL db / NULL rel
    try testing.expect(dl_iter_open(null, @ptrCast(&name_buf), null, 0) == null);
    try testing.expect(dl_iter_open(&db, null, null, 0) == null);
}

test "dl_merge_join cross-product + early stop + exhaustion" {
    // l: {(1,2),(1,3),(2,4)}  r: {(1,10),(1,11),(3,5)}  join on col0
    const l = makeTestRel() orelse return error.TestUnexpectedResult;
    const r = relation.rel_create(2) orelse return error.TestUnexpectedResult;
    const r1 = [2]u32{ 1, 10 };
    const r2 = [2]u32{ 1, 11 };
    const r3 = [2]u32{ 3, 5 };
    try testing.expect(relation.rel_add(r, &r1) > 0); // 1 = inserted
    try testing.expect(relation.rel_add(r, &r2) > 0);
    try testing.expect(relation.rel_add(r, &r3) > 0);

    // full cross-product: (1,2)x{(1,10),(1,11)} + (1,3)x{(1,10),(1,11)} = 4
    var ctx = MjCtx{};
    const li = dl_iter_open_live(l, null, 0) orelse return error.TestUnexpectedResult;
    const ri = dl_iter_open_live(r, null, 0) orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(c_long, 4), dl_merge_join(li, ri, 1, MjCtx.cb, &ctx));
    try testing.expectEqual(@as(usize, 4), ctx.n);
    // emission order: outer loop = left rows, inner = buffered right rows
    try testing.expectEqual(@as(u32, 2), ctx.pairs[0][0][1]); // (1,2)x(1,10)
    try testing.expectEqual(@as(u32, 10), ctx.pairs[0][1][1]);
    try testing.expectEqual(@as(u32, 2), ctx.pairs[1][0][1]); // (1,2)x(1,11)
    try testing.expectEqual(@as(u32, 11), ctx.pairs[1][1][1]);
    try testing.expectEqual(@as(u32, 3), ctx.pairs[2][0][1]); // (1,3)x(1,10)
    try testing.expectEqual(@as(u32, 10), ctx.pairs[2][1][1]);
    try testing.expectEqual(@as(u32, 3), ctx.pairs[3][0][1]); // (1,3)x(1,11)
    try testing.expectEqual(@as(u32, 11), ctx.pairs[3][1][1]);
    // both iterators left exhausted by the documented contract
    var out: [2]u32 = undefined;
    try testing.expectEqual(@as(c_int, 0), dl_iter_next(li, &out));
    try testing.expectEqual(@as(c_int, 0), dl_iter_next(ri, &out));
    dl_iter_close(li);
    dl_iter_close(ri);

    // early stop on the first pair -> returns 1 emitted
    var ctx2 = MjCtx{ .stop_after = 1 };
    const li2 = dl_iter_open_live(l, null, 0) orelse return error.TestUnexpectedResult;
    const ri2 = dl_iter_open_live(r, null, 0) orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(c_long, 1), dl_merge_join(li2, ri2, 1, MjCtx.cb, &ctx2));
    dl_iter_close(li2);
    dl_iter_close(ri2);

    // no overlap -> 0
    const r_hi = relation.rel_create(2) orelse return error.TestUnexpectedResult;
    const rh1 = [2]u32{ 7, 1 };
    try testing.expect(relation.rel_add(r_hi, &rh1) > 0); // 1 = inserted
    const li3 = dl_iter_open_live(l, null, 0) orelse return error.TestUnexpectedResult;
    const ri3 = dl_iter_open_live(r_hi, null, 0) orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(c_long, 0), dl_merge_join(li3, ri3, 1, MjCtx.cb, &ctx));
    dl_iter_close(li3);
    dl_iter_close(ri3);

    // bad args
    const li4 = dl_iter_open_live(l, null, 0) orelse return error.TestUnexpectedResult;
    const ri4 = dl_iter_open_live(r, null, 0) orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(c_long, -1), dl_merge_join(null, ri4, 1, MjCtx.cb, &ctx));
    try testing.expectEqual(@as(c_long, -1), dl_merge_join(li4, null, 1, MjCtx.cb, &ctx));
    try testing.expectEqual(@as(c_long, -1), dl_merge_join(li4, ri4, 0, MjCtx.cb, &ctx));
    try testing.expectEqual(@as(c_long, -1), dl_merge_join(li4, ri4, 3, MjCtx.cb, &ctx));
    try testing.expectEqual(@as(c_long, -1), dl_merge_join(li4, ri4, 1, null, &ctx));
    dl_iter_close(li4);
    dl_iter_close(ri4);
}
