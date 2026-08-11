# Makefile for datalog-dafsa M2: M0 + M1 + fixpoint + negation
# Builds libdatalog.so (shared), dl CLI, and test harness.

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
              vendor/dafsa_wal.o

# ─── Our library objects ─────────────────────────────────────────────────

LIB_OBJS = src/intern.o \
           src/relation.o \
           src/tupleset.o \
           src/parser.o \
           src/compiler.o \
           src/vm.o \
           src/dl.o

# Combined object list used for static links (tests, CLI).
ALL_OBJS = $(VENDOR_OBJS) $(LIB_OBJS)

# ─── Targets ─────────────────────────────────────────────────────────────

.PHONY: all clean test test-m1 test-m2

all: build-tmp libdatalog.so dl

build-tmp:
	@mkdir -p build-tmp

libdatalog.so: $(VENDOR_OBJS) $(LIB_OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(VENDOR_OBJS) $(LIB_OBJS)

dl: src/dl_cli.o $(ALL_OBJS)
	$(CC) $(CFLAGS) -static -o $@ src/dl_cli.o $(ALL_OBJS)

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

tests/test_review_adversarial: tests/test_review_adversarial.c $(ALL_OBJS)
	$(CC) $(CFLAGS) $(INC) -static -o $@ tests/test_review_adversarial.c $(ALL_OBJS)

test: tests/test_m0 tests/test_m1 tests/test_m2 tests/test_m3 dl build-tmp
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
	@echo "=== Running CLI smoke test ==="
	@sh tests/smoke.sh

test-m1: tests/test_m1 dl build-tmp
	@echo "=== Running M1 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m1

test-m2: tests/test_m2 dl build-tmp
	@echo "=== Running M2 unit tests ==="
	LD_LIBRARY_PATH=. ./tests/test_m2

# ─── Clean ───────────────────────────────────────────────────────────────

clean:
	rm -f vendor/*.o src/*.o
	rm -f libdatalog.so dl
	rm -f tests/test_m0 tests/test_m1 tests/test_m2 tests/test_m3
	rm -rf /tmp/dl-test-db build-tmp/smoke build-tmp/m1
