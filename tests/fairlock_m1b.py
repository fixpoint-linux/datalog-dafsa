#!/usr/bin/env python3
"""fairlock_m1b.py — Phase-0 M1b shape, re-built as the fairness gate.

The measured shape (Phase 0, M1b): the real agent workload is a FRESH PROCESS
PER CALL — `open2 -> declare own relation -> 3 facts with NEW strings ->
publish every 5th -> close` (tests/fairlock_batch.c) — run from K concurrent
slots that start staggered.  What Phase 0 measured with the writer-exclusive
acquire in FAIL-FAST mode: only 31%/14%/8% of opens at K=4/8/16 succeeded
first try, p95 open waits 1.08/3.35/7.78 s, maxima 3.14/9.12/11.1 s, retries
BIMODAL (attempt 1, or >10), throughput flat at ~1.0x sequential.  The lock
cost LATENCY through unfairness, not capacity.

Arms (same harness binary, same machine, same load — the only difference is
the lock primitive the engine picks):

  before  DL_LOCK_FAIL_FAST=1 — F_SETLK + immediate DL_E_LOCKED, caller
          retries in user space with a 1-3 ms jittered sleep.  This is
          byte-for-byte the pre-change code path (the old site called
          lockRange(..., wait=false)), so it is a faithful before arm and
          needs no second engine build.
  after   (default) — F_SETLKW: the waiter sleeps on the kernel's per-inode
          lock queue and is woken when the holder releases.

Reported per K: % of opens refused, first-try %, open-wait and completion
p50/p95/max, wall vs the sequential reference, and the RETRY DISTRIBUTION
(the bimodality is the thing being fixed).

Also here (correctness, on the same harness):
  --mode nooverlap  every concurrent batch reports the interval in which it
                    holds byte 0 ([open_ts, close_ts)); assert NO TWO
                    intervals overlap.  This is strictly stronger than
                    "mutation windows never overlap" — every mutation window
                    is a subset of its holder's handle interval.
  --mode crash      SIGKILL a writer mid-batch, then assert another batch
                    opens and completes inside a deadline (a crashed writer
                    cannot leave a stale fcntl lock).
  --mode reader     writers + readers concurrently: every reader must open
                    and read (a live writer excludes writers, not readers),
                    and no reader may hang.

Usage (from the repo root, after `zig build -Drelease -p zig-out ...`):
  python3 tests/fairlock_m1b.py --mode measure --arms before,after
  python3 tests/fairlock_m1b.py --mode nooverlap --k 16
  python3 tests/fairlock_m1b.py --mode crash
  python3 tests/fairlock_m1b.py --mode reader
"""
import argparse
import fcntl
import json
import os
import platform
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS_SRC = os.path.join(ROOT, "tests", "fairlock_batch.c")
HARNESS_BIN = os.path.join(ROOT, "build-tmp", "fairlock_batch")
LIBDIR = os.path.join(ROOT, "zig-out", "lib")
DL = os.path.join(ROOT, "zig-out", "bin", "dl")

BUCKETS = [(1, 1), (2, 2), (3, 5), (6, 10), (11, 20), (21, 10**9)]


def build_harness():
    os.makedirs(os.path.dirname(HARNESS_BIN), exist_ok=True)
    cmd = [
        "gcc", "-O2", "-Wall", "-Wextra", "-std=c11", "-D_POSIX_C_SOURCE=200809L",
        "-I" + os.path.join(ROOT, "src"), "-I" + os.path.join(ROOT, "vendor", "dafsa"),
        HARNESS_SRC, "-L", LIBDIR, "-ldatalog", "-Wl,-rpath," + LIBDIR,
        "-o", HARNESS_BIN,
    ]
    subprocess.run(cmd, check=True)
    return HARNESS_BIN


def fresh_db(tag):
    d = tempfile.mkdtemp(prefix="fairlock-%s-" % tag)
    return d


def build_seed_db(nfacts, nrels):
    """A store with CONTENT, so a batch's publish costs what a real one costs
    (a publish on an empty store is ~0.2 ms and would make the lock hold
    unrepresentatively short).  Facts go in through the CLI's CSV loader, the
    same path a real ingest uses.  Returns the directory; the driver copies it
    per measurement so every arm starts from identical state."""
    d = tempfile.mkdtemp(prefix="fairlock-seed-")
    per = max(1, nfacts // nrels)
    for r in range(nrels):
        subprocess.run([HARNESS_BIN, "--seed", d, "seed%d" % r, str(per)],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)
    return d


def copy_db(base, tag):
    d = fresh_db(tag)
    subprocess.run(["cp", "-a", base + "/.", d], check=True)
    return d


BASE_DB = None


def db_for(tag):
    """Every measurement starts from the SAME store state."""
    return copy_db(BASE_DB, tag) if BASE_DB else fresh_db(tag)


def child_env(before, extra=None):
    env = dict(os.environ)
    env.pop("DL_LOCK_FAIL_FAST", None)
    env.pop("DL_LOCK_READ_WAIT_MS", None)
    if before:
        env["DL_LOCK_FAIL_FAST"] = "1"
    if extra:
        env.update(extra)
    return env


def parse_line(out):
    rec = {}
    for tok in out.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                rec[k] = float(v) if "." in v else int(v)
            except ValueError:
                rec[k] = v
    return rec


def run_batch(db, slot, batch, before, timeout=120, extra_env=None, reader=False,
              hold_ms=0.0, no_retry=False):
    args = [HARNESS_BIN, db, str(slot), str(batch)]
    if reader:
        args.append("--reader")
    if no_retry:
        args.append("--no-retry")
    if hold_ms:
        args += ["--hold-ms", str(hold_ms)]
    t0 = time.monotonic()
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout,
                       env=child_env(before, extra_env))
    wall_ms = (time.monotonic() - t0) * 1000.0
    if p.returncode != 0:
        raise RuntimeError("harness rc=%d stderr=%s" % (p.returncode, p.stderr.strip()))
    rec = parse_line(p.stdout)
    rec["proc_wall_ms"] = wall_ms
    return rec


def pct(vals, q):
    if not vals:
        return 0.0
    s = sorted(vals)
    if len(s) == 1:
        return s[0]
    idx = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
    return s[idx]


def distribution(records):
    counts = [0] * len(BUCKETS)
    for r in records:
        a = r["attempts"]
        for i, (lo, hi) in enumerate(BUCKETS):
            if lo <= a <= hi:
                counts[i] += 1
                break
    return counts


def sequential_reference(nbatches, before, db):
    recs = []
    t0 = time.monotonic()
    for i in range(nbatches):
        recs.append(run_batch(db, 0, i, before))
    wall = time.monotonic() - t0
    return recs, wall


def concurrent_run(nbatches, k, before, db, stagger_ms=3.0, reader=False,
                   hold_ms=0.0):
    recs = [None] * nbatches
    errs = []
    per_slot = (nbatches + k - 1) // k

    def worker(slot):
        try:
            if slot > 0:
                time.sleep(slot * stagger_ms / 1000.0)
            mine = [i for i in range(nbatches) if i % k == slot]
            for b in mine:
                recs[b] = run_batch(db, slot, b, before, reader=reader,
                                    hold_ms=hold_ms)
        except Exception as e:  # noqa: BLE001 - reported, not swallowed
            errs.append("slot %d: %s" % (slot, e))

    threads = [threading.Thread(target=worker, args=(s,)) for s in range(k)]
    t0 = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.monotonic() - t0
    if errs:
        raise RuntimeError("; ".join(errs))
    assert all(r is not None for r in recs), "a slot produced no record"
    return recs, wall, per_slot


def overlap_check(recs):
    """No two batches may hold the writer-exclusive byte 0 at once.  Each
    batch reports [open_ts, close_ts) = the interval in which it holds it."""
    iv = sorted(((r["open_ts"], r["close_ts"], r["pid"], r["slot"]) for r in recs))
    viol = []
    maxconc = 0
    for i in range(len(iv)):
        conc = 1
        for j in range(i + 1, len(iv)):
            if iv[j][0] < iv[i][1]:
                conc += 1
                viol.append((iv[i], iv[j]))
        maxconc = max(maxconc, conc)
    return maxconc, viol


def machine_load():
    load = open("/proc/loadavg").read().strip()
    cpu = ""
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    return {
        "host": platform.node(),
        "cpus": os.cpu_count(),
        "cpu": cpu,
        "loadavg_start": load,
        "kernel": platform.release(),
    }


def mode_measure(args):
    load = machine_load()
    print("machine: %s | %s x%d | %s | loadavg %s"
          % (load["host"], load["cpu"], load["cpus"], load["kernel"],
             load["loadavg_start"]))
    print("harness: %s (fresh process per batch; %d batches per K)"
          % (os.path.relpath(HARNESS_BIN, ROOT), args.batches))
    print()

    labels = {
        "before": "before (DL_LOCK_FAIL_FAST=1: F_SETLK + user-space retry)",
        "after": "after  (default: F_SETLKW kernel queue)",
        "baseline": "baseline binary built from 110b5f9 (pre-change engine, "
                    "no env)",
    }
    for arm in args.arms.split(","):
        arm = arm.strip()
        before = arm == "before"
        label = labels[arm]
        seq_db = db_for("seq-" + arm)
        seq_recs, seq_wall = sequential_reference(args.batches, before, seq_db)
        seq_sum = sum(r["total_ms"] for r in seq_recs) / 1000.0
        print("=== arm %s ===" % label)
        print("sequential reference: %d batches, wall %.2fs (sum of batch totals %.2fs)"
              % (args.batches, seq_wall, seq_sum))
        print()
        hdr = ("  K | refused%% | firsttry%% | open p50/p95/max ms | "
               "total p50/p95/max ms | wall s | x seq | retries 1/2/3-5/6-10/11-20/>20")
        print(hdr)
        for k in [int(x) for x in args.ks.split(",")]:
            db = db_for("k%d-%s" % (k, arm))
            recs, wall, per_slot = concurrent_run(args.batches, k, before, db)
            n = len(recs)
            refused = sum(r["refused"] for r in recs)
            refused_batches = sum(1 for r in recs if r["refused"] > 0)
            firsttry = sum(1 for r in recs if r["attempts"] == 1)
            om = [r["open_ms"] for r in recs]
            tm = [r["total_ms"] for r in recs]
            dist = distribution(recs)
            maxconc, viol = overlap_check(recs)
            print("  %2d | %7.1f | %8.1f | %7.2f/%7.2f/%8.2f | "
                  "%7.1f/%7.1f/%8.1f | %6.2f | %5.2f | %s"
                  % (k, 100.0 * refused_batches / n, 100.0 * firsttry / n,
                     pct(om, 0.50), pct(om, 0.95), max(om),
                     pct(tm, 0.50), pct(tm, 0.95), max(tm),
                     wall, wall / seq_wall, "/".join(str(c) for c in dist)))
            if refused:
                print("       (DL_E_LOCKED returns seen: %d over %d batches)"
                      % (refused, n))
            if viol or maxconc > 1:
                print("       !! LOCK EXCLUSIVITY VIOLATION: max concurrent "
                      "holders=%d, overlapping pairs=%d" % (maxconc, len(viol)))
        print()
    load_end = open("/proc/loadavg").read().strip()
    print("loadavg at end: %s" % load_end)


def mode_nooverlap(args):
    """N writers, no overlap of the byte-0 hold intervals.

    --positive-control runs the SAME interval check over READER batches
    instead: readers hold no exclusive byte, so their intervals legitimately
    overlap — a checker that cannot report overlap is not a checker, so this
    proves the instrument has teeth."""
    k = args.k
    db = db_for("nooverlap")
    reader = args.positive_control
    if reader:
        run_batch(db, 0, 0, False)  # create the db metadata a reader needs
    recs, wall, per_slot = concurrent_run(args.batches, k, False, db,
                                          reader=reader,
                                          hold_ms=args.hold_ms)
    maxconc, viol = overlap_check(recs)
    label = "readers (positive control)" if reader else "writers"
    print("nooverlap[%s]: K=%d, %d batches, wall %.2fs, max concurrent "
          "holders = %d" % (label, k, len(recs), wall, maxconc))
    if viol:
        for a, b in viol[:5]:
            print("  OVERLAP pid=%s slot=%s [%d,%d) vs pid=%s slot=%s [%d,%d)"
                  % (a[2], a[3], a[0], a[1], b[2], b[3], b[0], b[1]))
        print("overlap detected in %d pairs" % len(viol))
    if reader:
        ok = bool(viol)
        print("%s: the checker DOES report overlap when overlap exists"
              % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    if viol or maxconc > 1:
        print("FAIL: two writers held byte 0 at the same time")
        return 1
    print("PASS: no two writers held byte 0 at the same time")
    return 0


def wait_locked(proc, timeout=30.0):
    """Block until the child reports it holds the handle (byte 0)."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        line = proc.stderr.readline()
        if not line:
            if proc.poll() is not None:
                return False
            continue
        if line.startswith("LOCKED"):
            return True
    return False


def mode_crash(args):
    """SIGKILL a writer while it HOLDS byte 0; another writer must proceed.
    fcntl locks are dropped by the kernel when the holder dies, so an
    unbounded wait cannot be wedged by a crashed writer."""
    db = db_for("crash")
    run_batch(db, 0, 0, False)  # create the db (LOCK, rels.txt)
    victim = subprocess.Popen(
        [HARNESS_BIN, db, "1", "5", "--hold-ms", "60000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if not wait_locked(victim):
        victim.kill()
        print("FAIL: the victim never reported holding the lock")
        return 1
    held = "yes"
    victim.send_signal(signal.SIGKILL)
    victim.wait(timeout=30)
    print("crash: killed pid %d while it held byte 0 (%s), rc=%d"
          % (victim.pid, held, victim.returncode))
    t0 = time.monotonic()
    rec = run_batch(db, 2, 10, False, timeout=args.deadline_s)
    waited = time.monotonic() - t0
    ok = rec["work_ok"] == 1 and waited < args.deadline_s
    print("crash: next writer opened+finished in %.2fs (deadline %.1fs), "
          "work_ok=%d -> %s"
          % (waited, args.deadline_s, rec["work_ok"], "PASS" if ok else "FAIL"))
    return 0 if ok else 1


def mode_reader(args):
    """Readers alongside a live writer: they open+read, and never hang."""
    db = db_for("reader")
    run_batch(db, 0, 0, False)
    holder = subprocess.Popen(
        [HARNESS_BIN, db, "1", "5", "--hold-ms", str(args.hold_ms)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if not wait_locked(holder):
        holder.kill()
        print("FAIL: the writer never reported holding the lock")
        return 1
    results = []
    errs = []

    def rd(i):
        try:
            results.append(run_batch(db, 9, i, False, timeout=args.deadline_s,
                                     reader=True))
        except Exception as e:  # noqa: BLE001
            errs.append("reader %d: %s" % (i, e))

    ts = [threading.Thread(target=rd, args=(i,)) for i in range(args.readers)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    holder.wait(timeout=120)
    bad = [r for r in results if r["work_ok"] != 1]
    print("reader: %d readers while a writer held byte 0; %d opened, "
          "%d refused (DL_E_LOCKED), max open %.2fms"
          % (args.readers, len(results), sum(1 for r in results if r["refused"]),
             max([r["open_ms"] for r in results] or [0.0])))
    if errs or bad:
        print("FAIL: %s" % (errs or bad))
        return 1
    print("PASS: readers coexisted with the live writer and none hung")
    return 0


def mode_readbound(args):
    """A reader must NOT hang behind a writer that holds the mutation window
    (byte 1) — its wait is bounded and it fails with DL_E_LOCKED.  The writer
    here is simulated from outside (an fcntl F_WRLCK on byte 1 of <dir>/LOCK,
    exactly what a live writer holds during a mutation window), because the
    engine's own window is too short to observe a deadline."""
    db = db_for("readbound")
    run_batch(db, 0, 0, False)  # create the metadata a reader needs
    lf = open(os.path.join(db, "LOCK"), "r+")
    fcntl.lockf(lf.fileno(), fcntl.LOCK_EX, 1, 1)  # byte 1, exclusive
    try:
        t0 = time.monotonic()
        p = subprocess.run([HARNESS_BIN, db, "9", "0", "--reader", "--no-retry"],
                           capture_output=True, text=True, timeout=60,
                           env=child_env(False, {"DL_LOCK_READ_WAIT_MS": "300"}))
        waited = time.monotonic() - t0
    finally:
        fcntl.lockf(lf.fileno(), fcntl.LOCK_UN, 1, 1)
        lf.close()
    refused = p.returncode == 2 and "err=1" in p.stderr
    bounded = 0.25 <= waited <= 5.0
    print("readbound: reader behind a held mutation window returned in %.2fs "
          "with %s (DL_LOCK_READ_WAIT_MS=300)"
          % (waited, "DL_E_LOCKED" if refused else "rc=%d/%s"
             % (p.returncode, p.stderr.strip())))
    if not (refused and bounded):
        print("FAIL: a reader must fail fast-ish (bounded), not hang")
        return 1
    # control: with the window free the same reader succeeds
    rec = run_batch(db, 9, 1, False, reader=True)
    ok = rec["work_ok"] == 1 and rec["refused"] == 0
    print("readbound control: with the window free the reader opened in %.2fms "
          "-> %s" % (rec["open_ms"], "PASS" if ok else "FAIL"))
    return 0 if ok else 1


def mode_failfast(args):
    """Both writer-acquisition modes, end to end, against a live holder:
    the DEFAULT queues (and succeeds), DL_LOCK_FAIL_FAST=1 is refused at
    once with DL_E_LOCKED."""
    db = db_for("failfast")
    run_batch(db, 0, 0, False)

    holder = subprocess.Popen(
        [HARNESS_BIN, db, "1", "5", "--hold-ms", str(args.hold_ms)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    wait_locked(holder)
    t0 = time.monotonic()
    rec = run_batch(db, 2, 7, False, timeout=args.deadline_s)
    queued = time.monotonic() - t0
    holder.wait(timeout=120)
    queue_ok = rec["work_ok"] == 1 and rec["attempts"] == 1 and queued > 0.5

    holder = subprocess.Popen(
        [HARNESS_BIN, db, "1", "5", "--hold-ms", str(args.hold_ms)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    wait_locked(holder)
    t0 = time.monotonic()
    p = subprocess.run([HARNESS_BIN, db, "3", "8", "--no-retry"],
                       capture_output=True, text=True, timeout=args.deadline_s,
                       env=child_env(True))
    refused = time.monotonic() - t0
    holder.wait(timeout=120)
    ff_ok = p.returncode == 2 and "err=1" in p.stderr and refused < 0.5

    print("failfast: DEFAULT writer queued %.2fs behind the holder and then "
          "succeeded -> %s" % (queued, "PASS" if queue_ok else "FAIL"))
    print("failfast: DL_LOCK_FAIL_FAST=1 writer refused in %.3fs with "
          "DL_E_LOCKED -> %s" % (refused, "PASS" if ff_ok else "FAIL"))
    return 0 if (queue_ok and ff_ok) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="measure",
                    choices=["measure", "nooverlap", "crash", "reader",
                             "readbound", "failfast"])
    ap.add_argument("--arms", default="before,after")
    ap.add_argument("--ks", default="2,4,8,16")
    ap.add_argument("--k", type=int, default=16)
    ap.add_argument("--batches", type=int, default=80)
    ap.add_argument("--readers", type=int, default=4)
    ap.add_argument("--kill-after-ms", type=float, default=15.0)
    ap.add_argument("--deadline-s", type=float, default=10.0)
    ap.add_argument("--hold-ms", type=float, default=3000.0)
    ap.add_argument("--positive-control", action="store_true",
                    help="nooverlap: run the interval check over READER "
                         "batches (which legitimately overlap) to prove the "
                         "checker can detect overlap at all")
    ap.add_argument("--seed-facts", type=int, default=100000,
                    help="facts pre-loaded into the store so a publish costs "
                         "what a real one costs (0 = empty store)")
    ap.add_argument("--seed-rels", type=int, default=4)
    ap.add_argument("--seed-dir", default=None,
                    help="reuse an already-seeded store instead of building "
                         "one (the seed costs ~60 s)")
    ap.add_argument("--harness", default=None,
                    help="harness binary to drive (default build-tmp/"
                         "fairlock_batch); point it at a pre-change build to "
                         "measure the real old engine")
    args = ap.parse_args()

    if not os.path.exists(os.path.join(LIBDIR, "libdatalog.so")):
        print("libdatalog.so missing — run: zig build -Drelease -p zig-out "
              "--build-file zig/build.zig", file=sys.stderr)
        return 1
    global HARNESS_BIN, BASE_DB
    if args.harness:
        HARNESS_BIN = os.path.abspath(args.harness)
    else:
        build_harness()
    if args.seed_dir:
        BASE_DB = args.seed_dir
        print("reusing seeded store: %s" % BASE_DB)
    elif args.seed_facts > 0:
        t0 = time.monotonic()
        BASE_DB = build_seed_db(args.seed_facts, args.seed_rels)
        print("seeded store: %d facts over %d relations (%s, %.1fs)"
              % (args.seed_facts, args.seed_rels, BASE_DB,
                 time.monotonic() - t0))
        cal_db = copy_db(BASE_DB, "cal")
        cal = [run_batch(cal_db, 0, i, False) for i in range(5)]
        print("work unit (5 sequential batches, uncontended): open %.2fms, "
              "publish %.2fms, in-process total %.2fms"
              % (sum(c["open_ms"] for c in cal) / 5,
                 sum(c["publish_ms"] for c in cal) / 5,
                 sum(c["total_ms"] for c in cal) / 5))
    if args.mode == "measure":
        mode_measure(args)
        return 0
    if args.mode == "nooverlap":
        return mode_nooverlap(args)
    if args.mode == "crash":
        return mode_crash(args)
    if args.mode == "readbound":
        return mode_readbound(args)
    if args.mode == "failfast":
        return mode_failfast(args)
    return mode_reader(args)


if __name__ == "__main__":
    sys.exit(main())
