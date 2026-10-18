//! tests.zig — unit-test root for the ported modules (`zig build test`).
//!
//! Importing each ported module from a `test` block pulls its inline `test`
//! decls into the test binary.  The modules call the still-C engine via
//! extern/@cImport (dafsa_*, crc32_compute, trans_find), so the binary also
//! links the vendored DAFSA engine.  regex_dfa_walk/symset_contains now come
//! from the ported regexwalk.zig (U6) — see the test step in build.zig.

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
    _ = @import("regexwalk.zig");
    _ = @import("snapshot.zig");
    _ = @import("permindex.zig");
    _ = @import("iter.zig");
    _ = @import("compiler.zig");
    _ = @import("vm.zig");
    _ = @import("wfs.zig");
    _ = @import("dl.zig");
    _ = @import("index.zig"); // U14: src/index.c
    _ = @import("vector.zig"); // U14: src/vector.c
    // U1: dl_abi.zig comptime witness — one @cImport("dl_internal.h") pins
    // the hand-declared C-layout structs field-for-field (test-only, never
    // in the lib/exe graphs).
    _ = @import("abi_witness.zig");
    // U15: dafsa Zig engine (abi.zig) — supplies dafsa_*/crc32_*/trans_find
    // symbols the ported modules' @cImport'd extern decls link against.
    _ = @import("dafsa_abi");
}
