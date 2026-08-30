// U1 strangler-hybrid skeleton: build libdatalog.so + dl CLI from 100% C.
//
// This is the migration harness, not a port: every source file below is the
// UNMODIFIED C from the Makefile build (LIB_OBJS + VENDOR_OBJS, 1:1).  Later
// units swap individual C files for Zig exports in this one build; the 41
// test suites re-linked against the resulting .so are the oracle.
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
// The rest of LIB_OBJS is still C.
const lib_srcs = [_][]const u8{
    "src/compiler.c",
    "src/vm.c",
    "src/snapshot.c",
    "src/regexwalk.c",
    "src/permindex.c",
    "src/dl.c",
    "src/iter.c",
    "src/magic.c",
    "src/topdown.c",
    "src/analyze.c",
    "src/index.c",
    "src/vector.c",
};

// Vendored DAFSA engine — mirrors Makefile VENDOR_OBJS.
const vendor_srcs = [_][]const u8{
    "vendor/dafsa/dafsa.c",
    "vendor/dafsa/dafsa_state.c",
    "vendor/dafsa/dafsa_core.c",
    "vendor/dafsa/dafsa_persist.c",
    "vendor/dafsa/dafsa_view.c",
    "vendor/dafsa/dafsa_crc32.c",
    "vendor/dafsa/dafsa_wal.c",
    "vendor/dafsa/dafsa_build.c",
    "vendor/dafsa/dafsa_rank.c",
    "vendor/dafsa/dafsa_view_rank.c",
};

const all_srcs = lib_srcs ++ vendor_srcs;

// CFLAGS mirror the Makefile's CFLAGS, with three deliberate differences:
//   - no -Werror: zig cc is clang and warns differently than gcc; warning
//     drift is expected and engine C is never edited to appease it.
//   - -fvisibility=default is explicit: every non-static symbol must stay
//     exported for the tests/consumers (the ABI audit checks this).
//   - -fno-sanitize=undefined: zig cc injects UBSan checks (in Debug) that
//     abort on glibc-tolerated UB the engine relies on, e.g.
//     qsort(NULL, 0, ...) on an empty vector (src/vector.c:130).  The oracle
//     is the gcc -O2 build; the harness must not introduce new aborts.
//   - -ftrivial-auto-var-init=zero: the engine C has latent uninitialized-
//     stack UB the gcc build passes by layout luck — e.g. regexwalk.c
//     regex_compile()'s fail path calls dsm_free(&dsm) before dsm_init on
//     parse errors (tests/test_m5_review G03).  Zero-init makes the clang
//     build deterministic and oracle-faithful without editing engine C.
const c_flags = [_][]const u8{
    "-std=c11",
    "-D_POSIX_C_SOURCE=200809L",
    "-fPIC",
    "-fvisibility=default",
    "-fno-sanitize=undefined",
    "-ftrivial-auto-var-init=zero",
    "-Wall",
    "-Wextra",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    // ReleaseFast: the C oracle builds with -O2; Debug would be needlessly
    // slow for the suite matrix (and asserts nothing extra for plain C).
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

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
    // `export fn`s carry the exact C ABI names the retained C expects.
    lmod.root_source_file = b.path("src/hybrid.zig");
    lmod.addIncludePath(b.path("../src"));
    lmod.addIncludePath(b.path("../vendor/dafsa"));
    lmod.addCSourceFiles(.{
        .root = b.path(".."),
        .files = &all_srcs,
        .flags = &c_flags,
    });
    b.installArtifact(lib);

    // ─── dl CLI, dynamically linked against the hybrid .so ───────────────
    // (test_m4_review popen()s ./dl and test_vector_cli execv()s it; the
    // smoke suite drives it too — all relinked/pointed at the Zig build.)
    const exe = b.addExecutable(.{
        .name = "dl",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    const emod = exe.root_module;
    emod.addIncludePath(b.path("../src"));
    emod.addIncludePath(b.path("../vendor/dafsa"));
    emod.addCSourceFiles(.{
        .root = b.path(".."),
        .files = &.{"src/dl_cli.c"},
        .flags = &c_flags,
    });
    emod.linkLibrary(lib);
    emod.addRPathSpecial("$ORIGIN/../lib");
    b.installArtifact(exe);

    // ─── Unit tests for the ported Zig modules (`zig build test`) ─────────
    // Runs every inline `test` decl of the ported modules (zig/src/tests.zig
    // imports them all).  Those tests exercise C symbols (dafsa_*, crc32,
    // regex_dfa_walk/symset_contains from regexwalk.c), so the test binary
    // links the same C the modules import: the vendored DAFSA engine plus
    // src/regexwalk.c.  No other engine C is reachable from the tests.
    const unit_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .root_source_file = b.path("src/tests.zig"),
        }),
    });
    const tmod = unit_tests.root_module;
    tmod.addIncludePath(b.path("../src"));
    tmod.addIncludePath(b.path("../vendor/dafsa"));
    tmod.addCSourceFiles(.{
        .root = b.path(".."),
        .files = &(vendor_srcs ++ .{"src/regexwalk.c"}),
        .flags = &c_flags,
    });
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
