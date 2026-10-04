"""bench_envelope.py — the measured envelope for per-step agent use.

Every number is AS MEASURED, including any that miss the <=10us budget.
Run: python3 tests/bench_envelope.py [--quick]
"""

import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import dlb
from dlb._binding import load_library

NS = 1_000.0  # ns -> us divisor
QUICK = "--quick" in sys.argv

BENCH_ROOT = os.path.join(tempfile.gettempdir(), "dlb-bench")


def now_us() -> float:
    return time.perf_counter_ns() / NS


def rmk(path):
    shutil.rmtree(path, ignore_errors=True)
    return path


def build_store(path, n_edges: int) -> str:
    rmk(path)
    db = dlb.Db.open(path)
    db.declare_relation("edge", 2)
    for i in range(1, n_edges + 1):
        db.add_fact("edge", (i, i + 1))
    db.close()
    return path


def timing_loop(fn, n_iters: int, n_warmup: int = 50):
    """Median + p10/p90 of per-call latency in us."""
    for _ in range(n_warmup):
        fn()
    xs = []
    for _ in range(n_iters):
        t0 = now_us()
        fn()
        xs.append(now_us() - t0)
    xs.sort()
    med = statistics.median(xs)
    return {
        "median_us": med,
        "p10_us": xs[max(0, int(0.10 * len(xs)) - 1)],
        "p90_us": xs[min(len(xs) - 1, int(0.90 * len(xs)))],
        "min_us": xs[0],
        "n": len(xs),
    }


def fmt(r):
    return (
        f"median {r['median_us']:9.3f} us | p10 {r['p10_us']:9.3f} | "
        f"p90 {r['p90_us']:9.3f} | min {r['min_us']:9.3f} | n={r['n']}"
    )


def size_of(path) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def bench_open_cost():
    print("\n=== 1. OPEN COST vs STORE SIZE (open_ro; handle held afterwards) ===")
    stores = []
    for n in (0, 1_000, 10_000, 100_000):
        p = os.path.join(BENCH_ROOT, f"store-{n}")
        build_store(p, n)
        stores.append((n, p, size_of(p)))

    mem_dl = os.path.expanduser("~/.config/hax/memory.dl")
    if os.path.isdir(mem_dl):
        stores.append(("memory.dl", mem_dl, size_of(mem_dl)))

    for n, p, sz in stores:
        db = dlb.Db.open_ro(p)  # warm the page cache once
        db.close()
        reps = 3 if sz > 100_000_000 else 5
        ts = []
        for _ in range(reps):
            t0 = now_us()
            db = dlb.Db.open_ro(p)
            ts.append(now_us() - t0)
            db.close()
        print(
            f"  edges={n!s:>10}  store={sz/1e6:9.2f} MB  open_ro "
            f"min={min(ts)/1000:8.2f} ms  max={max(ts)/1000:8.2f} ms"
        )

    # empty-dir RW open
    p = rmk(os.path.join(BENCH_ROOT, "empty"))
    t0 = now_us()
    db = dlb.Db.open(p)
    t_open = now_us() - t0
    db.close()
    print(f"  (rw open, empty dir: {t_open/1000:.3f} ms)")


def bench_percall():
    print("\n=== 2. PER-CALL LATENCY (handle held open) ===")
    n_edges = 1_000 if QUICK else 10_000
    p = os.path.join(BENCH_ROOT, "percall")
    build_store(p, n_edges)
    db = dlb.Db.open_ro(p)
    arity2 = (c_uint := __import__("ctypes").c_uint32)
    hit = (5, 6)
    miss = (5_000_000, 6_000_000)

    iters = 20_000

    print(f"  store: {n_edges} edge facts, {size_of(p)/1e6:.2f} MB")

    r = timing_loop(lambda: db.lookup("edge", hit), iters)
    print(f"  lookup (hit)              {fmt(r)}")
    r = timing_loop(lambda: db.lookup("edge", miss), iters)
    print(f"  lookup (miss)             {fmt(r)}")

    got = []

    def cb(t):
        got.append(t)

    r = timing_loop(lambda: got.clear() or db.prefix("edge", (5,), cb), iters // 2)
    print(f"  prefix (1 row, w/ cb)     {fmt(r)}")

    r = timing_loop(lambda: db.intern_find("entity-never-exists"), iters)
    print(f"  intern_find (miss)        {fmt(r)}")
    r = timing_loop(lambda: db.count("edge"), iters)
    print(f"  count (O(1) memoized)     {fmt(r)}")
    r = timing_loop(lambda: db.rank("edge", (500, 501)), iters)
    print(f"  rank                      {fmt(r)}")
    r = timing_loop(lambda: db.select("edge", 500), iters)
    print(f"  select (k=500)            {fmt(r)}")

    # iterator: open+scan+close of a bound slice (1 row)
    def iter_slice():
        with db.iter("edge", (7,)) as it:
            for _ in it:
                pass

    r = timing_loop(iter_slice, iters // 2)
    print(f"  iter open+scan+close(1row){fmt(r)}")

    # full-scan per-tuple cost
    def iter_full():
        k = 0
        with db.iter("edge") as it:
            for _ in it:
                k += 1
        return k

    r = timing_loop(iter_full, 20)
    per_tuple = r["median_us"] / n_edges
    print(
        f"  iter full scan {n_edges} rows  median {r['median_us']:9.1f} us"
        f"  => {per_tuple:7.3f} us/tuple | n={r['n']}"
    )
    db.close()


TC = "tc(X,Y) :- edge(X,Y).\ntc(X,Y) :- edge(X,Z), tc(Z,Y)."


def bench_recursive_query():
    print("\n=== 3. SMALL RECURSIVE QUERY (the expensive case) ===")
    for n_edges in (40, 100, 1_000, 5_000):
        p = os.path.join(BENCH_ROOT, f"rq-{n_edges}")
        build_store(p, n_edges)
        db = dlb.Db.open_ro(p)
        # chain 1->2->...->n+1 => n*(n+1)/2 tc tuples
        n_tc = n_edges * (n_edges + 1) // 2

        r = timing_loop(
            lambda: db.query_rules_ro(TC, "tc"), 5 if n_edges > 1000 else 20,
            n_warmup=1,
        )
        print(
            f"  chain={n_edges:6d} (tc tuples={n_tc:8d})  "
            f"query_rules_ro median {r['median_us']/1000:9.3f} ms"
        )
        db.close()

    # compiled+materialized path on a fresh RW store (writer-side view)
    for n_edges in (40, 1000):
        p = os.path.join(BENCH_ROOT, f"rqc-{n_edges}")
        build_store(p, n_edges)
        db = dlb.Db.open(p)
        t0 = now_us()
        db.load_rules(TC)
        db.compile_rules()
        t_compile = now_us() - t0
        n_tc = n_edges * (n_edges + 1) // 2
        print(
            f"  chain={n_edges:6d} one-time load_rules+compile {t_compile/1000:9.3f} ms"
        )
        r = timing_loop(lambda: db.query("tc"), 20, n_warmup=2)
        print(
            f"  chain={n_edges:6d} compiled dl_query  median {r['median_us']/1000:9.3f} ms"
            f"  (re-evaluation, tc tuples={n_tc})"
        )
        r = timing_loop(lambda: db.query_bound("tc", (1,)), 20, n_warmup=2)
        print(
            f"  chain={n_edges:6d} query_bound(1,) median {r['median_us']/1000:9.3f} ms"
        )
        r = timing_loop(lambda: db.query_magic("tc", (1,)), 20, n_warmup=2)
        print(
            f"  chain={n_edges:6d} query_magic(1,) median {r['median_us']/1000:9.3f} ms"
            f"  (scoped fixpoint: {n_edges} tuples)"
        )
        db.close()


def bench_locked():
    print("\n=== 4. DL_E_LOCKED BEHAVIOUR ===")
    d = rmk(os.path.join(BENCH_ROOT, "locked"))
    db = dlb.Db.open(d)
    db.declare_relation("edge", 2)
    db.add_fact("edge", (1, 2))

    # same-process second writer: fcntl cannot self-conflict, so the BINDING's
    # registry refuses it (the C library alone would allow it — measured).
    t0 = now_us()
    try:
        dlb.Db.open(d)
        print("  UNEXPECTED: second same-process writer opened")
    except dlb.DlLockedError:
        dt = now_us() - t0
        print(f"  same-process 2nd writer -> DlLockedError (binding registry) "
              f"after {dt:.1f} us")

    # cross-process writer + reader
    code = (
        "import dlb,sys,time\n"
        f"try:\n"
        f"    ro=dlb.Db.open_ro({d!r})\n"
        "except dlb.DlLockedError:\n"
        "    print('CROSS-PROCESS LOCKED'); sys.exit(0)\n"
        "print('UNEXPECTED OPEN'); sys.exit(1)\n"
    )
    env = {**os.environ, "PYTHONPATH": os.path.join(
        os.path.dirname(__file__), "..")}
    r = subprocess.run([sys.executable, "-c", code], capture_output=True,
                       text=True, env=env)
    print(f"  cross-process open_ro while writer holds: rc={r.returncode} "
          f"{r.stdout.strip()!r}")

    db.close()
    # lock is released after close
    ro = dlb.Db.open_ro(d)
    ro.close()
    print("  after writer close: open_ro succeeds (lock released)")


def bench_writer_commit_visibility():
    print("\n=== 5. WRITER-COMMIT VISIBILITY (no concurrent snapshot exists) ===")
    d = rmk(os.path.join(BENCH_ROOT, "visibility"))
    db = dlb.Db.open(d)
    db.declare_relation("edge", 2)
    db.add_fact("edge", (1, 2))
    db.close()
    ro = dlb.Db.open_ro(d)
    print(f"  RO handle opened after writer close sees commit: "
          f"lookup(edge,(1,2))={ro.lookup('edge',(1,2))}")
    ro.close()
    print("  (A writer and a reader can NEVER be open simultaneously — the")
    print("   LOCK fcntl excludes them in both directions; so 'snapshot")
    print("   consistency across a concurrent writer commit' is structurally")
    print("   vacuous. Consistency point = open time; WAL replayed at open.)")

    # durability/crash model: child adds facts and _exits without close
    code = (
        "import dlb, os\n"
        f"db=dlb.Db.open({d!r})\n"
        "db.add_fact('edge',(7,8))\n"
        "os._exit(0)\n"
    )
    env = {**os.environ, "PYTHONPATH": os.path.join(
        os.path.dirname(__file__), "..")}
    subprocess.run([sys.executable, "-c", code], capture_output=True,
                   text=True, env=env)
    ro = dlb.Db.open_ro(d)
    print(f"  RO after crashed writer (no close): sees WAL'd fact "
          f"(7,8)={ro.lookup('edge',(7,8))} count={ro.count('edge')}")
    ro.close()


def bench_threads():
    print("\n=== 6. THREAD SAFETY (one handle, N python threads) ===")
    p = os.path.join(BENCH_ROOT, "percall")
    if not os.path.isdir(p):
        build_store(p, 10_000)
    db = dlb.Db.open_ro(p)

    N_THREADS = 8
    ITERS = 5_000 if QUICK else 20_000
    errors: list = []
    counts = [0] * N_THREADS
    barrier = threading.Barrier(N_THREADS)
    expected = {(i, i + 1) for i in range(1, 10_001)}

    def worker(idx):
        try:
            barrier.wait()
            for k in range(ITERS):
                i = 1 + ((idx * ITERS + k) % 10_000)
                got = db.lookup("edge", (i, i + 1))
                if not got:
                    errors.append(f"missed ({i},{i+1})")
                    return
                counts[idx] += 1
        except Exception as e:  # noqa: BLE001
            errors.append(repr(e))

    t0 = now_us()
    ts = [threading.Thread(target=worker, args=(i,)) for i in range(N_THREADS)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    wall = now_us() - t0
    total = sum(counts)
    print(f"  {N_THREADS} threads x {ITERS} lookups (API, RLock-serialized): "
          f"{total} ok, {len(errors)} errors, wall {wall/1000:.1f} ms, "
          f"{total/(wall/1e6):,.0f} lookups/s aggregate")

    # unlocked RAW access: does the C library tolerate concurrent readers?
    # (each thread gets its OWN cols buffer — a shared buffer is a data race
    # in the bench, not the library)
    raw = db._raw
    h = db._h
    import ctypes
    from ctypes import c_uint8

    def raw_worker(idx):
        try:
            mycols = (ctypes.c_uint32 * 2)()
            barrier2.wait()
            for k in range(ITERS):
                i = 1 + ((idx * ITERS + k) % 10_000)
                mycols[0], mycols[1] = i, i + 1
                r = raw.dl_lookup(h, b"edge", mycols, c_uint8(2))
                if r != 1:
                    errors.append(f"raw miss ({i},{i+1}) rc={r}")
                    return
                counts[idx] += 1
        except Exception as e:  # noqa: BLE001
            errors.append(repr(e))

    N_THREADS2 = 8
    barrier2 = threading.Barrier(N_THREADS2)
    counts = [0] * N_THREADS2
    errors.clear()
    t0 = now_us()
    ts = [threading.Thread(target=raw_worker, args=(i,)) for i in range(N_THREADS2)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    wall = now_us() - t0
    total = sum(counts)
    verdict = "no errors observed" if not errors else f"{len(errors)} ERRORS"
    print(f"  {N_THREADS2} threads x {ITERS} RAW dl_lookup (no lock): "
          f"{total} ok, {verdict}, wall {wall/1000:.1f} ms, "
          f"{total/(wall/1e6):,.0f} lookups/s aggregate")
    if errors:
        for e in errors[:3]:
            print(f"    first error: {e}")
    print("  NOTE: concurrent raw reads showed no errors, but the engine docs")
    print("  do not promise thread safety; the API layer serializes with an")
    print("  RLock (first measurement). Single-threaded use is the contract.")
    db.close()


def bench_canonical_so():
    print("\n=== 7. CANONICAL .so + ABI ===")
    raw = load_library()
    print(f"  loaded: {raw.path}")
    print(f"  dafsa_abi_version(): {raw.abi} (binding SUPPORTED_ABI="
          f"{dlb.SUPPORTED_ABI})")
    others = [
        "~/fixpoint-linux/datalog-dafsa/libdatalog.so",
        "~/.config/hax/bin/libdatalog.so",
    ]
    for o in others:
        p = os.path.expanduser(o)
        if os.path.exists(p):
            try:
                r2 = load_library(p)
                print(f"  alt {o}: loads, abi={r2.abi}")
            except Exception as e:  # noqa: BLE001
                print(f"  alt {o}: LOAD FAILED: {e}")


def main():
    os.makedirs(BENCH_ROOT, exist_ok=True)
    print(f"dlb envelope bench  (python {sys.version.split()[0]}, "
          f"pid={os.getpid()})")
    bench_open_cost()
    bench_percall()
    bench_recursive_query()
    bench_locked()
    bench_writer_commit_visibility()
    bench_threads()
    bench_canonical_so()
    print("\ndone.")


if __name__ == "__main__":
    main()
