#!/bin/sh
# run_all.sh — parallel test harness for the datalog-dafsa suite.
#
# Runs every test binary in parallel with a concurrency cap of MAXJOBS, then
# prints a pass/fail summary.  A single failing job must not kill the others.
#
# Exit status is non-zero if any test failed.

set -u

# Run from the repo root so build-tmp/ relative paths work.
cd "$(dirname "$0")/.." || { echo "run_all.sh: cannot cd to repo root" >&2; exit 1; }

MAXJOBS=4

# Binary names, in the order the suites run.  There is no Makefile any more
# (the engine builds with `zig build`; zig/run_zig_suites.sh is the oracle).
# test_embed is standalone (run without LD_LIBRARY_PATH); smoke is a special
# step (see launch_one).  The C++ embed/ggml suites are gone with the C++
# embed sidecar.
TESTS="
test_m0
test_m1
test_m2
test_m3
test_m4
test_m4_review
test_bulk
test_m5
test_m5_review
test_m6
test_m6_review
test_m6_deep_review
test_m7
test_cas
test_concurrency
test_m9_arith
test_m9_str
test_ivm
test_bushy
test_vararity
test_lists
test_m10_rank
test_m11_range
test_m12_snap_rank
test_m13_iter
test_m14_permsel
test_m15_vmiter
test_m16_travel
test_positions
test_schema
test_typecheck
test_traverse
test_search
test_vector_storage
test_vector_cli
test_vector_search_content
test_embed
smoke
"

mkdir -p build-tmp || true
RESULT_DIR=build-tmp
LAUNCH_IDX=0

# launch_one <name> <idx>
# Echo the run banner, then execute the test.  Records "<name> <rc>" to a
# result file so the parent can collect exit codes in a race-free way.
launch_one() {
    name=$1
    idx=$2

    case "$name" in
        smoke)
            echo "=== Running CLI smoke test ==="
            sh tests/smoke.sh
            rc=$?
            ;;
        test_embed)
            # standalone binaries: no LD_LIBRARY_PATH needed
            echo "=== Running $name ==="
            ./tests/$name
            rc=$?
            ;;
        *)
            echo "=== Running $name ==="
            LD_LIBRARY_PATH=. ./tests/$name
            rc=$?
            ;;
    esac

    echo "$name $rc" > "$RESULT_DIR/.runall.$idx"
}

jobnames=""
jobpids=""
njobs=0

for name in $TESTS; do
    launch_one "$name" "$LAUNCH_IDX" &
    jobpids="$jobpids $!"
    njobs=$((njobs + 1))
    LAUNCH_IDX=$((LAUNCH_IDX + 1))

    # If the pool is full, reap the oldest job before launching more.
    if [ "$njobs" -ge "$MAXJOBS" ]; then
        set -- $jobpids
        first=$1
        shift
        jobpids="$*"
        njobs=$((njobs - 1))
        wait "$first"
    fi
done

# Drain any remaining jobs.
set -- $jobpids
for p; do
    wait "$p"
done

# Collect results in launch order.
total=0
passed=0
failed_list=""
i=0
while [ "$i" -lt "$LAUNCH_IDX" ]; do
    f="$RESULT_DIR/.runall.$i"
    total=$((total + 1))
    if [ -r "$f" ]; then
        read -r name rc < "$f" 2>/dev/null || rc=1
        if [ "$rc" -eq 0 ]; then
            passed=$((passed + 1))
        else
            failed_list="$failed_list $name(rc=$rc)"
        fi
    else
        failed_list="$failed_list <unknown-job-$i>"
    fi
    i=$((i + 1))
done

echo ""
echo "$passed/$total passed"

if [ -n "$failed_list" ]; then
    echo "FAILED:$failed_list"
    exit 1
fi

exit 0
