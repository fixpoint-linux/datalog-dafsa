//! hybrid.zig — Zig-side root of the strangler-hybrid libdatalog.so.
//!
//! Referencing each ported module from this root keeps its `export fn`s in
//! the shared library alongside the not-yet-migrated C objects (the C side
//! keeps its own declarations from src/*.h).  Each unit swaps more C files
//! out of build.zig and adds their modules here.

comptime {
    _ = @import("util.zig"); // U2: src/util.c
    _ = @import("tupleset.zig"); // U2: src/tupleset.c
    _ = @import("termstore.zig"); // U2: src/termstore.c
    _ = @import("schema.zig"); // U2: src/schema.c
    _ = @import("intern.zig"); // U3: src/intern.c
    _ = @import("relation.zig"); // U4: src/relation.c
    _ = @import("vrelation.zig"); // U4: src/vrelation.c
    _ = @import("txnwal.zig"); // U4: src/txnwal.c
    _ = @import("parser.zig"); // U5: src/parser.c
    _ = @import("typecheck.zig"); // U5: src/typecheck.c
}
