# Makefile for datalog-dafsa M7: fact store, interner, parser, compiler, VM,
# aggregates, snapshot, regex, permutation indices, durability.
# Builds libdatalog.so (shared), dl CLI, tests, and bench.

CC       = gcc
CFLAGS   = -O2 -Wall -Wextra -Werror -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L
LDFLAGS  = -shared -fPIC
TMPDIR   = $(CURDIR)/build-tmp
export TMPDIR

INC      = -Ivendor -Isrc

# ─── Vendor objects (DAFSA engine, vendored from jing-meta) ─────────────

VENDOR_OBJS = vendor/dafsa.o \
              vendor/dafsa_state.o \
              vendor/dafsa_core.o \
              vendor/dafsa_persist.o \
              vendor/dafsa_view.o \
              vendor/dafsa_crc32.o \
              vendor/dafsa_wal.o \
              vendor/dafsa_build.o \
              vendor/dafsa_rank.o \
              vendor/dafsa_view_rank.o

# ─── Our library objects ─────────────────────────────────────────────────

LIB_OBJS = src/intern.o \
           src/termstore.o \
           src/relation.o \
           src/vrelation.o \
           src/tupleset.o \
           src/parser.o \
           src/compiler.o \
           src/vm.o \
           src/snapshot.o \
           src/regexwalk.o \
           src/permindex.o \
           src/util.o \
           src/dl.o \
           src/iter.o \
           src/magic.o \
           src/topdown.o \
           src/analyze.o \
           src/schema.o

# Combined object list used for static links (tests, CLI).
ALL_OBJS = $(VENDOR_OBJS) $(LIB_OBJS)

# ─── Targets ─────────────────────────────────────────────────────────────

.PHONY: all clean test bench test-m1 test-m2 wasm lsp test-lsp

all: build-tmp libdatalog.so dl

build-tmp:
	@mkdir -p build-tmp

libdatalog.so: $(VENDOR_OBJS) $(LIB_OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(VENDOR_OBJS) $(LIB_OBJS)

dl: src/dl_cli.o $(ALL_OBJS)
	$(CC) $(CFLAGS) -static -o $@ src/dl_cli.o $(ALL_OBJS)

# ─── Language server ──────────────────────────────────────────────────────
# The native LSP server binary.  src/analyze.o is part of ALL_OBJS (LIB_OBJS);
# src/lsp.o + src/json.o are LSP-only.  Mirrors the `dl` static link.
dl-lsp: src/lsp.o src/json.o $(ALL_OBJS)
	$(CC) $(CFLAGS) -static -o $@ src/lsp.o src/json.o $(ALL_OBJS)

lsp: dl-lsp

test-lsp: dl-lsp
	@sh tests/lsp.sh ./dl-lsp

# ─── Vendor rules ────────────────────────────────────────────────────────

vendor/%.o: vendor/%.c vendor/dafsa.h vendor/dafsa_internal.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

# ─── Library rules ───────────────────────────────────────────────────────

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

# ─── CLI rule ────────────────────────────────────────────────────────────

src/dl_cli.o: src/dl_cli.c src/dl.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

# ─── Tests ───────────────────────────────────────────────────────────────

tests/test_m0: tests/test_m0.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m0.c $(ALL_OBJS)

tests/test_m1: tests/test_m1.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m1.c $(ALL_OBJS)

tests/test_m2: tests/test_m2.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m2.c $(ALL_OBJS)

tests/test_m3: tests/test_m3.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m3.c $(ALL_OBJS)

tests/test_m4: tests/test_m4.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m4.c $(ALL_OBJS)

tests/test_m4_review: tests/test_m4_review.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m4_review.c $(ALL_OBJS)

tests/test_bulk: tests/test_bulk.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_bulk.c $(ALL_OBJS)

tests/test_m5: tests/test_m5.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m5.c $(ALL_OBJS)

tests/test_m5_review: tests/test_m5_review.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m5_review.c $(ALL_OBJS)

tests/test_m6: tests/test_m6.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m6.c $(ALL_OBJS)

tests/test_m6_review: tests/test_m6_review.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m6_review.c $(ALL_OBJS)

tests/test_m6_deep_review: tests/test_m6_deep_review.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m6_deep_review.c $(ALL_OBJS)

tests/test_m7: tests/test_m7.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m7.c $(ALL_OBJS)

tests/test_m8_magic: tests/test_m8_magic.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m8_magic.c $(ALL_OBJS)

tests/test_topdown: tests/test_topdown.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_topdown.c $(ALL_OBJS)

tests/test_m9_arith: tests/test_m9_arith.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m9_arith.c $(ALL_OBJS)

tests/test_m9_str: tests/test_m9_str.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m9_str.c $(ALL_OBJS)

tests/test_ivm: tests/test_ivm.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_ivm.c $(ALL_OBJS)

tests/test_bushy: tests/test_bushy.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_bushy.c $(ALL_OBJS)

tests/test_vararity: tests/test_vararity.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_vararity.c $(ALL_OBJS)

tests/test_lists: tests/test_lists.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_lists.c $(ALL_OBJS)

tests/test_m10_rank: tests/test_m10_rank.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m10_rank.c $(ALL_OBJS)
tests/test_m11_range: tests/test_m11_range.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m11_range.c $(ALL_OBJS)
tests/test_m12_snap_rank: tests/test_m12_snap_rank.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m12_snap_rank.c $(ALL_OBJS)
tests/test_m13_iter: tests/test_m13_iter.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m13_iter.c $(ALL_OBJS)

tests/test_m14_permsel: tests/test_m14_permsel.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m14_permsel.c $(ALL_OBJS)

tests/test_m15_vmiter: tests/test_m15_vmiter.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m15_vmiter.c $(ALL_OBJS)

tests/test_m16_travel: tests/test_m16_travel.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_m16_travel.c $(ALL_OBJS)

tests/test_positions: tests/test_positions.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_positions.c $(ALL_OBJS)

tests/test_schema: tests/test_schema.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_schema.c $(ALL_OBJS)

tests/bench: tests/bench.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/bench.c $(ALL_OBJS)

bench: tests/bench
	@echo "=== Running demonstration benchmark ==="
	LD_LIBRARY_PATH=. ./tests/bench

test: tests/test_m0 tests/test_m1 tests/test_m2 tests/test_m3 tests/test_m4 tests/test_m4_review tests/test_bulk tests/test_m5 tests/test_m5_review tests/test_m6 tests/test_m6_review tests/test_m6_deep_review tests/test_m7 tests/test_m8_magic tests/test_topdown tests/test_m9_arith tests/test_m9_str tests/test_ivm tests/test_bushy tests/test_vararity tests/test_lists tests/test_m10_rank tests/test_m11_range tests/test_m12_snap_rank tests/test_m13_iter tests/test_m14_permsel tests/test_m15_vmiter tests/test_m16_travel tests/test_positions tests/test_schema dl build-tmp
	@echo "=== Running M0 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m0
	@echo ""
	@echo "=== Running M1 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m1
	@echo ""
	@echo "=== Running M2 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m2
	@echo ""
	@echo "=== Running M3 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m3
	@echo ""
	@echo "=== Running M4 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m4
	@echo ""
	@echo "=== Running M4 adversarial review tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m4_review
	@echo ""
	@echo "=== Running bulk DAFSA tests ==="
	LD_LIBRARY_PATH=. ./tests/test_bulk
	@echo ""
	@echo "=== Running M5 regex walker tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m5
	@echo ""
	@echo "=== Running M5 adversarial review tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m5_review
	@echo ""
	@echo "=== Running M6 permutation index tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m6
	@echo ""
	@echo "=== Running M6 adversarial tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m6_review
	@echo ""
	@echo "=== Running M6 deep adversarial tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m6_deep_review
	@echo ""
	@echo "=== Running M7 durability tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m7
	@echo ""
	@echo "=== Running M8 magic-sets tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m8_magic
	@echo ""
	@echo "=== Running top-down/QSQ tests ==="
	LD_LIBRARY_PATH=. ./tests/test_topdown
	@echo ""
	@echo "=== Running M9 arithmetic/comparison tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m9_arith
	@echo ""
	@echo "=== Running M9-strings builtin tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m9_str
	@echo ""
	@echo "=== Running IVM Slice 0 deletion-correctness tests ==="
	LD_LIBRARY_PATH=. ./tests/test_ivm
	@echo ""
	@echo "=== Running BUSHY join plan tests ==="
	LD_LIBRARY_PATH=. ./tests/test_bushy
	@echo ""
	@echo "=== Running v2 variable-arity tests ==="
	LD_LIBRARY_PATH=. ./tests/test_vararity
	@echo ""
	@echo "=== Running v2 lists tests ==="
	LD_LIBRARY_PATH=. ./tests/test_lists
	@echo ""
	@echo "=== Running M10 order-statistics (rank/select/range_count) tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m10_rank
	@echo ""
	@echo "=== Running M11 range predicate tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m11_range
	@echo ""
	@echo "=== Running M12 snapshot rank/select/range_count tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m12_snap_rank
	@echo ""
	@echo "=== Running M13 pull-iterator / merge-join tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m13_iter
	@echo ""
	@echo "=== Running M14 perm-index selection tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m14_permsel
	@echo ""
	@echo "=== Running M15 OP_RANGE lazy pull-iterator tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m15_vmiter
	@echo ""
	@echo "=== Running M16 time-travel (as-of) snapshot tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m16_travel
	@echo ""
	@echo "=== Running parser position (line:col) tests ==="
	LD_LIBRARY_PATH=. ./tests/test_positions
	@echo ""
	@echo "=== Running Dhall schema tests ==="
	LD_LIBRARY_PATH=. ./tests/test_schema
	@echo ""
	@echo "=== Running CLI smoke test ==="
	@sh tests/smoke.sh

test-m1: tests/test_m1 dl build-tmp
	@echo "=== Running M1 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m1

test-m2: tests/test_m2 dl build-tmp
	@echo "=== Running M2 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m2

test-m16: tests/test_m16_travel dl build-tmp
	@echo "=== Running M16 time-travel (as-of) snapshot tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m16_travel

# ─── WebAssembly playground ─────────────────────────────────────────────
# Builds the in-browser language playground (docs/playground.js + .wasm)
# from src/playground-wasm.c + the full engine core, then runs the headless
# node smoke test against the freshly-built bundle.  Requires emscripten +
# node on the HOST (see scripts/build-wasm.sh); the emitted artifacts are
# committed to docs/ so the Pages site needs no build step.

wasm:
	./scripts/build-wasm.sh
	@node tests/wasm-smoke.js
	@node tests/lsp-wasm-smoke.js

# ─── Clean ───────────────────────────────────────────────────────────────

clean:
	rm -f vendor/*.o src/*.o
	rm -f libdatalog.so dl dl-lsp
	rm -f tests/test_m0 tests/test_m1 tests/test_m2 tests/test_m3 tests/test_m4 \
	      tests/test_m4_review tests/test_m5 tests/test_m5_review tests/test_m6 \
	      tests/test_m6_review tests/test_bulk \
	      tests/test_m6_deep_review tests/test_m7 tests/test_m8_magic tests/test_topdown tests/test_m9_arith \
	      tests/test_m9_str tests/test_ivm tests/test_bushy tests/test_vararity \
	      tests/test_lists tests/test_m10_rank tests/test_m11_range tests/test_m12_snap_rank tests/test_m13_iter tests/test_m14_permsel tests/test_m15_vmiter tests/test_m16_travel tests/test_positions tests/test_schema tests/bench
	rm -rf /tmp/dl-test-db build-tmp/smoke build-tmp/m1 build-tmp/vararity build-tmp/lists build-tmp/rank build-tmp/m12snap build-tmp/m16travel
