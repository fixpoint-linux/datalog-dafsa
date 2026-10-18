//! dl_abi.zig — hand-declared C ABI surface of the 100%-Zig datalog engine.
//!
//! OUT-OF-LIBRARY ABI MIRROR.  This module is NOT a type source for the
//! library itself: every in-library module names the OWNER types directly
//! (dl.DlDb/RelEntry/ViewCacheSlot/PermIndexEntry/Txn/TxnOp, compiler.
//! compiled_rule/vm_instr/var_info, parser.token/expr/atom/rule, tupleset.
//! tuple_set, regexwalk.regex_dfa/sym_set, relation.Relation, …) and calls the
//! owner functions directly.  The C-shaped structs and `extern "c" fn`
//! declarations below exist only for the executables that link libdatalog.so
//! as a separate binary (dl_cli.zig) and cannot @import dl.zig
//! without compiling the whole engine into the exe, plus the comptime
//! drift oracle in abi_witness.zig.  Do not add an in-library reference here;
//! the witness pins every owner type against these mirrors so the two can
//! never diverge silently.
//!
//! Single source of truth for every C-layout type and extern symbol the
//! ported modules consume, in place of the removed translate-c blob
//! (`dl_internal.zig`, 1608 lines of @cImport noise).  Nothing here uses
//! @cImport; the layouts below are transcribed from the authoritative C
//! headers (src/dl_internal.h and the headers it includes: dl.h, intern.h,
//! relation.h, vrelation.h, snapshot.h, permindex.h, termstore.h, compiler.h
//! → parser.h, regexwalk.h, and dl.h → vector.h).
//!
//! The layouts are comptime-witnessed against ONE @cImport("dl_internal.h")
//! in zig/src/abi_witness.zig (imported only from tests.zig) so every field's
//! size/align/offset/type is pinned to the header rather than to memory here.
//! A wrong field type is silent memory corruption, not a compile error — the
//! witness is the guard.
//!
//! Pointer spellings match translate-c exactly (`[*c]` for C pointers, `?*`
//! for pointers to opaque structs, `?*const fn(...) callconv(.c)` for C
//! callbacks) so the ported modules' deref/cast patterns compile unchanged.

const c = std.c;
const std = @import("std");

// ─── Constants (from the headers) ────────────────────────────────────────
pub const MAX_RELS: c_int = 64; // dl_internal.h
pub const RELK_FIXED: c_int = 0; // dl_internal.h
pub const RELK_VARIADIC: c_int = 1; // dl_internal.h
pub const TERM_NIL: u32 = 0x80000000; // termstore.h TERM_BASE
pub const DL_VIEW_CACHE_SZ: c_int = 8; // snapshot.h
pub const VEC_IVEC_WORDS: c_int = 96; // vector.h VEC_D/4

// ─── Opaque handle types ─────────────────────────────────────────────────
pub const interner = opaque {}; // intern.h
pub const termstore = opaque {}; // termstore.h
pub const relation = opaque {}; // relation.h
pub const vrelation = opaque {}; // vrelation.h
pub const dafsa = opaque {}; // intern.h / relation.h forward decl
pub const dl_schema = opaque {}; // dl.h forward decl
pub const struct_tuple_set = opaque {}; // relation.h `struct tuple_set` forward decl (full def in tupleset.h)
pub const dl_iter = opaque {}; // dl.h
pub const parser = opaque {}; // parser.h

// ─── Structs (byte-identical to the headers; see abi_witness.zig) ────────

/// dl_internal.h: txn_op
pub const struct_txn_op = extern struct {
    kind: c_int,
    rel_id: c_int,
    arity: u8,
    cols: [8]u32,
    entity_sym: u32,
    expected: u32,
    next: u32,
};
pub const txn_op = struct_txn_op;

/// dl_internal.h: txn
pub const struct_txn = extern struct {
    ops: [*c]txn_op,
    nops: usize,
    cap: usize,
};
pub const txn = struct_txn;

/// snapshot.h: view_cache_slot
pub const view_cache_slot = extern struct {
    rel_name: [64]u8,
    view: ?*anyopaque,
    used: c_int,
};

/// permindex.h: perm_index_entry
pub const perm_index_entry = extern struct {
    rel_id: c_int,
    arity: u8,
    perm: [8]u8,
    pidx_rel: ?*relation,
    dirty: c_int,
};

/// compiler.h: var_info
pub const var_info = extern struct {
    name: [*c]u8,
    slot: u8,
};

/// compiler.h: vm_instr
pub const vm_instr = extern struct {
    op: u8,
    a: u8,
    b: u8,
    c: u8,
    imm: u32,
    slots: [8]u8,
    body_idx: u8,
};

/// regexwalk.h: regex_dfa
pub const struct_regex_dfa = extern struct {
    n_states: u32,
    trans: [*c]u32,
    accept: [*c]u8,
    errmsg: [*c]u8,
};
pub const regex_dfa = struct_regex_dfa;

/// regexwalk.h: sym_set
pub const struct_sym_set = extern struct {
    keys: [*c]u32,
    cap: c_int,
    used: c_int,
};
pub const sym_set = struct_sym_set;

/// compiler.h: compiled_rule
pub const compiled_rule = extern struct {
    head_pred: [*c]u8,
    head_rel_id: u8,
    n_vars: u8,
    vars: [*c]var_info,
    n_instrs: c_int,
    instrs: [*c]vm_instr,
    stratum: c_int,
    is_recursive: c_int,
    has_aggregate: c_int,
    n_patterns: c_int,
    patterns: [*c][*c]regex_dfa,
};

/// parser.h: token
pub const struct_token = extern struct {
    kind: c_uint, // token_kind enum
    off: u32,
    line: c_int,
    col: c_int,
    text: [*c]u8,
    ival: u32,
    children: [*c][*c]struct_token,
    nchildren: c_int,
    tail: [*c]struct_token,
};
pub const token = struct_token;

/// parser.h: expr
pub const struct_expr = extern struct {
    kind: c_uint, // expr_kind enum
    ival: u32,
    @"var": [*c]u8,
    op: u8,
    l: [*c]struct_expr,
    r: [*c]struct_expr,
};
pub const expr = struct_expr;

/// parser.h: atom
pub const atom = extern struct {
    pred: [*c]u8,
    off: u32,
    line: c_int,
    col: c_int,
    args: [*c][*c]token,
    nargs: c_int,
    negated: c_int,
    aggregate: c_int,
    agg_op: [*c]token,
    pattern: [*c]u8,
    pattern_col: c_int,
    arith: [*c]expr,
};

/// parser.h: rule
pub const rule = extern struct {
    head: [*c]atom,
    off: u32,
    body: [*c][*c]atom,
    nbody: c_int,
    has_negation: c_int,
    has_aggregate: c_int,
};

/// dl_internal.h: rel_entry
pub const rel_entry = extern struct {
    name: [*c]u8,
    kind: u8,
    arity: u8,
    rel: ?*relation,
    vrel: ?*vrelation,
};

/// vector.h (via dl.h): struct dl_vec_corpus
pub const struct_dl_vec_corpus = extern struct {
    filter_rel: [*c]const u8,
    filter_col: u8,
    vec_rel: [*c]const u8,
};
pub const dl_vec_corpus = struct_dl_vec_corpus;

/// dl_internal.h: struct dl_db — authoritative layout; must not grow.
pub const struct_dl_db = extern struct {
    dir: [*c]u8,
    ir: ?*interner,
    terms: ?*termstore,
    rels: [64]rel_entry, // MAX_RELS
    nrels: usize,
    lock_fd: c_int,
    read_only: c_int,
    meta_dirty: c_int,
    rev_rel_id: c_int,
    txn: [*c]txn,
    schema: ?*const dl_schema,
    crules: [*c][*c]compiled_rule,
    n_crules: c_int,
    fixpoint_dirty: c_int,
    snap_version: u32,
    snapshot_retain: c_uint,
    vcache: [8]view_cache_slot, // DL_VIEW_CACHE_SZ
    fault_hook: ?*const fn (fp: c_uint, user: ?*anyopaque) callconv(.c) c_int, // dl_fpoint enum
    fault_user: ?*anyopaque,
    perms: [64]perm_index_entry, // MAX_PERMS
    n_perms: c_int,
    ast_rules: [*c][*c]rule,
    n_ast_rules: c_int,
    full_reeval_pending: c_int,
    delta_pending: [64]?*struct_tuple_set,
    del_pending: [64]?*struct_tuple_set,
};
pub const dl_db = struct_dl_db;

// ─── Callback typedefs (function pointers) ───────────────────────────────
pub const dl_tuple_cb = ?*const fn (cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int;
pub const dl_join_cb = ?*const fn (l: [*c]const u32, la: u8, r: [*c]const u32, ra: u8, user: ?*anyopaque) callconv(.c) c_int;
pub const dl_relation_cb = ?*const fn (name: [*c]const u8, arity: u8, idb: c_int, user: ?*anyopaque) callconv(.c) c_int;
pub const dl_str_cb = ?*const fn (s: [*c]const u8, user: ?*anyopaque) callconv(.c) c_int;
pub const dl_traverse_cb = ?*const fn (node_sym: u32, depth: u8, user: ?*anyopaque) callconv(.c) c_int;
pub const dl_vec_cb = ?*const fn (entity_sym: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int;
pub const rel_enum_cb = ?*const fn (cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int;
pub const sym_walk_cb = ?*const fn (sym_id: u32, user: ?*anyopaque) callconv(.c) c_int;

// ─── dl.h / dl_internal.h ────────────────────────────────────────────────
pub extern "c" fn dl_open(dir: [*c]const u8) [*c]dl_db;
pub extern "c" fn dl_close(db: [*c]dl_db) void;
pub extern "c" fn dl_declare_relation(db: [*c]dl_db, name: [*c]const u8, arity: u8) c_int;
pub extern "c" fn dl_load_facts(db: [*c]dl_db, rel: [*c]const u8, csv_path: [*c]const u8) c_int;
pub extern "c" fn dl_add_fact(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int;
pub extern "c" fn dl_cas_revision(db: [*c]dl_db, entity: [*c]const u8, expected: u32, new_value: u32) c_int;
pub extern "c" fn dl_rev_get(db: [*c]dl_db, entity: [*c]const u8, out: [*c]u32) c_int;
pub extern "c" fn dl_txn_begin(db: [*c]dl_db) c_int;
pub extern "c" fn dl_txn_cas(db: [*c]dl_db, entity: [*c]const u8, expected: u32, new_value: u32) c_int;
pub extern "c" fn dl_txn_add_fact(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int;
pub extern "c" fn dl_txn_commit(db: [*c]dl_db) c_int;
pub extern "c" fn dl_txn_rollback(db: [*c]dl_db) c_int;
pub extern "c" fn dl_lookup(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int;
pub extern "c" fn dl_prefix(db: [*c]dl_db, rel: [*c]const u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern "c" fn dl_iter_open_live(rel: ?*relation, leading: [*c]const u32, k: u8) ?*dl_iter;
pub extern "c" fn dl_iter_next(it: ?*dl_iter, cols_out: [*c]u32) c_int;
pub extern "c" fn dl_iter_close(it: ?*dl_iter) void;
pub extern "c" fn dl_attach_schema(db: [*c]dl_db, schema: ?*const dl_schema) c_int;
pub extern "c" fn dl_load_rules(db: [*c]dl_db, dl_source: [*c]const u8) c_int;
pub extern "c" fn dl_compile(db: [*c]dl_db) c_int;
pub extern "c" fn dl_query(db: [*c]dl_db, goal_rel: [*c]const u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern "c" fn dl_query_bound(db: [*c]dl_db, goal_rel: [*c]const u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern "c" fn dl_pattern(db: [*c]dl_db, rel_name: [*c]const u8, col: u8, dfa: [*c]const regex_dfa, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern "c" fn dl_publish_snapshot(db: [*c]dl_db) c_int;
pub extern "c" fn dl_snapshot_versions(db: [*c]const dl_db, out: [*c]u32, cap: usize) c_long;
pub extern "c" fn dl_set_snapshot_retain(db: [*c]dl_db, n: c_uint) c_int;
pub extern "c" fn dl_intern_str(db: [*c]dl_db, str: [*c]const u8) u32;
pub extern "c" fn dl_intern_str_of(db: [*c]dl_db, sym_id: u32) [*c]const u8;
pub extern "c" fn dl_traverse(db: [*c]dl_db, start: [*c]const u8, depth: c_int, max_nodes: c_int, cb: dl_traverse_cb, user: ?*anyopaque) c_long;
pub extern "c" fn dl_node_observations(db: [*c]dl_db, node: [*c]const u8, max_obs: c_int, cb: dl_str_cb, user: ?*anyopaque) c_long;
pub extern "c" fn dl_term_cons(db: [*c]dl_db, head: u32, tail: u32) u32;
pub extern "c" fn dl_term_is_list(db: [*c]const dl_db, v: u32) c_int;
pub extern "c" fn dl_term_car(db: [*c]const dl_db, h: u32) u32;
pub extern "c" fn dl_term_cdr(db: [*c]const dl_db, h: u32) u32;
pub extern "c" fn dl_vector_search(db: [*c]dl_db, q_int8: [*c]const u32, k: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;
pub extern "c" fn dl_vector_search_version(db: [*c]dl_db, version: u32, q_int8: [*c]const u32, k: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;
pub extern "c" fn dl_vector_rerank(db: [*c]dl_db, q_int8: [*c]const u32, cand_syms: [*c]const u32, n_cand: c_int, k: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;

// ─── dl_internal.h dispatch helpers ──────────────────────────────────────
pub extern "c" fn db_has_variadic(db: [*c]const dl_db) c_int;
pub extern "c" fn db_has_list_builtin(db: [*c]const dl_db) c_int;
pub extern "c" fn db_has_range_builtin(db: [*c]const dl_db) c_int;
pub extern "c" fn db_rel_at_arity_ro(db: [*c]const dl_db, rel_id: c_int, arity: u8) ?*relation;
pub extern "c" fn db_rel_at_arity_rw(db: [*c]dl_db, rel_id: c_int, arity: u8) ?*relation;
pub extern "c" fn dl_ensure_variant(db: [*c]dl_db, rel_id: c_int, arity: u8) ?*relation;

// ─── intern.h ────────────────────────────────────────────────────────────
pub extern "c" fn intern_fwd(ir: ?*interner) ?*const dafsa;
pub extern "c" fn intern_str(ir: ?*interner, str: [*c]const u8) u32;
pub extern "c" fn intern_str_of(ir: ?*interner, sym_id: u32) [*c]const u8;

// ─── relation.h ──────────────────────────────────────────────────────────
pub extern "c" fn rel_add(rel: ?*relation, cols: [*c]const u32) c_int;
pub extern "c" fn rel_add_tupleset(rel: ?*relation, ts: ?*const struct_tuple_set) c_int;
pub extern "c" fn rel_arity(rel: ?*const relation) u8;
pub extern "c" fn rel_build_from_tupleset(rel: ?*relation, ts: ?*const struct_tuple_set) c_int;
pub extern "c" fn rel_count(rel: ?*const relation) u64;
pub extern "c" fn rel_count_subtree(rel: ?*const relation) u64;
pub extern "c" fn rel_delete(rel: ?*relation, cols: [*c]const u32) c_int;
pub extern "c" fn rel_exact(rel: ?*const relation, cols: [*c]const u32) c_int;
pub extern "c" fn rel_exact_base(rel: ?*const relation, cols: [*c]const u32) c_int;
pub extern "c" fn rel_filter_col(rel: ?*const relation, col: u8, set: [*c]const sym_set, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern "c" fn rel_has_col0(rel: ?*const relation, x: u32) c_int;
pub extern "c" fn rel_prefix(rel: ?*const relation, leading: [*c]const u32, k: u8, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern "c" fn rel_prefix_base(rel: ?*const relation, leading: [*c]const u32, k: u8, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern "c" fn rel_reset_view(rel: ?*relation) c_int;
pub extern "c" fn ts_sink_cb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) c_int;

// ─── vrelation.h ─────────────────────────────────────────────────────────
pub extern "c" fn vrel_count(v: ?*const vrelation) u64;
pub extern "c" fn vrel_prefix(v: ?*const vrelation, leading: [*c]const u32, k: u8, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern "c" fn vrel_reset_views(v: ?*vrelation) c_int;

// ─── permindex.h ─────────────────────────────────────────────────────────
pub extern "c" fn dl_db_declare_perm(db: [*c]dl_db, rel_id: c_int, arity: u8, perm: [*c]const u8) c_int;
pub extern "c" fn dl_db_find_perm(db: [*c]dl_db, rel_id: c_int, arity: u8, perm: [*c]const u8) c_int;
pub extern "c" fn dl_db_get_perm(db: [*c]dl_db, rel_id: c_int, perm_id: c_int) [*c]const u8;
pub extern "c" fn dl_db_get_perm_rel(db: [*c]dl_db, rel_id: c_int, perm_id: c_int) ?*relation;
pub extern "c" fn permindex_build_dirty(db: [*c]dl_db) c_int;
pub extern "c" fn permindex_mark_dirty(db: [*c]dl_db, rel_id: c_int) void;

// ─── termstore.h ─────────────────────────────────────────────────────────
pub extern "c" fn term_car(t: ?*const termstore, h: u32) u32;
pub extern "c" fn term_cdr(t: ?*const termstore, h: u32) u32;
pub extern "c" fn term_cons(t: ?*termstore, head: u32, tail: u32) u32;
pub extern "c" fn term_append(t: ?*termstore, a: u32, b: u32) u32;
pub extern "c" fn term_is_list(t: ?*const termstore, v: u32) c_int;
pub extern "c" fn term_length(t: ?*const termstore, h: u32) u32;

// ─── parser.h ────────────────────────────────────────────────────────────
pub extern "c" fn parse_create(source: [*c]const u8) ?*parser;
pub extern "c" fn parse_last_error(p: ?*const parser, off: [*c]u32) [*c]const u8;
pub extern "c" fn parse_rules(p: ?*parser, n_rules: [*c]c_int) [*c][*c]rule;
pub extern "c" fn parse_free(p: ?*parser) void;
pub extern "c" fn rule_free(r: [*c]rule) void;

// ─── regexwalk.h ─────────────────────────────────────────────────────────
pub extern "c" fn regex_compile(pattern: [*c]const u8) [*c]regex_dfa;
pub extern "c" fn regex_dfa_free(dfa: [*c]regex_dfa) void;
pub extern "c" fn symbols_dfa_walk(d: ?*const dafsa, dfa: [*c]const regex_dfa, cb: sym_walk_cb, user: ?*anyopaque) c_long;
pub extern "c" fn symset_init(s: [*c]sym_set) c_int;
pub extern "c" fn symset_free(s: [*c]sym_set) void;
pub extern "c" fn symset_add(s: [*c]sym_set, sym_id: u32) c_int;

// ─── index.h ─────────────────────────────────────────────────────────────
pub extern "c" fn tokenize(text: [*c]const u8, n_out: [*c]usize) [*c][*c]u8;
pub extern "c" fn token_free(tokens: [*c][*c]u8) void;
pub extern "c" fn aux_index_ensure_postings(db: [*c]dl_db) c_int;
pub extern "c" fn dl_search_top(db: [*c]dl_db, terms: [*c]const u32, n_terms: c_int, obs_ids_out: [*c]u32, scores_out: [*c]c_int, limit: c_int) c_int;
pub extern "c" fn dl_search_top_version(db: [*c]dl_db, version: u32, terms: [*c]const u32, n_terms: c_int, obs_ids_out: [*c]u32, scores_out: [*c]c_int, limit: c_int) c_int;
pub extern "c" fn dl_index_observations(db: [*c]dl_db) c_long;
