//! dlp.zig — Zig port of dlp/* (the dl-project tool; main/init/schema_load/
//! schema_check/csv_load/json_load/workflow/coerce.h + dlp.h).
//!
//! Oracle: dlp/*.c (never modified).  The port is behavior- and
//! diagnostic-exact: every stdout/stderr byte — `dlp schema` table, loader
//! diagnostics (`path:LINE:COL:` CSV / `path: element N:` JSON positions,
//! "expects %s, got %s" header/object errors, "out of range (allowed
//! [%lld..%lld])", "does not match regex"), query rows — and every exit code
//! flow must match the C tool, whose behavioral oracle is tests/dlp_golden.sh.
//!
//! WIRING (mirrors dl_cli.zig): the engine is reached through the C ABI of
//! the 100%-Zig libdatalog.so — dl_internal.zig declares the extern surface
//! (dl_open/dl_close/dl_attach_schema/dl_declare_relation/dl_add_fact/
//! dl_load_rules/dl_compile/dl_publish_snapshot/dl_query/dl_lookup/
//! dl_intern_str[_of]/dl_term_cons/car/cdr/is_list/parse_*/rule_free/
//! regex_compile/regex_dfa_free).  Two types are CONCRETE C structs here:
//! dl_schema/dl_reldef/dl_colspec/dl_coltype come from a native file-import
//! of schema.zig (the exported dl_schema_add/dl_schema_find are resolved
//! from the .so at link time — schema.zig's exports duplicate dl_internal's
//! opaque struct_dl_schema on purpose; only the layouts are shared), and
//! dx.rule/dx.regex_dfa are dl_internal's extern-struct mirrors.
//!
//! The Dhall schema evaluation statically compiles the vendored dhall-c Zig
//! core (vendor/dhake/vendor/dhall-c/zig/src) as the `dhall_c` build module:
//! native calls into parser.parse_source / typecheck.infer_type /
//! normalize.normalize, walking the extern Term tree (TmRecordLit/TmUnionLit/
//! TmCons/TmText/TmSome/TmNone/TmConst) exactly like dlp/schema_load.c.
//!
//! UTF-8/regex/zigzag/date coercers and regex_dfa_full_match are dlp-LOCAL
//! (coerce.h static-inline helpers; regex_dfa_full_match walks the DFA's
//! trans[s*256+byte] graph — it is NOT an engine export), ported below.

const std = @import("std");
const builtin = @import("builtin");

// Engine C ABI (libdatalog.so exports) + the rule/regex_dfa extern structs.
const dx = @import("dl_internal.zig");
// Concrete dl_schema/dl_reldef/dl_colspec layouts (byte-identical to
// src/schema.h; also exported from the .so by this very module).
const schema_mod = @import("schema.zig");
// Vendored dhall-c Zig core (single-module facade; compiled into this exe).
const dm = @import("dhall_c");

const c = std.c;

// ─── libc decls (precedent: dl_cli.zig) ────────────────────────────────────
extern "c" fn printf(fmt: [*:0]const u8, ...) c_int;
extern "c" fn fprintf(stream: *std.c.FILE, fmt: [*:0]const u8, ...) c_int;
extern "c" fn snprintf(buf: [*c]u8, size: usize, fmt: [*:0]const u8, ...) c_int;
extern "c" fn strerror(errnum: c_int) [*:0]u8;
extern "c" var stderr: *std.c.FILE;

/// dl_typecheck_rules lives on the .so but is not (yet) declared in
/// dl_internal.zig; bind it here (same callconv/signature as typecheck.zig's
/// export, which dl.zig itself binds through a raw extern).
extern "c" fn dl_typecheck_rules(
    schm: ?*const schema_mod.dl_schema,
    rules: ?*anyopaque,
    n_rules: c_int,
    srcname: ?[*:0]const u8,
    errbuf: ?[*]u8,
    errcap: usize,
) c_int;

const dl_db = dx.dl_db;
const dl_reldef = schema_mod.dl_reldef;
const dl_colspec = schema_mod.dl_colspec;
const dl_schema = schema_mod.dl_schema;
const dl_coltype = schema_mod.dl_coltype;
const DLT_NATURAL = schema_mod.DLT_NATURAL;
const DLT_TEXT = schema_mod.DLT_TEXT;
const DLT_BOOL = schema_mod.DLT_BOOL;
const DLT_CHAR = schema_mod.DLT_CHAR;
const DLT_DATE = schema_mod.DLT_DATE;
const DLT_TIMESTAMP = schema_mod.DLT_TIMESTAMP;
const DLT_SIGNED = schema_mod.DLT_SIGNED;
const DLT_LIST = schema_mod.DLT_LIST;
const DLT_OPTIONAL = schema_mod.DLT_OPTIONAL;
const DLT_ENUM = schema_mod.DLT_ENUM;
const DL_SCHEMA_MAX_RELS = schema_mod.DL_SCHEMA_MAX_RELS;
const DL_SCHEMA_MAX_ARITY = schema_mod.DL_SCHEMA_MAX_ARITY;
const DL_SCHEMA_NAME_MAX = schema_mod.DL_SCHEMA_NAME_MAX;
const DL_ENUM_MAX_VALUES = schema_mod.DL_ENUM_MAX_VALUES;
const DL_ENUM_VALUE_MAX = schema_mod.DL_ENUM_VALUE_MAX;

// dlp.h: Optional<elem> None sentinel (same as the VM's UNBOUND).
const DLP_OPT_NONE: u32 = 0xFFFFFFFF;
// coerce.h: max elements in a List column value (v1 fixed cap).
const DLP_LIST_MAX_ELEMS = 8 * 8; // 64

const ERR_CAP = 512;

fn errClear(e: *Err) void {
    e.buf[0] = 0;
}
const Err = struct {
    buf: [ERR_CAP:0]u8 = @splat(0),
};

/// seterr(...)-equivalent: vsnprintf into errbuf, return -1.
fn seterr(e: *Err, comptime fmt: [:0]const u8, args: anytype) c_int {
    _ = @call(.auto, snprintf, .{ &e.buf, ERR_CAP, fmt.ptr } ++ args);
    return -1;
}

/// printf/fprintf/snprintf wrappers: Zig cannot pass a runtime tuple to a
/// variadic extern directly, so splat it with @call (same trick dl_cli.zig
/// avoids by passing pointers inline; here every call goes through these).
fn pf(comptime fmt: [:0]const u8, args: anytype) void {
    _ = @call(.auto, printf, .{@as([*:0]const u8, fmt.ptr)} ++ args);
}

fn errf(stream: *std.c.FILE, comptime fmt: [:0]const u8, args: anytype) void {
    _ = @call(.auto, fprintf, .{ @as(*std.c.FILE, stream), @as([*:0]const u8, fmt.ptr) } ++ args);
}

fn errf0(stream: *std.c.FILE, comptime fmt: [:0]const u8) void {
    _ = @call(.auto, fprintf, .{ stream, "%s", fmt.ptr });
}

fn sn(buf: [*c]u8, cap: usize, comptime fmt: [:0]const u8, args: anytype) c_int {
    const full = .{ @as([*c]u8, buf), @as(usize, cap), @as([*:0]const u8, fmt.ptr) } ++ args;
    return @call(.auto, snprintf, full);
}

/// strlen for many-item NUL-terminated C strings.
fn slen(s: [*:0]const u8) usize {
    var i: usize = 0;
    while (s[i] != 0) i += 1;
    return i;
}

fn seq(a: [*:0]const u8, b: [*:0]const u8) bool {
    var i: usize = 0;
    while (a[i] == b[i]) {
        if (a[i] == 0) return true;
        i += 1;
    }
    return false;
}

fn hasSuffix(s: [*:0]const u8, suffix: []const u8) bool {
    const n = slen(s);
    const m = suffix.len;
    if (n < m) return false;
    var i: usize = 0;
    while (i < m) : (i += 1) {
        if (s[n - m + i] != suffix[i]) return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// dlp/init.c
// ═══════════════════════════════════════════════════════════════════════════

const TEMPLATE_SCHEMA =
    "-- dlp project schema (worked example)\n" ++
    "-- Optional-payload union DSL: scalars carry Optional constraint payloads\n" ++
    "--   < Natural = { min = None Natural, max = None Natural } > etc.\n" ++
    "-- List/Optional/Enum carry a payload record: < List = { elem = < Text = {=} > } >\n" ++
    "-- Constrain a scalar: < Natural = { min = Some 0, max = Some 150 } >\n" ++
    "--                     < Signed = { min = Some -10, max = Some +10 } >  (explicit sign)\n" ++
    "--                     < Text = { regex = Some \"[A-Z]+\" } >  (full-key match; no ^...$ anchors)\n" ++
    "-- Helper bindings cut the verbosity of unconstrained scalars.\n" ++
    "let Elem = < Natural : {=} | Text : {=} | Bool : {=} | Char : {=} | Date : {=} | Timestamp : {=} | Signed : {=} >\n" ++
    "in let NC = { min = None Natural, max = None Natural }\n" ++
    "in let TC = { regex = None Text }\n" ++
    "in let SC = { min = None Integer, max = None Integer }\n" ++
    "in let ColumnType = < Natural : { min : Optional Natural, max : Optional Natural } | Text : { regex : Optional Text } | Bool : {=} | Char : { min : Optional Natural, max : Optional Natural } | Date : { min : Optional Natural, max : Optional Natural } | Timestamp : { min : Optional Natural, max : Optional Natural } | Signed : { min : Optional Integer, max : Optional Integer } | List : { elem : Elem } | Optional : { elem : Elem } | Enum : { values : List Text } >\n" ++
    "in let Column = { name : Text, type : ColumnType }\n" ++
    "in let Relation = { name : Text, columns : List Column }\n" ++
    "in let Schema = { relations : List Relation }\n" ++
    "in { relations =\n" ++
    "     [ { name = \"node\",\n" ++
    "         columns = [ { name = \"id\", type = < Text = TC > },\n" ++
    "                     { name = \"active\", type = < Bool = {=} > },\n" ++
    "                     { name = \"born\", type = < Date = NC > },\n" ++
    "                     { name = \"seen\", type = < Timestamp = NC > },\n" ++
    "                     { name = \"initial\", type = < Char = NC > },\n" ++
    "                     { name = \"delta\", type = < Signed = SC > } ] },\n" ++
    "       { name = \"catalog\",\n" ++
    "         columns = [ { name = \"tags\", type = < List = { elem = < Text = {=} > } > },\n" ++
    "                     { name = \"nick\", type = < Optional = { elem = < Text = {=} > } > },\n" ++
    "                     { name = \"color\", type = < Enum = { values = [ \"red\", \"green\", \"blue\" ] } > } ] } ] } : Schema\n";

var gpa_state: std.heap.ArenaAllocator = undefined;

/// Whole-process allocator (CLI that runs start-to-end; every dlp allocation
/// that outlives a command lives here and is never individually freed — the
/// process exits right after the command, exactly like the C tool).
var galloc: std.mem.Allocator = undefined;
var galloc_ready = false;

fn alloc() std.mem.Allocator {
    if (!galloc_ready) {
        gpa_state = std.heap.ArenaAllocator.init(std.heap.page_allocator);
        galloc = gpa_state.allocator();
        galloc_ready = true;
    }
    return galloc;
}

/// OOM policy: every dlp allocation failure is fatal to the command (the C
/// tool either fatals or degrades into the same single diagnostic).
fn oom() noreturn {
    _ = errf0(stderr, "dlp: out of memory\n");
    c.exit(1);
}

// libc decls not re-exported by std.c (precedent: dl_cli.zig's extern "c" set).
extern "c" fn getline(lineptr: *?[*]u8, n: *usize, stream: *std.c.FILE) isize;
const DIR = opaque {};
const Dirent = extern struct {
    d_ino: u64,
    d_off: i64,
    d_reclen: u16,
    d_type: u8,
    d_name: [256]u8, // glibc null-terminated name
};
extern "c" fn opendir(name: [*:0]const u8) ?*DIR;
extern "c" fn readdir(d: *DIR) ?*Dirent;
extern "c" fn closedir(d: *DIR) c_int;
const StatBuf = extern struct {
    buf: [160]u8 align(8) = @splat(0),
};
extern "c" fn stat(path: [*:0]const u8, buf: *StatBuf) c_int;

/// st_mode offset in glibc's struct stat (Linux x86_64/aarch64: 24).
const ST_MODE_OFF: usize = switch (builtin.target.cpu.arch) {
    .x86_64, .aarch64 => 24,
    else => @compileError("unsupported arch for raw stat layout"),
};
const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;

// ═══════════════════════════════════════════════════════════════════════════
// dlp/schema_load.c — Dhall evaluation + term walk
// ═══════════════════════════════════════════════════════════════════════════

/// Column-name table retained from the Dhall schema (dl_reldef stores TYPES
/// only).  Filled during the walk; read via dlpSchemaColname().
var colnames: [DL_SCHEMA_MAX_RELS][DL_SCHEMA_MAX_ARITY][DL_SCHEMA_NAME_MAX:0]u8 = @splat(@splat(@splat(0)));

fn colnameOf(s: *const dl_schema, rel: [*:0]const u8, col: usize) ?[*:0]const u8 {
    if (col >= DL_SCHEMA_MAX_ARITY) return null;
    var i: usize = 0;
    while (i < @as(usize, @intCast(@max(s.n_rels, 0)))) : (i += 1) {
        if (seq(@ptrCast(&s.rels[i].name), rel)) {
            if (col >= s.rels[i].arity) return null;
            return @ptrCast(&colnames[i][col]);
        }
    }
    return null;
}

/// walk_error diagnostic channel (schema_load.c's static walk_err).
var walk_err_buf: [256:0]u8 = @splat(0);
fn walkError(comptime fmt: [:0]const u8, args: anytype) void {
    _ = @call(.auto, snprintf, .{ &walk_err_buf, walk_err_buf.len, fmt.ptr } ++ args);
}

/// Human-readable column-type name (List/Optional render one level of elem;
/// Enum renders bare).
fn coltypeName(spec: ?*const dl_colspec, out: [*c]u8, cap: usize) void {
    const cc = spec orelse {
        _ = sn(out, cap, "%s", .{@as([*:0]const u8, "?")});
        return;
    };
    switch (cc.tag) {
        DLT_NATURAL => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "Natural")}),
        DLT_TEXT => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "Text")}),
        DLT_BOOL => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "Bool")}),
        DLT_CHAR => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "Char")}),
        DLT_DATE => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "Date")}),
        DLT_TIMESTAMP => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "Timestamp")}),
        DLT_SIGNED => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "Signed")}),
        DLT_LIST, DLT_OPTIONAL => {
            var ec: dl_colspec = std.mem.zeroes(dl_colspec);
            ec.tag = cc.elem;
            var en: [32]u8 = undefined;
            coltypeName(&ec, &en, en.len);
            _ = sn(out, cap, "%s<%s>", .{
                @as([*:0]const u8, if (cc.tag == DLT_LIST) "List" else "Optional"),
                @as([*:0]const u8, @ptrCast(&en)),
            });
        },
        DLT_ENUM => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "Enum")}),
        else => _ = sn(out, cap, "%s", .{@as([*:0]const u8, "?")}),
    }
}

// ─── Dhall term-walk helpers (schema_load.c statics) ───────────────────────

/// Look up a record-literal field BY LABEL (normalize() sorts fields
/// alphabetically, so index-based access is wrong).
fn recGet(t: ?*dm.dhall.Term, label: [*:0]const u8) ?*dm.dhall.Term {
    const tt = t orelse return null;
    if (tt.tag != .TmRecordLit) return null;
    const n: usize = @intCast(@max(tt.as.rec.n, 0));
    if (tt.as.rec.fs == null) return null;
    const fs = tt.as.rec.fs.?[0..n];
    for (fs) |f| {
        const lbl = f.label orelse continue;
        if (seq(lbl, label)) return f.value;
    }
    return null;
}

/// Flatten a (normalized) Text term into out (cap bytes incl NUL).
fn textFlat(t: ?*dm.dhall.Term, out: []u8) bool {
    const tt = t orelse {
        walkError("expected Text", .{});
        return false;
    };
    if (tt.tag != .TmText or tt.as.text == null) {
        walkError("expected Text", .{});
        return false;
    }
    var n: usize = 0;
    var p: ?*dm.dhall.TextPart = tt.as.text;
    while (p) |part| : (p = part.next) {
        if (part.expr != null) {
            walkError("text interpolation not normalized", .{});
            return false;
        }
        if (part.lit) |lit| n += slen(lit);
    }
    if (n + 1 > out.len) {
        walkError("text too long", .{});
        return false;
    }
    var len: usize = 0;
    p = tt.as.text;
    while (p) |part| : (p = part.next) {
        if (part.lit) |lit| {
            const l = slen(lit);
            @memcpy(out[len .. len + l], lit[0..l]);
            len += l;
        }
    }
    out[len] = 0;
    return true;
}

/// Collect a normalized list term's elements into elems[0..cap].  Returns the
/// count, or null on error.  (Mirrors list_elems, including the cap check
/// firing at cap elems — callers pass cap+1 slots when overflow must be an
/// error, and the raw cap when it must truncate.)
fn listElems(t: ?*dm.dhall.Term, elems: []?*dm.dhall.Term) ?usize {
    var p: ?*dm.dhall.Term = t;
    var n: usize = 0;
    while (p) |pp| {
        if (pp.tag != .TmCons) break;
        if (n == elems.len) {
            walkError("list too long (cap %d)", .{@as(c_int, @intCast(elems.len))});
            return null;
        }
        elems[n] = pp.as.cons.head;
        n += 1;
        p = pp.as.cons.tail;
    }
    if (p == null or p.?.tag != .TmNil) {
        if (n == 0) {
            walkError("expected a list", .{});
            return null;
        }
        // Non-nil, non-cons tail after >=1 elems: C's loop stops and the
        // tag check only rejects when n==0 — treat as end of list.
    }
    return n;
}

/// Read Some n / None _ for a Natural bound (must fit u32).
fn readOptNat(t: ?*dm.dhall.Term, out: *u32, present: *bool) bool {
    const tt = t orelse {
        walkError("constraint field missing", .{});
        return false;
    };
    if (tt.tag == .TmNone) {
        present.* = false;
        return true;
    }
    if (tt.tag == .TmSome) {
        if (tt.as.some.val) |v| {
            if (v.tag == .TmConst) {
                const k = v.as.c;
                if (k.kind != .C_NAT) {
                    walkError("Natural bound expected", .{});
                    return false;
                }
                var val: u64 = undefined;
                var ok = true;
                if (k.bnat) |bn| {
                    val = dm.bignum.bignat_to_u64(bn, &ok);
                } else val = k.nat;
                if (!ok or val > 4294967295) {
                    walkError("Natural bound out of u32 range", .{});
                    return false;
                }
                out.* = @intCast(val);
                present.* = true;
                return true;
            }
        }
    }
    walkError("Natural bound expected", .{});
    return false;
}

/// Read Some n / None _ for an Integer bound (must fit i32).
fn readOptInt(t: ?*dm.dhall.Term, out: *i32, present: *bool) bool {
    const tt = t orelse {
        walkError("constraint field missing", .{});
        return false;
    };
    if (tt.tag == .TmNone) {
        present.* = false;
        return true;
    }
    if (tt.tag == .TmSome) {
        if (tt.as.some.val) |v| {
            if (v.tag == .TmConst) {
                const k = v.as.c;
                if (k.kind != .C_INT) {
                    walkError("Integer bound expected", .{});
                    return false;
                }
                if (k.big != null or k.i64 < std.math.minInt(i32) or k.i64 > std.math.maxInt(i32)) {
                    walkError("Integer bound out of i32 range", .{});
                    return false;
                }
                out.* = @intCast(k.i64);
                present.* = true;
                return true;
            }
        }
    }
    walkError("Integer bound expected", .{});
    return false;
}

/// Read Some s / None _ for a Text bound (regex).
fn readOptText(t: ?*dm.dhall.Term, out: []u8, present: *bool) bool {
    const tt = t orelse {
        walkError("constraint field missing", .{});
        return false;
    };
    if (tt.tag == .TmNone) {
        present.* = false;
        return true;
    }
    if (tt.tag == .TmSome) {
        if (tt.as.some.val) |v| {
            present.* = true;
            return textFlat(v, out);
        }
    }
    walkError("Text bound expected", .{});
    return false;
}

/// Read min/max bounds from a scalar payload record into `c`.
fn readMinMaxBounds(cs: *dl_colspec, value: ?*dm.dhall.Term, is_signed: bool) bool {
    const mn = recGet(value, "min");
    const mx = recGet(value, "max");
    if (is_signed) {
        var lo: i32 = 0;
        var hi: i32 = 0;
        var hlo = false;
        var hhi = false;
        if (!readOptInt(mn, &lo, &hlo)) return false;
        if (!readOptInt(mx, &hi, &hhi)) return false;
        if (hlo) {
            cs.has_min = 1;
            cs.min = @as(i64, lo);
        }
        if (hhi) {
            cs.has_max = 1;
            cs.max = @as(i64, hi);
        }
    } else {
        var lo: u32 = 0;
        var hi: u32 = 0;
        var hlo = false;
        var hhi = false;
        if (!readOptNat(mn, &lo, &hlo)) return false;
        if (!readOptNat(mx, &hi, &hhi)) return false;
        if (hlo) {
            cs.has_min = 1;
            cs.min = @as(i64, lo);
        }
        if (hhi) {
            cs.has_max = 1;
            cs.max = @as(i64, hi);
        }
    }
    if (cs.has_min != 0 and cs.has_max != 0 and cs.min > cs.max) {
        walkError("min > max", .{});
        return false;
    }
    return true;
}

/// Read a column's payload-union literal and map its selected alternative to
/// a dl_colspec (schema_load.c walk_coltype).
fn walkColtype(out: *dl_colspec, t: ?*dm.dhall.Term) bool {
    const tt = t orelse {
        walkError("column type must be a union literal", .{});
        return false;
    };
    if (tt.tag != .TmUnionLit) {
        walkError("column type must be a union literal", .{});
        return false;
    }
    var label: ?[*:0]u8 = null;
    var value: ?*dm.dhall.Term = null;
    {
        const n: usize = @intCast(@max(tt.as.uni.n, 0));
        if (tt.as.uni.fs != null) {
            const fs = tt.as.uni.fs.?[0..n];
            for (fs) |f| {
                if (f.value != null) {
                    label = f.label;
                    value = f.value;
                    break;
                }
            }
        }
    }
    const lbl = label orelse {
        walkError("column type union has no selected alternative", .{});
        return false;
    };

    var spec: dl_colspec = std.mem.zeroes(dl_colspec);

    if (seq(lbl, "Natural")) {
        spec.tag = DLT_NATURAL;
    } else if (seq(lbl, "Text")) {
        spec.tag = DLT_TEXT;
    } else if (seq(lbl, "Bool")) {
        spec.tag = DLT_BOOL;
    } else if (seq(lbl, "Char")) {
        spec.tag = DLT_CHAR;
    } else if (seq(lbl, "Date")) {
        spec.tag = DLT_DATE;
    } else if (seq(lbl, "Timestamp")) {
        spec.tag = DLT_TIMESTAMP;
    } else if (seq(lbl, "Signed")) {
        spec.tag = DLT_SIGNED;
    } else if (seq(lbl, "List") or seq(lbl, "Optional")) {
        const elem = recGet(value, "elem") orelse {
            walkError("column type '%s' is missing its 'elem' field", .{lbl});
            return false;
        };
        var ec: dl_colspec = undefined;
        if (!walkColtype(&ec, elem)) return false;
        if (ec.tag == DLT_LIST or ec.tag == DLT_OPTIONAL or ec.tag == DLT_ENUM) {
            walkError("nested parameterized element type not supported (v1)", .{});
            return false;
        }
        spec.tag = if (seq(lbl, "List")) DLT_LIST else DLT_OPTIONAL;
        spec.elem = ec.tag;
    } else if (seq(lbl, "Enum")) {
        const vals = recGet(value, "values") orelse {
            walkError("column type 'Enum' is missing its 'values' field", .{});
            return false;
        };
        var elems: [DL_ENUM_MAX_VALUES + 1]?*dm.dhall.Term = @splat(null);
        const n = listElems(vals, &elems) orelse return false;
        if (n == 0) {
            walkError("Enum must have at least one value", .{});
            return false;
        }
        if (n > DL_ENUM_MAX_VALUES) {
            walkError("Enum has more than %d values", .{@as(c_int, DL_ENUM_MAX_VALUES)});
            return false;
        }
        spec.tag = DLT_ENUM;
        spec.n_evalues = @intCast(n);
        var k: usize = 0;
        while (k < n) : (k += 1) {
            if (!textFlat(elems[k], &spec.evalues[k])) return false;
        }
    } else {
        walkError("unknown column type '%s'", .{lbl});
        return false;
    }

    // Per-column value constraints from the scalar payload record (data-load
    // metadata only; dl_colspec_eq ignores them).
    if (spec.tag == DLT_NATURAL or spec.tag == DLT_CHAR or spec.tag == DLT_DATE or spec.tag == DLT_TIMESTAMP) {
        if (recGet(value, "min") != null or recGet(value, "max") != null) {
            if (!readMinMaxBounds(&spec, value, false)) return false;
        }
    } else if (spec.tag == DLT_SIGNED) {
        if (recGet(value, "min") != null or recGet(value, "max") != null) {
            if (!readMinMaxBounds(&spec, value, true)) return false;
        }
    } else if (spec.tag == DLT_TEXT) {
        if (recGet(value, "regex")) |rx| {
            var present = false;
            if (!readOptText(rx, &spec.regex, &present)) return false;
            spec.has_regex = if (present) 1 else 0;
        }
    }

    out.* = spec;
    return true;
}

fn buildSchema(s: *dl_schema, nf: ?*dm.dhall.Term) bool {
    s.* = std.mem.zeroes(dl_schema);
    const root = nf orelse {
        walkError("schema must be a record", .{});
        return false;
    };
    if (root.tag != .TmRecordLit) {
        walkError("schema must be a record", .{});
        return false;
    }
    const relations = recGet(root, "relations") orelse {
        walkError("schema missing 'relations'", .{});
        return false;
    };
    var relems: [DL_SCHEMA_MAX_RELS]?*dm.dhall.Term = @splat(null);
    const nrels = listElems(relations, &relems) orelse return false;
    var i: usize = 0;
    while (i < nrels) : (i += 1) {
        var rname: [DL_SCHEMA_NAME_MAX]u8 = undefined;
        if (!textFlat(recGet(relems[i], "name"), &rname)) return false;
        const rname_z: [*:0]const u8 = @ptrCast(&rname);
        const columns = recGet(relems[i], "columns") orelse {
            walkError("relation '%s' missing 'columns'", .{rname_z});
            return false;
        };
        var celems: [DL_SCHEMA_MAX_ARITY]?*dm.dhall.Term = @splat(null);
        const arity = listElems(columns, &celems) orelse return false;
        var cols: [DL_SCHEMA_MAX_ARITY]dl_colspec = undefined;
        var j: usize = 0;
        while (j < arity) : (j += 1) {
            var cname: [DL_SCHEMA_NAME_MAX]u8 = undefined;
            const cnamet = recGet(celems[j], "name") orelse {
                walkError("relation '%s' column %d missing 'name'", .{ rname_z, @as(c_int, @intCast(j)) });
                return false;
            };
            if (!textFlat(cnamet, &cname)) return false;
            @memcpy(colnames[i][j][0..DL_SCHEMA_NAME_MAX], &(std.mem.zeroes([DL_SCHEMA_NAME_MAX]u8)));
            {
                var k: usize = 0;
                while (k < DL_SCHEMA_NAME_MAX and cname[k] != 0) : (k += 1) colnames[i][j][k] = cname[k];
            }
            const typ = recGet(celems[j], "type") orelse {
                walkError("relation '%s' column %d missing 'type'", .{ rname_z, @as(c_int, @intCast(j)) });
                return false;
            };
            if (!walkColtype(&cols[j], typ)) return false;
        }
        // is_idb is inferred later (rule-head analysis); all EDB here.
        if (schema_add_dyn(s, rname_z, @intCast(arity), &cols, 0) != 0) {
            walkError("cannot add relation '%s' (arity %d)", .{ rname_z, @as(c_int, @intCast(arity)) });
            return false;
        }
    }
    return true;
}

/// dl_schema_add / dl_schema_find resolved from the .so (schema.zig exports
/// them; declaring them extern here avoids a second copy of the exports in
/// the exe).
extern "c" fn dl_schema_add(s: ?*dl_schema, name: ?[*:0]const u8, arity: u8, cols: ?[*]const dl_colspec, is_idb: c_int) c_int;
extern "c" fn dl_schema_find(s: ?*const dl_schema, name: ?[*:0]const u8) ?*const dl_reldef;

fn schema_add_dyn(s: *dl_schema, name: [*:0]const u8, arity: u8, cols: [*]const dl_colspec, is_idb: c_int) c_int {
    return dl_schema_add(s, name, arity, cols, is_idb);
}

fn readAllAlloc(path: [*:0]const u8) ?[]u8 {
    const f = c.fopen(path, "rb") orelse return null;
    defer _ = c.fclose(f);
    var list: std.ArrayList(u8) = .empty;
    var buf: [65536]u8 = undefined;
    while (true) {
        const n = c.fread(&buf, 1, buf.len, f);
        if (n == 0) break;
        list.appendSlice(alloc(), buf[0..n]) catch oom();
    }
    // NUL-terminate (parse_source takes a C string).
    list.append(alloc(), 0) catch oom();
    return list.items;
}

/// dlp_schema_load: parse + typecheck + normalize + walk a schema.dhall into
/// a dl_schema.  0 on success, -1 with a diagnostic in errbuf.
fn schemaLoad(s: *dl_schema, path: [*:0]const u8, e: *Err) c_int {
    errClear(e);
    const src = readAllAlloc(path) orelse
        return seterr(e, "cannot open schema file '%s'", .{path});
    // readAllAlloc NUL-terminates the buffer for parse_source.
    const src_z: [*:0]const u8 = @ptrCast(src.ptr);

    if (dm.arena.dhall_arena == null) dm.arena.dhall_arena = dm.arena.arena_new();
    dm.arena.arena_reset(dm.arena.dhall_arena.?);

    const loader = dm.import_mod.import_loader_new();
    dm.import_mod.import_loader_push_root(loader, path);

    var p: dm.dhall.Parser = std.mem.zeroes(dm.dhall.Parser);
    p.loader = loader;
    var err: dm.dhall.DhallError = undefined;
    dm.ast.dhall_error_clear(&err);

    const t = dm.parser.parse_source(&p, src_z, path, &err);
    if (t == null) {
        _ = seterr(e, "schema parse error: %s", .{@as([*:0]const u8, @ptrCast(&err.msg))});
        dm.import_mod.import_loader_free(loader);
        return -1;
    }
    // Typecheck the schema against its own declarations before walking it.
    const ty = dm.typecheck.infer_type(&p, t.?, &err);
    if (ty == null) {
        _ = seterr(e, "schema type error: %s", .{@as([*:0]const u8, @ptrCast(&err.msg))});
        dm.import_mod.import_loader_free(loader);
        return -1;
    }
    dm.normalize.normalize_clear_error();
    const nf = dm.normalize.normalize(t.?);
    if (dm.normalize.normalize_has_error()) {
        const nerr = dm.normalize.normalize_get_error();
        _ = seterr(e, "schema normalize error: %s", .{@as([*:0]const u8, @ptrCast(&nerr.msg))});
        dm.import_mod.import_loader_free(loader);
        return -1;
    }
    dm.import_mod.import_loader_free(loader);

    if (!buildSchema(s, nf)) {
        _ = seterr(e, "schema error: %s", .{@as([*:0]const u8, &walk_err_buf)});
        return -1;
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// dlp/coerce.h — scalar coercion/printing helpers (dlp-local)
// ═══════════════════════════════════════════════════════════════════════════

/// True when `s` equals `lit` case-insensitively (ASCII).
fn strIeq(s: [*c]const u8, lit: [*:0]const u8) bool {
    if (@intFromPtr(s) == 0) return false;
    var i: usize = 0;
    while (true) : (i += 1) {
        var a = s[i];
        var b = lit[i];
        if (a >= 'A' and a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' and b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return false;
        if (a == 0) return true;
    }
}

/// Parse Bool: "true"/"false" (case-insens) or "0"/"1".
fn parseBool(s: [*c]const u8, out: *u32) bool {
    if (@intFromPtr(s) == 0) return false;
    if (strIeq(s, "true")) {
        out.* = 1;
        return true;
    }
    if (strIeq(s, "false")) {
        out.* = 0;
        return true;
    }
    if (s[0] == '1' and s[1] == 0) {
        out.* = 1;
        return true;
    }
    if (s[0] == '0' and s[1] == 0) {
        out.* = 0;
        return true;
    }
    return false;
}

/// Decode one UTF-8 scalar from s (len bytes available).  0xFFFFFFFF on error.
fn utf8DecodeCp(s: [*]const u8, len: usize, consumed: *usize) u32 {
    consumed.* = 1;
    if (len == 0) return 0xFFFFFFFF;
    const u = s;
    if (u[0] < 0x80) return u[0];
    if ((u[0] & 0xE0) == 0xC0) {
        if (len < 2 or (u[1] & 0xC0) != 0x80) return 0xFFFFFFFF;
        const cp: u32 = (@as(u32, u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        if (cp < 0x80) return 0xFFFFFFFF; // overlong
        consumed.* = 2;
        return cp;
    }
    if ((u[0] & 0xF0) == 0xE0) {
        if (len < 3 or (u[1] & 0xC0) != 0x80 or (u[2] & 0xC0) != 0x80) return 0xFFFFFFFF;
        const cp: u32 = (@as(u32, u[0] & 0x0F) << 12) | (@as(u32, u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        if (cp < 0x800) return 0xFFFFFFFF; // overlong
        if (cp >= 0xD800 and cp <= 0xDFFF) return 0xFFFFFFFF; // surrogate
        consumed.* = 3;
        return cp;
    }
    if ((u[0] & 0xF8) == 0xF0) {
        if (len < 4 or (u[1] & 0xC0) != 0x80 or (u[2] & 0xC0) != 0x80 or (u[3] & 0xC0) != 0x80) return 0xFFFFFFFF;
        const cp: u32 = (@as(u32, u[0] & 0x07) << 18) | (@as(u32, u[1] & 0x3F) << 12) | (@as(u32, u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        if (cp < 0x10000 or cp > 0x10FFFF) return 0xFFFFFFFF; // overlong / out of range
        consumed.* = 4;
        return cp;
    }
    return 0xFFFFFFFF; // continuation byte / invalid lead
}

/// Parse Char: EXACTLY one UTF-8 scalar, no trailing bytes.
fn parseChar(s: [*c]const u8, len: usize, out: *u32) bool {
    if (@intFromPtr(s) == 0 or len == 0) return false;
    var used: usize = 0;
    const cp = utf8DecodeCp(s, len, &used);
    if (cp == 0xFFFFFFFF or used != len) return false;
    out.* = cp;
    return true;
}

/// Days-in-month Gregorian check; returns the yyyymmdd u32 or 0 on error.
fn dateYmd(y: i64, m: i64, d: i64) u32 {
    if (m < 1 or m > 12) return 0;
    const dim: i64 = switch (m) {
        1, 3, 5, 7, 8, 10, 12 => 31,
        4, 6, 9, 11 => 30,
        else => if ((@mod(y, 4) == 0 and @mod(y, 100) != 0) or @mod(y, 400) == 0) 29 else 28,
    };
    if (d < 1 or d > dim) return 0;
    if (y < 0 or y > 9999) return 0;
    return @intCast(y * 10000 + m * 100 + d);
}

/// Parse Date "yyyy-mm-dd" (validated) -> yyyymmdd u32.
fn parseDate(s: [*c]const u8, out: *u32) bool {
    if (@intFromPtr(s) == 0) return false;
    var y: i64 = 0;
    var m: i64 = 0;
    var d: i64 = 0;
    var i: usize = 0;
    // yyyy
    while (i < 4 and s[i] >= '0' and s[i] <= '9') : (i += 1) {
        y = y * 10 + (s[i] - '0');
    }
    if (i != 4 or s[i] != '-') return false;
    i += 1;
    // mm
    while (i < 7 and s[i] >= '0' and s[i] <= '9') : (i += 1) {
        m = m * 10 + (s[i] - '0');
    }
    if (i != 7 or s[i] != '-') return false;
    i += 1;
    // dd
    while (i < 10 and s[i] >= '0' and s[i] <= '9') : (i += 1) {
        d = d * 10 + (s[i] - '0');
    }
    if (i != 10 or s[i] != 0) return false;
    const v = dateYmd(y, m, d);
    if (v == 0) return false;
    out.* = v;
    return true;
}

/// Print a yyyymmdd u32 as "yyyy-mm-dd" (buf >= 11 bytes).
fn printDate(ymd: u32, buf: [*c]u8, cap: usize) void {
    const y: u32 = ymd / 10000;
    const md: u32 = ymd % 10000;
    const m: u32 = md / 100;
    const d: u32 = md % 100;
    _ = sn(buf, cap, "%04u-%02u-%02u", .{ y, m, d });
}

/// Zigzag-encode an i32 into a u32 (bijective; preserves equality, NOT order).
fn zigzag(v: i32) u32 {
    return (@as(u32, @bitCast(v)) << 1) ^ @as(u32, @bitCast(v >> 31));
}

/// Parse Signed: optional +/- then decimal digits within i32.
fn parseSigned(s: [*c]const u8, out: *u32) bool {
    if (@intFromPtr(s) == 0) return false;
    var neg = false;
    var p: usize = 0;
    if (s[p] == '-') {
        neg = true;
        p += 1;
    } else if (s[p] == '+') {
        p += 1;
    }
    if (s[p] == 0) return false;
    var n: i64 = 0;
    while (s[p] != 0) : (p += 1) {
        if (s[p] < '0' or s[p] > '9') return false;
        n = n * 10 + (s[p] - '0');
        if (n > 2147483648) return false; // > INT32_MAX+1
    }
    if (neg) {
        if (n > 2147483648) return false; // < INT32_MIN
        n = -n;
    } else if (n > 2147483647) {
        return false;
    }
    out.* = zigzag(@intCast(n));
    return true;
}

/// De-zigzag a stored u32 back to i32.
fn dezigzag(z: u32) i32 {
    return @bitCast((z >> 1) ^ (0 -% (z & 1)));
}

/// Enforce a column's min/max on a coerced raw u32 (coerce.h check_minmax).
fn checkMinmax(cs: ?*const dl_colspec, raw: u32) bool {
    const cc = cs orelse return true;
    const v: i64 = switch (cc.tag) {
        DLT_NATURAL, DLT_TIMESTAMP, DLT_DATE, DLT_CHAR => @as(i64, raw),
        DLT_SIGNED => @as(i64, dezigzag(raw)),
        else => return true,
    };
    if (cc.has_min != 0 and v < cc.min) return false;
    if (cc.has_max != 0 and v > cc.max) return false;
    return true;
}

/// Full-key match of NUL-terminated s against a compiled regex DFA
/// (implicitly ^...$ anchored).  dlp-local (coerce.h), walked over the DFA's
/// trans[s*256+byte] graph.
fn regexDfaFullMatch(dfa: ?*const dx.regex_dfa, s: ?[*:0]const u8) bool {
    if (dfa == null or s == null or dfa.?.n_states == 0) return false;
    const d: *const dx.regex_dfa = @ptrCast(dfa.?);
    const trans: ?[*]const u32 = @ptrCast(d.trans);
    const accept: ?[*]const u8 = @ptrCast(d.accept);
    if (trans == null or accept == null) return false;
    const str: [*:0]const u8 = s.?;
    var st: u32 = 0;
    var p: usize = 0;
    while (str[p] != 0) : (p += 1) {
        st = trans.?[@as(usize, st) * 256 + str[p]];
        if (st == 0xFFFFFFFF) return false; // DFA_DEAD
    }
    return accept.?[st] != 0;
}

/// UTF-8-encode one codepoint into buf (>= 4 bytes); returns bytes written.
fn utf8EncodeCp(cp: u32, buf: [*]u8) usize {
    if (cp < 0x80) {
        buf[0] = @intCast(cp);
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = @intCast(0xC0 | (cp >> 6));
        buf[1] = @intCast(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = @intCast(0xE0 | (cp >> 12));
        buf[1] = @intCast(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = @intCast(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = @intCast(0xF0 | (cp >> 18));
    buf[1] = @intCast(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = @intCast(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = @intCast(0x80 | (cp & 0x3F));
    return 4;
}

// ═══════════════════════════════════════════════════════════════════════════
// Small shared helpers (csv_load.c/json_load.c/workflow.c statics)
// ═══════════════════════════════════════════════════════════ heaps═════════

/// Free every compiled regex DFA in rdfs[0..n] (NULL-safe, idempotent).
fn freeRdfs(rdfs: []?*dx.regex_dfa) void {
    for (rdfs) |*slot| {
        if (slot.* != null) {
            dx.regex_dfa_free(slot.*);
            slot.* = null;
        }
    }
}

/// Append "[a, b, c]" of names to buf (join_names; snprintf-append
/// semantics, then the closing ']').
fn joinNames2(buf: []u8, names: []const [*:0]const u8) void {
    var off: usize = 0;
    _ = sn(buf.ptr + off, buf.len - off, "%s", .{@as([*:0]const u8, "[")});
    off += 1;
    for (names, 0..) |nm, i| {
        if (off >= buf.len) break;
        const w = sn(buf.ptr + off, buf.len - off, "%s%s", .{
            @as([*:0]const u8, if (i != 0) ", " else ""),
            nm,
        });
        if (w < 0) break;
        const uw: usize = @intCast(w);
        if (off + uw >= buf.len) break;
        off += uw;
    }
    if (off + 1 < buf.len) {
        buf[off] = ']';
        buf[off + 1] = 0;
    }
}

/// Parse a Natural (decimal digits only, <= u32 max) from a NUL-terminated
/// cell (csv_load.c parse_nat).
fn parseNat(cell: [*:0]const u8, out: *u32) bool {
    if (cell[0] == 0) return false;
    var v: u64 = 0;
    var p: usize = 0;
    while (cell[p] != 0) : (p += 1) {
        if (cell[p] < '0' or cell[p] > '9') return false;
        v = v * 10 + (cell[p] - '0');
        if (v > 4294967295) return false;
    }
    out.* = @intCast(v);
    return true;
}

/// Trim ASCII whitespace in a NUL-terminated mutable buffer; returns the
/// leading pointer with trailing ws NUL'd.
fn trimWs(s: [*]u8) [*:0]u8 {
    var p: [*]u8 = s;
    while (p[0] != 0 and isSpace(p[0])) p += 1;
    var end: usize = 0;
    while (p[end] != 0) end += 1;
    while (end > 0 and isSpace(p[end - 1])) {
        end -= 1;
        p[end] = 0;
    }
    return @ptrCast(p);
}

fn isSpace(ch: u8) bool {
    return ch == ' ' or ch == '\t' or ch == '\n' or ch == '\r' or ch == 0x0b or ch == 0x0c;
}

/// Strip a single surrounding "..." pair if present (in place).
fn unquote(s: [*]u8) void {
    const n = slen(@ptrCast(s));
    if (n >= 2 and s[0] == '"' and s[n - 1] == '"') {
        var i: usize = 0;
        while (i + 1 < n - 1) : (i += 1) {} // no-op; memmove below
        std.mem.copyForwards(u8, s[0 .. n - 2], (s + 1)[0 .. n - 2]);
        s[n - 2] = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// dlp/csv_load.c — typed CSV loader
// ═══════════════════════════════════════════════════════════════════════════

/// Split one CSV line into fields, honouring a single surrounding "..." quote
/// pair (csv_split).  Fields are NUL-terminated in place.  Returns the count
/// (fields beyond cap are counted but not stored).
fn csvSplit(line: [*:0]u8, out: [][*:0]u8) usize {
    var n: usize = 0;
    var p: [*]u8 = @ptrCast(line);
    while (p[0] != 0) {
        var start: [*]u8 = p;
        var quoted = false;
        if (p[0] == '"') {
            quoted = true;
            start = p + 1;
            p += 1;
        }
        while (p[0] != 0) {
            if (p[0] == '"') {
                p[0] = 0;
                p += 1;
                break;
            }
            if (p[0] == ',' and !quoted) break;
            p += 1;
        }
        if (p[0] == ',') {
            p[0] = 0;
            p += 1;
        }
        if (n < out.len) {
            out[n] = @ptrCast(start);
            n += 1;
        } else {
            n += 1;
        }
    }
    return n;
}

/// Split a list-inner string "a,b,c" (already un-bracketed) into elements,
/// honoring a single surrounding '...' quote pair (list_split).  Returns the
/// TOTAL element count (may exceed out.len; callers reject overflow).
fn listSplit(s: [*:0]u8, out: [][*:0]u8) usize {
    var n: usize = 0;
    var p: [*]u8 = @ptrCast(s);
    while (p[0] != 0) {
        var start: [*]u8 = p;
        var quoted = false;
        if (p[0] == '\'') {
            quoted = true;
            start = p + 1;
            p += 1;
        }
        while (p[0] != 0) {
            if (p[0] == '\'') {
                p[0] = 0;
                p += 1;
                break;
            }
            if (p[0] == ',' and !quoted) break;
            p += 1;
        }
        if (p[0] == ',') {
            p[0] = 0;
            p += 1;
        }
        if (n < out.len) out[n] = @ptrCast(start);
        n += 1;
    }
    return n;
}

/// Coerce ONE List/Optional ELEMENT cell against a flat scalar elem type.
fn coerceElemCell(db: ?*dl_db, elem: dl_coltype, cell: [*:0]const u8, out: *u32) bool {
    switch (elem) {
        DLT_NATURAL, DLT_TIMESTAMP => return parseNat(cell, out),
        DLT_TEXT => {
            out.* = if (db) |d| dx.dl_intern_str(d, cell) else 0;
            return true;
        },
        DLT_BOOL => return parseBool(cell, out),
        DLT_CHAR => return parseChar(cell, slen(cell), out),
        DLT_DATE => return parseDate(cell, out),
        DLT_SIGNED => return parseSigned(cell, out),
        else => return false,
    }
}

/// dlp_csv_load: load a CSV file into relation `rel` of schema `s`.  db ==
/// null is a DRY-RUN (validate + coerce only).  Returns the fact count, or
/// -1 with a diagnostic in errbuf.
fn csvLoad(db: ?*dl_db, s: *const dl_schema, rel: [*:0]const u8, path: [*:0]const u8, e: *Err) c_int {
    errClear(e);
    const r = dl_schema_find(s, rel) orelse
        return seterr(e, "relation '%s' not declared in schema.dhall", .{rel});
    if (r.is_idb != 0)
        return seterr(e, "%s:1: relation '%s' is rule-defined (IDB); put facts in an EDB relation", .{ path, rel });

    const f = c.fopen(path, "rb") orelse
        return seterr(e, "cannot open '%s'", .{path});

    var line_buf: ?[*]u8 = null;
    var line_cap: usize = 0;
    var lineno: c_int = 0;
    var fact_count: c_int = 0;

    // Compile each regex-constrained Text column's regex ONCE (~49 MiB per
    // DFA — never per-cell).
    var rdfs: [DL_SCHEMA_MAX_ARITY]?*dx.regex_dfa = @splat(null);
    var nerr: ?c_int = null; // set on the compile-fail early return
    compile_rdfs: {
        var cj: usize = 0;
        while (cj < r.arity) : (cj += 1) {
            if (r.cols[cj].tag == DLT_TEXT and r.cols[cj].has_regex != 0) {
                const rx: [*:0]const u8 = @ptrCast(&r.cols[cj].regex);
                const d0 = dx.regex_compile(rx);
                const d: ?*dx.regex_dfa = @ptrCast(d0);
                if (d == null or d.?.errmsg != null or d.?.n_states == 0) {
                    const em: [*:0]const u8 = if (d != null and d.?.errmsg != null) d.?.errmsg.? else "compile failed";
                    nerr = seterr(e, "%s: bad regex '%s' on column '%s': %s", .{ path, rx, colnameOf(s, rel, cj), em });
                    break :compile_rdfs;
                }
                rdfs[cj] = d;
            }
        }

        // Header line.
        const hlen = getline(&line_buf, &line_cap, f);
        if (hlen <= 0) {
            nerr = seterr(e, "%s: empty file (no header row)", .{path});
            break :compile_rdfs;
        }
        var len: usize = @intCast(hlen);
        lineno = 1;
        trimEol(line_buf.?[0..len], &len);
        line_buf.?[len] = 0;

        var hdr: [DL_SCHEMA_MAX_ARITY][*:0]u8 = undefined;
        const nhdr = csvSplit(@ptrCast(line_buf.?), &hdr);

        var colmap: [DL_SCHEMA_MAX_ARITY]usize = undefined;
        var used: [DL_SCHEMA_MAX_ARITY]bool = @splat(false);

        var hdr_err: ?c_int = null;
        header: {
            var i: usize = 0;
            while (i < nhdr) : (i += 1) {
                const name = trimWs(hdr[i]);
                var idx: ?usize = null;
                var j: usize = 0;
                while (j < r.arity) : (j += 1) {
                    if (seq(name, colnameOf(s, rel, j).?)) {
                        idx = j;
                        break;
                    }
                }
                if (idx == null) {
                    var got: [DL_SCHEMA_MAX_ARITY][*:0]const u8 = undefined;
                    var k: usize = 0;
                    while (k < nhdr and k < DL_SCHEMA_MAX_ARITY) : (k += 1) got[k] = trimWs(hdr[k]);
                    var exp: [256]u8 = undefined;
                    var gots: [256]u8 = undefined;
                    var expn: [DL_SCHEMA_MAX_ARITY][*:0]const u8 = undefined;
                    var m: usize = 0;
                    while (m < r.arity) : (m += 1) expn[m] = colnameOf(s, rel, m).?;
                    joinNames2(&exp, expn[0..r.arity]);
                    joinNames2(&gots, got[0..@min(nhdr, DL_SCHEMA_MAX_ARITY)]);
                    hdr_err = seterr(e, "%s:1: header error: relation '%s' has unknown column '%s'; expects %s, got %s", .{ path, rel, name, @as([*:0]const u8, @ptrCast(&exp)), @as([*:0]const u8, @ptrCast(&gots)) });
                    break :header;
                }
                if (used[idx.?]) {
                    hdr_err = seterr(e, "%s:1: header error: relation '%s' has duplicate column '%s'", .{ path, rel, name });
                    break :header;
                }
                used[idx.?] = true;
                colmap[i] = idx.?;
            }
            // Missing columns.
            var j: usize = 0;
            while (j < r.arity) : (j += 1) {
                if (!used[j]) {
                    var got: [DL_SCHEMA_MAX_ARITY][*:0]const u8 = undefined;
                    var k: usize = 0;
                    while (k < nhdr and k < DL_SCHEMA_MAX_ARITY) : (k += 1) got[k] = trimWs(hdr[k]);
                    var exp: [256]u8 = undefined;
                    var gots: [256]u8 = undefined;
                    var expn: [DL_SCHEMA_MAX_ARITY][*:0]const u8 = undefined;
                    var m: usize = 0;
                    while (m < r.arity) : (m += 1) expn[m] = colnameOf(s, rel, m).?;
                    joinNames2(&exp, expn[0..r.arity]);
                    joinNames2(&gots, got[0..@min(nhdr, DL_SCHEMA_MAX_ARITY)]);
                    hdr_err = seterr(e, "%s:1: header error: relation '%s' is missing column '%s'; expects %s, got %s", .{ path, rel, colnameOf(s, rel, j), @as([*:0]const u8, @ptrCast(&exp)), @as([*:0]const u8, @ptrCast(&gots)) });
                    break :header;
                }
            }
        }
        if (hdr_err != null) {
            nerr = hdr_err;
            break :compile_rdfs;
        }

        // Data rows.
        data: while (true) {
            const dlen = getline(&line_buf, &line_cap, f);
            if (dlen <= 0) break :data;
            var dlen_u: usize = @intCast(dlen);
            lineno += 1;
            trimEol(line_buf.?[0..dlen_u], &dlen_u);
            line_buf.?[dlen_u] = 0;
            const s2 = trimWs(line_buf.?);
            if (s2[0] == 0) continue; // skip blank lines

            var fields: [DL_SCHEMA_MAX_ARITY][*:0]u8 = undefined;
            const nf = csvSplit(@ptrCast(line_buf.?), &fields);
            if (nf != r.arity) {
                nerr = seterr(e, "%s:%d: expected %d columns, got %d", .{ path, lineno, @as(c_int, r.arity), @as(c_int, @intCast(nf)) });
                break :compile_rdfs;
            }

            var cols: [DL_SCHEMA_MAX_ARITY]u32 = undefined;
            var i: usize = 0;
            while (i < nf) : (i += 1) {
                const j = colmap[i];
                switch (r.cols[j].tag) {
                    DLT_NATURAL, DLT_TIMESTAMP => {
                        const cell = trimWs(fields[i]);
                        if (!parseNat(cell, &cols[j])) {
                            nerr = seterr(e, "%s:%d:%d: column '%s' expects Natural, got \"%s\"", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), cell });
                            break :compile_rdfs;
                        }
                    },
                    DLT_TEXT => {
                        // Verbatim (minus one quote pair), interned.  NOT
                        // trimmed.  Regex-constrained columns must match.
                        const cell: [*]u8 = @ptrCast(fields[i]);
                        unquote(cell);
                        if (rdfs[j] != null and !regexDfaFullMatch(rdfs[j].?, @ptrCast(cell))) {
                            const rx: [*:0]const u8 = @ptrCast(&r.cols[j].regex);
                            nerr = seterr(e, "%s:%d:%d: column '%s' value \"%s\" does not match regex '%s'", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), @as([*:0]const u8, @ptrCast(cell)), rx });
                            break :compile_rdfs;
                        }
                        cols[j] = if (db) |d| dx.dl_intern_str(d, cell) else 0;
                    },
                    DLT_BOOL => {
                        const cell = trimWs(fields[i]);
                        if (!parseBool(cell, &cols[j])) {
                            nerr = seterr(e, "%s:%d:%d: column '%s' expects Bool, got \"%s\"", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), cell });
                            break :compile_rdfs;
                        }
                    },
                    DLT_CHAR => {
                        const cell = trimWs(fields[i]);
                        if (!parseChar(cell, slen(cell), &cols[j])) {
                            nerr = seterr(e, "%s:%d:%d: column '%s' expects Char (one UTF-8 scalar), got \"%s\"", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), cell });
                            break :compile_rdfs;
                        }
                    },
                    DLT_DATE => {
                        const cell = trimWs(fields[i]);
                        if (!parseDate(cell, &cols[j])) {
                            nerr = seterr(e, "%s:%d:%d: column '%s' expects Date (yyyy-mm-dd), got \"%s\"", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), cell });
                            break :compile_rdfs;
                        }
                    },
                    DLT_SIGNED => {
                        const cell = trimWs(fields[i]);
                        if (!parseSigned(cell, &cols[j])) {
                            nerr = seterr(e, "%s:%d:%d: column '%s' expects Signed, got \"%s\"", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), cell });
                            break :compile_rdfs;
                        }
                    },
                    DLT_LIST => {
                        const cell: [*]u8 = @ptrCast(fields[i]);
                        unquote(cell);
                        const ln = slen(@ptrCast(cell));
                        if (ln < 2 or cell[0] != '[' or cell[ln - 1] != ']') {
                            nerr = seterr(e, "%s:%d:%d: column '%s' expects List \"[...]\", got \"%s\"", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), @as([*:0]const u8, @ptrCast(cell)) });
                            break :compile_rdfs;
                        }
                        cell[ln - 1] = 0;
                        const inner: [*:0]u8 = @ptrCast(cell + 1); // skip '['
                        var elems: [DLP_LIST_MAX_ELEMS][*:0]u8 = undefined;
                        const ne = listSplit(inner, &elems);
                        if (ne > DLP_LIST_MAX_ELEMS) {
                            nerr = seterr(e, "%s:%d:%d: column '%s' List has too many elements (cap %d)", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), @as(c_int, DLP_LIST_MAX_ELEMS) });
                            break :compile_rdfs;
                        }
                        var acc: u32 = dx.TERM_NIL;
                        var ei: isize = @as(isize, @intCast(ne)) - 1;
                        while (ei >= 0) : (ei -= 1) {
                            const ecell = trimWs(elems[@intCast(ei)]);
                            var ev: u32 = undefined;
                            if (!coerceElemCell(db, r.cols[j].elem, ecell, &ev)) {
                                nerr = seterr(e, "%s:%d:%d: column '%s' List element %d is not coercible to its element type", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), @as(c_int, @intCast(ei)) });
                                break :compile_rdfs;
                            }
                            acc = dx.dl_term_cons(db, ev, acc);
                        }
                        cols[j] = acc;
                    },
                    DLT_OPTIONAL => {
                        const cell = trimWs(fields[i]);
                        if (cell[0] == 0) {
                            cols[j] = DLP_OPT_NONE;
                        } else if (!coerceElemCell(db, r.cols[j].elem, cell, &cols[j])) {
                            nerr = seterr(e, "%s:%d:%d: column '%s' Optional element is not coercible to its element type", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j) });
                            break :compile_rdfs;
                        }
                    },
                    DLT_ENUM => {
                        const cell: [*]u8 = @ptrCast(trimWs(fields[i]));
                        unquote(cell);
                        var ok = false;
                        var k: usize = 0;
                        while (k < r.cols[j].n_evalues) : (k += 1) {
                            if (seq(@ptrCast(cell), @ptrCast(&r.cols[j].evalues[k]))) {
                                ok = true;
                                break;
                            }
                        }
                        if (!ok) {
                            const first: [*:0]const u8 = if (r.cols[j].n_evalues > 0) @ptrCast(&r.cols[j].evalues[0]) else "";
                            nerr = seterr(e, "%s:%d:%d: column '%s' expects an Enum value from {%s}, got \"%s\"", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j), first, @as([*:0]const u8, @ptrCast(cell)) });
                            break :compile_rdfs;
                        }
                        cols[j] = if (db) |d| dx.dl_intern_str(d, cell) else 0;
                    },
                    else => {
                        nerr = seterr(e, "%s:%d:%d: column '%s' has an unsupported type for CSV loading", .{ path, lineno, @as(c_int, @intCast(i + 1)), colnameOf(s, rel, j) });
                        break :compile_rdfs;
                    },
                }
            }

            // Per-column min/max constraints.
            var j: usize = 0;
            while (j < r.arity) : (j += 1) {
                if (!checkMinmax(&r.cols[j], cols[j])) {
                    var rb: [64]u8 = undefined;
                    const cc = &r.cols[j];
                    if (cc.has_min != 0 and cc.has_max != 0)
                        _ = sn(&rb, rb.len, "[%lld..%lld]", .{ cc.min, cc.max })
                    else if (cc.has_min != 0)
                        _ = sn(&rb, rb.len, "[%lld..]", .{cc.min})
                    else if (cc.has_max != 0)
                        _ = sn(&rb, rb.len, "[..%lld]", .{cc.max})
                    else
                        rb[0] = 0;
                    nerr = seterr(e, "%s:%d: column '%s' value out of range (allowed %s)", .{ path, lineno, colnameOf(s, rel, j), @as([*:0]const u8, @ptrCast(&rb)) });
                    break :compile_rdfs;
                }
            }

            if (db) |d| {
                if (dx.dl_add_fact(d, rel, &cols, r.arity) < 0) {
                    nerr = seterr(e, "%s:%d: dl_add_fact failed", .{ path, lineno });
                    break :compile_rdfs;
                }
            }
            fact_count += 1;
        }
    }

    freeRdfs(&rdfs);
    if (line_buf != null) c.free(line_buf);
    _ = c.fclose(f);
    if (nerr != null) return nerr.?;
    return fact_count;
}

/// Strip a trailing "\n" then "\r" (C's two-step EOL chomp) and NUL-terminate.
fn trimEol(buf: []u8, len: *usize) void {
    if (len.* > 0 and buf[len.* - 1] == '\n') len.* -= 1;
    if (len.* > 0 and buf[len.* - 1] == '\r') len.* -= 1;
    buf[len.*] = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// dlp/json_load.c — typed JSON loader (std.json parse tree)
// ═══════════════════════════════════════════════════════════════════════════

const Json = std.json.Value;

/// Human-readable name for a JSON value type (diagnostics).
fn jsonTypeName(v: ?*const Json) [*:0]const u8 {
    const vv = v orelse return "missing";
    return switch (vv.*) {
        .null => @as([*:0]const u8, "null"),
        .bool => @as([*:0]const u8, "boolean"),
        .integer, .float, .number_string => @as([*:0]const u8, "number"),
        .string => @as([*:0]const u8, "string"),
        .array => @as([*:0]const u8, "array"),
        .object => @as([*:0]const u8, "object"),
    };
}

fn jsonObjGet(v: ?Json, key: []const u8) ?Json {
    const vv = v orelse return null;
    if (vv != .object) return null;
    return vv.object.get(key);
}

/// Coerce ONE List/Optional ELEMENT Json value against a flat scalar type.
fn coerceElemJson(db: ?*dl_db, elem: dl_coltype, v: anytype, out: *u32) bool {
    const vv: ?Json = switch (@typeInfo(@TypeOf(v))) {
        .optional => v,
        .pointer => v.*,
        else => v,
    };
    switch (elem) {
        DLT_NATURAL, DLT_TIMESTAMP => {
            const d = jsonNum(vv) orelse return false;
            if (!(d >= 0.0) or d > 4294967295.0 or d != @as(f64, @floatFromInt(@as(u64, @intFromFloat(d))))) return false;
            out.* = @intCast(@as(u64, @intFromFloat(d)));
            return true;
        },
        DLT_TEXT => {
            const val = vv orelse return false;
            if (val != .string) return false;
            out.* = if (db) |d| dx.dl_intern_str(d, val.string.ptr) else 0;
            return true;
        },
        DLT_BOOL => {
            const val = vv orelse return false;
            if (val != .bool) return false;
            out.* = if (val.bool) 1 else 0;
            return true;
        },
        DLT_CHAR => {
            const val = vv orelse return false;
            if (val != .string) return false;
            return parseChar(val.string.ptr, val.string.len, out);
        },
        DLT_DATE => {
            const val = vv orelse return false;
            if (val != .string) return false;
            return parseDate(val.string.ptr, out);
        },
        DLT_SIGNED => {
            const d = jsonNum(vv) orelse return false;
            if (d != @as(f64, @floatFromInt(@as(i64, @intFromFloat(d))))) return false;
            if (d < -2147483648.0 or d > 2147483647.0) return false;
            const sv: i64 = @intFromFloat(d);
            out.* = zigzag(@intCast(sv));
            return true;
        },
        else => return false,
    }
}

fn jsonNum(v: ?Json) ?f64 {
    const vv = v orelse return null;
    return switch (vv) {
        .integer => |i| @as(f64, @floatFromInt(i)),
        .float => |fl| fl,
        .number_string => |ns| std.fmt.parseFloat(f64, ns) catch null,
        else => null,
    };
}

/// dlp_json_load: load a JSON array-of-objects file into relation `rel`.
/// db == null is a DRY-RUN.  Returns the fact count, or -1 with a diagnostic.
fn jsonLoad(db: ?*dl_db, s: *const dl_schema, rel: [*:0]const u8, path: [*:0]const u8, e: *Err) c_int {
    errClear(e);
    const r = dl_schema_find(s, rel) orelse
        return seterr(e, "relation '%s' not declared in schema.dhall", .{rel});
    if (r.is_idb != 0)
        return seterr(e, "%s: relation '%s' is rule-defined (IDB); put facts in an EDB relation", .{ path, rel });

    const src = readAllAlloc(path) orelse
        return seterr(e, "cannot open '%s'", .{path});
    const body = src[0 .. src.len - 1];

    // Compile each regex-constrained Text column's regex ONCE.
    var rdfs: [DL_SCHEMA_MAX_ARITY]?*dx.regex_dfa = @splat(null);
    var j: usize = 0;
    while (j < r.arity) : (j += 1) {
        if (r.cols[j].tag == DLT_TEXT and r.cols[j].has_regex != 0) {
            const rx: [*:0]const u8 = @ptrCast(&r.cols[j].regex);
            const d0 = dx.regex_compile(rx);
            const d: ?*dx.regex_dfa = @ptrCast(d0);
            if (d == null or d.?.errmsg != null or d.?.n_states == 0) {
                const em: [*:0]const u8 = if (d != null and d.?.errmsg != null) d.?.errmsg.? else "compile failed";
                freeRdfs(&rdfs);
                return seterr(e, "%s: bad regex '%s' on column '%s': %s", .{ path, rx, colnameOf(s, rel, j), em });
            }
            rdfs[j] = d;
        }
    }

    const parsed = std.json.parseFromSlice(std.json.Value, alloc(), body, .{}) catch {
        freeRdfs(&rdfs);
        return seterr(e, "%s: malformed JSON", .{path});
    };
    const root = parsed.value;
    defer parsed.deinit();

    if (root != .array) {
        freeRdfs(&rdfs);
        return seterr(e, "%s: expected a JSON array of objects, got %s", .{ path, jsonTypeName(&root) });
    }

    var fact_count: c_int = 0;
    var failed = false;

    outer: for (root.array.items, 0..) |*el, eidx| {
        const e_i: c_int = @intCast(eidx);
        if (el.* != .object) {
            _ = seterr(e, "%s: element %d is not an object (got %s)", .{ path, e_i, jsonTypeName(el) });
            failed = true;
                        break :outer;
        }

        // Every object key must be a schema column; every column must appear.
        var used: [DL_SCHEMA_MAX_ARITY]bool = @splat(false);
        var it = el.object.iterator();
        while (it.next()) |kv| {
            const key = kv.key_ptr.*;
            const key_z = std.fmt.allocPrintSentinel(alloc(), "{s}", .{key}, 0) catch oom();
            var idx: ?usize = null;
            var jj: usize = 0;
            while (jj < r.arity) : (jj += 1) {
                if (seq(key_z.ptr, colnameOf(s, rel, jj).?)) {
                    idx = jj;
                    break;
                }
            }
            if (idx == null) {
                const ngot = @min(el.object.count(), DL_SCHEMA_MAX_ARITY);
                var got: [DL_SCHEMA_MAX_ARITY][*:0]const u8 = undefined;
                var mit = el.object.iterator();
                var m: usize = 0;
                while (mit.next()) |kv2| : (m += 1) {
                    if (m >= ngot) break;
                    got[m] = (std.fmt.allocPrintSentinel(alloc(), "{s}", .{kv2.key_ptr.*}, 0) catch oom()).ptr;
                }
                var exp: [256]u8 = undefined;
                var gots: [256]u8 = undefined;
                var expn: [DL_SCHEMA_MAX_ARITY][*:0]const u8 = undefined;
                var m2: usize = 0;
                while (m2 < r.arity) : (m2 += 1) expn[m2] = colnameOf(s, rel, m2).?;
                joinNames2(&exp, expn[0..r.arity]);
                joinNames2(&gots, got[0..ngot]);
                _ = seterr(e, "%s: element %d: unknown column '%s'; expects %s, got %s", .{ path, e_i, key_z.ptr, @as([*:0]const u8, @ptrCast(&exp)), @as([*:0]const u8, @ptrCast(&gots)) });
                failed = true;
                        break :outer;
            }
            used[idx.?] = true;
        }
        var jj: usize = 0;
        while (jj < r.arity) : (jj += 1) {
            if (!used[jj]) {
                const ngot = @min(el.object.count(), DL_SCHEMA_MAX_ARITY);
                var got: [DL_SCHEMA_MAX_ARITY][*:0]const u8 = undefined;
                var mit = el.object.iterator();
                var m: usize = 0;
                while (mit.next()) |kv2| : (m += 1) {
                    if (m >= ngot) break;
                    got[m] = (std.fmt.allocPrintSentinel(alloc(), "{s}", .{kv2.key_ptr.*}, 0) catch oom()).ptr;
                }
                var exp: [256]u8 = undefined;
                var gots: [256]u8 = undefined;
                var expn: [DL_SCHEMA_MAX_ARITY][*:0]const u8 = undefined;
                var m2: usize = 0;
                while (m2 < r.arity) : (m2 += 1) expn[m2] = colnameOf(s, rel, m2).?;
                joinNames2(&exp, expn[0..r.arity]);
                joinNames2(&gots, got[0..ngot]);
                _ = seterr(e, "%s: element %d: missing column '%s'; expects %s, got %s", .{ path, e_i, colnameOf(s, rel, jj), @as([*:0]const u8, @ptrCast(&exp)), @as([*:0]const u8, @ptrCast(&gots)) });
                failed = true;
                        break :outer;
            }
        }

        // Strictly typed values, cols[] in schema order.
        var cols: [DL_SCHEMA_MAX_ARITY]u32 = undefined;
        var cn: usize = 0;
        while (cn < r.arity) : (cn += 1) {
            const cname = colnameOf(s, rel, cn).?;
            const cname_s = std.mem.sliceTo(cname, 0);
            const v: ?Json = jsonObjGet(el.*, cname_s);
            switch (r.cols[cn].tag) {
                DLT_NATURAL, DLT_TIMESTAMP => {
                    if (jsonNum(v) == null) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Natural, got %s", .{ path, e_i, cname, jsonTypeName(if (v) |*vv| @as(*const Json, vv) else null) });
                        failed = true;
                        break :outer;
                    }
                    const d = jsonNum(v).?;
                    if (!(d >= 0.0) or d > 4294967295.0 or d != @as(f64, @floatFromInt(@as(u64, @intFromFloat(d))))) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Natural, got number", .{ path, e_i, cname });
                        failed = true;
                        break :outer;
                    }
                    cols[cn] = @intCast(@as(u64, @intFromFloat(d)));
                },
                DLT_TEXT => {
                    if (v == null or v.? != .string) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Text, got %s", .{ path, e_i, cname, jsonTypeName(if (v) |*vv| @as(*const Json, vv) else null) });
                        failed = true;
                        break :outer;
                    }
                    const str = v.?.string;
                    if (rdfs[cn] != null and !regexDfaFullMatch(rdfs[cn], @ptrCast(str.ptr))) {
                        const rx: [*:0]const u8 = @ptrCast(&r.cols[cn].regex);
                        _ = seterr(e, "%s: element %d: column '%s' value \"%s\" does not match regex '%s'", .{ path, e_i, cname, str.ptr, rx });
                        failed = true;
                        break :outer;
                    }
                    cols[cn] = if (db) |d| dx.dl_intern_str(d, str.ptr) else 0;
                },
                DLT_BOOL => {
                    if (v == null or v.? != .bool) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Bool, got %s", .{ path, e_i, cname, jsonTypeName(if (v) |*vv| @as(*const Json, vv) else null) });
                        failed = true;
                        break :outer;
                    }
                    cols[cn] = if (v.?.bool) 1 else 0;
                },
                DLT_CHAR => {
                    if (v == null or v.? != .string) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Char (one UTF-8 scalar), got %s", .{ path, e_i, cname, jsonTypeName(if (v) |*vv| @as(*const Json, vv) else null) });
                        failed = true;
                        break :outer;
                    }
                    if (!parseChar(v.?.string.ptr, v.?.string.len, &cols[cn])) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Char (one UTF-8 scalar), got string", .{ path, e_i, cname });
                        failed = true;
                        break :outer;
                    }
                },
                DLT_DATE => {
                    if (v == null or v.? != .string) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Date (yyyy-mm-dd), got %s", .{ path, e_i, cname, jsonTypeName(if (v) |*vv| @as(*const Json, vv) else null) });
                        failed = true;
                        break :outer;
                    }
                    if (!parseDate(v.?.string.ptr, &cols[cn])) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Date (yyyy-mm-dd), got string", .{ path, e_i, cname });
                        failed = true;
                        break :outer;
                    }
                },
                DLT_SIGNED => {
                    const d = jsonNum(v) orelse {
                        _ = seterr(e, "%s: element %d: column '%s' expects Signed, got %s", .{ path, e_i, cname, jsonTypeName(if (v) |*vv| @as(*const Json, vv) else null) });
                        failed = true;
                        break :outer;
                    };
                    if (d != @as(f64, @floatFromInt(@as(i64, @intFromFloat(d)))) or d < -2147483648.0 or d > 2147483647.0) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Signed (i32 integer), got number", .{ path, e_i, cname });
                        failed = true;
                        break :outer;
                    }
                    const sv: i64 = @intFromFloat(d);
                    cols[cn] = zigzag(@intCast(sv));
                },
                DLT_LIST => {
                    if (v == null or v.? != .array) {
                        _ = seterr(e, "%s: element %d: column '%s' expects List (JSON array), got %s", .{ path, e_i, cname, jsonTypeName(if (v) |*vv| @as(*const Json, vv) else null) });
                        failed = true;
                        break :outer;
                    }
                    const items = v.?.array.items;
                    const ne = items.len;
                    if (ne > DLP_LIST_MAX_ELEMS) {
                        _ = seterr(e, "%s: element %d: column '%s' List has too many elements (cap %d)", .{ path, e_i, cname, @as(c_int, DLP_LIST_MAX_ELEMS) });
                        failed = true;
                        break :outer;
                    }
                    var acc: u32 = dx.TERM_NIL;
                    var ei: usize = ne;
                    while (ei > 0) {
                        ei -= 1;
                        var ev: u32 = undefined;
                        if (!coerceElemJson(db, r.cols[cn].elem, &items[ei], &ev)) {
                            _ = seterr(e, "%s: element %d: column '%s' List element %d is not coercible to its element type", .{ path, e_i, cname, @as(c_int, @intCast(ei)) });
                            failed = true;
                        break :outer;
                        }
                        acc = dx.dl_term_cons(db, ev, acc);
                    }
                    cols[cn] = acc;
                },
                DLT_OPTIONAL => {
                    // JSON null -> None; else coerce elem.  A MISSING key is
                    // already an error (enforced above).
                    if (v != null and v.? == .null) {
                        cols[cn] = DLP_OPT_NONE;
                    } else if (!coerceElemJson(db, r.cols[cn].elem, v, &cols[cn])) {
                        _ = seterr(e, "%s: element %d: column '%s' Optional element is not coercible to its element type", .{ path, e_i, cname });
                        failed = true;
                        break :outer;
                    }
                },
                DLT_ENUM => {
                    if (v == null or v.? != .string) {
                        _ = seterr(e, "%s: element %d: column '%s' expects Enum (string), got %s", .{ path, e_i, cname, jsonTypeName(if (v) |*vv| @as(*const Json, vv) else null) });
                        failed = true;
                        break :outer;
                    }
                    const estr = v.?.string;
                    var ok = false;
                    var k: usize = 0;
                    while (k < r.cols[cn].n_evalues) : (k += 1) {
                        const ev: [*:0]const u8 = @ptrCast(&r.cols[cn].evalues[k]);
                        if (std.mem.eql(u8, estr, std.mem.sliceTo(ev, 0))) {
                            ok = true;
                            break;
                        }
                    }
                    if (!ok) {
                        const first: [*:0]const u8 = if (r.cols[cn].n_evalues > 0) @ptrCast(&r.cols[cn].evalues[0]) else "";
                        _ = seterr(e, "%s: element %d: column '%s' expects an Enum value from {%s}, got \"%s\"", .{ path, e_i, cname, first, estr.ptr });
                        failed = true;
                        break :outer;
                    }
                    cols[cn] = if (db) |d| dx.dl_intern_str(d, estr.ptr) else 0;
                },
                else => {
                    _ = seterr(e, "%s: element %d: column '%s' has an unsupported type for JSON loading", .{ path, e_i, cname });
                    failed = true;
                        break :outer;
                },
            }
        }

        // Per-column min/max constraints.
        var j2: usize = 0;
        while (j2 < r.arity) : (j2 += 1) {
            if (!checkMinmax(&r.cols[j2], cols[j2])) {
                var rb: [64]u8 = undefined;
                const cc = &r.cols[j2];
                if (cc.has_min != 0 and cc.has_max != 0)
                    _ = sn(&rb, rb.len, "[%lld..%lld]", .{ cc.min, cc.max })
                else if (cc.has_min != 0)
                    _ = sn(&rb, rb.len, "[%lld..]", .{cc.min})
                else if (cc.has_max != 0)
                    _ = sn(&rb, rb.len, "[..%lld]", .{cc.max})
                else
                    rb[0] = 0;
                _ = seterr(e, "%s: element %d: column '%s' value out of range (allowed %s)", .{ path, e_i, colnameOf(s, rel, j2), @as([*:0]const u8, @ptrCast(&rb)) });
                failed = true;
                        break :outer;
            }
        }

        if (db) |d| {
            if (dx.dl_add_fact(d, rel, &cols, r.arity) < 0) {
                _ = seterr(e, "%s: element %d: dl_add_fact failed", .{ path, e_i });
                failed = true;
                        break :outer;
            }
        }
        fact_count += 1;
    }

    freeRdfs(&rdfs);
    if (failed) return -1; // diagnostic already set
    return fact_count;
}

// ═══════════════════════════════════════════════════════════════════════════
// dlp/workflow.c — project check/build/query orchestration
// ═══════════════════════════════════════════════════════════════════════════

/// List the files in `dir` matching `suffix`, sorted alphabetically (sorted
/// duplicated basenames; the process arena backs them).
fn listDir(dir: [*:0]const u8, suffix: []const u8, count: *c_int) ?[][]const u8 {
    const d = opendir(dir) orelse {
        count.* = -1;
        return null;
    };
    var names: std.ArrayList([]const u8) = .empty;
    while (true) {
        const e = readdir(d) orelse break;
        const d_name: [*]u8 = @constCast(&e.d_name);
        var n: usize = 0;
        while (d_name[n] != 0) n += 1;
        const name = d_name[0..n];
        if (name.len == 0 or name[0] == '.') continue;
        if (!hasSuffixSlice(name, suffix)) continue;
        const dupd = alloc().dupe(u8, name) catch oom();
        names.append(alloc(), dupd) catch oom();
    }
    _ = closedir(d);
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lt(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.order(u8, a, b) == .lt;
        }
    }.lt);
    count.* = @intCast(names.items.len);
    return names.items;
}

fn hasSuffixSlice(s: []const u8, suffix: []const u8) bool {
    if (s.len < suffix.len) return false;
    return std.mem.eql(u8, s[s.len - suffix.len ..], suffix);
}

/// List the data files in `dir` (both .csv and .json), sorted across both.
fn listDataDir(dir: [*:0]const u8, count: *c_int) ?[][]const u8 {
    var nc: c_int = 0;
    var nj: c_int = 0;
    const csv = listDir(dir, ".csv", &nc);
    const json = listDir(dir, ".json", &nj);
    if ((nc < 0 and csv == null) or (nj < 0 and json == null)) {
        count.* = -1;
        return null;
    }
    var out: std.ArrayList([]const u8) = .empty;
    if (csv) |cc| for (cc) |nm| out.append(alloc(), nm) catch oom();
    if (json) |jj| for (jj) |nm| out.append(alloc(), nm) catch oom();
    std.mem.sort([]const u8, out.items, {}, struct {
        fn lt(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.order(u8, a, b) == .lt;
        }
    }.lt);
    count.* = @intCast(out.items.len);
    return out.items;
}

/// Load (db != null) or dry-run validate (db == null) one data file, choosing
/// the CSV or JSON typed loader by extension.
fn loadDataFile(db: ?*dl_db, s: *const dl_schema, fname: []const u8, path: [*:0]const u8, e: *Err) c_long {
    var n = fname.len;
    var rel_buf: [256]u8 = undefined;
    var rel_len: usize = n;
    if (hasSuffixSlice(fname, ".json")) {
        if (n >= 5) n -= 5;
        rel_len = n;
        if (rel_len >= rel_buf.len) rel_len = rel_buf.len - 1;
        @memcpy(rel_buf[0..rel_len], fname[0..rel_len]);
        rel_buf[rel_len] = 0;
        return jsonLoad(db, s, @ptrCast(&rel_buf), path, e);
    }
    if (n >= 4) n -= 4; // strip ".csv"
    rel_len = n;
    if (rel_len >= rel_buf.len) rel_len = rel_buf.len - 1;
    @memcpy(rel_buf[0..rel_len], fname[0..rel_len]);
    rel_buf[rel_len] = 0;
    return csvLoad(db, s, @ptrCast(&rel_buf), path, e);
}

/// Parse each rules file in sorted order, mark every rule-head relation
/// is_idb=1 on the schema, and typecheck the rules.  Prints one error line
/// per failing file to stderr; returns the number of failing files (-1 fatal).
fn typecheckRulesDir(schema: *dl_schema, rules_dir: [*:0]const u8, rules_prefix: [*:0]const u8) c_int {
    var nfiles_v: c_int = 0;
    const files = listDir(rules_dir, ".datalog", &nfiles_v) orelse {
        return -1;
    };
    const nfiles: usize = @intCast(nfiles_v);
    var errs: c_int = 0;

    var f: usize = 0;
    while (f < nfiles) : (f += 1) {
        var path_buf: [1024]u8 = undefined;
        const path_len = std.fmt.bufPrintZ(&path_buf, "{s}/{s}", .{ std.mem.sliceTo(@as([*:0]const u8, rules_prefix), 0), files[f] }) catch {
            errs += 1;
            continue;
        };
        _ = path_len;
        const path: [*:0]const u8 = @ptrCast(&path_buf);
        const src = readAllAlloc(path) orelse {
            _ = errf(stderr, "rules/%s: cannot read\n", .{files[f].ptr});
            errs += 1;
            continue;
        };

        const p = dx.parse_create(@ptrCast(src.ptr));
        if (p == null) {
            _ = errf(stderr, "rules/%s: parse error: unknown\n", .{files[f].ptr});
            errs += 1;
            continue;
        }
        var n_rules: c_int = 0;
        const rules = dx.parse_rules(p, &n_rules);
        if (rules == null) {
            const msg = dx.parse_last_error(p, null);
            _ = errf(stderr, "rules/%s: parse error: %s\n", .{ files[f].ptr, msg });
            errs += 1;
            dx.parse_free(p);
            continue;
        }

        // Mark every rule head as IDB.
        var i: usize = 0;
        while (i < @as(usize, @intCast(@max(n_rules, 0)))) : (i += 1) {
            const ru0: [*c]dx.rule = rules[@intCast(i)];
            if (ru0 == null) continue;
            const ru: *dx.rule = @ptrCast(ru0);
            if (ru.head == null) continue;
            const head: *dx.atom = @ptrCast(ru.head);
            if (head.pred == null) continue;
            const pred: [*:0]const u8 = @ptrCast(head.pred);
            var k: usize = 0;
            while (k < @as(usize, @intCast(@max(schema.n_rels, 0)))) : (k += 1) {
                if (seq(@ptrCast(&schema.rels[k].name), pred)) {
                    schema.rels[k].is_idb = 1;
                }
            }
        }

        // Typecheck this file's rules against the schema.
        var te: Err = .{};
        if (dl_typecheck_rules(schema, @ptrCast(rules), n_rules, path, &te.buf, te.buf.len) != 0) {
            _ = errf(stderr, "rules/%s: %s\n", .{ files[f].ptr, @as([*:0]const u8, if (te.buf[0] != 0) &te.buf else "typecheck failed") });
            errs += 1;
        }

        i = 0;
        while (i < @as(usize, @intCast(@max(n_rules, 0)))) : (i += 1) {
            if (rules[@intCast(i)] != null) dx.rule_free(rules[@intCast(i)]);
        }
        dx.parse_free(p);
    }

    return errs;
}

/// Dry-run validate every data file; prints errors; returns failing count.
fn checkDataDir(schema: *const dl_schema, data_dir: [*:0]const u8, data_prefix: [*:0]const u8) c_int {
    var nfiles_v: c_int = 0;
    const files = listDataDir(data_dir, &nfiles_v) orelse return -1;
    const nfiles: usize = @intCast(@max(nfiles_v, 0));
    var errs: c_int = 0;
    var f: usize = 0;
    while (f < nfiles) : (f += 1) {
        var path_buf: [1024]u8 = undefined;
        const path = std.fmt.bufPrintZ(&path_buf, "{s}/{s}", .{ std.mem.sliceTo(@as([*:0]const u8, data_prefix), 0), files[f] }) catch {
            errs += 1;
            continue;
        };
        var e: Err = .{};
        const cnt = loadDataFile(null, schema, files[f], path.ptr, &e);
        if (cnt < 0) {
            _ = errf(stderr, "%s\n", .{@as([*:0]const u8, if (e.buf[0] != 0) &e.buf else "data load failed")});
            errs += 1;
        }
    }
    return errs;
}

/// Concatenate every rules file in sorted order with '\n' separators.  Zero
/// files yields null (with has_rules=false).
fn concatRules(rules_dir: [*:0]const u8, rules_prefix: [*:0]const u8, has_rules: *bool) ?[]u8 {
    var nfiles_v: c_int = 0;
    const files = listDir(rules_dir, ".datalog", &nfiles_v) orelse return null;
    const nfiles: usize = @intCast(@max(nfiles_v, 0));
    has_rules.* = nfiles > 0;
    if (nfiles == 0) return null;
    var buf: std.ArrayList(u8) = .empty;
    var f: usize = 0;
    while (f < nfiles) : (f += 1) {
        var path_buf: [1024]u8 = undefined;
        const path = std.fmt.bufPrintZ(&path_buf, "{s}/{s}", .{ std.mem.sliceTo(@as([*:0]const u8, rules_prefix), 0), files[f] }) catch continue;
        if (readAllAlloc(path.ptr)) |src| {
            buf.appendSlice(alloc(), src[0 .. src.len - 1]) catch oom();
        }
        buf.append(alloc(), '\n') catch oom();
    }
    buf.append(alloc(), 0) catch oom();
    return buf.items;
}

const Paths = struct {
    schema: [1024:0]u8 = undefined,
    data: [1024:0]u8 = undefined,
    rules: [1024:0]u8 = undefined,
    build: [1024:0]u8 = undefined,
};

/// Resolve project-relative paths for schema/data/rules given the project dir.
fn projectPaths(dir: [*:0]const u8, p: *Paths, e: *Err) c_int {
    if (dir[0] == 0 or seq(dir, ".")) {
        setZ(&p.schema, "schema.dhall");
        setZ(&p.data, "data");
        setZ(&p.rules, "rules");
        setZ(&p.build, ".build");
    } else {
        if (pz(&p.schema, "%s/schema.dhall", .{dir}) == null or
            pz(&p.data, "%s/data", .{dir}) == null or
            pz(&p.rules, "%s/rules", .{dir}) == null or
            pz(&p.build, "%s/.build", .{dir}) == null)
        {
            _ = seterr(e, "project path too long", .{});
            return -1;
        }
    }
    return 0;
}

fn setZ(buf: *[1024:0]u8, s: []const u8) void {
    @memcpy(buf[0..s.len], s);
    buf[s.len] = 0;
}

fn pz(buf: *[1024:0]u8, comptime fmt: [:0]const u8, args: anytype) ?void {
    const n = @call(.auto, snprintf, .{ buf, buf.len, fmt.ptr } ++ args);
    if (n < 0 or n >= buf.len) return null;
    return {};
}

fn pathIsDir(path: [*:0]const u8) bool {
    var st: StatBuf = .{};
    if (stat(path, &st) != 0) return false;
    const mode = std.mem.bytesToValue(u32, st.buf[ST_MODE_OFF..][0..4]);
    return (mode & S_IFMT) == S_IFDIR;
}

/// Shared: full check (schema load + rules typecheck/IDB + data dry-run).
/// Returns 0 if clean, -1 on any error (already printed, or in errbuf).
fn doCheck(dir: [*:0]const u8, schema: *dl_schema, e: *Err) c_int {
    var p: Paths = .{};
    if (projectPaths(dir, &p, e) != 0) return -1;

    if (schemaLoad(schema, &p.schema, e) != 0)
        return -1; // main prints the diagnostic (no double-print)

    var errs: c_int = 0;
    if (pathIsDir(&p.rules)) {
        const r = typecheckRulesDir(schema, &p.rules, &p.rules);
        if (r < 0) {
            _ = seterr(e, "cannot read rules dir '%s'", .{&p.rules});
            return -1;
        }
        errs += r;
    }
    if (pathIsDir(&p.data)) {
        const d = checkDataDir(schema, &p.data, &p.data);
        if (d < 0) {
            _ = seterr(e, "cannot read data dir '%s'", .{&p.data});
            return -1;
        }
        errs += d;
    }
    if (errs > 0) return -1;
    return 0;
}

/// ─── dlp check ────────────────────────────────────────────────────────────
fn projectCheck(dir: [*:0]const u8, e: *Err) c_int {
    errClear(e);
    var schema: dl_schema = undefined;
    return if (doCheck(dir, &schema, e) == 0) 0 else 1;
}

/// ─── dlp build / query shared engine pipeline ─────────────────────────────
/// check → open build dir → attach schema → declare EDBs → load data →
/// load rules → compile → publish.  Returns 0 on success (db closed), else 1
/// with the diagnostic printed or in errbuf.
fn projectRunEngine(dir: [*:0]const u8, e: *Err) c_int {
    var schema: dl_schema = undefined;
    if (doCheck(dir, &schema, e) != 0) return 1;

    var p: Paths = .{};
    if (projectPaths(dir, &p, e) != 0) return 1;

    const db = dx.dl_open(&p.build) orelse {
        _ = seterr(e, "cannot open build dir '%s'", .{&p.build});
        return 1;
    };
    if (dx.dl_attach_schema(db, @ptrCast(&schema)) != 0) {
        dx.dl_close(db);
        _ = seterr(e, "cannot attach schema", .{});
        return 1;
    }

    // Declare ONLY EDB relations — compile_rules auto-declares rule heads.
    {
        var i: usize = 0;
        while (i < @as(usize, @intCast(@max(schema.n_rels, 0)))) : (i += 1) {
            const r = &schema.rels[i];
            if (r.is_idb != 0) continue;
            if (dx.dl_declare_relation(db, @ptrCast(&r.name), r.arity) != 0) {
                dx.dl_close(db);
                _ = seterr(e, "cannot declare relation '%s'", .{@as([*:0]const u8, @ptrCast(&r.name))});
                return 1;
            }
        }
    }

    // Load data files (typed; CSV + JSON).
    if (pathIsDir(&p.data)) {
        var nfiles_v: c_int = 0;
        const files = listDataDir(&p.data, &nfiles_v) orelse {
            dx.dl_close(db);
            _ = seterr(e, "cannot read data dir '%s'", .{&p.data});
            return 1;
        };
        const nfiles: usize = @intCast(@max(nfiles_v, 0));
        var f: usize = 0;
        while (f < nfiles) : (f += 1) {
            var path_buf: [1024]u8 = undefined;
            const path = std.fmt.bufPrintZ(&path_buf, "{s}/{s}", .{ std.mem.sliceTo(&p.data, 0), files[f] }) catch {
                dx.dl_close(db);
                _ = seterr(e, "data path too long", .{});
                return 1;
            };
            var le: Err = .{};
            if (loadDataFile(db, &schema, files[f], path.ptr, &le) < 0) {
                _ = errf(stderr, "%s\n", .{@as([*:0]const u8, if (le.buf[0] != 0) &le.buf else "data load failed")});
                dx.dl_close(db);
                return 1;
            }
        }
    }

    // Concatenate + load rules; EDB-only projects skip rule load/compile.
    var has_rules = false;
    const rules_src = concatRules(&p.rules, &p.rules, &has_rules);
    if (rules_src == null and has_rules) {
        dx.dl_close(db);
        _ = seterr(e, "cannot read rules", .{});
        return 1;
    }
    if (rules_src) |rs| {
        const loadrc = dx.dl_load_rules(db, @ptrCast(rs.ptr));
        if (loadrc != 0) {
            dx.dl_close(db);
            _ = seterr(e, "rule load/compile failed", .{});
            return 1;
        }
        if (dx.dl_compile(db) != 0) {
            dx.dl_close(db);
            _ = seterr(e, "compile failed", .{});
            return 1;
        }
    }
    if (dx.dl_publish_snapshot(db) != 0) {
        dx.dl_close(db);
        _ = seterr(e, "publish failed", .{});
        return 1;
    }
    dx.dl_close(db);
    return 0;
}

/// ─── dlp build ────────────────────────────────────────────────────────────
fn projectBuild(dir: [*:0]const u8, e: *Err) c_int {
    errClear(e);
    return projectRunEngine(dir, e);
}

// ─── Goal parsing + type-aware row printer (workflow.c D3) ─────────────────

const Goal = struct {
    rel: [DL_SCHEMA_NAME_MAX:0]u8 = @splat(0),
    nargs: usize = 0,
    bound: [DL_SCHEMA_MAX_ARITY]bool = @splat(false),
    vals: [DL_SCHEMA_MAX_ARITY]u32 = @splat(0),
};

/// Parse a goal string: rel ( arg (, arg)* )?  Free iff first char is in
/// [A-Z_]; a bare integer is a raw u32 bound value; other bound args are
/// interned.
fn parseGoal(db: *dl_db, s: [*:0]const u8, g: *Goal) bool {
    g.* = .{};
    var p: usize = 0;
    while (s[p] != 0 and s[p] != '(') p += 1;
    const rlen = p;
    if (rlen == 0 or rlen >= DL_SCHEMA_NAME_MAX) return false;
    @memcpy(g.rel[0..rlen], s[0..rlen]);
    g.rel[rlen] = 0;
    if (s[p] == 0) {
        g.nargs = 0;
        return true;
    }
    p += 1; // skip '('
    var i: usize = 0;
    while (i < DL_SCHEMA_MAX_ARITY) {
        while (s[p] != 0 and (isSpace(s[p]) or s[p] == ',')) p += 1;
        if (s[p] == ')') break;
        const start = p;
        while (s[p] != 0 and s[p] != ',' and s[p] != ')') p += 1;
        const alen = p - start;
        var arg: [256]u8 = undefined;
        if (alen >= arg.len) return false;
        @memcpy(arg[0..alen], s[start..][0..alen]);
        arg[alen] = 0;
        const is_var = (alen > 0 and (arg[0] == '_' or (arg[0] >= 'A' and arg[0] <= 'Z')));
        if (!is_var) {
            var is_int = true;
            var v: u64 = 0;
            var q: usize = 0;
            while (q < alen) : (q += 1) {
                if (arg[q] < '0' or arg[q] > '9') {
                    is_int = false;
                    break;
                }
                v = v * 10 + (arg[q] - '0');
                if (v > 4294967295) {
                    is_int = false;
                    break;
                }
            }
            if (is_int and alen > 0) {
                g.bound[i] = true;
                g.vals[i] = @intCast(v);
            } else {
                g.bound[i] = true;
                g.vals[i] = dx.dl_intern_str(db, @ptrCast(&arg));
            }
        } else {
            g.bound[i] = false;
        }
        i += 1;
        if (s[p] == ')') break;
    }
    g.nargs = i;
    while (s[p] != 0 and s[p] != ')') p += 1;
    if (s[p] != ')') return false;
    return true;
}

const PrintCtx = struct {
    db: *dl_db,
    r: *const dl_reldef,
    g: *const Goal,
};

/// Print ONE scalar value (flat colspec + raw u32).
fn printScalar(ctx: *PrintCtx, spec: *const dl_colspec, v: u32) void {
    var buf: [16]u8 = undefined;
    switch (spec.tag) {
        DLT_NATURAL, DLT_TIMESTAMP => _ = pf("%u", .{v}),
        DLT_TEXT, DLT_ENUM => {
            const str = dx.dl_intern_str_of(ctx.db, v);
            _ = pf("%s", .{@as([*:0]const u8, if (str != null) str else "")});
        },
        DLT_BOOL => _ = pf("%s", .{@as([*:0]const u8, if (v != 0) "true" else "false")}),
        DLT_CHAR => {
            const n = utf8EncodeCp(v, &buf);
            _ = pf("%.*s", .{ @as(c_int, @intCast(n)), @as([*]const u8, &buf) });
        },
        DLT_DATE => {
            printDate(v, &buf, buf.len);
            _ = pf("%s", .{@as([*:0]const u8, @ptrCast(&buf))});
        },
        DLT_SIGNED => _ = pf("%d", .{dezigzag(v)}),
        else => {},
    }
}

fn printRow(cols: [*c]const u32, arity: u8, user: ?*anyopaque) callconv(.c) c_int {
    const ctx: *PrintCtx = @ptrCast(@alignCast(user.?));
    _ = arity;
    // Filter on the bound positions (v1 evaluates via full materialization).
    var i: usize = 0;
    while (i < ctx.g.nargs) : (i += 1) {
        if (ctx.g.bound[i] and cols[i] != ctx.g.vals[i]) return 0;
    }
    var first = true;
    var col: usize = 0;
    i = 0;
    while (i < ctx.g.nargs) : (i += 1) {
        if (ctx.g.bound[i]) continue;
        if (!first) _ = pf("%s", .{" "});
        first = false;
        switch (ctx.r.cols[i].tag) {
            DLT_NATURAL, DLT_TIMESTAMP, DLT_TEXT, DLT_ENUM, DLT_BOOL, DLT_CHAR, DLT_DATE, DLT_SIGNED => {
                printScalar(ctx, &ctx.r.cols[i], cols[i]);
            },
            DLT_LIST => {
                var ec: dl_colspec = std.mem.zeroes(dl_colspec);
                ec.tag = ctx.r.cols[i].elem;
                _ = pf("%s", .{"["});
                var fe = true;
                var node = cols[i];
                while (node != dx.TERM_NIL and dx.dl_term_is_list(ctx.db, node) != 0) {
                    if (!fe) _ = pf("%s", .{", "});
                    fe = false;
                    printScalar(ctx, &ec, dx.dl_term_car(ctx.db, node));
                    node = dx.dl_term_cdr(ctx.db, node);
                }
                _ = pf("%s", .{"]"});
            },
            DLT_OPTIONAL => {
                if (cols[i] == DLP_OPT_NONE) {
                    _ = pf("%s", .{"null"});
                } else {
                    var ec: dl_colspec = std.mem.zeroes(dl_colspec);
                    ec.tag = ctx.r.cols[i].elem;
                    printScalar(ctx, &ec, cols[i]);
                }
            },
            else => {},
        }
        col += 1;
    }
    if (col > 0) _ = pf("%s", .{"\n"});
    return 0;
}

/// ─── dlp query ────────────────────────────────────────────────────────────
fn projectQuery(dir: [*:0]const u8, goal_str: [*:0]const u8, e: *Err) c_int {
    errClear(e);
    var schema: dl_schema = undefined;
    if (doCheck(dir, &schema, e) != 0) return 1;

    var p: Paths = .{};
    if (projectPaths(dir, &p, e) != 0) return 1;

    const db = dx.dl_open(&p.build) orelse {
        _ = seterr(e, "cannot open build dir '%s'", .{&p.build});
        return 1;
    };
    if (dx.dl_attach_schema(db, @ptrCast(&schema)) != 0) {
        dx.dl_close(db);
        _ = seterr(e, "cannot attach schema", .{});
        return 1;
    }

    {
        var i: usize = 0;
        while (i < @as(usize, @intCast(@max(schema.n_rels, 0)))) : (i += 1) {
            const r = &schema.rels[i];
            if (r.is_idb != 0) continue;
            if (dx.dl_declare_relation(db, @ptrCast(&r.name), r.arity) != 0) {
                dx.dl_close(db);
                _ = seterr(e, "cannot declare relation '%s'", .{@as([*:0]const u8, @ptrCast(&r.name))});
                return 1;
            }
        }
    }

    if (pathIsDir(&p.data)) {
        var nfiles_v: c_int = 0;
        const files = listDataDir(&p.data, &nfiles_v) orelse {
            dx.dl_close(db);
            _ = seterr(e, "cannot read data dir '%s'", .{&p.data});
            return 1;
        };
        const nfiles: usize = @intCast(@max(nfiles_v, 0));
        var f: usize = 0;
        while (f < nfiles) : (f += 1) {
            var path_buf: [1024]u8 = undefined;
            const path = std.fmt.bufPrintZ(&path_buf, "{s}/{s}", .{ std.mem.sliceTo(&p.data, 0), files[f] }) catch {
                dx.dl_close(db);
                _ = seterr(e, "data path too long", .{});
                return 1;
            };
            var le: Err = .{};
            if (loadDataFile(db, &schema, files[f], path.ptr, &le) < 0) {
                _ = errf(stderr, "%s\n", .{@as([*:0]const u8, if (le.buf[0] != 0) &le.buf else "data load failed")});
                dx.dl_close(db);
                return 1;
            }
        }
    }

    var has_rules = false;
    const rules_src = concatRules(&p.rules, &p.rules, &has_rules);
    if (rules_src == null and has_rules) {
        dx.dl_close(db);
        _ = seterr(e, "cannot read rules", .{});
        return 1;
    }
    if (rules_src) |rs| {
        const loadrc = dx.dl_load_rules(db, @ptrCast(rs.ptr));
        if (loadrc != 0) {
            dx.dl_close(db);
            _ = seterr(e, "rule load/compile failed", .{});
            return 1;
        }
        if (dx.dl_compile(db) != 0) {
            dx.dl_close(db);
            _ = seterr(e, "compile failed", .{});
            return 1;
        }
    }
    if (dx.dl_publish_snapshot(db) != 0) {
        dx.dl_close(db);
        _ = seterr(e, "publish failed", .{});
        return 1;
    }

    // Parse the goal.
    var g: Goal = .{};
    if (!parseGoal(db, goal_str, &g)) {
        dx.dl_close(db);
        _ = seterr(e, "bad goal: %s", .{goal_str});
        return 1;
    }
    const r = dl_schema_find(&schema, &g.rel) orelse {
        dx.dl_close(db);
        _ = seterr(e, "goal relation '%s' not in schema", .{@as([*:0]const u8, &g.rel)});
        return 1;
    };
    if (g.nargs != r.arity) {
        dx.dl_close(db);
        _ = seterr(e, "goal '%s' has %d args; relation '%s' has arity %d", .{ goal_str, @as(c_int, @intCast(g.nargs)), @as([*:0]const u8, &g.rel), @as(c_int, r.arity) });
        return 1;
    }

    var ctx: PrintCtx = .{ .db = db, .r = r, .g = &g };

    // A fully-ground goal (no free args) is an exact lookup.
    var any_free = false;
    var i: usize = 0;
    while (i < g.nargs) : (i += 1) {
        if (!g.bound[i]) {
            any_free = true;
            break;
        }
    }

    if (!any_free) {
        var lookup_cols: [DL_SCHEMA_MAX_ARITY]u32 = @splat(0);
        i = 0;
        while (i < g.nargs) : (i += 1) lookup_cols[i] = g.vals[i];
        const found = dx.dl_lookup(db, &g.rel, &lookup_cols, @intCast(g.nargs));
        _ = pf("%s\n", .{@as([*:0]const u8, if (found != 0) "found" else "not found")});
        dx.dl_close(db);
        return 0;
    }

    // Otherwise: evaluate via full materialization, filter on the bound
    // positions in the callback.
    const n = dx.dl_query(db, &g.rel, printRow, &ctx);
    if (n < 0) {
        dx.dl_close(db);
        _ = seterr(e, "query failed", .{});
        return 1;
    }

    dx.dl_close(db);
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// dlp/init.c — project scaffolding
// ═══════════════════════════════════════════════════════════════════════════

extern "c" fn mkdir(path: [*:0]const u8, mode: c.mode_t) c_int;
extern "c" fn __errno_location() *c_int;

fn mkdirIfMissing(dir: [*:0]const u8) bool {
    if (mkdir(dir, 0o755) == 0) return true;
    const errno = __errno_location().*;
    return errno == @as(c_int, @intFromEnum(std.c.E.EXIST)) and pathIsDir(dir);
}

fn writeSchemaFile(path: [*:0]const u8, content: []const u8, e: *Err) bool {
    const f = c.fopen(path, "w") orelse {
        _ = seterr(e, "cannot create '%s': %s", .{ path, strerror(__errno_location().*) });
        return false;
    };
    _ = c.fwrite(content.ptr, 1, content.len, f);
    if (c.fclose(f) != 0) {
        _ = seterr(e, "cannot write '%s': %s", .{ path, strerror(__errno_location().*) });
        return false;
    }
    return true;
}

fn projectInit(dir: [*:0]const u8, e: *Err) c_int {
    errClear(e);
    const d: [*:0]const u8 = if (dir[0] == 0) "." else dir;

    var p: Paths = .{};
    if (seq(d, ".")) {
        setZ(&p.schema, "schema.dhall");
        setZ(&p.data, "data");
        setZ(&p.rules, "rules");
        setZ(&p.build, ".build");
    } else {
        if (pz(&p.schema, "%s/schema.dhall", .{d}) == null or
            pz(&p.data, "%s/data", .{d}) == null or
            pz(&p.rules, "%s/rules", .{d}) == null or
            pz(&p.build, "%s/.build", .{d}) == null)
        {
            _ = seterr(e, "project path too long", .{});
            return -1;
        }
    }

    if (!mkdirIfMissing(d)) {
        _ = seterr(e, "cannot create project dir '%s': %s", .{ d, strerror(__errno_location().*) });
        return -1;
    }
    if (!mkdirIfMissing(&p.data)) {
        _ = seterr(e, "cannot create '%s': %s", .{ &p.data, strerror(__errno_location().*) });
        return -1;
    }
    if (!mkdirIfMissing(&p.rules)) {
        _ = seterr(e, "cannot create '%s': %s", .{ &p.rules, strerror(__errno_location().*) });
        return -1;
    }
    if (!mkdirIfMissing(&p.build)) {
        _ = seterr(e, "cannot create '%s': %s", .{ &p.build, strerror(__errno_location().*) });
        return -1;
    }

    if (!writeSchemaFile(&p.schema, TEMPLATE_SCHEMA, e)) return -1;

    _ = pf("dlp: initialized project in '%s'\n", .{d});
    _ = pf("  %s\n", .{&p.schema});
    _ = pf("  %s/\n", .{&p.data});
    _ = pf("  %s/\n", .{&p.rules});
    _ = pf("  %s/\n", .{&p.build});
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// dlp/main.c — CLI dispatch
// ═══════════════════════════════════════════════════════════════════════════

fn usage(out: *std.c.FILE) void {
    _ = errf0(out, "dlp — datalog-dafsa project tool (Dhall-driven schema)\n" ++
        "usage:\n" ++
        "  dlp init [dir]           scaffold a new project directory\n" ++
        "  dlp schema [dir]         walk schema.dhall into a typed schema and print it\n" ++
        "  dlp check-schema [dir]   alias for `schema`\n" ++
        "  dlp check [dir]          validate schema + typecheck rules + dry-run data\n" ++
        "  dlp build [dir]          check, then build a snapshot under dir/.build\n" ++
        "  dlp query [dir] 'goal'   build in-process and evaluate a query goal\n" ++
        "\n" ++
        "dir defaults to \".\" (the current directory).\n");
}

/// `dlp schema [dir]` / `dlp check-schema [dir]`.
fn cmdSchema(dir: [*:0]const u8) c_int {
    var path_buf: [1024]u8 = undefined;
    var path: [*:0]const u8 = undefined;
    if (dir[0] == 0 or seq(dir, ".")) {
        path = "schema.dhall";
    } else {
        const n = sn(&path_buf, path_buf.len, "%s/schema.dhall", .{dir});
        if (n < 0 or n >= path_buf.len) {
            _ = errf0(stderr, "dlp: schema path too long\n");
            return 1;
        }
        path = @ptrCast(&path_buf);
    }

    var s: dl_schema = undefined;
    var e: Err = .{};
    if (schemaLoad(&s, path, &e) != 0) {
        _ = @call(.auto, fprintf, .{ stderr, "dlp: %s\n", @as([*:0]const u8, &e.buf) });
        return 1;
    }

    _ = pf("schema: %s\n", .{path});
    _ = pf("%-12s %-6s %s\n", .{ @as([*:0]const u8, "relation"), @as([*:0]const u8, "arity"), @as([*:0]const u8, "columns") });
    var i: usize = 0;
    while (i < @as(usize, @intCast(@max(s.n_rels, 0)))) : (i += 1) {
        const r = &s.rels[i];
        _ = pf("%-12s %-6d ", .{ @as([*:0]const u8, @ptrCast(&r.name)), @as(c_int, r.arity) });
        var j: usize = 0;
        while (j < r.arity) : (j += 1) {
            var tn: [64]u8 = undefined;
            coltypeName(&r.cols[j], &tn, tn.len);
            _ = pf("%s%s", .{ @as([*:0]const u8, if (j != 0) "," else ""), @as([*:0]const u8, @ptrCast(&tn)) });
            // Compact constraint suffix: Natural[1..150], Signed[-10..+10],
            // Text~regex.
            const cc = &r.cols[j];
            if (cc.has_min != 0 and cc.has_max != 0)
                _ = pf("[%lld..%lld]", .{ cc.min, cc.max })
            else if (cc.has_min != 0)
                _ = pf("[%lld..]", .{cc.min})
            else if (cc.has_max != 0)
                _ = pf("[..%lld]", .{cc.max});
            if (cc.has_regex != 0)
                _ = pf("~%s", .{@as([*:0]const u8, @ptrCast(&cc.regex))});
        }
        _ = pf("%s", .{"\n"});
    }
    return 0;
}

pub fn main(init: std.process.Init) void {
    const args = init.minimal.args.toSlice(init.arena.allocator()) catch
        @panic("dlp: cannot read argv");
    c.exit(mainDispatch(args));
}

fn mainDispatch(args: []const [:0]const u8) c_int {
    const cmd: ?[:0]const u8 = if (args.len > 1) args[1] else null;
    const dir: [:0]const u8 = if (args.len > 2) args[2] else ".";
    const dir_z: [*:0]const u8 = dir.ptr;

    if (cmd == null) {
        usage(stderr);
        return 2;
    }
    const cm = cmd.?;

    if (std.mem.eql(u8, cm, "init")) {
        var e: Err = .{};
        if (projectInit(dir_z, &e) != 0) {
            _ = @call(.auto, fprintf, .{ stderr, "dlp: %s\n", @as([*:0]const u8, &e.buf) });
            return 1;
        }
        return 0;
    }

    if (std.mem.eql(u8, cm, "schema") or std.mem.eql(u8, cm, "check-schema"))
        return cmdSchema(dir_z);

    if (std.mem.eql(u8, cm, "check")) {
        var e: Err = .{};
        const rc = projectCheck(dir_z, &e);
        if (rc != 0 and e.buf[0] != 0)
            _ = @call(.auto, fprintf, .{ stderr, "dlp: %s\n", @as([*:0]const u8, &e.buf) });
        return if (rc != 0) 1 else 0;
    }

    if (std.mem.eql(u8, cm, "build")) {
        var e: Err = .{};
        const rc = projectBuild(dir_z, &e);
        if (rc != 0 and e.buf[0] != 0)
            _ = @call(.auto, fprintf, .{ stderr, "dlp: %s\n", @as([*:0]const u8, &e.buf) });
        return if (rc != 0) 1 else 0;
    }

    if (std.mem.eql(u8, cm, "query")) {
        if (args.len <= 3) {
            _ = errf(stderr, "dlp: query requires a goal argument\n\n", .{});
            usage(stderr);
            return 2;
        }
        const goal = args[3];
        var e: Err = .{};
        const rc = projectQuery(dir_z, goal.ptr, &e);
        if (rc != 0 and e.buf[0] != 0)
            _ = @call(.auto, fprintf, .{ stderr, "dlp: %s\n", @as([*:0]const u8, &e.buf) });
        return if (rc != 0) 1 else 0;
    }

    _ = errf(stderr, "dlp: unknown command '%s'\n\n", .{cm.ptr});
    usage(stderr);
    return 2;
}
