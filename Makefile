# Makefile for datalog-dafsa — DEFERRED C++ EMBED/GGML TARGETS ONLY.
#
# The engine itself is 100% Zig and is built by zig/build.zig (the canonical
# build: `zig build -Drelease -p zig-out --build-file zig/build.zig`).  The
# pre-migration C engine targets this Makefile used to carry (libdatalog.so,
# dl, dl-lsp, tests/*, bench, dlp, ...) are gone — they compiled the retired
# C oracle sources and the deleted vendored DAFSA C.
#
# What remains is the opt-in, never-migrates C++ embed machinery
# (MIGRATION.md §7): ggml-backed dl-embed / libembed.so plus their tests and
# the model fetch.  These are independent of the engine (src/embed/dl_driver.cpp
# drives the dl CLI via fork+execv, it does not link it).
#
# Targets:
#   make dl-embed          ggml-backed vector embed CLI (needs vendor/ggml)
#   make libembed.so       in-process query encoder shared library
#   make test_embed_math   offline embed-math suite (no ggml)
#   make test_bert_load    bert GGUF-load contract test (needs vendor/ggml)
#   make fetch-model       download the bge-small-en-v1.5 GGUF
#   make embed-test        full opt-in gate: build + run all of the above
#   make clean             remove embed artifacts only

.PHONY: help ggml-check dl-embed fetch-model embed-test test_embed_math test_bert_load clean

# Default: print help; never builds the (removed) C engine.
help:
	@echo "datalog-dafsa Makefile — deferred C++ embed/ggml targets only."
	@echo "The engine is built by zig/build.zig (canonical; see repo README)."
	@echo
	@echo "Opt-in targets:"
	@echo "  dl-embed         ggml-backed vector embed CLI (needs vendor/ggml)"
	@echo "  libembed.so      in-process query encoder shared library"
	@echo "  test_embed_math  offline embed-math suite (no ggml)"
	@echo "  test_bert_load   bert GGUF-load contract test (needs vendor/ggml)"
	@echo "  fetch-model      download the bge-small-en-v1.5 GGUF"
	@echo "  embed-test       full opt-in gate (build + run all of the above)"
	@echo "  clean            remove embed artifacts only"

# ─── dl-embed: vector-tier embed tool (ggml C++; OPT-IN) ─────────────────
# Requires (host, once):  git submodule update --init vendor/ggml   (pinned
# v0.20.2, commit 8c63e70)  +  cmake on PATH.  ggml builds under its OWN
# cmake flags (relaxed) — the project -Werror stays scoped to src/embed's
# own tests and is NOT applied to ggml or to src/embed's ggml include.

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
test_embed_math: tests/test_embed_math
tests/test_embed_math: tests/test_embed_math.cpp src/embed/itq.cpp src/embed/tokenizer.cpp
	g++ -O2 -Wall -Wextra -Werror -std=c++17 -Isrc \
	    tests/test_embed_math.cpp src/embed/itq.cpp src/embed/tokenizer.cpp -o $@ -lm

# bert_load contract test on a synthetic GGUF (needs ggml libs, NOT the
# model): pins that a bge-layout GGUF WITHOUT output_norm loads (the real
# CompendiumLabs GGUF ships no final encoder norm) while output_norm, when
# present, is still applied.
test_bert_load: tests/test_bert_load
tests/test_bert_load: tests/test_bert_load.cpp src/embed/bert.o src/embed/tokenizer.o $(GGML_LIBS)
	g++ $(EMBED_CXXFLAGS) -o $@ tests/test_bert_load.cpp src/embed/bert.o \
	    src/embed/tokenizer.o -L$(GGML_BUILD)/src \
	    -lggml -lggml-cpu -lggml-base -lpthread -lm

# Full dl-embed gate: build + self-test (golden embeddings when the model is
# present; offline checks otherwise).
embed-test: dl-embed tests/test_embed_math tests/test_bert_load
	./tests/test_embed_math
	./tests/test_bert_load
	./dl-embed self-test

# ─── Clean (embed artifacts only) ────────────────────────────────────────

clean:
	rm -f dl-embed libembed.so
	rm -f src/embed/*.o
	rm -f vendor/http_client/*.o vendor/yyjson/*.o
	rm -f tests/test_embed_math tests/test_bert_load
	rm -rf build-tmp/ggml
