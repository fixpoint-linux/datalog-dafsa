// Build libdatalog.so + dl CLI from 100% Zig.
//
// The pre-migration C engine (src/*.c + vendor/dafsa/*.c) is REMOVED; every
// source below is a Zig port (zig/src/*.zig) or the vendored dafsa Zig engine
// (abi.zig).  The 42 C test suites re-linked against the resulting .so are
// the oracle (their re-link is the ABI-completeness check; their asserts are
// the behavioral oracle).
//
//   zig build -p zig-out --build-file zig/build.zig
//     -> zig-out/lib/libdatalog.so   (SONAME libdatalog.so)
//     -> zig-out/bin/dl              (CLI linked against that .so)
const std = @import("std");

// Engine library objects — mirrors Makefile LIB_OBJS.
// U2: util.c, tupleset.c, termstore.c and schema.c are now ported to Zig
// (zig/src/{util,tupleset,termstore,schema}.zig, referenced by hybrid.zig).
// U3: intern.c is ported to zig/src/intern.zig the same way.
// U4: relation.c, vrelation.c and txnwal.c are ported to
// zig/src/{relation,vrelation,txnwal}.zig the same way.
// U5: parser.c and typecheck.c are ported to
// zig/src/{parser,typecheck}.zig the same way.
// U6: regexwalk.c is ported to zig/src/regexwalk.zig the same way.
// U7: snapshot.c, permindex.c and iter.c are ported to
// zig/src/{snapshot,permindex,iter}.zig the same way.
// U8: compiler.c is ported to zig/src/compiler.zig the same way.
// U11: dl.c is ported to zig/src/dl.zig the same way.
// U14: index.c, vector.c and analyze.c are ported to
// zig/src/{index,vector,analyze}.zig — the datalog engine is now 100% Zig
// (the migration completes; the C oracle sources are removed).

// The vendored DAFSA engine is no longer compiled from C (vendor/dafsa/*.c).
// Since U15 it comes from the dafsa ZIG engine (vendor/dafsa/zig/src/*.zig),
// imported below as the `dafsa_abi` module: abi.zig exports the full dafsa.h /
// dafsa_internal.h C ABI (dafsa_*/view_*/wal_*/rank/select/range_count +
// trans_find/view_trans_find/view_edge_next/view_enum_dfs) plus the C-layout
// CFacade/CViewFacade handles, so the ported modules' @cImport'd extern decls
// and struct-field derefs (d->states/initial, v->csr/state_off/final_bits)
// resolve against the Zig engine unchanged.  The vendor C files are gone
// (removed with the C oracle).  No C is compiled into the .so anymore.

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // ReleaseFast: the C test suites (the behavioral oracle) build with -O2;
    // Debug would be needlessly slow for the suite matrix.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    // ─── dafsa Zig engine (the C-ABI export layer, replacing vendor/dafsa/*.c)
    // abi.zig links libc (c_allocator facades + fprintf in dafsa_dot).
    const dafsa_abi = b.createModule(.{
        .root_source_file = b.path("../vendor/dafsa/zig/src/abi.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    // ─── libdatalog.so (the strangler hybrid) ────────────────────────────
    const lib = b.addLibrary(.{
        .name = "datalog",
        .linkage = .dynamic,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    const lmod = lib.root_module;
    // Zig side of the hybrid: the root references the ported modules whose
    // `export fn`s carry the exact C ABI names the retained C expects, plus
    // the dafsa Zig engine (abi.zig) which supplies the dafsa_* surface.
    lmod.root_source_file = b.path("src/hybrid.zig");
    lmod.addImport("dafsa_abi", dafsa_abi);
    lmod.addIncludePath(b.path("../src"));
    lmod.addIncludePath(b.path("../vendor/dafsa"));
    b.installArtifact(lib);

    // ─── dl CLI, dynamically linked against the 100%-Zig .so ─────────────
    // (test_m4_review popen()s ./dl and test_vector_cli execv()s it; the
    // smoke suite drives it too — all relinked/pointed at the Zig build.)
    // U12: the exe is the ported zig/src/dl_cli.zig (src/dl_cli.c is removed
    // with the C oracle).  It resolves every engine
    // symbol from the .so exports at link time — dl_*/regex_*/intern_* from
    // the ported Zig modules and, since U14, tokenize/dl_search_top/
    // dl_vector_* from the ported index.zig/vector.zig.
    const exe = b.addExecutable(.{
        .name = "dl",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .root_source_file = b.path("src/dl_cli.zig"),
        }),
    });
    const emod = exe.root_module;
    emod.addIncludePath(b.path("../src"));
    emod.addIncludePath(b.path("../vendor/dafsa"));
    emod.linkLibrary(lib);
    emod.addRPathSpecial("$ORIGIN/../lib");
    b.installArtifact(exe);

    // ─── Unit tests for the ported Zig modules (`zig build test`) ─────────
    // Runs every inline `test` decl of the ported modules (zig/src/tests.zig
    // imports them all).  Those tests exercise C symbols (dafsa_*, crc32;
    // the vendored DAFSA engine stays C), so the binary links the vendored
    // DAFSA engine.  Since U14 the engine itself is 100% Zig — lib_srcs is
    // EMPTY and only vendor_srcs is compiled as C for the test link (all
    // dl_*/index/vector/analyze symbols now come from the ported modules).
    // Since U15 the dafsa engine is also Zig: tests.zig references dafsa_abi
    // so the dafsa_*/crc32_*/trans_find symbols come from abi.zig's exports.
    const unit_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .root_source_file = b.path("src/tests.zig"),
        }),
    });
    const tmod = unit_tests.root_module;
    tmod.addImport("dafsa_abi", dafsa_abi);
    tmod.addIncludePath(b.path("../src"));
    tmod.addIncludePath(b.path("../vendor/dafsa"));
    const run_unit_tests = b.addRunArtifact(unit_tests);
    // The gate IS the exit code: addRunArtifact marks this step failed when
    // the test binary reports any failure/crash/timeout/leak over the
    // --listen IPC, and `zig build test` then exits non-zero (verified by
    // temporarily breaking an expectation).  Note: tests exercising lexer
    // error paths make the ported parser fprintf(stderr) ("parser: unclosed
    // ..."), same noise as the C test suites; on success the build runner
    // echoes that captured stderr plus a "failed command:" line (build_runner
    // printErrorMessages prints whenever result_stderr is non-empty) WITHOUT
    // failing — the exit code is the only reliable signal.
    const test_step = b.step("test", "Run ported-module unit tests");
    test_step.dependOn(&run_unit_tests.step);
}
