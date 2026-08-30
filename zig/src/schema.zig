//! schema.zig — port of src/schema.c (dl_schema builder + lookup).
//!
//! Strangler-hybrid ABI: dl_schema / dl_reldef / dl_colspec / dl_coltype are
//! EXPOSED types (tests, dlp and other C TUs declare and dereference them)
//! and must stay byte-identical to src/schema.h; dl_schema_add /
//! dl_schema_find are `export fn`s with the exact C names/signatures.
//! dl_colspec_eq is static-inline in schema.h (each C TU inlines its own
//! copy); it is ported here and exported for completeness.
//!
//! Oracle: src/schema.c + src/schema.h (never modified).

const std = @import("std");

// ─── Capacity caps (schema.h) ─────────────────────────────────────────────
pub const DL_SCHEMA_MAX_RELS = 64;
pub const DL_SCHEMA_MAX_ARITY = 8;
pub const DL_SCHEMA_NAME_MAX = 64;
pub const DL_ENUM_MAX_VALUES = 8;
pub const DL_ENUM_VALUE_MAX = 32;
pub const DL_REGEX_MAX = 128;

// Column type tag.  C enum => c_int; kept as a plain integer alias so that
// values C wrote directly (or garbage) stay readable without enum-UB.
pub const dl_coltype = c_int;
pub const DLT_NATURAL: dl_coltype = 1; // raw u32
pub const DLT_TEXT: dl_coltype = 2; // interned sym_id
pub const DLT_BOOL: dl_coltype = 3; // raw u32 0/1
pub const DLT_CHAR: dl_coltype = 4; // raw u32 Unicode scalar value
pub const DLT_DATE: dl_coltype = 5; // raw u32 yyyymmdd
pub const DLT_TIMESTAMP: dl_coltype = 6; // raw u32 unix (epoch) seconds
pub const DLT_SIGNED: dl_coltype = 7; // raw u32 zigzag(i32)
pub const DLT_LIST: dl_coltype = 8; // term handle; param.elem = element type
pub const DLT_OPTIONAL: dl_coltype = 9; // param.elem; None = 0xFFFFFFFF
pub const DLT_ENUM: dl_coltype = 10; // value = interned sym_id

/// typedef struct { ... } dl_colspec — MUST stay byte-identical to schema.h.
/// (Field defaults exist only so Zig struct literals can omit trailing
/// zeros, as C's zero-init does; they do not affect layout.)
pub const dl_colspec = extern struct {
    tag: dl_coltype = 0,
    elem: dl_coltype = 0, // LIST/OPTIONAL element (flat scalar only, v1)
    evalues: [DL_ENUM_MAX_VALUES][DL_ENUM_VALUE_MAX]u8 = @splat(@splat(0)),
    n_evalues: u8 = 0,
    // Per-column value constraints (data-load metadata, NOT structural type);
    // dl_colspec_eq intentionally ignores these.
    has_min: c_int = 0,
    has_max: c_int = 0,
    min: i64 = 0,
    max: i64 = 0,
    has_regex: c_int = 0,
    regex: [DL_REGEX_MAX]u8 = @splat(0),
};

/// typedef struct { ... } dl_reldef — MUST stay byte-identical to schema.h.
pub const dl_reldef = extern struct {
    name: [DL_SCHEMA_NAME_MAX]u8,
    arity: u8,
    is_idb: u8,
    cols: [DL_SCHEMA_MAX_ARITY]dl_colspec,
};

/// typedef struct dl_schema — MUST stay byte-identical to schema.h.
pub const dl_schema = extern struct {
    n_rels: c_int,
    rels: [DL_SCHEMA_MAX_RELS]dl_reldef,
};

/// strcmp == 0 for NUL-terminated byte strings.
fn cstrEq(a: [*]const u8, b: [*]const u8) bool {
    var i: usize = 0;
    while (true) : (i += 1) {
        const ca = a[i];
        const cb = b[i];
        if (ca != cb) return false;
        if (ca == 0) return true;
    }
}

/// int dl_schema_add(dl_schema *s, const char *name, uint8_t arity,
///                   const dl_colspec *cols, int is_idb)
/// Returns 0 on success, -1 on error (NULL args, arity bounds, duplicate
/// name, schema full).  Name is copied strncpy-style (NUL-terminated).
export fn dl_schema_add(s: ?*dl_schema, name: ?[*:0]const u8, arity: u8, cols: ?[*]const dl_colspec, is_idb: c_int) c_int {
    const sch = s orelse return -1;
    const name_p = name orelse return -1;
    const cols_p = cols orelse return -1;
    if (arity < 1 or arity > DL_SCHEMA_MAX_ARITY) return -1;
    if (sch.n_rels < 0 or sch.n_rels >= DL_SCHEMA_MAX_RELS) return -1; // full or corrupted

    {
        var i: usize = 0;
        while (i < sch.n_rels) : (i += 1) {
            if (cstrEq(&sch.rels[i].name, name_p)) return -1; // duplicate name
        }
    }

    const r: *dl_reldef = &sch.rels[@intCast(sch.n_rels)];
    // strncpy(r->name, name, sizeof(r->name) - 1); r->name[63] = '\0';
    // (strncpy pads the remainder with NULs, so the whole field is defined)
    var i: usize = 0;
    while (i < DL_SCHEMA_NAME_MAX - 1 and name_p[i] != 0) : (i += 1) r.name[i] = name_p[i];
    while (i < DL_SCHEMA_NAME_MAX) : (i += 1) r.name[i] = 0;
    r.arity = arity;
    r.is_idb = if (is_idb != 0) 1 else 0;
    @memcpy(r.cols[0..arity], cols_p[0..arity]);

    sch.n_rels += 1;
    return 0;
}

/// const dl_reldef *dl_schema_find(const dl_schema *s, const char *name)
export fn dl_schema_find(s: ?*const dl_schema, name: ?[*:0]const u8) ?*const dl_reldef {
    const sch = s orelse return null;
    const name_p = name orelse return null;
    var i: usize = 0;
    while (i < sch.n_rels) : (i += 1) {
        if (cstrEq(&sch.rels[i].name, name_p)) return &sch.rels[i];
    }
    return null;
}

/// static inline int dl_colspec_eq(dl_colspec a, dl_colspec b)
/// Structural type equality (tag + elem for LIST/OPTIONAL; tag + value set
/// for ENUM; tag alone for flat).  Exported although C TUs inline their own
/// copies from schema.h.
export fn dl_colspec_eq(a: dl_colspec, b: dl_colspec) c_int {
    if (a.tag != b.tag) return 0;
    if (a.tag == DLT_LIST or a.tag == DLT_OPTIONAL) return if (a.elem == b.elem) 1 else 0;
    if (a.tag == DLT_ENUM) {
        // n_evalues is caller-controlled; clamp to the declared capacity so a
        // corrupt spec cannot slice past evalues[] (C memcmp would read OOB).
        const n: usize = @as(usize, @min(a.n_evalues, DL_ENUM_MAX_VALUES)) * DL_ENUM_VALUE_MAX;
        const ab = std.mem.sliceAsBytes(a.evalues[0..]);
        const bb = std.mem.sliceAsBytes(b.evalues[0..]);
        return if (a.n_evalues == b.n_evalues and std.mem.eql(u8, ab[0..n], bb[0..n])) 1 else 0;
    }
    return 1;
}

// ─── Tests ────────────────────────────────────────────────────────────────

test "dl_schema add/find/dup/full" {
    var s: dl_schema = std.mem.zeroes(dl_schema);

    const cols2 = [_]dl_colspec{ .{ .tag = DLT_NATURAL }, .{ .tag = DLT_TEXT } };
    try std.testing.expectEqual(@as(c_int, 0), dl_schema_add(&s, "edge", 2, &cols2, 0));
    try std.testing.expectEqual(@as(c_int, 1), s.n_rels);
    try std.testing.expectEqualStrings("edge", s.rels[0].name[0..4]);
    try std.testing.expectEqual(@as(u8, 0), s.rels[0].is_idb);
    try std.testing.expectEqual(DLT_NATURAL, s.rels[0].cols[0].tag);
    try std.testing.expectEqual(DLT_TEXT, s.rels[0].cols[1].tag);

    const cols3 = [_]dl_colspec{ .{ .tag = DLT_TEXT }, .{ .tag = DLT_TEXT }, .{ .tag = DLT_TEXT } };
    try std.testing.expectEqual(@as(c_int, 0), dl_schema_add(&s, "path", 3, &cols3, 1));
    try std.testing.expectEqual(@as(u8, 1), s.rels[1].is_idb);

    // duplicate name rejected
    try std.testing.expectEqual(@as(c_int, -1), dl_schema_add(&s, "edge", 2, &cols2, 0));
    // bad arity
    try std.testing.expectEqual(@as(c_int, -1), dl_schema_add(&s, "x", 0, &cols2, 0));
    try std.testing.expectEqual(@as(c_int, -1), dl_schema_add(&s, "x", 9, &cols2, 0));
    // NULL args
    try std.testing.expectEqual(@as(c_int, -1), dl_schema_add(null, "x", 1, &cols2, 0));
    try std.testing.expectEqual(@as(c_int, -1), dl_schema_add(&s, null, 1, &cols2, 0));
    try std.testing.expectEqual(@as(c_int, -1), dl_schema_add(&s, "x", 1, null, 0));

    // find: hit / miss / NULL
    try std.testing.expect(dl_schema_find(&s, "path") == &s.rels[1]);
    try std.testing.expect(dl_schema_find(&s, "nope") == null);
    try std.testing.expect(dl_schema_find(null, "path") == null);
    try std.testing.expect(dl_schema_find(&s, null) == null);

    // long name truncation at 63 chars (strncpy semantics)
    var longname: [80:0]u8 = @splat('a');
    const one = [_]dl_colspec{.{ .tag = DLT_NATURAL }};
    try std.testing.expectEqual(@as(c_int, 0), dl_schema_add(&s, &longname, 1, &one, 0));
    try std.testing.expectEqual(@as(u8, 0), s.rels[2].name[63]);
    try std.testing.expectEqual(@as(u8, 'a'), s.rels[2].name[62]);

    // fill to DL_SCHEMA_MAX_RELS with unique names, then overflow
    var namebuf: [32:0]u8 = undefined;
    while (s.n_rels < DL_SCHEMA_MAX_RELS) {
        const nm = try std.fmt.bufPrintZ(&namebuf, "fill{d}", .{s.n_rels});
        try std.testing.expectEqual(@as(c_int, 0), dl_schema_add(&s, nm, 1, &one, 0));
    }
    try std.testing.expectEqual(@as(c_int, -1), dl_schema_add(&s, "overflow", 1, &one, 0));
}

test "dl_colspec_eq structural equality" {
    const flat_a: dl_colspec = .{ .tag = DLT_NATURAL };
    const flat_b: dl_colspec = .{ .tag = DLT_NATURAL, .min = 1, .max = 10, .has_min = 1, .has_max = 1 };
    // bounds are NOT structural — same type
    try std.testing.expectEqual(@as(c_int, 1), dl_colspec_eq(flat_a, flat_b));
    try std.testing.expectEqual(@as(c_int, 0), dl_colspec_eq(flat_a, .{ .tag = DLT_TEXT }));

    const la: dl_colspec = .{ .tag = DLT_LIST, .elem = DLT_NATURAL };
    const lb: dl_colspec = .{ .tag = DLT_LIST, .elem = DLT_NATURAL };
    const lc: dl_colspec = .{ .tag = DLT_LIST, .elem = DLT_TEXT };
    try std.testing.expectEqual(@as(c_int, 1), dl_colspec_eq(la, lb));
    try std.testing.expectEqual(@as(c_int, 0), dl_colspec_eq(la, lc));

    var ea: dl_colspec = .{ .tag = DLT_ENUM, .n_evalues = 2 };
    var eb: dl_colspec = .{ .tag = DLT_ENUM, .n_evalues = 2 };
    @memcpy(ea.evalues[0][0..3], "foo");
    @memcpy(eb.evalues[0][0..3], "foo");
    @memcpy(ea.evalues[1][0..3], "bar");
    @memcpy(eb.evalues[1][0..3], "bar");
    try std.testing.expectEqual(@as(c_int, 1), dl_colspec_eq(ea, eb));
    eb.n_evalues = 1;
    try std.testing.expectEqual(@as(c_int, 0), dl_colspec_eq(ea, eb));
}
