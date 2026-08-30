//! tests.zig — unit-test root for the ported modules (`zig build test`).
//!
//! Importing each ported module from a `test` block pulls its inline `test`
//! decls into the test binary.  The modules call the still-C engine via
//! extern/@cImport (dafsa_*, crc32_compute, trans_find, regex_dfa_walk,
//! symset_contains), so the binary also links the vendored DAFSA engine and
//! src/regexwalk.c — see the test step in build.zig.

test {
    _ = @import("util.zig");
    _ = @import("tupleset.zig");
    _ = @import("termstore.zig");
    _ = @import("schema.zig");
    _ = @import("intern.zig");
    _ = @import("relation.zig");
    _ = @import("vrelation.zig");
    _ = @import("txnwal.zig");
    _ = @import("parser.zig");
    _ = @import("typecheck.zig");
}
