pub const __builtin = @import("std").zig.c_translation.builtins;
pub const __helpers = @import("std").zig.c_translation.helpers;
pub const ptrdiff_t = c_long;
pub const wchar_t = c_int;
pub const max_align_t = extern struct {
    __aro_max_align_ll: c_longlong = 0,
    __aro_max_align_ld: c_longdouble = 0,
};
pub const __u_char = u8;
pub const __u_short = c_ushort;
pub const __u_int = c_uint;
pub const __u_long = c_ulong;
pub const __int8_t = i8;
pub const __uint8_t = u8;
pub const __int16_t = c_short;
pub const __uint16_t = c_ushort;
pub const __int32_t = c_int;
pub const __uint32_t = c_uint;
pub const __int64_t = c_long;
pub const __uint64_t = c_ulong;
pub const __int_least8_t = __int8_t;
pub const __uint_least8_t = __uint8_t;
pub const __int_least16_t = __int16_t;
pub const __uint_least16_t = __uint16_t;
pub const __int_least32_t = __int32_t;
pub const __uint_least32_t = __uint32_t;
pub const __int_least64_t = __int64_t;
pub const __uint_least64_t = __uint64_t;
pub const __quad_t = c_long;
pub const __u_quad_t = c_ulong;
pub const __intmax_t = c_long;
pub const __uintmax_t = c_ulong;
pub const __dev_t = c_ulong;
pub const __uid_t = c_uint;
pub const __gid_t = c_uint;
pub const __ino_t = c_ulong;
pub const __ino64_t = c_ulong;
pub const __mode_t = c_uint;
pub const __nlink_t = c_ulong;
pub const __off_t = c_long;
pub const __off64_t = c_long;
pub const __pid_t = c_int;
pub const __fsid_t = extern struct {
    __val: [2]c_int = @import("std").mem.zeroes([2]c_int),
};
pub const __clock_t = c_long;
pub const __rlim_t = c_ulong;
pub const __rlim64_t = c_ulong;
pub const __id_t = c_uint;
pub const __time_t = c_long;
pub const __useconds_t = c_uint;
pub const __suseconds_t = c_long;
pub const __suseconds64_t = c_long;
pub const __daddr_t = c_int;
pub const __key_t = c_int;
pub const __clockid_t = c_int;
pub const __timer_t = ?*anyopaque;
pub const __blksize_t = c_long;
pub const __blkcnt_t = c_long;
pub const __blkcnt64_t = c_long;
pub const __fsblkcnt_t = c_ulong;
pub const __fsblkcnt64_t = c_ulong;
pub const __fsfilcnt_t = c_ulong;
pub const __fsfilcnt64_t = c_ulong;
pub const __fsword_t = c_long;
pub const __ssize_t = c_long;
pub const __syscall_slong_t = c_long;
pub const __syscall_ulong_t = c_ulong;
pub const __loff_t = __off64_t;
pub const __caddr_t = [*c]u8;
pub const __intptr_t = c_long;
pub const __socklen_t = c_uint;
pub const __sig_atomic_t = c_int;
pub const int_least8_t = __int_least8_t;
pub const int_least16_t = __int_least16_t;
pub const int_least32_t = __int_least32_t;
pub const int_least64_t = __int_least64_t;
pub const uint_least8_t = __uint_least8_t;
pub const uint_least16_t = __uint_least16_t;
pub const uint_least32_t = __uint_least32_t;
pub const uint_least64_t = __uint_least64_t;
pub const int_fast8_t = i8;
pub const int_fast16_t = c_long;
pub const int_fast32_t = c_long;
pub const int_fast64_t = c_long;
pub const uint_fast8_t = u8;
pub const uint_fast16_t = c_ulong;
pub const uint_fast32_t = c_ulong;
pub const uint_fast64_t = c_ulong;
pub const intmax_t = __intmax_t;
pub const uintmax_t = __uintmax_t;
pub const struct_interner = opaque {
};
pub const interner = struct_interner;
pub const struct_termstore = opaque {
};
pub const termstore = struct_termstore;
pub const struct_txn_op = extern struct {
    kind: c_int = 0,
    rel_id: c_int = 0,
    arity: u8 = 0,
    cols: [8]u32 = @import("std").mem.zeroes([8]u32),
    entity_sym: u32 = 0,
    expected: u32 = 0,
    next: u32 = 0,
};
pub const txn_op = struct_txn_op;
pub const struct_txn = extern struct {
    ops: [*c]txn_op = null,
    nops: usize = 0,
    cap: usize = 0,
};
pub const txn = struct_txn;
pub const struct_dl_schema = opaque {};
pub const struct_tuple_set = opaque {};
pub const struct_dl_db = extern struct {
    dir: [*c]u8 = null,
    ir: ?*interner = null,
    terms: ?*termstore = null,
    rels: [64]rel_entry = @import("std").mem.zeroes([64]rel_entry),
    nrels: usize = 0,
    lock_fd: c_int = 0,
    read_only: c_int = 0,
    meta_dirty: c_int = 0,
    rev_rel_id: c_int = 0,
    txn: [*c]txn = null,
    schema: ?*const struct_dl_schema = null,
    crules: [*c][*c]compiled_rule = null,
    n_crules: c_int = 0,
    fixpoint_dirty: c_int = 0,
    snap_version: u32 = 0,
    snapshot_retain: c_uint = 0,
    vcache: [8]view_cache_slot = @import("std").mem.zeroes([8]view_cache_slot),
    fault_hook: ?*const fn (dl_fpoint, ?*anyopaque) callconv(.c) c_int = null,
    fault_user: ?*anyopaque = null,
    perms: [64]perm_index_entry = @import("std").mem.zeroes([64]perm_index_entry),
    n_perms: c_int = 0,
    ast_rules: [*c][*c]rule = null,
    n_ast_rules: c_int = 0,
    full_reeval_pending: c_int = 0,
    delta_pending: [64]?*struct_tuple_set = @import("std").mem.zeroes([64]?*struct_tuple_set),
    del_pending: [64]?*struct_tuple_set = @import("std").mem.zeroes([64]?*struct_tuple_set),
};
// selfreg-dl-storage: dl.zig's DlDb appends Zig-only fields (compact_checks,
// access_epoch) PAST this sizeof; dl.zig's comptime gate asserts the tail
// lands at/after @sizeOf(struct_dl_db).  This mirror must NOT grow.
pub const dl_db = struct_dl_db;
pub const dl_schema = struct_dl_schema;
pub extern fn dl_open(dir: [*c]const u8) [*c]dl_db;
pub extern fn dl_close(db: [*c]dl_db) void;
pub extern fn dl_open2(dir: [*c]const u8, err_out: [*c]c_int) [*c]dl_db;
pub extern fn dl_open_ro(dir: [*c]const u8, err_out: [*c]c_int) [*c]dl_db;
pub extern fn dl_declare_relation(db: [*c]dl_db, name: [*c]const u8, arity: u8) c_int;
pub extern fn dl_declare_relation_variadic(db: [*c]dl_db, name: [*c]const u8) c_int;
pub extern fn dl_load_facts(db: [*c]dl_db, rel: [*c]const u8, csv_path: [*c]const u8) c_int;
pub extern fn dl_add_fact(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int;
pub extern fn dl_delete_fact(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int;
pub extern fn dl_cas_revision(db: [*c]dl_db, entity: [*c]const u8, expected: u32, new_value: u32) c_int;
pub extern fn dl_rev_get(db: [*c]dl_db, entity: [*c]const u8, out: [*c]u32) c_int;
pub extern fn dl_txn_begin(db: [*c]dl_db) c_int;
pub extern fn dl_txn_cas(db: [*c]dl_db, entity: [*c]const u8, expected: u32, new_value: u32) c_int;
pub extern fn dl_txn_add_fact(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int;
pub extern fn dl_txn_delete_fact(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int;
pub extern fn dl_txn_commit(db: [*c]dl_db) c_int;
pub extern fn dl_txn_rollback(db: [*c]dl_db) c_int;
pub extern fn dl_lookup(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) c_int;
pub const dl_tuple_cb = ?*const fn (cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn dl_prefix(db: [*c]dl_db, rel: [*c]const u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub const struct_dl_iter = opaque {
};
pub const dl_iter = struct_dl_iter;
pub extern fn dl_iter_open(db: [*c]dl_db, rel: [*c]const u8, leading: [*c]const u32, k: u8) ?*dl_iter;
pub extern fn dl_iter_seek(it: ?*dl_iter, leading: [*c]const u32, k: u8) c_int;
pub extern fn dl_iter_next(it: ?*dl_iter, cols_out: [*c]u32) c_int;
pub extern fn dl_iter_arity(it: ?*const dl_iter) u8;
pub extern fn dl_iter_close(it: ?*dl_iter) void;
pub const dl_join_cb = ?*const fn (l: [*c]const u32, la: u8, r: [*c]const u32, ra: u8, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn dl_merge_join(l: ?*dl_iter, r: ?*dl_iter, jcols: u8, cb: dl_join_cb, user: ?*anyopaque) c_long;
pub extern fn dl_rank(db: [*c]dl_db, rel: [*c]const u8, cols: [*c]const u32, arity: u8) u64;
pub extern fn dl_select(db: [*c]dl_db, rel: [*c]const u8, k: u64, cols_out: [*c]u32, arity: u8) c_int;
pub extern fn dl_range_count(db: [*c]dl_db, rel: [*c]const u8, lo: [*c]const u32, hi: [*c]const u32, arity: u8) u64;
pub extern fn dl_count(db: [*c]dl_db, rel: [*c]const u8) u64;
pub extern fn dl_rank_bound(db: [*c]dl_db, rel: [*c]const u8, leading: [*c]const u32, k: u8, cols: [*c]const u32, arity: u8) u64;
pub extern fn dl_select_bound(db: [*c]dl_db, rel: [*c]const u8, leading: [*c]const u32, k: u8, idx: u64, cols_out: [*c]u32, arity: u8) c_int;
pub extern fn dl_range_count_bound(db: [*c]dl_db, rel: [*c]const u8, leading: [*c]const u32, k: u8, lo: [*c]const u32, hi: [*c]const u32, arity: u8) u64;
pub extern fn dl_rank_perm(db: [*c]dl_db, rel: [*c]const u8, perm_id: c_int, cols: [*c]const u32, arity: u8) u64;
pub extern fn dl_select_perm(db: [*c]dl_db, rel: [*c]const u8, perm_id: c_int, k: u64, cols_out: [*c]u32, arity: u8) c_int;
pub extern fn dl_range_count_perm(db: [*c]dl_db, rel: [*c]const u8, perm_id: c_int, lo: [*c]const u32, hi: [*c]const u32, arity: u8) u64;
pub extern fn dl_db_perm_count(db: [*c]const dl_db) c_int;
pub extern fn dl_attach_schema(db: [*c]dl_db, schema: ?*const dl_schema) c_int;
pub extern fn dl_load_rules(db: [*c]dl_db, dl_source: [*c]const u8) c_int;
pub extern fn dl_compile(db: [*c]dl_db) c_int;
pub extern fn dl_query(db: [*c]dl_db, goal_rel: [*c]const u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn dl_query_rules_ro(db: [*c]dl_db, dl_source: [*c]const u8, goal_rel: [*c]const u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn dl_query_bound(db: [*c]dl_db, goal_rel: [*c]const u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn dl_query_magic(db: [*c]dl_db, goal_rel: [*c]const u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn dl_query_magic_adorn(db: [*c]dl_db, goal_rel: [*c]const u8, adorn: [*c]const u8, vals: [*c]const u32, nvals: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn dl_query_topdown(db: [*c]dl_db, goal_rel: [*c]const u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn dl_query_topdown_adorn(db: [*c]dl_db, goal_rel: [*c]const u8, adorn: [*c]const u8, vals: [*c]const u32, nvals: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub const struct_regex_dfa = extern struct {
    n_states: u32 = 0,
    trans: [*c]u32 = null,
    accept: [*c]u8 = null,
    errmsg: [*c]u8 = null,
};
pub extern fn dl_pattern(db: [*c]dl_db, rel_name: [*c]const u8, col: u8, dfa: [*c]const struct_regex_dfa, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn dl_publish_snapshot(db: [*c]dl_db) c_int;
pub extern fn dl_snapshot_versions(db: [*c]const dl_db, out: [*c]u32, cap: usize) c_long;
pub extern fn dl_query_version(db: [*c]dl_db, version: u32, goal_rel: [*c]const u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn dl_query_bound_version(db: [*c]dl_db, version: u32, goal_rel: [*c]const u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub const dl_relation_cb = ?*const fn (name: [*c]const u8, arity: u8, idb: c_int, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn dl_snapshot_relations(db: [*c]dl_db, version: u32, cb: dl_relation_cb, user: ?*anyopaque) c_long;
pub extern fn dl_set_snapshot_retain(db: [*c]dl_db, n: c_uint) c_int;
pub const DL_FPOINT_AFTER_REL_SAVE: c_int = 0;
pub const DL_FPOINT_AFTER_RENAME: c_int = 1;
pub const DL_FPOINT_TXN_BEFORE_MARKER: c_int = 2;
pub const dl_fpoint = c_uint;
pub extern fn dl_set_fault_hook(db: [*c]dl_db, hook: ?*const fn (fp: dl_fpoint, user: ?*anyopaque) callconv(.c) c_int, user: ?*anyopaque) void;
pub extern fn dl_intern_str(db: [*c]dl_db, str: [*c]const u8) u32;
pub extern fn dl_intern_str_find(db: [*c]dl_db, str: [*c]const u8) u32;
pub extern fn dl_intern_str_of(db: [*c]dl_db, sym_id: u32) [*c]const u8;
pub extern fn dl_intern_fwd_mutable(db: [*c]dl_db) ?*anyopaque;
pub const dl_traverse_cb = ?*const fn (node_sym: u32, depth: u8, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn dl_traverse(db: [*c]dl_db, start: [*c]const u8, depth: c_int, max_nodes: c_int, cb: dl_traverse_cb, user: ?*anyopaque) c_long;
pub const dl_str_cb = ?*const fn (s: [*c]const u8, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn dl_node_observations(db: [*c]dl_db, node: [*c]const u8, max_obs: c_int, cb: dl_str_cb, user: ?*anyopaque) c_long;
pub extern fn dl_term_cons(db: [*c]dl_db, head: u32, tail: u32) u32;
pub extern fn dl_term_append(db: [*c]dl_db, a: u32, b: u32) u32;
pub extern fn dl_term_is_list(db: [*c]const dl_db, v: u32) c_int;
pub extern fn dl_term_car(db: [*c]const dl_db, h: u32) u32;
pub extern fn dl_term_cdr(db: [*c]const dl_db, h: u32) u32;
pub const struct_dl_vec_corpus = extern struct {
    filter_rel: [*c]const u8 = null,
    filter_col: u8 = 0,
    sig_rel_fmt: [*c]const u8 = null,
    vec_rel: [*c]const u8 = null,
    basis_suffix: [*c]const u8 = null,
};
pub const dl_vec_cb = ?*const fn (entity_sym: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn dl_vector_search(db: [*c]dl_db, q_sig: [*c]const u32, k: c_int, r: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;
pub extern fn dl_vector_search_version(db: [*c]dl_db, version: u32, q_sig: [*c]const u32, k: c_int, r: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;
pub extern fn dl_vector_search_corpus(db: [*c]dl_db, corpus: [*c]const struct_dl_vec_corpus, q_sig: [*c]const u32, k: c_int, r: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;
pub extern fn dl_vector_search_corpus_version(db: [*c]dl_db, version: u32, corpus: [*c]const struct_dl_vec_corpus, q_sig: [*c]const u32, k: c_int, r: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;
pub extern fn dl_vector_rerank(db: [*c]dl_db, q_int8: [*c]const u32, cand_syms: [*c]const u32, n_cand: c_int, k: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;
pub extern fn dl_vector_rerank_corpus(db: [*c]dl_db, corpus: [*c]const struct_dl_vec_corpus, q_int8: [*c]const u32, cand_syms: [*c]const u32, n_cand: c_int, k: c_int, cb: dl_vec_cb, user: ?*anyopaque) c_long;
pub const struct___va_list_tag_1 = extern struct {
    unnamed_0: c_uint = 0,
    unnamed_1: c_uint = 0,
    unnamed_2: ?*anyopaque = null,
    unnamed_3: ?*anyopaque = null,
};
pub const __builtin_va_list = [1]struct___va_list_tag_1;
pub const va_list = __builtin_va_list;
pub const __gnuc_va_list = __builtin_va_list;
const union_unnamed_2 = extern union {
    __wch: c_uint,
    __wchb: [4]u8,
};
pub const __mbstate_t = extern struct {
    __count: c_int = 0,
    __value: union_unnamed_2 = @import("std").mem.zeroes(union_unnamed_2),
};
pub const struct__G_fpos_t = extern struct {
    __pos: __off_t = 0,
    __state: __mbstate_t = @import("std").mem.zeroes(__mbstate_t),
};
pub const __fpos_t = struct__G_fpos_t;
pub const struct__G_fpos64_t = extern struct {
    __pos: __off64_t = 0,
    __state: __mbstate_t = @import("std").mem.zeroes(__mbstate_t),
};
pub const __fpos64_t = struct__G_fpos64_t;
pub const struct__IO_marker = opaque {}; // /usr/include/bits/types/struct_FILE.h:75:7: warning: struct demoted to opaque type - has bitfield
pub const struct__IO_FILE = opaque {
};
pub const __FILE = struct__IO_FILE;
pub const FILE = struct__IO_FILE;
pub const struct__IO_codecvt = opaque {};
pub const struct__IO_wide_data = opaque {};
pub const _IO_lock_t = anyopaque;
pub const cookie_read_function_t = fn (__cookie: ?*anyopaque, __buf: [*c]u8, __nbytes: usize) callconv(.c) __ssize_t;
pub const cookie_write_function_t = fn (__cookie: ?*anyopaque, __buf: [*c]const u8, __nbytes: usize) callconv(.c) __ssize_t;
pub const cookie_seek_function_t = fn (__cookie: ?*anyopaque, __pos: [*c]__off64_t, __w: c_int) callconv(.c) c_int;
pub const cookie_close_function_t = fn (__cookie: ?*anyopaque) callconv(.c) c_int;
pub const struct__IO_cookie_io_functions_t = extern struct {
    read: ?*const cookie_read_function_t = null,
    write: ?*const cookie_write_function_t = null,
    seek: ?*const cookie_seek_function_t = null,
    close: ?*const cookie_close_function_t = null,
};
pub const cookie_io_functions_t = struct__IO_cookie_io_functions_t;
pub const off_t = __off_t;
pub const fpos_t = __fpos_t;
pub extern var stdin: ?*FILE;
pub extern var stdout: ?*FILE;
pub extern var stderr: ?*FILE;
pub extern fn remove(__filename: [*c]const u8) c_int;
pub extern fn rename(__old: [*c]const u8, __new: [*c]const u8) c_int;
pub extern fn renameat(__oldfd: c_int, __old: [*c]const u8, __newfd: c_int, __new: [*c]const u8) c_int;
pub extern fn fclose(__stream: ?*FILE) c_int;
pub extern fn tmpfile() ?*FILE;
pub extern fn tmpnam([*c]u8) [*c]u8;
pub extern fn tmpnam_r(__s: [*c]u8) [*c]u8;
pub extern fn tempnam(__dir: [*c]const u8, __pfx: [*c]const u8) [*c]u8;
pub extern fn fflush(__stream: ?*FILE) c_int;
pub extern fn fflush_unlocked(__stream: ?*FILE) c_int;
pub extern fn fopen(noalias __filename: [*c]const u8, noalias __modes: [*c]const u8) ?*FILE;
pub extern fn freopen(noalias __filename: [*c]const u8, noalias __modes: [*c]const u8, noalias __stream: ?*FILE) ?*FILE;
pub extern fn fdopen(__fd: c_int, __modes: [*c]const u8) ?*FILE;
pub extern fn fopencookie(noalias __magic_cookie: ?*anyopaque, noalias __modes: [*c]const u8, __io_funcs: cookie_io_functions_t) ?*FILE;
pub extern fn fmemopen(__s: ?*anyopaque, __len: usize, __modes: [*c]const u8) ?*FILE;
pub extern fn open_memstream(__bufloc: [*c][*c]u8, __sizeloc: [*c]usize) ?*FILE;
pub extern fn setbuf(noalias __stream: ?*FILE, noalias __buf: [*c]u8) void;
pub extern fn setvbuf(noalias __stream: ?*FILE, noalias __buf: [*c]u8, __modes: c_int, __n: usize) c_int;
pub extern fn setbuffer(noalias __stream: ?*FILE, noalias __buf: [*c]u8, __size: usize) void;
pub extern fn setlinebuf(__stream: ?*FILE) void;
pub extern fn fprintf(noalias __stream: ?*FILE, noalias __format: [*c]const u8, ...) c_int;
pub extern fn printf(noalias __format: [*c]const u8, ...) c_int;
pub extern fn sprintf(noalias __s: [*c]u8, noalias __format: [*c]const u8, ...) c_int;
pub extern fn vfprintf(noalias __s: ?*FILE, noalias __format: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn vprintf(noalias __format: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn vsprintf(noalias __s: [*c]u8, noalias __format: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn snprintf(noalias __s: [*c]u8, __maxlen: usize, noalias __format: [*c]const u8, ...) c_int;
pub extern fn vsnprintf(noalias __s: [*c]u8, __maxlen: usize, noalias __format: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn vasprintf(noalias __ptr: [*c][*c]u8, noalias __f: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn __asprintf(noalias __ptr: [*c][*c]u8, noalias __fmt: [*c]const u8, ...) c_int;
pub extern fn asprintf(noalias __ptr: [*c][*c]u8, noalias __fmt: [*c]const u8, ...) c_int;
pub extern fn vdprintf(__fd: c_int, noalias __fmt: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn dprintf(__fd: c_int, noalias __fmt: [*c]const u8, ...) c_int;
pub extern fn fscanf(noalias __stream: ?*FILE, noalias __format: [*c]const u8, ...) c_int;
pub extern fn scanf(noalias __format: [*c]const u8, ...) c_int;
pub extern fn sscanf(noalias __s: [*c]const u8, noalias __format: [*c]const u8, ...) c_int;
pub extern fn vfscanf(noalias __s: ?*FILE, noalias __format: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn vscanf(noalias __format: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn vsscanf(noalias __s: [*c]const u8, noalias __format: [*c]const u8, __arg: [*c]struct___va_list_tag_1) c_int;
pub extern fn fgetc(__stream: ?*FILE) c_int;
pub extern fn getc(__stream: ?*FILE) c_int;
pub extern fn getchar() c_int;
pub extern fn getc_unlocked(__stream: ?*FILE) c_int;
pub extern fn getchar_unlocked() c_int;
pub extern fn fgetc_unlocked(__stream: ?*FILE) c_int;
pub extern fn fputc(__c: c_int, __stream: ?*FILE) c_int;
pub extern fn putc(__c: c_int, __stream: ?*FILE) c_int;
pub extern fn putchar(__c: c_int) c_int;
pub extern fn fputc_unlocked(__c: c_int, __stream: ?*FILE) c_int;
pub extern fn putc_unlocked(__c: c_int, __stream: ?*FILE) c_int;
pub extern fn putchar_unlocked(__c: c_int) c_int;
pub extern fn getw(__stream: ?*FILE) c_int;
pub extern fn putw(__w: c_int, __stream: ?*FILE) c_int;
pub extern fn fgets(noalias __s: [*c]u8, __n: c_int, noalias __stream: ?*FILE) [*c]u8;
pub extern fn __getdelim(noalias __lineptr: [*c][*c]u8, noalias __n: [*c]usize, __delimiter: c_int, noalias __stream: ?*FILE) __ssize_t;
pub extern fn getdelim(noalias __lineptr: [*c][*c]u8, noalias __n: [*c]usize, __delimiter: c_int, noalias __stream: ?*FILE) __ssize_t;
pub extern fn getline(noalias __lineptr: [*c][*c]u8, noalias __n: [*c]usize, noalias __stream: ?*FILE) __ssize_t;
pub extern fn fputs(noalias __s: [*c]const u8, noalias __stream: ?*FILE) c_int;
pub extern fn puts(__s: [*c]const u8) c_int;
pub extern fn ungetc(__c: c_int, __stream: ?*FILE) c_int;
pub extern fn fread(noalias __ptr: ?*anyopaque, __size: usize, __n: usize, noalias __stream: ?*FILE) usize;
pub extern fn fwrite(noalias __ptr: ?*const anyopaque, __size: usize, __n: usize, noalias __s: ?*FILE) usize;
pub extern fn fread_unlocked(noalias __ptr: ?*anyopaque, __size: usize, __n: usize, noalias __stream: ?*FILE) usize;
pub extern fn fwrite_unlocked(noalias __ptr: ?*const anyopaque, __size: usize, __n: usize, noalias __stream: ?*FILE) usize;
pub extern fn fseek(__stream: ?*FILE, __off: c_long, __whence: c_int) c_int;
pub extern fn ftell(__stream: ?*FILE) c_long;
pub extern fn rewind(__stream: ?*FILE) void;
pub extern fn fseeko(__stream: ?*FILE, __off: __off_t, __whence: c_int) c_int;
pub extern fn ftello(__stream: ?*FILE) __off_t;
pub extern fn fgetpos(noalias __stream: ?*FILE, noalias __pos: [*c]fpos_t) c_int;
pub extern fn fsetpos(__stream: ?*FILE, __pos: [*c]const fpos_t) c_int;
pub extern fn clearerr(__stream: ?*FILE) void;
pub extern fn feof(__stream: ?*FILE) c_int;
pub extern fn ferror(__stream: ?*FILE) c_int;
pub extern fn clearerr_unlocked(__stream: ?*FILE) void;
pub extern fn feof_unlocked(__stream: ?*FILE) c_int;
pub extern fn ferror_unlocked(__stream: ?*FILE) c_int;
pub extern fn perror(__s: [*c]const u8) void;
pub extern fn fileno(__stream: ?*FILE) c_int;
pub extern fn fileno_unlocked(__stream: ?*FILE) c_int;
pub extern fn pclose(__stream: ?*FILE) c_int;
pub extern fn popen(__command: [*c]const u8, __modes: [*c]const u8) ?*FILE;
pub extern fn ctermid(__s: [*c]u8) [*c]u8;
pub extern fn flockfile(__stream: ?*FILE) void;
pub extern fn ftrylockfile(__stream: ?*FILE) c_int;
pub extern fn funlockfile(__stream: ?*FILE) void;
pub extern fn __uflow(?*FILE) c_int;
pub extern fn __overflow(?*FILE, c_int) c_int;
pub const struct_dafsa = opaque {
};
pub const dafsa = struct_dafsa;
pub extern fn intern_fwd(ir: ?*interner) ?*const dafsa;
pub extern fn intern_fwd_mutable(ir: ?*interner) ?*const dafsa;
pub extern fn intern_create() ?*interner;
pub extern fn intern_free(ir: ?*interner) void;
pub extern fn intern_str(ir: ?*interner, str: [*c]const u8) u32;
pub extern fn intern_str_find(ir: ?*interner, str: [*c]const u8) u32;
pub extern fn intern_str_of(ir: ?*interner, sym_id: u32) [*c]const u8;
pub extern fn intern_save(ir: ?*interner, fwd_path: [*c]const u8, rev_path: [*c]const u8) c_int;
pub extern fn intern_load(fwd_path: [*c]const u8, rev_path: [*c]const u8) ?*interner;
pub extern fn intern_is_dirty(ir: ?*interner) c_int;
pub extern fn intern_clear_dirty(ir: ?*interner) void;
pub const struct_relation = opaque {
};
pub const relation = struct_relation;
pub const struct_dafsa_wal = opaque {};
pub const dafsa_wal = struct_dafsa_wal;
pub extern fn rel_create(arity: u8) ?*relation;
pub extern fn rel_open(path: [*c]const u8, arity: u8) ?*relation;
pub extern fn rel_open_writable(dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*relation;
pub extern fn rel_open_writable_idb(base_path: [*c]const u8, dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*relation;
pub extern fn rel_open_readonly(dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*relation;
pub extern fn rel_open_readonly_idb(base_path: [*c]const u8, dafsa_path: [*c]const u8, wal_path: [*c]const u8, arity: u8) ?*relation;
pub extern fn rel_save(rel: ?*const relation, path: [*c]const u8) c_int;
pub extern fn rel_free(rel: ?*relation) void;
pub extern fn rel_arity(rel: ?*const relation) u8;
pub extern fn rel_count(rel: ?*const relation) u64;
pub extern fn rel_dafsa(rel: ?*const relation) ?*const dafsa;
pub extern fn rel_rank(rel: ?*const relation, cols: [*c]const u32) u64;
pub extern fn rel_select(rel: ?*const relation, k: u64, cols_out: [*c]u32) c_int;
pub extern fn rel_range_count(rel: ?*const relation, lo: [*c]const u32, hi: [*c]const u32) u64;
pub extern fn rel_count_subtree(rel: ?*const relation) u64;
pub extern fn rel_rank_bound(rel: ?*const relation, leading: [*c]const u32, k: u8, cols: [*c]const u32) u64;
pub extern fn rel_select_bound(rel: ?*const relation, leading: [*c]const u32, k: u8, idx: u64, cols_out: [*c]u32) c_int;
pub extern fn rel_range_count_bound(rel: ?*const relation, leading: [*c]const u32, k: u8, lo: [*c]const u32, hi: [*c]const u32) u64;
pub extern fn rel_add(rel: ?*relation, cols: [*c]const u32) c_int;
pub extern fn rel_add_base(rel: ?*relation, cols: [*c]const u32) c_int;
pub extern fn rel_exact(rel: ?*const relation, cols: [*c]const u32) c_int;
pub extern fn rel_exact_base(rel: ?*const relation, cols: [*c]const u32) c_int;
pub extern fn rel_delete(rel: ?*relation, cols: [*c]const u32) c_int;
pub extern fn rel_delete_base(rel: ?*relation, cols: [*c]const u32) c_int;
pub extern fn rel_build_from_tupleset(rel: ?*relation, ts: ?*const struct_tuple_set) c_int;
pub extern fn rel_build_base_from_tupleset(rel: ?*relation, ts: ?*const struct_tuple_set) c_int;
pub extern fn rel_is_idb(rel: ?*const relation) c_int;
pub extern fn rel_is_dirty(rel: ?*const relation) c_int;
pub extern fn rel_reset_view(rel: ?*relation) c_int;
pub extern fn rel_save_base(rel: ?*const relation, path: [*c]const u8) c_int;
pub extern fn rel_wal_append_add(rel: ?*relation, key: [*c]const u8, key_len: u32) c_int;
pub extern fn rel_wal_append_del(rel: ?*relation, key: [*c]const u8, key_len: u32) c_int;
pub extern fn rel_wal_replay_into(rel: ?*relation) c_int;
pub extern fn rel_compact(rel: ?*relation, dafsa_path: [*c]const u8) c_int;
pub extern fn rel_wal_size(rel: ?*const relation) u64;
pub extern fn rel_dafsa_size(rel: ?*const relation) u64;
pub extern fn ts_sink_cb(cols: [*c]const u32, arity: u8, user: ?*anyopaque) c_int;
pub const rel_enum_cb = ?*const fn (cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn rel_prefix(rel: ?*const relation, leading: [*c]const u32, k: u8, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern fn rel_prefix_base(rel: ?*const relation, leading: [*c]const u32, k: u8, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern fn rel_has_col0(rel: ?*const relation, x: u32) c_int;
pub extern fn rel_range_each(rel: ?*const relation, lo: u32, hi: u32, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub const struct_sym_set = extern struct {
    keys: [*c]u32 = null,
    cap: c_int = 0,
    used: c_int = 0,
};
pub extern fn rel_pattern(rel: ?*const relation, dfa: [*c]const struct_regex_dfa, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern fn rel_filter_col(rel: ?*const relation, col: u8, set: [*c]const struct_sym_set, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub const struct_vrelation = opaque {
};
pub const vrelation = struct_vrelation;
pub const vrel_iter_cb = ?*const fn (variant: ?*relation, arity: u8, user: ?*anyopaque) callconv(.c) void;
pub extern fn vrel_create() ?*vrelation;
pub extern fn vrel_free(v: ?*vrelation) void;
pub extern fn vrel_variant(v: ?*vrelation, arity: u8) ?*relation;
pub extern fn vrel_variant_or_null(v: ?*const vrelation, arity: u8) ?*relation;
pub extern fn vrel_attach(v: ?*vrelation, arity: u8, r: ?*relation) c_int;
pub extern fn vrel_foreach(v: ?*vrelation, cb: vrel_iter_cb, user: ?*anyopaque) void;
pub extern fn vrel_any_idb(v: ?*const vrelation) c_int;
pub extern fn vrel_reset_views(v: ?*vrelation) c_int;
pub extern fn vrel_exact(v: ?*const vrelation, cols: [*c]const u32, arity: u8) c_int;
pub extern fn vrel_exact_base(v: ?*const vrelation, cols: [*c]const u32, arity: u8) c_int;
pub extern fn vrel_prefix(v: ?*const vrelation, leading: [*c]const u32, k: u8, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern fn vrel_prefix_base(v: ?*const vrelation, leading: [*c]const u32, k: u8, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern fn vrel_pattern(v: ?*const vrelation, dfa: [*c]const struct_regex_dfa, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern fn vrel_filter_col(v: ?*const vrelation, col: u8, set: [*c]const struct_sym_set, cb: rel_enum_cb, user: ?*anyopaque) c_long;
pub extern fn vrel_count(v: ?*const vrelation) u64;
pub const view_cache_slot = extern struct {
    rel_name: [64]u8 = @import("std").mem.zeroes([64]u8),
    view: ?*anyopaque = null,
    used: c_int = 0,
};
pub extern fn snapshot_read_current(db_dir: [*c]const u8) u32;
pub extern fn vcache_invalidate(vcache: [*c]view_cache_slot) void;
pub extern fn snapshot_query_scan(db_dir: [*c]const u8, snap_version: u32, vcache: [*c]view_cache_slot, goal_rel: [*c]const u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn view_prefix(view_handle: ?*anyopaque, arity: u8, leading: [*c]const u32, k: u8, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn view_open_cached(vcache: [*c]view_cache_slot, rel_name: [*c]const u8, sdir: [*c]const u8) ?*anyopaque;
pub extern fn view_pattern(view_handle: ?*anyopaque, arity: u8, dfa: [*c]const struct_regex_dfa, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn view_filter_col(view_handle: ?*anyopaque, arity: u8, col: u8, set: [*c]const struct_sym_set, cb: dl_tuple_cb, user: ?*anyopaque) c_long;
pub extern fn view_rank(view_handle: ?*anyopaque, arity: u8, cols: [*c]const u32) u64;
pub extern fn view_select(view_handle: ?*anyopaque, arity: u8, k: u64, cols_out: [*c]u32) c_int;
pub extern fn view_range_count(view_handle: ?*anyopaque, arity: u8, lo: [*c]const u32, hi: [*c]const u32) u64;
pub extern fn view_count(view_handle: ?*anyopaque) u64;
pub extern fn manifest_find_rel(sdir: [*c]const u8, rel_name: [*c]const u8, arity_out: [*c]u8) c_int;
pub extern fn manifest_find_rel_ex(sdir: [*c]const u8, rel_name: [*c]const u8, arity_out: [*c]u8, variadic_out: [*c]c_int) c_int;
pub extern fn manifest_find_variants(sdir: [*c]const u8, rel_name: [*c]const u8, present: [*c]u8) void;
pub const perm_index_entry = extern struct {
    rel_id: c_int = 0,
    arity: u8 = 0,
    perm: [8]u8 = @import("std").mem.zeroes([8]u8),
    pidx_rel: ?*struct_relation = null,
    dirty: c_int = 0,
};
pub extern fn dl_db_declare_perm(db: [*c]struct_dl_db, rel_id: c_int, arity: u8, perm: [*c]const u8) c_int;
pub extern fn dl_db_find_perm(db: [*c]struct_dl_db, rel_id: c_int, arity: u8, perm: [*c]const u8) c_int;
pub extern fn dl_db_get_perm(db: [*c]struct_dl_db, rel_id: c_int, perm_id: c_int) [*c]const u8;
pub extern fn dl_db_get_perm_rel(db: [*c]struct_dl_db, rel_id: c_int, perm_id: c_int) ?*struct_relation;
pub extern fn permindex_build(db: [*c]struct_dl_db, rel_id: c_int, perm_id: c_int) c_int;
pub extern fn permindex_build_dirty(db: [*c]struct_dl_db) c_int;
pub extern fn permindex_mark_dirty(db: [*c]struct_dl_db, rel_id: c_int) void;
pub extern fn permindex_free_all(db: [*c]struct_dl_db) void;
pub extern fn term_create() ?*termstore;
pub extern fn term_free(t: ?*termstore) void;
pub extern fn term_is_list(t: ?*const termstore, v: u32) c_int;
pub extern fn term_car(t: ?*const termstore, h: u32) u32;
pub extern fn term_cdr(t: ?*const termstore, h: u32) u32;
pub extern fn term_cons(t: ?*termstore, head: u32, tail: u32) u32;
pub extern fn term_append(t: ?*termstore, a: u32, b: u32) u32;
pub extern fn term_length(t: ?*const termstore, h: u32) u32;
pub extern fn term_node_count(t: ?*const termstore) u32;
pub extern fn term_is_dirty(t: ?*const termstore) c_int;
pub extern fn term_clear_dirty(t: ?*termstore) void;
pub extern fn term_save(t: ?*termstore, path: [*c]const u8) c_int;
pub extern fn term_load(path: [*c]const u8) ?*termstore;
pub const TOK_EOF: c_int = 0;
pub const TOK_IDENT: c_int = 1;
pub const TOK_VAR: c_int = 2;
pub const TOK_INT: c_int = 3;
pub const TOK_COLONMINUS: c_int = 4;
pub const TOK_COMMA: c_int = 5;
pub const TOK_DOT: c_int = 6;
pub const TOK_LPAREN: c_int = 7;
pub const TOK_RPAREN: c_int = 8;
pub const TOK_NOT: c_int = 9;
pub const TOK_AGGREGATE: c_int = 10;
pub const TOK_EQ: c_int = 11;
pub const TOK_LT: c_int = 12;
pub const TOK_LE: c_int = 13;
pub const TOK_GT: c_int = 14;
pub const TOK_GE: c_int = 15;
pub const TOK_NE: c_int = 16;
pub const TOK_PLUS: c_int = 17;
pub const TOK_MINUS: c_int = 18;
pub const TOK_STAR: c_int = 19;
pub const TOK_SLASH: c_int = 20;
pub const TOK_PERCENT: c_int = 21;
pub const TOK_TILDE: c_int = 22;
pub const TOK_STRING: c_int = 23;
pub const TOK_LBRACKET: c_int = 24;
pub const TOK_RBRACKET: c_int = 25;
pub const TOK_PIPE: c_int = 26;
pub const TOK_LIST: c_int = 27;
pub const token_kind = c_uint;
pub const struct_token = extern struct {
    kind: token_kind = @import("std").mem.zeroes(token_kind),
    off: u32 = 0,
    line: c_int = 0,
    col: c_int = 0,
    text: [*c]u8 = null,
    ival: u32 = 0,
    children: [*c][*c]struct_token = null,
    nchildren: c_int = 0,
    tail: [*c]struct_token = null,
};
pub const token = struct_token;
pub const EX_INT: c_int = 0;
pub const EX_VAR: c_int = 1;
pub const EX_BINOP: c_int = 2;
pub const expr_kind = c_uint;
pub const struct_expr = extern struct {
    kind: expr_kind = @import("std").mem.zeroes(expr_kind),
    ival: u32 = 0,
    @"var": [*c]u8 = null,
    op: u8 = 0,
    l: [*c]struct_expr = null,
    r: [*c]struct_expr = null,
};
pub const expr = struct_expr;
pub extern fn expr_clone(e: [*c]const expr) [*c]expr;
pub extern fn expr_free(e: [*c]expr) void;
pub const atom = extern struct {
    pred: [*c]u8 = null,
    off: u32 = 0,
    line: c_int = 0,
    col: c_int = 0,
    args: [*c][*c]token = null,
    nargs: c_int = 0,
    negated: c_int = 0,
    aggregate: c_int = 0,
    agg_op: [*c]token = null,
    pattern: [*c]u8 = null,
    pattern_col: c_int = 0,
    arith: [*c]expr = null,
};
pub const rule = extern struct {
    head: [*c]atom = null,
    off: u32 = 0,
    body: [*c][*c]atom = null,
    nbody: c_int = 0,
    has_negation: c_int = 0,
    has_aggregate: c_int = 0,
};
pub const struct_parser = opaque {
};
pub const parser = struct_parser;
pub extern fn parse_create(source: [*c]const u8) ?*parser;
pub extern fn parse_create_reporting(source: [*c]const u8) ?*parser;
pub extern fn parse_last_error(p: ?*const parser, off: [*c]u32) [*c]const u8;
pub extern fn parse_rules(p: ?*parser, n_rules: [*c]c_int) [*c][*c]rule;
pub extern fn parse_free(p: ?*parser) void;
pub extern fn rule_free(r: [*c]rule) void;
pub const struct_dafsa_view = opaque {
};
pub const dafsa_view = struct_dafsa_view;
pub const regex_dfa = struct_regex_dfa;
pub extern fn regex_compile(pattern: [*c]const u8) [*c]regex_dfa;
pub extern fn regex_dfa_free(dfa: [*c]regex_dfa) void;
pub const regex_walk_cb = ?*const fn (key_bytes: [*c]const u8, key_len: usize, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn regex_dfa_walk(d: ?*const dafsa, dfa: [*c]const regex_dfa, cb: regex_walk_cb, user: ?*anyopaque) c_long;
pub extern fn regex_dfa_walk_view(v: ?*const dafsa_view, dfa: [*c]const regex_dfa, cb: regex_walk_cb, user: ?*anyopaque) c_long;
pub const sym_walk_cb = ?*const fn (sym_id: u32, user: ?*anyopaque) callconv(.c) c_int;
pub const sym_set = struct_sym_set;
pub extern fn symset_init(s: [*c]sym_set) c_int;
pub extern fn symset_free(s: [*c]sym_set) void;
pub extern fn symset_add(s: [*c]sym_set, sym_id: u32) c_int;
pub extern fn symset_contains(s: [*c]const sym_set, sym_id: u32) c_int;
pub extern fn symbols_dfa_walk(d: ?*const dafsa, dfa: [*c]const regex_dfa, cb: sym_walk_cb, user: ?*anyopaque) c_long;
pub extern fn symbols_dfa_walk_view(v: ?*const dafsa_view, dfa: [*c]const regex_dfa, cb: sym_walk_cb, user: ?*anyopaque) c_long;
pub const OP_HALT: c_int = 0;
pub const OP_SCAN: c_int = 1;
pub const OP_LOOKUP: c_int = 2;
pub const OP_EQ: c_int = 3;
pub const OP_EQ_CONST: c_int = 4;
pub const OP_PROJECT: c_int = 5;
pub const OP_OPEN_REL: c_int = 6;
pub const OP_NEG_CHECK: c_int = 7;
pub const OP_AGG_ACC: c_int = 8;
pub const OP_AGG_EMIT: c_int = 9;
pub const OP_WALK: c_int = 10;
pub const OP_LOOKUP_PERM: c_int = 11;
pub const OP_HASH_JOIN: c_int = 12;
pub const OP_CMP: c_int = 13;
pub const OP_ARITH: c_int = 14;
pub const OP_STR_FILTER: c_int = 15;
pub const OP_STR_LEN: c_int = 16;
pub const OP_STR_BIND: c_int = 17;
pub const OP_MAT_BEGIN: c_int = 18;
pub const OP_MAT_JOIN: c_int = 19;
pub const OP_LIST_CONS: c_int = 20;
pub const OP_LIST_CAR: c_int = 21;
pub const OP_LIST_CDR: c_int = 22;
pub const OP_LIST_APPEND: c_int = 23;
pub const OP_LIST_MEMBER: c_int = 24;
pub const OP_RANGE: c_int = 25;
pub const vm_opcode = c_uint;
pub const vm_instr = extern struct {
    op: u8 = 0,
    a: u8 = 0,
    b: u8 = 0,
    c: u8 = 0,
    imm: u32 = 0,
    slots: [8]u8 = @import("std").mem.zeroes([8]u8),
    body_idx: u8 = 0,
};
pub const var_info = extern struct {
    name: [*c]u8 = null,
    slot: u8 = 0,
};
pub const compiled_rule = extern struct {
    head_pred: [*c]u8 = null,
    head_rel_id: u8 = 0,
    n_vars: u8 = 0,
    vars: [*c]var_info = null,
    n_instrs: c_int = 0,
    instrs: [*c]vm_instr = null,
    stratum: c_int = 0,
    is_recursive: c_int = 0,
    has_aggregate: c_int = 0,
    n_patterns: c_int = 0,
    patterns: [*c][*c]regex_dfa = null,
};
pub extern fn compile_rules(db: [*c]dl_db, rules: [*c][*c]rule, n_rules: c_int, out_rules: [*c][*c][*c]compiled_rule, out_n: [*c]c_int) c_int;
pub extern fn compiled_rule_free(cr: [*c]compiled_rule) void;
pub extern fn compile_last_error(off: [*c]u32) [*c]const u8;
pub extern var g_bushy: c_int;
pub extern var g_reorder: c_int;
pub extern var g_perm_select: c_int;
pub extern var g_perm_card_threshold: c_int;
pub const rel_entry = extern struct {
    name: [*c]u8 = null,
    kind: u8 = 0,
    arity: u8 = 0,
    rel: ?*relation = null,
    vrel: ?*vrelation = null,
};
pub extern fn db_entry_is_variadic(e: [*c]const rel_entry) c_int;
pub extern fn db_has_variadic(db: [*c]const dl_db) c_int;
pub extern fn db_has_list_builtin(db: [*c]const dl_db) c_int;
pub extern fn db_has_range_builtin(db: [*c]const dl_db) c_int;
pub extern fn dl_iter_open_live(rel: ?*relation, leading: [*c]const u32, k: u8) ?*dl_iter;
pub extern fn db_entry_variant_ro(e: [*c]const rel_entry, arity: u8) ?*relation;
pub extern fn db_entry_variant_rw(e: [*c]rel_entry, arity: u8) ?*relation;
pub extern fn db_rel_at_arity_ro(db: [*c]const dl_db, rel_id: c_int, arity: u8) ?*relation;
pub extern fn db_rel_at_arity_rw(db: [*c]dl_db, rel_id: c_int, arity: u8) ?*relation;
pub extern fn dl_ensure_variant(db: [*c]dl_db, rel_id: c_int, arity: u8) ?*relation;
pub extern fn tokenize(text: [*c]const u8, n_out: [*c]usize) [*c][*c]u8;
pub extern fn token_free(tokens: [*c][*c]u8) void;
pub extern fn aux_index_ensure_postings(db: [*c]dl_db) c_int;
pub extern fn aux_index_add_posting(db: [*c]dl_db, term_sym: u32, obs_id: u32) c_int;
pub const dl_search_cb = ?*const fn (obs_id: u32, score: c_int, user: ?*anyopaque) callconv(.c) c_int;
pub extern fn dl_search(db: [*c]dl_db, terms: [*c]const u32, n_terms: c_int, cb: dl_search_cb, user: ?*anyopaque) c_long;
pub extern fn dl_search_top(db: [*c]dl_db, terms: [*c]const u32, n_terms: c_int, obs_ids_out: [*c]u32, scores_out: [*c]c_int, limit: c_int) c_int;
pub extern fn dl_search_version(db: [*c]dl_db, version: u32, terms: [*c]const u32, n_terms: c_int, cb: dl_search_cb, user: ?*anyopaque) c_long;
pub extern fn dl_search_top_version(db: [*c]dl_db, version: u32, terms: [*c]const u32, n_terms: c_int, obs_ids_out: [*c]u32, scores_out: [*c]c_int, limit: c_int) c_int;
pub extern fn dl_index_observations(db: [*c]dl_db) c_long;

pub const __VERSION__ = "Aro aro-zig";
pub const __Aro__ = "";
pub const __STDC__ = @as(c_int, 1);
pub const __STDC_HOSTED__ = @as(c_int, 1);
pub const __STDC_UTF_16__ = @as(c_int, 1);
pub const __STDC_UTF_32__ = @as(c_int, 1);
pub const __STDC_EMBED_NOT_FOUND__ = @as(c_int, 0);
pub const __STDC_EMBED_FOUND__ = @as(c_int, 1);
pub const __STDC_EMBED_EMPTY__ = @as(c_int, 2);
pub const __STDC_VERSION__ = @as(c_long, 201710);
pub const __GNUC__ = @as(c_int, 7);
pub const __GNUC_MINOR__ = @as(c_int, 1);
pub const __GNUC_PATCHLEVEL__ = @as(c_int, 0);
pub const __ARO_EMULATE_NO__ = @as(c_int, 0);
pub const __ARO_EMULATE_CLANG__ = @as(c_int, 1);
pub const __ARO_EMULATE_GCC__ = @as(c_int, 2);
pub const __ARO_EMULATE_MSVC__ = @as(c_int, 3);
pub const __ARO_EMULATE__ = __ARO_EMULATE_GCC__;
pub inline fn __building_module(x: anytype) @TypeOf(@as(c_int, 0)) {
    _ = &x;
    return @as(c_int, 0);
}
pub const linux = @as(c_int, 1);
pub const __linux = @as(c_int, 1);
pub const __linux__ = @as(c_int, 1);
pub const unix = @as(c_int, 1);
pub const __unix = @as(c_int, 1);
pub const __unix__ = @as(c_int, 1);
pub const __code_model_small__ = @as(c_int, 1);
pub const __amd64__ = @as(c_int, 1);
pub const __amd64 = @as(c_int, 1);
pub const __x86_64__ = @as(c_int, 1);
pub const __x86_64 = @as(c_int, 1);
pub const __SEG_GS = @as(c_int, 1);
pub const __SEG_FS = @as(c_int, 1);
pub const __seg_gs = @compileError("unable to translate macro: undefined identifier `address_space`"); // <builtin>:33:9
pub const __seg_fs = @compileError("unable to translate macro: undefined identifier `address_space`"); // <builtin>:34:9
pub const __LAHF_SAHF__ = @as(c_int, 1);
pub const __AES__ = @as(c_int, 1);
pub const __PCLMUL__ = @as(c_int, 1);
pub const __LZCNT__ = @as(c_int, 1);
pub const __RDRND__ = @as(c_int, 1);
pub const __FSGSBASE__ = @as(c_int, 1);
pub const __BMI__ = @as(c_int, 1);
pub const __BMI2__ = @as(c_int, 1);
pub const __POPCNT__ = @as(c_int, 1);
pub const __PRFCHW__ = @as(c_int, 1);
pub const __RDSEED__ = @as(c_int, 1);
pub const __ADX__ = @as(c_int, 1);
pub const __MWAITX__ = @as(c_int, 1);
pub const __MOVBE__ = @as(c_int, 1);
pub const __SSE4A__ = @as(c_int, 1);
pub const __FMA__ = @as(c_int, 1);
pub const __F16C__ = @as(c_int, 1);
pub const __SHA__ = @as(c_int, 1);
pub const __FXSR__ = @as(c_int, 1);
pub const __XSAVE__ = @as(c_int, 1);
pub const __XSAVEOPT__ = @as(c_int, 1);
pub const __XSAVEC__ = @as(c_int, 1);
pub const __XSAVES__ = @as(c_int, 1);
pub const __CLFLUSHOPT__ = @as(c_int, 1);
pub const __CLWB__ = @as(c_int, 1);
pub const __WBNOINVD__ = @as(c_int, 1);
pub const __CLZERO__ = @as(c_int, 1);
pub const __RDPID__ = @as(c_int, 1);
pub const __RDPRU__ = @as(c_int, 1);
pub const __CRC32__ = @as(c_int, 1);
pub const __AVX2__ = @as(c_int, 1);
pub const __AVX__ = @as(c_int, 1);
pub const __SSE4_2__ = @as(c_int, 1);
pub const __SSE4_1__ = @as(c_int, 1);
pub const __SSSE3__ = @as(c_int, 1);
pub const __SSE3__ = @as(c_int, 1);
pub const __SSE2__ = @as(c_int, 1);
pub const __SSE__ = @as(c_int, 1);
pub const __SSE_MATH__ = @as(c_int, 1);
pub const __MMX__ = @as(c_int, 1);
pub const __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8 = @as(c_int, 1);
pub const __SIZEOF_FLOAT128__ = @as(c_int, 16);
pub const _LP64 = @as(c_int, 1);
pub const __LP64__ = @as(c_int, 1);
pub const __FLOAT128__ = @as(c_int, 1);
pub const __ORDER_LITTLE_ENDIAN__ = @as(c_int, 1234);
pub const __ORDER_BIG_ENDIAN__ = @as(c_int, 4321);
pub const __ORDER_PDP_ENDIAN__ = @as(c_int, 3412);
pub const __BYTE_ORDER__ = __ORDER_LITTLE_ENDIAN__;
pub const __LITTLE_ENDIAN__ = @as(c_int, 1);
pub const __ELF__ = @as(c_int, 1);
pub const __ATOMIC_RELAXED = @as(c_int, 0);
pub const __ATOMIC_CONSUME = @as(c_int, 1);
pub const __ATOMIC_ACQUIRE = @as(c_int, 2);
pub const __ATOMIC_RELEASE = @as(c_int, 3);
pub const __ATOMIC_ACQ_REL = @as(c_int, 4);
pub const __ATOMIC_SEQ_CST = @as(c_int, 5);
pub const __ATOMIC_BOOL_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_CHAR_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_CHAR16_T_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_CHAR32_T_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_WCHAR_T_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_WINT_T_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_SHORT_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_INT_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_LONG_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_LLONG_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_POINTER_LOCK_FREE = @as(c_int, 1);
pub const __WINT_UNSIGNED__ = @as(c_int, 1);
pub const __CHAR_BIT__ = @as(c_int, 8);
pub const __BOOL_WIDTH__ = @as(c_int, 8);
pub const __SCHAR_MAX__ = @as(c_int, 127);
pub const __SCHAR_WIDTH__ = @as(c_int, 8);
pub const __SHRT_MAX__ = @as(c_int, 32767);
pub const __SHRT_WIDTH__ = @as(c_int, 16);
pub const __INT_MAX__ = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const __INT_WIDTH__ = @as(c_int, 32);
pub const __LONG_MAX__ = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const __LONG_WIDTH__ = @as(c_int, 64);
pub const __LONG_LONG_MAX__ = @as(c_longlong, 9223372036854775807);
pub const __LONG_LONG_WIDTH__ = @as(c_int, 64);
pub const __WCHAR_MAX__ = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const __WCHAR_WIDTH__ = @as(c_int, 32);
pub const __WINT_MAX__ = __helpers.promoteIntLiteral(c_uint, 4294967295, .decimal);
pub const __WINT_WIDTH__ = @as(c_int, 32);
pub const __INTMAX_MAX__ = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const __INTMAX_WIDTH__ = @as(c_int, 64);
pub const __SIZE_MAX__ = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const __SIZE_WIDTH__ = @as(c_int, 64);
pub const __UINTMAX_MAX__ = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const __UINTMAX_WIDTH__ = @as(c_int, 64);
pub const __PTRDIFF_MAX__ = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const __PTRDIFF_WIDTH__ = @as(c_int, 64);
pub const __INTPTR_MAX__ = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const __INTPTR_WIDTH__ = @as(c_int, 64);
pub const __UINTPTR_MAX__ = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const __UINTPTR_WIDTH__ = @as(c_int, 64);
pub const __SIG_ATOMIC_MAX__ = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const __SIG_ATOMIC_WIDTH__ = @as(c_int, 32);
pub const __BITINT_MAXWIDTH__ = __helpers.promoteIntLiteral(c_int, 65535, .decimal);
pub const __SIZEOF_FLOAT__ = @as(c_int, 4);
pub const __SIZEOF_DOUBLE__ = @as(c_int, 8);
pub const __SIZEOF_LONG_DOUBLE__ = @as(c_int, 10);
pub const __SIZEOF_SHORT__ = @as(c_int, 2);
pub const __SIZEOF_INT__ = @as(c_int, 4);
pub const __SIZEOF_LONG__ = @as(c_int, 8);
pub const __SIZEOF_LONG_LONG__ = @as(c_int, 8);
pub const __SIZEOF_POINTER__ = @as(c_int, 8);
pub const __SIZEOF_PTRDIFF_T__ = @as(c_int, 8);
pub const __SIZEOF_SIZE_T__ = @as(c_int, 8);
pub const __SIZEOF_WCHAR_T__ = @as(c_int, 4);
pub const __SIZEOF_WINT_T__ = @as(c_int, 4);
pub const __SIZEOF_INT128__ = @as(c_int, 16);
pub const __INTPTR_TYPE__ = c_long;
pub const __UINTPTR_TYPE__ = c_ulong;
pub const __INTMAX_TYPE__ = c_long;
pub const __INTMAX_C_SUFFIX__ = @compileError("unable to translate macro: undefined identifier `L`"); // <builtin>:152:9
pub const __INTMAX_C = __helpers.L_SUFFIX;
pub const __UINTMAX_TYPE__ = c_ulong;
pub const __UINTMAX_C_SUFFIX__ = @compileError("unable to translate macro: undefined identifier `UL`"); // <builtin>:155:9
pub const __UINTMAX_C = __helpers.UL_SUFFIX;
pub const __PTRDIFF_TYPE__ = c_long;
pub const __SIZE_TYPE__ = c_ulong;
pub const __WCHAR_TYPE__ = c_int;
pub const __WINT_TYPE__ = c_uint;
pub const __CHAR16_TYPE__ = c_ushort;
pub const __CHAR32_TYPE__ = c_uint;
pub const __INT8_TYPE__ = i8;
pub const __INT8_FMTd__ = "hhd";
pub const __INT8_FMTi__ = "hhi";
pub const __INT8_C_SUFFIX__ = "";
pub inline fn __INT8_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub const __INT16_TYPE__ = c_short;
pub const __INT16_FMTd__ = "hd";
pub const __INT16_FMTi__ = "hi";
pub const __INT16_C_SUFFIX__ = "";
pub inline fn __INT16_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub const __INT32_TYPE__ = c_int;
pub const __INT32_FMTd__ = "d";
pub const __INT32_FMTi__ = "i";
pub const __INT32_C_SUFFIX__ = "";
pub inline fn __INT32_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub const __INT64_TYPE__ = c_long;
pub const __INT64_FMTd__ = "ld";
pub const __INT64_FMTi__ = "li";
pub const __INT64_C_SUFFIX__ = @compileError("unable to translate macro: undefined identifier `L`"); // <builtin>:181:9
pub const __UINT8_TYPE__ = u8;
pub const __UINT8_FMTo__ = "hho";
pub const __UINT8_FMTu__ = "hhu";
pub const __UINT8_FMTx__ = "hhx";
pub const __UINT8_FMTX__ = "hhX";
pub const __UINT8_C_SUFFIX__ = "";
pub inline fn __UINT8_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub const __UINT8_MAX__ = @as(c_int, 255);
pub const __INT8_MAX__ = @as(c_int, 127);
pub const __UINT16_TYPE__ = c_ushort;
pub const __UINT16_FMTo__ = "ho";
pub const __UINT16_FMTu__ = "hu";
pub const __UINT16_FMTx__ = "hx";
pub const __UINT16_FMTX__ = "hX";
pub const __UINT16_C_SUFFIX__ = "";
pub inline fn __UINT16_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub const __UINT16_MAX__ = __helpers.promoteIntLiteral(c_int, 65535, .decimal);
pub const __INT16_MAX__ = @as(c_int, 32767);
pub const __UINT32_TYPE__ = c_uint;
pub const __UINT32_FMTo__ = "o";
pub const __UINT32_FMTu__ = "u";
pub const __UINT32_FMTx__ = "x";
pub const __UINT32_FMTX__ = "X";
pub const __UINT32_C_SUFFIX__ = @compileError("unable to translate macro: undefined identifier `U`"); // <builtin>:206:9
pub const __UINT32_C = __helpers.U_SUFFIX;
pub const __UINT32_MAX__ = __helpers.promoteIntLiteral(c_uint, 4294967295, .decimal);
pub const __INT32_MAX__ = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const __UINT64_TYPE__ = c_ulong;
pub const __UINT64_FMTo__ = "lo";
pub const __UINT64_FMTu__ = "lu";
pub const __UINT64_FMTx__ = "lx";
pub const __UINT64_FMTX__ = "lX";
pub const __UINT64_C_SUFFIX__ = @compileError("unable to translate macro: undefined identifier `UL`"); // <builtin>:215:9
pub const __UINT64_MAX__ = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const __INT64_MAX__ = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const __INT_LEAST8_TYPE__ = i8;
pub const __INT_LEAST8_MAX__ = @as(c_int, 127);
pub const __INT_LEAST8_WIDTH__ = @as(c_int, 8);
pub const INT_LEAST8_FMTd__ = "hhd";
pub const INT_LEAST8_FMTi__ = "hhi";
pub const __UINT_LEAST8_TYPE__ = u8;
pub const __UINT_LEAST8_MAX__ = @as(c_int, 255);
pub const UINT_LEAST8_FMTo__ = "hho";
pub const UINT_LEAST8_FMTu__ = "hhu";
pub const UINT_LEAST8_FMTx__ = "hhx";
pub const UINT_LEAST8_FMTX__ = "hhX";
pub const __INT_FAST8_TYPE__ = i8;
pub const __INT_FAST8_MAX__ = @as(c_int, 127);
pub const __INT_FAST8_WIDTH__ = @as(c_int, 8);
pub const INT_FAST8_FMTd__ = "hhd";
pub const INT_FAST8_FMTi__ = "hhi";
pub const __UINT_FAST8_TYPE__ = u8;
pub const __UINT_FAST8_MAX__ = @as(c_int, 255);
pub const UINT_FAST8_FMTo__ = "hho";
pub const UINT_FAST8_FMTu__ = "hhu";
pub const UINT_FAST8_FMTx__ = "hhx";
pub const UINT_FAST8_FMTX__ = "hhX";
pub const __INT_LEAST16_TYPE__ = c_short;
pub const __INT_LEAST16_MAX__ = @as(c_int, 32767);
pub const __INT_LEAST16_WIDTH__ = @as(c_int, 16);
pub const INT_LEAST16_FMTd__ = "hd";
pub const INT_LEAST16_FMTi__ = "hi";
pub const __UINT_LEAST16_TYPE__ = c_ushort;
pub const __UINT_LEAST16_MAX__ = __helpers.promoteIntLiteral(c_int, 65535, .decimal);
pub const UINT_LEAST16_FMTo__ = "ho";
pub const UINT_LEAST16_FMTu__ = "hu";
pub const UINT_LEAST16_FMTx__ = "hx";
pub const UINT_LEAST16_FMTX__ = "hX";
pub const __INT_FAST16_TYPE__ = c_short;
pub const __INT_FAST16_MAX__ = @as(c_int, 32767);
pub const __INT_FAST16_WIDTH__ = @as(c_int, 16);
pub const INT_FAST16_FMTd__ = "hd";
pub const INT_FAST16_FMTi__ = "hi";
pub const __UINT_FAST16_TYPE__ = c_ushort;
pub const __UINT_FAST16_MAX__ = __helpers.promoteIntLiteral(c_int, 65535, .decimal);
pub const UINT_FAST16_FMTo__ = "ho";
pub const UINT_FAST16_FMTu__ = "hu";
pub const UINT_FAST16_FMTx__ = "hx";
pub const UINT_FAST16_FMTX__ = "hX";
pub const __INT_LEAST32_TYPE__ = c_int;
pub const __INT_LEAST32_MAX__ = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const __INT_LEAST32_WIDTH__ = @as(c_int, 32);
pub const INT_LEAST32_FMTd__ = "d";
pub const INT_LEAST32_FMTi__ = "i";
pub const __UINT_LEAST32_TYPE__ = c_uint;
pub const __UINT_LEAST32_MAX__ = __helpers.promoteIntLiteral(c_uint, 4294967295, .decimal);
pub const UINT_LEAST32_FMTo__ = "o";
pub const UINT_LEAST32_FMTu__ = "u";
pub const UINT_LEAST32_FMTx__ = "x";
pub const UINT_LEAST32_FMTX__ = "X";
pub const __INT_FAST32_TYPE__ = c_int;
pub const __INT_FAST32_MAX__ = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const __INT_FAST32_WIDTH__ = @as(c_int, 32);
pub const INT_FAST32_FMTd__ = "d";
pub const INT_FAST32_FMTi__ = "i";
pub const __UINT_FAST32_TYPE__ = c_uint;
pub const __UINT_FAST32_MAX__ = __helpers.promoteIntLiteral(c_uint, 4294967295, .decimal);
pub const UINT_FAST32_FMTo__ = "o";
pub const UINT_FAST32_FMTu__ = "u";
pub const UINT_FAST32_FMTx__ = "x";
pub const UINT_FAST32_FMTX__ = "X";
pub const __INT_LEAST64_TYPE__ = c_long;
pub const __INT_LEAST64_MAX__ = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const __INT_LEAST64_WIDTH__ = @as(c_int, 64);
pub const INT_LEAST64_FMTd__ = "ld";
pub const INT_LEAST64_FMTi__ = "li";
pub const __UINT_LEAST64_TYPE__ = c_ulong;
pub const __UINT_LEAST64_MAX__ = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const UINT_LEAST64_FMTo__ = "lo";
pub const UINT_LEAST64_FMTu__ = "lu";
pub const UINT_LEAST64_FMTx__ = "lx";
pub const UINT_LEAST64_FMTX__ = "lX";
pub const __INT_FAST64_TYPE__ = c_long;
pub const __INT_FAST64_MAX__ = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const __INT_FAST64_WIDTH__ = @as(c_int, 64);
pub const INT_FAST64_FMTd__ = "ld";
pub const INT_FAST64_FMTi__ = "li";
pub const __UINT_FAST64_TYPE__ = c_ulong;
pub const __UINT_FAST64_MAX__ = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const UINT_FAST64_FMTo__ = "lo";
pub const UINT_FAST64_FMTu__ = "lu";
pub const UINT_FAST64_FMTx__ = "lx";
pub const UINT_FAST64_FMTX__ = "lX";
pub const __FLT16_DENORM_MIN__ = @as(f16, 5.9604644775390625e-8);
pub const __FLT16_HAS_DENORM__ = "";
pub const __FLT16_DIG__ = @as(c_int, 3);
pub const __FLT16_DECIMAL_DIG__ = @as(c_int, 5);
pub const __FLT16_EPSILON__ = @as(f16, 9.765625e-4);
pub const __FLT16_HAS_INFINITY__ = "";
pub const __FLT16_HAS_QUIET_NAN__ = "";
pub const __FLT16_MANT_DIG__ = @as(c_int, 11);
pub const __FLT16_MAX_10_EXP__ = @as(c_int, 4);
pub const __FLT16_MAX_EXP__ = @as(c_int, 16);
pub const __FLT16_MAX__ = @as(f16, 6.5504e+4);
pub const __FLT16_MIN_10_EXP__ = -@as(c_int, 4);
pub const __FLT16_MIN_EXP__ = -@as(c_int, 13);
pub const __FLT16_MIN__ = @as(f16, 6.103515625e-5);
pub const __FLT_DENORM_MIN__ = @as(f32, 1.40129846e-45);
pub const __FLT_HAS_DENORM__ = "";
pub const __FLT_DIG__ = @as(c_int, 6);
pub const __FLT_DECIMAL_DIG__ = @as(c_int, 9);
pub const __FLT_EPSILON__ = @as(f32, 1.19209290e-7);
pub const __FLT_HAS_INFINITY__ = "";
pub const __FLT_HAS_QUIET_NAN__ = "";
pub const __FLT_MANT_DIG__ = @as(c_int, 24);
pub const __FLT_MAX_10_EXP__ = @as(c_int, 38);
pub const __FLT_MAX_EXP__ = @as(c_int, 128);
pub const __FLT_MAX__ = @as(f32, 3.40282347e+38);
pub const __FLT_MIN_10_EXP__ = -@as(c_int, 37);
pub const __FLT_MIN_EXP__ = -@as(c_int, 125);
pub const __FLT_MIN__ = @as(f32, 1.17549435e-38);
pub const __DBL_DENORM_MIN__ = @as(f64, 4.9406564584124654e-324);
pub const __DBL_HAS_DENORM__ = "";
pub const __DBL_DIG__ = @as(c_int, 15);
pub const __DBL_DECIMAL_DIG__ = @as(c_int, 17);
pub const __DBL_EPSILON__ = @as(f64, 2.2204460492503131e-16);
pub const __DBL_HAS_INFINITY__ = "";
pub const __DBL_HAS_QUIET_NAN__ = "";
pub const __DBL_MANT_DIG__ = @as(c_int, 53);
pub const __DBL_MAX_10_EXP__ = @as(c_int, 308);
pub const __DBL_MAX_EXP__ = @as(c_int, 1024);
pub const __DBL_MAX__ = @as(f64, 1.7976931348623157e+308);
pub const __DBL_MIN_10_EXP__ = -@as(c_int, 307);
pub const __DBL_MIN_EXP__ = -@as(c_int, 1021);
pub const __DBL_MIN__ = @as(f64, 2.2250738585072014e-308);
pub const __LDBL_DENORM_MIN__ = @as(c_longdouble, 3.64519953188247460253e-4951);
pub const __LDBL_HAS_DENORM__ = "";
pub const __LDBL_DIG__ = @as(c_int, 18);
pub const __LDBL_DECIMAL_DIG__ = @as(c_int, 21);
pub const __LDBL_EPSILON__ = @as(c_longdouble, 1.08420217248550443401e-19);
pub const __LDBL_HAS_INFINITY__ = "";
pub const __LDBL_HAS_QUIET_NAN__ = "";
pub const __LDBL_MANT_DIG__ = @as(c_int, 64);
pub const __LDBL_MAX_10_EXP__ = @as(c_int, 4932);
pub const __LDBL_MAX_EXP__ = @as(c_int, 16384);
pub const __LDBL_MAX__ = @as(c_longdouble, 1.18973149535723176502e+4932);
pub const __LDBL_MIN_10_EXP__ = -@as(c_int, 4931);
pub const __LDBL_MIN_EXP__ = -@as(c_int, 16381);
pub const __LDBL_MIN__ = @as(c_longdouble, 3.36210314311209350626e-4932);
pub const __FLT_EVAL_METHOD__ = @as(c_int, 0);
pub const __FLT_RADIX__ = @as(c_int, 2);
pub const __DECIMAL_DIG__ = __LDBL_DECIMAL_DIG__;
pub const DL_INTERNAL_H = "";
pub const DL_H = "";
pub const __STDC_VERSION_STDDEF_H__ = @as(c_long, 202311);
pub const NULL = __helpers.cast(?*anyopaque, @as(c_int, 0));
pub const offsetof = @compileError("unable to translate macro: undefined identifier `__builtin_offsetof`"); // /home/jaye/.local/lib/zig-0.16.0/lib/compiler/aro/include/stddef.h:18:9
pub const _STDINT_H = @as(c_int, 1);
pub const _FEATURES_H = @as(c_int, 1);
pub const __KERNEL_STRICT_NAMES = "";
pub inline fn __GNUC_PREREQ(maj: anytype, min: anytype) @TypeOf(((__GNUC__ << @as(c_int, 16)) + __GNUC_MINOR__) >= ((maj << @as(c_int, 16)) + min)) {
    _ = &maj;
    _ = &min;
    return ((__GNUC__ << @as(c_int, 16)) + __GNUC_MINOR__) >= ((maj << @as(c_int, 16)) + min);
}
pub inline fn __glibc_clang_prereq(maj: anytype, min: anytype) @TypeOf(@as(c_int, 0)) {
    _ = &maj;
    _ = &min;
    return @as(c_int, 0);
}
pub const __GLIBC_USE = @compileError("unable to translate macro: undefined identifier `__GLIBC_USE_`"); // /usr/include/features.h:197:9
pub const _DEFAULT_SOURCE = @as(c_int, 1);
pub const __GLIBC_USE_ISOC2Y = @as(c_int, 0);
pub const __GLIBC_USE_ISOC23 = @as(c_int, 0);
pub const __USE_ISOC11 = @as(c_int, 1);
pub const __USE_POSIX_IMPLICITLY = @as(c_int, 1);
pub const _POSIX_SOURCE = @as(c_int, 1);
pub const _POSIX_C_SOURCE = @as(c_long, 202405);
pub const __USE_POSIX = @as(c_int, 1);
pub const __USE_POSIX2 = @as(c_int, 1);
pub const __USE_POSIX199309 = @as(c_int, 1);
pub const __USE_POSIX199506 = @as(c_int, 1);
pub const __USE_XOPEN2K = @as(c_int, 1);
pub const __USE_ISOC95 = @as(c_int, 1);
pub const __USE_ISOC99 = @as(c_int, 1);
pub const __USE_XOPEN2K8 = @as(c_int, 1);
pub const _ATFILE_SOURCE = @as(c_int, 1);
pub const __USE_XOPEN2K24 = @as(c_int, 1);
pub const __WORDSIZE = @as(c_int, 64);
pub const __WORDSIZE_TIME64_COMPAT32 = @as(c_int, 1);
pub const __SYSCALL_WORDSIZE = @as(c_int, 64);
pub const __TIMESIZE = __WORDSIZE;
pub const __USE_TIME_BITS64 = @as(c_int, 1);
pub const __USE_MISC = @as(c_int, 1);
pub const __USE_ATFILE = @as(c_int, 1);
pub const __USE_FORTIFY_LEVEL = @as(c_int, 0);
pub const __GLIBC_USE_DEPRECATED_GETS = @as(c_int, 0);
pub const __GLIBC_USE_DEPRECATED_SCANF = @as(c_int, 0);
pub const __GLIBC_USE_C23_STRTOL = @as(c_int, 0);
pub const _STDC_PREDEF_H = @as(c_int, 1);
pub const __STDC_IEC_559__ = @as(c_int, 1);
pub const __STDC_IEC_60559_BFP__ = @as(c_long, 201404);
pub const __STDC_IEC_559_COMPLEX__ = @as(c_int, 1);
pub const __STDC_IEC_60559_COMPLEX__ = @as(c_long, 201404);
pub const __STDC_ISO_10646__ = @as(c_long, 201706);
pub const __GNU_LIBRARY__ = @as(c_int, 6);
pub const __GLIBC__ = @as(c_int, 2);
pub const __GLIBC_MINOR__ = @as(c_int, 44);
pub inline fn __GLIBC_PREREQ(maj: anytype, min: anytype) @TypeOf(((__GLIBC__ << @as(c_int, 16)) + __GLIBC_MINOR__) >= ((maj << @as(c_int, 16)) + min)) {
    _ = &maj;
    _ = &min;
    return ((__GLIBC__ << @as(c_int, 16)) + __GLIBC_MINOR__) >= ((maj << @as(c_int, 16)) + min);
}
pub const _SYS_CDEFS_H = @as(c_int, 1);
pub const __glibc_has_attribute = @compileError("unable to translate macro: undefined identifier `__has_attribute`"); // /usr/include/sys/cdefs.h:45:10
pub inline fn __glibc_has_builtin(name: anytype) @TypeOf(__builtin.has_builtin(name)) {
    _ = &name;
    return __builtin.has_builtin(name);
}
pub const __glibc_has_extension = @compileError("unable to translate macro: undefined identifier `__has_extension`"); // /usr/include/sys/cdefs.h:55:10
pub const __LEAF = @compileError("unable to translate macro: undefined identifier `__leaf__`"); // /usr/include/sys/cdefs.h:65:11
pub const __LEAF_ATTR = @compileError("unable to translate macro: undefined identifier `__leaf__`"); // /usr/include/sys/cdefs.h:66:11
pub const __THROW = @compileError("unable to translate macro: undefined identifier `__nothrow__`"); // /usr/include/sys/cdefs.h:79:11
pub const __THROWNL = @compileError("unable to translate macro: undefined identifier `__nothrow__`"); // /usr/include/sys/cdefs.h:80:11
pub const __NTH = @compileError("unable to translate macro: undefined identifier `__nothrow__`"); // /usr/include/sys/cdefs.h:81:11
pub const __NTHNL = @compileError("unable to translate macro: undefined identifier `__nothrow__`"); // /usr/include/sys/cdefs.h:82:11
pub const __COLD = @compileError("unable to translate macro: undefined identifier `__cold__`"); // /usr/include/sys/cdefs.h:102:11
pub inline fn __P(args: anytype) @TypeOf(args) {
    _ = &args;
    return args;
}
pub inline fn __PMT(args: anytype) @TypeOf(args) {
    _ = &args;
    return args;
}
pub const __CONCAT = @compileError("unable to translate C expr: unexpected token '##'"); // /usr/include/sys/cdefs.h:131:9
pub const __STRING = @compileError("unable to translate C expr: unexpected token ''"); // /usr/include/sys/cdefs.h:132:9
pub const __ptr_t = ?*anyopaque;
pub const __BEGIN_DECLS = "";
pub const __END_DECLS = "";
pub const __attribute_overloadable__ = "";
pub inline fn __bos(ptr: anytype) @TypeOf(__builtin.object_size(ptr, __USE_FORTIFY_LEVEL > @as(c_int, 1))) {
    _ = &ptr;
    return __builtin.object_size(ptr, __USE_FORTIFY_LEVEL > @as(c_int, 1));
}
pub inline fn __bos0(ptr: anytype) @TypeOf(__builtin.object_size(ptr, @as(c_int, 0))) {
    _ = &ptr;
    return __builtin.object_size(ptr, @as(c_int, 0));
}
pub inline fn __glibc_objsize0(__o: anytype) @TypeOf(__bos0(__o)) {
    _ = &__o;
    return __bos0(__o);
}
pub inline fn __glibc_objsize(__o: anytype) @TypeOf(__bos(__o)) {
    _ = &__o;
    return __bos(__o);
}
pub const __warnattr = @compileError("unable to translate macro: undefined identifier `__warning__`"); // /usr/include/sys/cdefs.h:366:10
pub const __errordecl = @compileError("unable to translate macro: undefined identifier `__error__`"); // /usr/include/sys/cdefs.h:367:10
pub const __flexarr = @compileError("unable to translate C expr: unexpected token '['"); // /usr/include/sys/cdefs.h:379:10
pub const __glibc_c99_flexarr_available = @as(c_int, 1);
pub const __REDIRECT = @compileError("unable to translate C expr: unexpected token '__asm__'"); // /usr/include/sys/cdefs.h:410:10
pub const __REDIRECT_NTH = @compileError("unable to translate C expr: unexpected token '__asm__'"); // /usr/include/sys/cdefs.h:417:11
pub const __REDIRECT_NTHNL = @compileError("unable to translate C expr: unexpected token '__asm__'"); // /usr/include/sys/cdefs.h:419:11
pub const __ASMNAME = @compileError("unable to translate macro: undefined identifier `__USER_LABEL_PREFIX__`"); // /usr/include/sys/cdefs.h:422:10
pub inline fn __ASMNAME2(prefix: anytype, cname: anytype) @TypeOf(__STRING(prefix) ++ cname) {
    _ = &prefix;
    _ = &cname;
    return __STRING(prefix) ++ cname;
}
pub const __REDIRECT_FORTIFY = __REDIRECT;
pub const __REDIRECT_FORTIFY_NTH = __REDIRECT_NTH;
pub const __attribute_malloc__ = @compileError("unable to translate macro: undefined identifier `__malloc__`"); // /usr/include/sys/cdefs.h:452:10
pub const __attribute_alloc_size__ = @compileError("unable to translate macro: undefined identifier `__alloc_size__`"); // /usr/include/sys/cdefs.h:460:10
pub const __attribute_alloc_align__ = @compileError("unable to translate macro: undefined identifier `__alloc_align__`"); // /usr/include/sys/cdefs.h:469:10
pub const __attribute_pure__ = @compileError("unable to translate macro: undefined identifier `__pure__`"); // /usr/include/sys/cdefs.h:479:10
pub const __attribute_const__ = @compileError("unable to translate C expr: unexpected token '__attribute__'"); // /usr/include/sys/cdefs.h:486:10
pub const __attribute_maybe_unused__ = @compileError("unable to translate macro: undefined identifier `__unused__`"); // /usr/include/sys/cdefs.h:492:10
pub const __attribute_used__ = @compileError("unable to translate macro: undefined identifier `__used__`"); // /usr/include/sys/cdefs.h:501:10
pub const __attribute_noinline__ = @compileError("unable to translate macro: undefined identifier `__noinline__`"); // /usr/include/sys/cdefs.h:502:10
pub const __attribute_deprecated__ = @compileError("unable to translate macro: undefined identifier `__deprecated__`"); // /usr/include/sys/cdefs.h:510:10
pub const __attribute_deprecated_msg__ = @compileError("unable to translate macro: undefined identifier `__deprecated__`"); // /usr/include/sys/cdefs.h:520:10
pub const __attribute_format_arg__ = @compileError("unable to translate macro: undefined identifier `__format_arg__`"); // /usr/include/sys/cdefs.h:533:10
pub const __attribute_format_strfmon__ = @compileError("unable to translate macro: undefined identifier `__format__`"); // /usr/include/sys/cdefs.h:543:10
pub const __attribute_nonnull__ = @compileError("unable to translate macro: undefined identifier `__nonnull__`"); // /usr/include/sys/cdefs.h:555:11
pub inline fn __nonnull(params: anytype) @TypeOf(__attribute_nonnull__(params)) {
    _ = &params;
    return __attribute_nonnull__(params);
}
pub const __returns_nonnull = @compileError("unable to translate macro: undefined identifier `__returns_nonnull__`"); // /usr/include/sys/cdefs.h:568:10
pub const __attribute_warn_unused_result__ = @compileError("unable to translate macro: undefined identifier `__warn_unused_result__`"); // /usr/include/sys/cdefs.h:577:10
pub const __wur = "";
pub const __always_inline = @compileError("unable to translate macro: undefined identifier `__always_inline__`"); // /usr/include/sys/cdefs.h:595:10
pub const __attribute_artificial__ = @compileError("unable to translate macro: undefined identifier `__artificial__`"); // /usr/include/sys/cdefs.h:604:10
pub const __extern_inline = @compileError("unable to translate C expr: unexpected token 'extern'"); // /usr/include/sys/cdefs.h:626:11
pub const __extern_always_inline = @compileError("unable to translate C expr: unexpected token 'extern'"); // /usr/include/sys/cdefs.h:627:11
pub const __fortify_function = __extern_always_inline ++ __attribute_artificial__;
pub const __va_arg_pack = @compileError("unable to translate macro: undefined identifier `__builtin_va_arg_pack`"); // /usr/include/sys/cdefs.h:638:10
pub const __va_arg_pack_len = @compileError("unable to translate macro: undefined identifier `__builtin_va_arg_pack_len`"); // /usr/include/sys/cdefs.h:639:10
pub const __restrict_arr = @compileError("unable to translate C expr: unexpected token '__restrict'"); // /usr/include/sys/cdefs.h:666:10
pub inline fn __glibc_unlikely(cond: anytype) @TypeOf(__builtin.expect(cond, @as(c_int, 0))) {
    _ = &cond;
    return __builtin.expect(cond, @as(c_int, 0));
}
pub inline fn __glibc_likely(cond: anytype) @TypeOf(__builtin.expect(cond, @as(c_int, 1))) {
    _ = &cond;
    return __builtin.expect(cond, @as(c_int, 1));
}
pub const __attribute_nonstring__ = "";
pub inline fn __attribute_copy__(arg: anytype) void {
    _ = &arg;
    return;
}
pub const __LDOUBLE_REDIRECTS_TO_FLOAT128_ABI = @as(c_int, 0);
pub inline fn __LDBL_REDIR1(name: anytype, proto: anytype, alias: anytype) @TypeOf(name ++ proto) {
    _ = &name;
    _ = &proto;
    _ = &alias;
    return name ++ proto;
}
pub inline fn __LDBL_REDIR(name: anytype, proto: anytype) @TypeOf(name ++ proto) {
    _ = &name;
    _ = &proto;
    return name ++ proto;
}
pub inline fn __LDBL_REDIR1_NTH(name: anytype, proto: anytype, alias: anytype) @TypeOf(name ++ proto ++ __THROW) {
    _ = &name;
    _ = &proto;
    _ = &alias;
    return name ++ proto ++ __THROW;
}
pub inline fn __LDBL_REDIR_NTH(name: anytype, proto: anytype) @TypeOf(name ++ proto ++ __THROW) {
    _ = &name;
    _ = &proto;
    return name ++ proto ++ __THROW;
}
pub inline fn __LDBL_REDIR2_DECL(name: anytype) void {
    _ = &name;
    return;
}
pub inline fn __LDBL_REDIR_DECL(name: anytype) void {
    _ = &name;
    return;
}
pub inline fn __REDIRECT_LDBL(name: anytype, proto: anytype, alias: anytype) @TypeOf(__REDIRECT(name, proto, alias)) {
    _ = &name;
    _ = &proto;
    _ = &alias;
    return __REDIRECT(name, proto, alias);
}
pub inline fn __REDIRECT_NTH_LDBL(name: anytype, proto: anytype, alias: anytype) @TypeOf(__REDIRECT_NTH(name, proto, alias)) {
    _ = &name;
    _ = &proto;
    _ = &alias;
    return __REDIRECT_NTH(name, proto, alias);
}
pub const __glibc_macro_warning1 = @compileError("unable to translate macro: undefined identifier `_Pragma`"); // /usr/include/sys/cdefs.h:807:10
pub const __glibc_macro_warning = @compileError("unable to translate macro: undefined identifier `GCC`"); // /usr/include/sys/cdefs.h:808:10
pub const __HAVE_GENERIC_SELECTION = @as(c_int, 1);
pub const __glibc_const_generic = @compileError("unable to translate C expr: expected type instead got 'const'"); // /usr/include/sys/cdefs.h:837:10
pub inline fn __fortified_attr_access(a: anytype, o: anytype, s: anytype) void {
    _ = &a;
    _ = &o;
    _ = &s;
    return;
}
pub inline fn __attr_access(x: anytype) void {
    _ = &x;
    return;
}
pub inline fn __attr_access_none(argno: anytype) void {
    _ = &argno;
    return;
}
pub inline fn __attr_dealloc(dealloc: anytype, argno: anytype) void {
    _ = &dealloc;
    _ = &argno;
    return;
}
pub const __attr_dealloc_free = "";
pub const __attribute_returns_twice__ = @compileError("unable to translate macro: undefined identifier `__returns_twice__`"); // /usr/include/sys/cdefs.h:884:10
pub const __attribute_struct_may_alias__ = @compileError("unable to translate macro: undefined identifier `__may_alias__`"); // /usr/include/sys/cdefs.h:893:10
pub const __stub___compat_bdflush = "";
pub const __stub_chflags = "";
pub const __stub_fchflags = "";
pub const __stub_gtty = "";
pub const __stub_revoke = "";
pub const __stub_setlogin = "";
pub const __stub_sigreturn = "";
pub const __stub_stty = "";
pub const _BITS_TYPES_H = @as(c_int, 1);
pub const __S16_TYPE = c_short;
pub const __U16_TYPE = c_ushort;
pub const __S32_TYPE = c_int;
pub const __U32_TYPE = c_uint;
pub const __SLONGWORD_TYPE = c_long;
pub const __ULONGWORD_TYPE = c_ulong;
pub const __SQUAD_TYPE = c_long;
pub const __UQUAD_TYPE = c_ulong;
pub const __SWORD_TYPE = c_long;
pub const __UWORD_TYPE = c_ulong;
pub const __SLONG32_TYPE = c_int;
pub const __ULONG32_TYPE = c_uint;
pub const __S64_TYPE = c_long;
pub const __U64_TYPE = c_ulong;
pub const _BITS_TYPESIZES_H = @as(c_int, 1);
pub const __SYSCALL_SLONG_TYPE = __SLONGWORD_TYPE;
pub const __SYSCALL_ULONG_TYPE = __ULONGWORD_TYPE;
pub const __DEV_T_TYPE = __UQUAD_TYPE;
pub const __UID_T_TYPE = __U32_TYPE;
pub const __GID_T_TYPE = __U32_TYPE;
pub const __INO_T_TYPE = __SYSCALL_ULONG_TYPE;
pub const __INO64_T_TYPE = __UQUAD_TYPE;
pub const __MODE_T_TYPE = __U32_TYPE;
pub const __NLINK_T_TYPE = __SYSCALL_ULONG_TYPE;
pub const __FSWORD_T_TYPE = __SYSCALL_SLONG_TYPE;
pub const __OFF_T_TYPE = __SYSCALL_SLONG_TYPE;
pub const __OFF64_T_TYPE = __SQUAD_TYPE;
pub const __PID_T_TYPE = __S32_TYPE;
pub const __RLIM_T_TYPE = __SYSCALL_ULONG_TYPE;
pub const __RLIM64_T_TYPE = __UQUAD_TYPE;
pub const __BLKCNT_T_TYPE = __SYSCALL_SLONG_TYPE;
pub const __BLKCNT64_T_TYPE = __SQUAD_TYPE;
pub const __FSBLKCNT_T_TYPE = __SYSCALL_ULONG_TYPE;
pub const __FSBLKCNT64_T_TYPE = __UQUAD_TYPE;
pub const __FSFILCNT_T_TYPE = __SYSCALL_ULONG_TYPE;
pub const __FSFILCNT64_T_TYPE = __UQUAD_TYPE;
pub const __ID_T_TYPE = __U32_TYPE;
pub const __CLOCK_T_TYPE = __SYSCALL_SLONG_TYPE;
pub const __TIME_T_TYPE = __SYSCALL_SLONG_TYPE;
pub const __USECONDS_T_TYPE = __U32_TYPE;
pub const __SUSECONDS_T_TYPE = __SYSCALL_SLONG_TYPE;
pub const __SUSECONDS64_T_TYPE = __SQUAD_TYPE;
pub const __DADDR_T_TYPE = __S32_TYPE;
pub const __KEY_T_TYPE = __S32_TYPE;
pub const __CLOCKID_T_TYPE = __S32_TYPE;
pub const __TIMER_T_TYPE = ?*anyopaque;
pub const __BLKSIZE_T_TYPE = __SYSCALL_SLONG_TYPE;
pub const __FSID_T_TYPE = @compileError("unable to translate macro: undefined identifier `__val`"); // /usr/include/bits/typesizes.h:73:9
pub const __SSIZE_T_TYPE = __SWORD_TYPE;
pub const __CPU_MASK_TYPE = __SYSCALL_ULONG_TYPE;
pub const __OFF_T_MATCHES_OFF64_T = @as(c_int, 1);
pub const __INO_T_MATCHES_INO64_T = @as(c_int, 1);
pub const __RLIM_T_MATCHES_RLIM64_T = @as(c_int, 1);
pub const __STATFS_MATCHES_STATFS64 = @as(c_int, 1);
pub const __KERNEL_OLD_TIMEVAL_MATCHES_TIMEVAL64 = @as(c_int, 1);
pub const __FD_SETSIZE = @as(c_int, 1024);
pub const _BITS_TIME64_H = @as(c_int, 1);
pub const __TIME64_T_TYPE = __TIME_T_TYPE;
pub const _BITS_WCHAR_H = @as(c_int, 1);
pub const __WCHAR_MAX = __WCHAR_MAX__;
pub const __WCHAR_MIN = -__WCHAR_MAX - @as(c_int, 1);
pub const _BITS_STDINT_INTN_H = @as(c_int, 1);
pub const _BITS_STDINT_UINTN_H = @as(c_int, 1);
pub const _BITS_STDINT_LEAST_H = @as(c_int, 1);
pub const __intptr_t_defined = "";
pub const __INT64_C = __helpers.L_SUFFIX;
pub const __UINT64_C = __helpers.UL_SUFFIX;
pub const INT8_MIN = -@as(c_int, 128);
pub const INT16_MIN = -@as(c_int, 32767) - @as(c_int, 1);
pub const INT32_MIN = -__helpers.promoteIntLiteral(c_int, 2147483647, .decimal) - @as(c_int, 1);
pub const INT64_MIN = -__INT64_C(__helpers.promoteIntLiteral(c_int, 9223372036854775807, .decimal)) - @as(c_int, 1);
pub const INT8_MAX = @as(c_int, 127);
pub const INT16_MAX = @as(c_int, 32767);
pub const INT32_MAX = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const INT64_MAX = __INT64_C(__helpers.promoteIntLiteral(c_int, 9223372036854775807, .decimal));
pub const UINT8_MAX = @as(c_int, 255);
pub const UINT16_MAX = __helpers.promoteIntLiteral(c_int, 65535, .decimal);
pub const UINT32_MAX = __helpers.promoteIntLiteral(c_uint, 4294967295, .decimal);
pub const UINT64_MAX = __UINT64_C(__helpers.promoteIntLiteral(c_int, 18446744073709551615, .decimal));
pub const INT_LEAST8_MIN = -@as(c_int, 128);
pub const INT_LEAST16_MIN = -@as(c_int, 32767) - @as(c_int, 1);
pub const INT_LEAST32_MIN = -__helpers.promoteIntLiteral(c_int, 2147483647, .decimal) - @as(c_int, 1);
pub const INT_LEAST64_MIN = -__INT64_C(__helpers.promoteIntLiteral(c_int, 9223372036854775807, .decimal)) - @as(c_int, 1);
pub const INT_LEAST8_MAX = @as(c_int, 127);
pub const INT_LEAST16_MAX = @as(c_int, 32767);
pub const INT_LEAST32_MAX = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const INT_LEAST64_MAX = __INT64_C(__helpers.promoteIntLiteral(c_int, 9223372036854775807, .decimal));
pub const UINT_LEAST8_MAX = @as(c_int, 255);
pub const UINT_LEAST16_MAX = __helpers.promoteIntLiteral(c_int, 65535, .decimal);
pub const UINT_LEAST32_MAX = __helpers.promoteIntLiteral(c_uint, 4294967295, .decimal);
pub const UINT_LEAST64_MAX = __UINT64_C(__helpers.promoteIntLiteral(c_int, 18446744073709551615, .decimal));
pub const INT_FAST8_MIN = -@as(c_int, 128);
pub const INT_FAST16_MIN = -__helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal) - @as(c_int, 1);
pub const INT_FAST32_MIN = -__helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal) - @as(c_int, 1);
pub const INT_FAST64_MIN = -__INT64_C(__helpers.promoteIntLiteral(c_int, 9223372036854775807, .decimal)) - @as(c_int, 1);
pub const INT_FAST8_MAX = @as(c_int, 127);
pub const INT_FAST16_MAX = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const INT_FAST32_MAX = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const INT_FAST64_MAX = __INT64_C(__helpers.promoteIntLiteral(c_int, 9223372036854775807, .decimal));
pub const UINT_FAST8_MAX = @as(c_int, 255);
pub const UINT_FAST16_MAX = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const UINT_FAST32_MAX = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const UINT_FAST64_MAX = __UINT64_C(__helpers.promoteIntLiteral(c_int, 18446744073709551615, .decimal));
pub const INTPTR_MIN = -__helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal) - @as(c_int, 1);
pub const INTPTR_MAX = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const UINTPTR_MAX = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const INTMAX_MIN = -__INT64_C(__helpers.promoteIntLiteral(c_int, 9223372036854775807, .decimal)) - @as(c_int, 1);
pub const INTMAX_MAX = __INT64_C(__helpers.promoteIntLiteral(c_int, 9223372036854775807, .decimal));
pub const UINTMAX_MAX = __UINT64_C(__helpers.promoteIntLiteral(c_int, 18446744073709551615, .decimal));
pub const PTRDIFF_MIN = -__helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal) - @as(c_int, 1);
pub const PTRDIFF_MAX = __helpers.promoteIntLiteral(c_long, 9223372036854775807, .decimal);
pub const SIG_ATOMIC_MIN = -__helpers.promoteIntLiteral(c_int, 2147483647, .decimal) - @as(c_int, 1);
pub const SIG_ATOMIC_MAX = __helpers.promoteIntLiteral(c_int, 2147483647, .decimal);
pub const SIZE_MAX = __helpers.promoteIntLiteral(c_ulong, 18446744073709551615, .decimal);
pub const WCHAR_MIN = __WCHAR_MIN;
pub const WCHAR_MAX = __WCHAR_MAX;
pub const WINT_MIN = @as(c_uint, 0);
pub const WINT_MAX = __helpers.promoteIntLiteral(c_uint, 4294967295, .decimal);
pub inline fn INT8_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub inline fn INT16_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub inline fn INT32_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub const INT64_C = __helpers.L_SUFFIX;
pub inline fn UINT8_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub inline fn UINT16_C(c: anytype) @TypeOf(c) {
    _ = &c;
    return c;
}
pub const UINT32_C = __helpers.U_SUFFIX;
pub const UINT64_C = __helpers.UL_SUFFIX;
pub const INTMAX_C = __helpers.L_SUFFIX;
pub const UINTMAX_C = __helpers.UL_SUFFIX;
pub const DL_E_LOCKED = @as(c_int, 1);
pub const DL_E_CONFLICT = @as(c_int, 2);
pub const VECTOR_H = "";
pub const VEC_D = @as(c_int, 384);
pub const VEC_C = @as(c_int, 256);
pub const VEC_M = @as(c_int, 16);
pub const VEC_W = @as(c_int, 16);
pub const VEC_SIG_WORDS = __helpers.div(VEC_C, @as(c_int, 32));
pub const VEC_IVEC_WORDS = __helpers.div(VEC_D, @as(c_int, 4));
pub const VEC_ENTITY_REL = "entity";
pub const DL_VEC_CORPUS_ENTITY = @import("std").mem.zeroInit(struct_dl_vec_corpus, .{ "entity", @as(c_int, 0), "__sig%d__", "__vec_q__", "" });
pub const DL_VEC_CORPUS_OBSERVATION_CONTENT = @import("std").mem.zeroInit(struct_dl_vec_corpus, .{ "observation", @as(c_int, 1), "__obssig%d__", "__vec_obs__", "_obs" });
pub const INTERN_H = "";
pub const _STDIO_H = @as(c_int, 1);
pub const __GLIBC_USE_LIB_EXT2 = @as(c_int, 0);
pub const __GLIBC_USE_IEC_60559_BFP_EXT = @as(c_int, 0);
pub const __GLIBC_USE_IEC_60559_BFP_EXT_C23 = @as(c_int, 0);
pub const __GLIBC_USE_IEC_60559_EXT = @as(c_int, 0);
pub const __GLIBC_USE_IEC_60559_FUNCS_EXT = @as(c_int, 0);
pub const __GLIBC_USE_IEC_60559_FUNCS_EXT_C23 = @as(c_int, 0);
pub const __GLIBC_USE_IEC_60559_TYPES_EXT = @as(c_int, 0);
pub const __need_size_t = "";
pub const __need_NULL = "";
pub const __need___va_list = "";
pub const __STDC_VERSION_STDARG_H__ = @as(c_int, 0);
pub const va_start = @compileError("unable to translate macro: undefined identifier `__builtin_va_start`"); // /home/jaye/.local/lib/zig-0.16.0/lib/compiler/aro/include/stdarg.h:12:9
pub const va_end = @compileError("unable to translate macro: undefined identifier `__builtin_va_end`"); // /home/jaye/.local/lib/zig-0.16.0/lib/compiler/aro/include/stdarg.h:14:9
pub const va_arg = @compileError("unable to translate macro: undefined identifier `__builtin_va_arg`"); // /home/jaye/.local/lib/zig-0.16.0/lib/compiler/aro/include/stdarg.h:15:9
pub const __va_copy = @compileError("unable to translate macro: undefined identifier `__builtin_va_copy`"); // /home/jaye/.local/lib/zig-0.16.0/lib/compiler/aro/include/stdarg.h:18:9
pub const va_copy = @compileError("unable to translate macro: undefined identifier `__builtin_va_copy`"); // /home/jaye/.local/lib/zig-0.16.0/lib/compiler/aro/include/stdarg.h:22:9
pub const __GNUC_VA_LIST = @as(c_int, 1);
pub const _____fpos_t_defined = @as(c_int, 1);
pub const ____mbstate_t_defined = @as(c_int, 1);
pub const _____fpos64_t_defined = @as(c_int, 1);
pub const ____FILE_defined = @as(c_int, 1);
pub const __FILE_defined = @as(c_int, 1);
pub const __struct_FILE_defined = @as(c_int, 1);
pub const __getc_unlocked_body = @compileError("TODO postfix inc/dec expr"); // /usr/include/bits/types/struct_FILE.h:113:9
pub const __putc_unlocked_body = @compileError("TODO postfix inc/dec expr"); // /usr/include/bits/types/struct_FILE.h:117:9
pub const _IO_EOF_SEEN = @as(c_int, 0x0010);
pub inline fn __feof_unlocked_body(_fp: anytype) @TypeOf((_fp.*._flags & _IO_EOF_SEEN) != @as(c_int, 0)) {
    _ = &_fp;
    return (_fp.*._flags & _IO_EOF_SEEN) != @as(c_int, 0);
}
pub const _IO_ERR_SEEN = @as(c_int, 0x0020);
pub inline fn __ferror_unlocked_body(_fp: anytype) @TypeOf((_fp.*._flags & _IO_ERR_SEEN) != @as(c_int, 0)) {
    _ = &_fp;
    return (_fp.*._flags & _IO_ERR_SEEN) != @as(c_int, 0);
}
pub const _IO_USER_LOCK = __helpers.promoteIntLiteral(c_int, 0x8000, .hex);
pub const __cookie_io_functions_t_defined = @as(c_int, 1);
pub const _VA_LIST_DEFINED = "";
pub const __off_t_defined = "";
pub const __ssize_t_defined = "";
pub const _IOFBF = @as(c_int, 0);
pub const _IOLBF = @as(c_int, 1);
pub const _IONBF = @as(c_int, 2);
pub const BUFSIZ = @as(c_int, 8192);
pub const EOF = -@as(c_int, 1);
pub const SEEK_SET = @as(c_int, 0);
pub const SEEK_CUR = @as(c_int, 1);
pub const SEEK_END = @as(c_int, 2);
pub const P_tmpdir = "/tmp";
pub const L_tmpnam = @as(c_int, 20);
pub const TMP_MAX = __helpers.promoteIntLiteral(c_int, 238328, .decimal);
pub const _BITS_STDIO_LIM_H = @as(c_int, 1);
pub const FILENAME_MAX = @as(c_int, 4096);
pub const L_ctermid = @as(c_int, 9);
pub const FOPEN_MAX = @as(c_int, 16);
pub const __attr_dealloc_fclose = __attr_dealloc(fclose, @as(c_int, 1));
pub const _BITS_FLOATN_H = "";
pub const __HAVE_FLOAT128 = @as(c_int, 1);
pub const __HAVE_DISTINCT_FLOAT128 = @as(c_int, 1);
pub const __HAVE_FLOAT64X = @as(c_int, 1);
pub const __HAVE_FLOAT64X_LONG_DOUBLE = @as(c_int, 1);
pub const __f128 = @compileError("unable to translate macro: undefined identifier `f128`"); // /usr/include/bits/floatn.h:72:12
pub const __CFLOAT128 = @compileError("unable to translate: invalid numeric type"); // /usr/include/bits/floatn.h:86:12
pub const _BITS_FLOATN_COMMON_H = "";
pub const __HAVE_FLOAT16 = @as(c_int, 0);
pub const __HAVE_FLOAT32 = @as(c_int, 1);
pub const __HAVE_FLOAT64 = @as(c_int, 1);
pub const __HAVE_FLOAT32X = @as(c_int, 1);
pub const __HAVE_FLOAT128X = @as(c_int, 0);
pub const __HAVE_DISTINCT_FLOAT16 = __HAVE_FLOAT16;
pub const __HAVE_DISTINCT_FLOAT32 = @as(c_int, 0);
pub const __HAVE_DISTINCT_FLOAT64 = @as(c_int, 0);
pub const __HAVE_DISTINCT_FLOAT32X = @as(c_int, 0);
pub const __HAVE_DISTINCT_FLOAT64X = @as(c_int, 0);
pub const __HAVE_DISTINCT_FLOAT128X = __HAVE_FLOAT128X;
pub const __HAVE_FLOAT128_UNLIKE_LDBL = (__HAVE_DISTINCT_FLOAT128 != 0) and (__LDBL_MANT_DIG__ != @as(c_int, 113));
pub const __HAVE_FLOATN_NOT_TYPEDEF = @as(c_int, 1);
pub const __f32 = @compileError("unable to translate macro: undefined identifier `f32`"); // /usr/include/bits/floatn-common.h:93:12
pub const __f64 = @compileError("unable to translate macro: undefined identifier `f64`"); // /usr/include/bits/floatn-common.h:105:12
pub const __f32x = @compileError("unable to translate macro: undefined identifier `f32x`"); // /usr/include/bits/floatn-common.h:113:12
pub const __f64x = @compileError("unable to translate macro: undefined identifier `f64x`"); // /usr/include/bits/floatn-common.h:125:12
pub const __CFLOAT32 = @compileError("unable to translate: invalid numeric type"); // /usr/include/bits/floatn-common.h:151:12
pub const __CFLOAT64 = @compileError("unable to translate: invalid numeric type"); // /usr/include/bits/floatn-common.h:163:12
pub const __CFLOAT32X = @compileError("unable to translate: invalid numeric type"); // /usr/include/bits/floatn-common.h:171:12
pub const __CFLOAT64X = @compileError("unable to translate: invalid numeric type"); // /usr/include/bits/floatn-common.h:183:12
pub const RELATION_H = "";
pub const VRELATION_H = "";
pub const MAX_VAR_ARITY = @as(c_int, 8);
pub const SNAPSHOT_H = "";
pub const DL_VIEW_CACHE_SZ = @as(c_int, 8);
pub const PERMINDEX_H = "";
pub const MAX_PERMS_PER_REL = @as(c_int, 8);
pub const MAX_PERMS = @as(c_int, 64);
pub const TERMSTORE_H = "";
pub const TERM_BASE = @as(c_ulong, 0x80000000);
pub const TERM_NIL = __helpers.cast(u32, TERM_BASE);
pub const COMPILER_H = "";
pub const PARSER_H = "";
pub const REGEXWALK_H = "";
pub const REGEX_DFA_MAX_STATES = __helpers.promoteIntLiteral(c_int, 50000, .decimal);
pub const REGEX_DFA_ABORT_EARLY = @as(c_int, 8192);
pub const SYMSET_INIT_CAP = @as(c_int, 64);
pub const MAX_VARS = @as(c_int, 64);
pub const MAX_ARITY = @as(c_int, 8);
pub const MAX_RELS = @as(c_int, 64);
pub const RELK_FIXED = @as(c_int, 0);
pub const RELK_VARIADIC = @as(c_int, 1);
pub const TXN_ADD = @as(c_int, 1);
pub const TXN_DEL = @as(c_int, 2);
pub const TXN_CAS = @as(c_int, 3);
pub const INDEX_H = "";
pub const tuple_set = struct_tuple_set;
pub const dl_vec_corpus = struct_dl_vec_corpus;
pub const _G_fpos_t = struct__G_fpos_t;
pub const _G_fpos64_t = struct__G_fpos64_t;
pub const _IO_marker = struct__IO_marker;
pub const _IO_FILE = struct__IO_FILE;
pub const _IO_codecvt = struct__IO_codecvt;
pub const _IO_wide_data = struct__IO_wide_data;
pub const _IO_cookie_io_functions_t = struct__IO_cookie_io_functions_t;

