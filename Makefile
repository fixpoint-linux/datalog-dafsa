# Makefile for datalog-dafsa M7: fact store, interner, parser, compiler, VM,
# aggregates, snapshot, regex, permutation indices, durability.
# Builds libdatalog.so (shared), dl CLI, tests, and bench.

CC       = gcc
CFLAGS   = -O2 -Wall -Wextra -Werror -std=c11 -fPIC -D_POSIX_C_SOURCE=200809L
LDFLAGS  = -shared -fPIC -Wl,-soname,libdatalog.so
TMPDIR   = $(CURDIR)/build-tmp
export TMPDIR

INC      = -Ivendor/dafsa -Isrc

# ─── Vendor objects (DAFSA engine, submodule fixpoint-linux/dafsa) ───────

VENDOR_OBJS = vendor/dafsa/dafsa.o \
              vendor/dafsa/dafsa_state.o \
              vendor/dafsa/dafsa_core.o \
              vendor/dafsa/dafsa_persist.o \
              vendor/dafsa/dafsa_view.o \
              vendor/dafsa/dafsa_crc32.o \
              vendor/dafsa/dafsa_wal.o \
              vendor/dafsa/dafsa_build.o \
              vendor/dafsa/dafsa_rank.o \
              vendor/dafsa/dafsa_view_rank.o

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
           src/schema.o \
           src/typecheck.o \
           src/txnwal.o \
           src/index.o \
           src/vector.o

# Combined object list used for static links (tests, CLI).
ALL_OBJS = $(VENDOR_OBJS) $(LIB_OBJS)

# ─── Targets ─────────────────────────────────────────────────────────────

.PHONY: all clean test test-parallel bench test-m1 test-m2 wasm lsp test-lsp dlp dlp_schema_check dlp-check dlp-golden dl-embed fetch-model embed-test

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

vendor/dafsa/%.o: vendor/dafsa/%.c vendor/dafsa/dafsa.h vendor/dafsa/dafsa_internal.h
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

tests/test_cas: tests/test_cas.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_cas.c $(ALL_OBJS)

tests/test_concurrency: tests/test_concurrency.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_concurrency.c $(ALL_OBJS)

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

tests/test_traverse: tests/test_traverse.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_traverse.c $(ALL_OBJS)

tests/test_search: tests/test_search.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_search.c $(ALL_OBJS)

tests/test_vector_storage: tests/test_vector_storage.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_vector_storage.c $(ALL_OBJS)

tests/test_vector_search: tests/test_vector_search.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_vector_search.c $(ALL_OBJS) -lm

tests/test_vector_cli: tests/test_vector_cli.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_vector_cli.c $(ALL_OBJS) -lm

tests/test_vector_search_content: tests/test_vector_search_content.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_vector_search_content.c $(ALL_OBJS)

# ─── dl-embed: vector-tier embed tool (ggml C++; OPT-IN) ─────────────────
# Requires (host, once):  git submodule update --init vendor/ggml   (pinned
# v0.20.2, commit 8c63e70)  +  cmake on PATH.  ggml builds under its OWN
# cmake flags (relaxed) — the project -Werror stays scoped to src/ tests/
# vendor/dafsa and is NOT applied to ggml or to src/embed's ggml include.
# The default `all`/`test` targets never require ggml or the model; dl-embed
# and its gates are separate opt-in targets (`make dl-embed`, `make embed-test`).

GGML_SRC   := vendor/ggml
GGML_BUILD := build-tmp/ggml
GGML_CMAKE := $(GGML_BUILD)/CMakeCache.txt
GGML_LIBS  := $(GGML_BUILD)/src/libggml.a \
              $(GGML_BUILD)/src/libggml-cpu.a \
              $(GGML_BUILD)/src/libggml-base.a
EMBED_CXXFLAGS := -O2 -Wall -Wextra -fPIC -D_GNU_SOURCE -std=c++17 \
                  -Ivendor/ggml/include -Ivendor/http_client -Ivendor/yyjson -Isrc
VENDOR_EMBED_OBJS := vendor/http_client/http_client.o vendor/yyjson/yyjson.o
EMBED_OBJS := src/embed/itq.o src/embed/tokenizer.o src/embed/bert.o \
              src/embed/csv_emit.o src/embed/dl_driver.o src/embed/dl-embed.o \
              src/embed/remote_embed.o $(VENDOR_EMBED_OBJS)
LIBEMBED_OBJS := src/embed/itq.o src/embed/tokenizer.o src/embed/bert.o \
                 src/embed/csv_emit.o src/embed/embed_api.o \
                 src/embed/remote_embed.o $(VENDOR_EMBED_OBJS)

# fail fast (before any g++) if the submodule is not materialized
.PHONY: ggml-check
ggml-check:
	@test -f $(GGML_SRC)/CMakeLists.txt || { echo "vendor/ggml missing — run: git submodule update --init vendor/ggml"; exit 2; }

$(GGML_CMAKE): ggml-check $(GGML_SRC)/CMakeLists.txt
	cmake -S $(GGML_SRC) -B $(GGML_BUILD) -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DGGML_BUILD_EXAMPLES=OFF -DGGML_BUILD_TESTS=OFF \
	      -DGGML_CUDA=OFF -DGGML_METAL=OFF -DGGML_VULKAN=OFF -DGGML_OPENCL=OFF \
	      -DGGML_NATIVE=OFF -DBUILD_SHARED_LIBS=OFF -DGGML_OPENMP=OFF

$(GGML_LIBS): $(GGML_CMAKE)
	$(MAKE) -C $(GGML_BUILD) ggml ggml-cpu

EMBED_HDRS := src/embed/bert.h src/embed/tokenizer.h src/embed/itq.h \
              src/embed/csv_emit.h src/embed/dl_driver.h src/embed/embed_api.h \
              src/embed/vec_bits.h src/embed/remote_embed.h

src/embed/%.o: src/embed/%.cpp $(EMBED_HDRS) | ggml-check
	g++ $(EMBED_CXXFLAGS) -c -o $@ $<

# vendored C (http client + yyjson) — plain gcc, self-contained, no libcurl.
vendor/http_client/http_client.o: vendor/http_client/http_client.c vendor/http_client/http_client.h
	gcc -O2 -Wall -Wextra -fPIC -std=c11 -c -o $@ $<
vendor/yyjson/yyjson.o: vendor/yyjson/yyjson.c vendor/yyjson/yyjson.h
	gcc -O2 -Wall -Wextra -fPIC -std=c11 -c -o $@ $<

dl-embed: $(EMBED_OBJS) $(GGML_LIBS)
	g++ $(EMBED_CXXFLAGS) -o $@ $(EMBED_OBJS) \
	    -L$(GGML_BUILD)/src \
	    -lggml -lggml-cpu -lggml-base -lpthread -lm

# libembed.so: the in-process query encoder for external hosts (fx-agent-memory
# vsearch).  ggml is statically absorbed; only libstdc++.so.6 is a runtime dep.
libembed.so: $(LIBEMBED_OBJS) $(GGML_LIBS)
	g++ -shared -fPIC $(EMBED_CXXFLAGS) -o $@ $(LIBEMBED_OBJS) \
	    -L$(GGML_BUILD)/src \
	    -lggml -lggml-cpu -lggml-base -lpthread -lm

# Model: bge-small-en-v1.5 GGUF, ~67 MB f16.  The repo-local `models/` copy is
# tracked with git-lfs (see .gitattributes); `fetch-model` (re)downloads the
# canonical file into that dir and is also how a fresh clone materializes it
# before `git lfs pull`.
MODEL_URL  := https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf/resolve/main/bge-small-en-v1.5-f16.gguf
MODEL_DIR  ?= models
MODEL_PATH := $(MODEL_DIR)/bge-small-en-v1.5-f16.gguf

fetch-model:
	@mkdir -p $(MODEL_DIR)
	curl -L --fail --progress-bar -o $(MODEL_PATH) $(MODEL_URL)
	@ls -l $(MODEL_PATH)

# Offline embed-math suite (tokenizer + ITQ vs numpy/LAPACK goldens; no ggml).
tests/test_embed_math: tests/test_embed_math.cpp src/embed/itq.cpp src/embed/tokenizer.cpp
	g++ -O2 -Wall -Wextra -Werror -std=c++17 -Isrc \
	    tests/test_embed_math.cpp src/embed/itq.cpp src/embed/tokenizer.cpp -o $@ -lm

# Engine-side byte-identity + emission round-trip (links the C engine only).
tests/test_embed: tests/test_embed.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_embed.c $(ALL_OBJS) -lm

# bert_load contract test on a synthetic GGUF (needs ggml libs, NOT the
# model): pins that a bge-layout GGUF WITHOUT output_norm loads (the real
# CompendiumLabs GGUF ships no final encoder norm) while output_norm, when
# present, is still applied.
tests/test_bert_load: tests/test_bert_load.cpp src/embed/bert.o src/embed/tokenizer.o $(GGML_LIBS)
	g++ $(EMBED_CXXFLAGS) -o $@ tests/test_bert_load.cpp src/embed/bert.o \
	    src/embed/tokenizer.o -L$(GGML_BUILD)/src \
	    -lggml -lggml-cpu -lggml-base -lpthread -lm

# Full dl-embed gate: build + self-test (golden embeddings when the model is
# present; offline checks otherwise).
embed-test: dl-embed tests/test_embed tests/test_embed_math tests/test_bert_load
	./tests/test_embed
	./tests/test_embed_math
	./tests/test_bert_load
	./dl-embed self-test

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

tests/test_typecheck: tests/test_typecheck.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_typecheck.c $(ALL_OBJS)

tests/bench: tests/bench.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/bench.c $(ALL_OBJS)

bench: tests/bench
	@echo "=== Running demonstration benchmark ==="
	LD_LIBRARY_PATH=. ./tests/bench

test: tests/test_m0 tests/test_m1 tests/test_m2 tests/test_m3 tests/test_m4 tests/test_m4_review tests/test_bulk tests/test_m5 tests/test_m5_review tests/test_m6 tests/test_m6_review tests/test_m6_deep_review tests/test_m7 tests/test_cas tests/test_concurrency tests/test_m8_magic tests/test_topdown tests/test_m9_arith tests/test_m9_str tests/test_ivm tests/test_bushy tests/test_vararity tests/test_lists tests/test_m10_rank tests/test_m11_range tests/test_m12_snap_rank tests/test_m13_iter tests/test_m14_permsel tests/test_m15_vmiter tests/test_m16_travel tests/test_positions tests/test_schema tests/test_typecheck tests/test_traverse tests/test_search tests/test_vector_storage tests/test_vector_search tests/test_vector_cli tests/test_vector_search_content tests/test_embed_math tests/test_embed dl build-tmp
	./tests/run_all.sh

test-parallel: tests/run_all.sh
	./tests/run_all.sh

test-m1: tests/test_m1 dl build-tmp
	@echo "=== Running M1 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m1

test-m2: tests/test_m2 dl build-tmp
	@echo "=== Running M2 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m2

test-m16: tests/test_m16_travel dl build-tmp
	@echo "=== Running M16 time-travel (as-of) snapshot tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m16_travel

# ─── dlp (dl-project tool) — OPT-IN, links the engine + dhall-c ──────────
# A NEW top-level tool that scaffolds a project and loads/walks a schema.dhall
# into a typed dl_schema.  Built with cosmocc (the dhall-c interpreter is not
# gcc-clean for this link) and is NOT part of the default `make`/`make test`
# (which stay gcc-only and never touch dhall-c).  Usage:
#   make dlp            # uses $(CURDIR)/../dhall-c by default
#   make dlp DHALLC=/path/to/dhall-c
#   make dlp-check      # build + run the schema-check harness

# dhall-c core sources (link directly, in dhall-c's own order; exclude its
# entry-point/extra TUs: main/wasm/bench/lsp and json.c which only LSP links).
DHALLC ?= $(CURDIR)/../dhall-c
CORE_SRCS = $(DHALLC)/src/arena.c $(DHALLC)/src/lexer.c \
            $(DHALLC)/src/parser.c $(DHALLC)/src/ast.c \
            $(DHALLC)/src/normalize.c $(DHALLC)/src/typecheck.c \
            $(DHALLC)/src/builtins.c $(DHALLC)/src/serialize.c \
            $(DHALLC)/src/import.c $(DHALLC)/src/bignum.c \
            $(DHALLC)/src/sha256.c $(DHALLC)/src/ssrf.c $(DHALLC)/src/http.c

# Engine sources for dlp: the LIB_OBJS source set + vendored dafsa*.c,
# EXCLUDING the TUs that carry their own entry points (dl_cli.c main,
# lsp.c main, playground-wasm.c wasm entry).  We compile from source again
# with cosmocc (the gcc-built .o files are not cosmo-safe to reuse).
DLP_ENGINE_SRCS = src/intern.c src/termstore.c src/relation.c \
                  src/vrelation.c src/tupleset.c src/parser.c src/compiler.c \
                  src/vm.c src/snapshot.c src/regexwalk.c src/permindex.c \
                  src/util.c src/dl.c src/iter.c src/magic.c src/topdown.c \
                  src/analyze.c src/schema.c src/typecheck.c src/json.c \
                  src/txnwal.c \
                  vendor/dafsa/dafsa.c vendor/dafsa/dafsa_state.c vendor/dafsa/dafsa_core.c \
                  vendor/dafsa/dafsa_persist.c vendor/dafsa/dafsa_view.c \
                  vendor/dafsa/dafsa_crc32.c vendor/dafsa/dafsa_wal.c vendor/dafsa/dafsa_build.c \
                  vendor/dafsa/dafsa_rank.c vendor/dafsa/dafsa_view_rank.c

DLP_SRCS = dlp/main.c dlp/schema_load.c dlp/init.c dlp/csv_load.c dlp/json_load.c dlp/workflow.c

# Use := (not ?=) so the environment's CC=cc does not override cosmocc.
COSMOCC := cosmocc
DLP_CFLAGS = -std=c11 -O2 -g -Wall -Wextra -I$(DHALLC)/src -Ivendor/dafsa -Isrc

dlp: $(DLP_SRCS) $(DLP_ENGINE_SRCS) $(CORE_SRCS) dlp/dlp.h
	$(COSMOCC) $(DLP_CFLAGS) -o dlp/dlp $(DLP_SRCS) $(DLP_ENGINE_SRCS) $(CORE_SRCS)

# Verification harness: assert the worked-example schema.dhall walks to the
# expected dl_schema.  Links the same sources as `dlp`.
dlp_schema_check: dlp/schema_check.c dlp/schema_load.c $(DLP_ENGINE_SRCS) $(CORE_SRCS) dlp/dlp.h
	$(COSMOCC) $(DLP_CFLAGS) -o $@ dlp/schema_check.c dlp/schema_load.c $(DLP_ENGINE_SRCS) $(CORE_SRCS)

dlp-check: dlp dlp_schema_check
	./dlp_schema_check
	@rm -rf /tmp/dlp-check-proj && ./dlp/dlp init /tmp/dlp-check-proj && ./dlp/dlp schema /tmp/dlp-check-proj

# S5 golden test: build dlp then run the end-to-end worked-example assertions
# (good project check/build/query + bug-rule rejection).
dlp-golden: dlp
	@tests/dlp_golden.sh ./dlp/dlp


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
	rm -f vendor/dafsa/*.o src/*.o
	rm -f libdatalog.so dl dl-lsp dlp/dlp dlp_schema_check
	rm -f dlp/dlp.aarch64.elf dlp/dlp.com.dbg \
	      dlp_schema_check.aarch64.elf dlp_schema_check.com.dbg
	rm -f tests/test_m0 tests/test_m1 tests/test_m2 tests/test_m3 tests/test_m4 \
	      tests/test_m4_review tests/test_m5 tests/test_m5_review tests/test_m6 \
	      tests/test_m6_review tests/test_bulk \
	      tests/test_m6_deep_review tests/test_m7 tests/test_m8_magic tests/test_concurrency tests/test_topdown tests/test_m9_arith \
	      tests/test_m9_str tests/test_ivm tests/test_bushy tests/test_vararity \
	      tests/test_lists tests/test_m10_rank tests/test_m11_range tests/test_m12_snap_rank tests/test_m13_iter tests/test_m14_permsel tests/test_m15_vmiter tests/test_m16_travel tests/test_positions tests/test_schema tests/test_typecheck tests/test_traverse tests/test_search tests/test_vector_storage tests/test_vector_search tests/test_vector_cli tests/test_embed_math tests/test_embed tests/test_bert_load tests/bench
	rm -f dl-embed libembed.so src/embed/itq.o src/embed/tokenizer.o src/embed/bert.o \
	      src/embed/csv_emit.o src/embed/dl_driver.o src/embed/dl-embed.o src/embed/embed_api.o
	rm -rf build-tmp/ggml
	rm -rf /tmp/dl-test-db build-tmp/smoke build-tmp/m1 build-tmp/vararity build-tmp/lists build-tmp/rank build-tmp/m12snap build-tmp/m16travel build-tmp/search
