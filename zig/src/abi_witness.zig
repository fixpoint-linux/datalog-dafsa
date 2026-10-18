//! abi_witness.zig — the comptime oracle for dl_abi.zig.
//!
//! Exactly ONE @cImport of src/dl_internal.h (and its transitive include
//! graph) pins every hand-declared struct in dl_abi.zig to the real header:
//! @sizeOf/@alignOf must match, and every field's @offsetOf and @typeInfo
//! must match.  A wrong field type in dl_abi.zig is silent memory corruption
//! (not a compile error) — this witness is what turns it into a compile
//! error.  It is imported ONLY from tests.zig, so the one @cImport never
//! enters the lib/exe module graphs.

const std = @import("std");
const abi = @import("dl_abi.zig");

const ch = @cImport({
    @cInclude("dl_internal.h");
});

/// Structural type comparison via @typeInfo.  `.@"struct"`/`.@"opaque"`/
/// `.@"union"`/`.@"enum"` are compared by KIND only (their layouts are
/// verified separately by the @sizeOf/@offsetOf checks at the top level), so
/// two nominally-distinct opaque handle types (dl_abi.relation vs the
/// @cImport's relation) and self-referential structs compare without infinite
/// recursion.
fn typesMatch(a: type, b: type) bool {
    @setEvalBranchQuota(100_000);
    const A = @typeInfo(a);
    const B = @typeInfo(b);
    if (std.meta.activeTag(A) != std.meta.activeTag(B)) return false;
    switch (A) {
        .type, .void, .bool, .noreturn, .comptime_float, .comptime_int, .undefined, .null, .enum_literal => return true,
        .int => |i| return i.signedness == B.int.signedness and i.bits == B.int.bits,
        .float => |f| return f.bits == B.float.bits,
        .pointer => |p| {
            const q = B.pointer;
            if (p.size != q.size) return false;
            if (p.is_const != q.is_const) return false;
            if (p.is_volatile != q.is_volatile) return false;
            if (p.address_space != q.address_space) return false;
            if (p.is_allowzero != q.is_allowzero) return false;
            if ((p.sentinel_ptr == null) != (q.sentinel_ptr == null)) return false;
            return typesMatch(p.child, q.child);
        },
        .array => |ar| return ar.len == B.array.len and typesMatch(ar.child, B.array.child),
        .optional => |o| return typesMatch(o.child, B.optional.child),
        .@"fn" => |f| {
            const g = B.@"fn";
            if (std.meta.activeTag(f.calling_convention) != std.meta.activeTag(g.calling_convention)) return false;
            if (f.is_generic != g.is_generic) return false;
            if (f.is_var_args != g.is_var_args) return false;
            const fr = f.return_type orelse return false;
            const gr = g.return_type orelse return false;
            if (!typesMatch(fr, gr)) return false;
            if (f.params.len != g.params.len) return false;
            for (f.params, g.params) |fp, gp| {
                if (fp.is_noalias != gp.is_noalias) return false;
                const fpt = fp.type orelse return false;
                const gpt = gp.type orelse return false;
                if (!typesMatch(fpt, gpt)) return false;
            }
            return true;
        },
        .@"struct", .@"union", .@"enum", .@"opaque" => return true,
        else => return false,
    }
}

/// Verify a hand-declared extern struct against its @cImport twin.
fn checkStruct(comptime mine: type, comptime theirs: type) void {
    if (@sizeOf(mine) != @sizeOf(theirs))
        @compileError("abi_witness: size mismatch for " ++ @typeName(mine));
    if (@alignOf(mine) != @alignOf(theirs))
        @compileError("abi_witness: align mismatch for " ++ @typeName(mine));
    const mf = @typeInfo(mine).@"struct".fields;
    const tf = @typeInfo(theirs).@"struct".fields;
    if (mf.len != tf.len)
        @compileError("abi_witness: field count mismatch for " ++ @typeName(mine));
    inline for (mf, tf) |ma, ta| {
        if (!std.mem.eql(u8, ma.name, ta.name))
            @compileError("abi_witness: field name mismatch " ++ @typeName(mine) ++ "." ++ ma.name ++ " vs " ++ ta.name);
        if (@offsetOf(mine, ma.name) != @offsetOf(theirs, ta.name))
            @compileError("abi_witness: offset mismatch " ++ @typeName(mine) ++ "." ++ ma.name);
        if (!typesMatch(ma.type, ta.type))
            @compileError("abi_witness: field type mismatch " ++ @typeName(mine) ++ "." ++ ma.name);
    }
}

comptime {
    checkStruct(abi.struct_dl_db, ch.struct_dl_db);
    checkStruct(abi.struct_txn, ch.struct_txn);
    checkStruct(abi.struct_txn_op, ch.struct_txn_op);
    checkStruct(abi.rel_entry, ch.rel_entry);
    checkStruct(abi.view_cache_slot, ch.view_cache_slot);
    checkStruct(abi.perm_index_entry, ch.perm_index_entry);
    checkStruct(abi.compiled_rule, ch.compiled_rule);
    checkStruct(abi.vm_instr, ch.vm_instr);
    checkStruct(abi.var_info, ch.var_info);
    checkStruct(abi.atom, ch.atom);
    checkStruct(abi.rule, ch.rule);
    checkStruct(abi.struct_token, ch.struct_token);
    checkStruct(abi.struct_expr, ch.struct_expr);
    checkStruct(abi.struct_regex_dfa, ch.struct_regex_dfa);
    checkStruct(abi.struct_sym_set, ch.struct_sym_set);
    checkStruct(abi.struct_dl_vec_corpus, ch.struct_dl_vec_corpus);
}
