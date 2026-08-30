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
    _ = @import("regexwalk.zig"); // U6: src/regexwalk.c
    _ = @import("snapshot.zig"); // U7: src/snapshot.c
    _ = @import("permindex.zig"); // U7: src/permindex.c
    _ = @import("iter.zig"); // U7: src/iter.c
    _ = @import("compiler.zig"); // U8: src/compiler.c
    _ = @import("vm.zig"); // U9: src/vm.c
    _ = @import("magic.zig"); // U10: src/magic.c
    _ = @import("topdown.zig"); // U10: src/topdown.c
    _ = @import("dl.zig"); // U11: src/dl.c
    _ = @import("index.zig"); // U14: src/index.c
    _ = @import("vector.zig"); // U14: src/vector.c
    _ = @import("analyze.zig"); // U14: src/analyze.c
}
