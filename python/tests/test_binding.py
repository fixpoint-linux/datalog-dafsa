"""Correctness tests for the dlb binding. Run: python3 tests/test_binding.py"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import dlb
from dlb import (
    DlConflictError,
    DlError,
    DlLockedError,
)

TEST_DB = os.path.join(tempfile.gettempdir(), "dlb-test-db")


def fresh_dir():
    d = TEST_DB + "-rw"
    shutil.rmtree(d, ignore_errors=True)
    return d


class TestLifecycle(unittest.TestCase):
    def test_open_close_reopen(self):
        d = fresh_dir()
        db = dlb.Db.open(d)
        db.declare_relation("edge", 2)
        db.add_fact("edge", (1, 2))
        db.close()
        db2 = dlb.Db.open(d)
        self.assertTrue(db2.lookup("edge", (1, 2)))
        db2.close()

    def test_open_ro_needs_existing_db(self):
        d = TEST_DB + "-nonexistent"
        shutil.rmtree(d, ignore_errors=True)
        with self.assertRaises(DlError):
            dlb.Db.open_ro(d)

    def test_open_ro_sees_committed_state(self):
        d = fresh_dir()
        db = dlb.Db.open(d)
        db.declare_relation("edge", 2)
        db.add_fact("edge", (1, 2))
        db.close()
        ro = dlb.Db.open_ro(d)
        self.assertTrue(ro.lookup("edge", (1, 2)))
        self.assertEqual(ro.count("edge"), 1)
        ro.close()

    def test_close_is_idempotent_and_guards_use(self):
        db = dlb.Db.open(fresh_dir())
        db.close()
        db.close()
        with self.assertRaises(DlError):
            db.lookup("edge", (1, 2))

    def test_context_manager_closes(self):
        with dlb.Db.open(fresh_dir()) as db:
            db.declare_relation("edge", 2)
        with self.assertRaises(DlError):
            db.lookup("edge", (1, 2))

    def test_abi_gate(self):
        self.assertEqual(dlb.abi_version(), dlb.SUPPORTED_ABI)


class TestFactsAndReads(unittest.TestCase):
    def setUp(self):
        self.dir = fresh_dir()
        self.db = db = dlb.Db.open(self.dir)
        db.declare_relation("edge", 2)
        for t in [(1, 2), (1, 3), (2, 3), (2, 4), (3, 5)]:
            db.add_fact("edge", t)

    def tearDown(self):
        self.db.close()

    def test_add_fact_duplicate_semantics(self):
        self.assertTrue(self.db.add_fact("edge", (7, 8)))
        self.assertFalse(self.db.add_fact("edge", (7, 8)))  # duplicate -> False

    def test_lookup(self):
        self.assertTrue(self.db.lookup("edge", (1, 2)))
        self.assertFalse(self.db.lookup("edge", (2, 1)))
        self.assertFalse(self.db.lookup("edge", (99, 99)))

    def test_prefix_enumerates_and_counts(self):
        seen = []
        n = self.db.prefix("edge", (1,), seen.append)
        self.assertEqual(n, 2)
        self.assertEqual(sorted(seen), [(1, 2), (1, 3)])

    def test_prefix_early_stop(self):
        seen = []
        n = self.db.prefix(
            "edge", (2,), lambda t: (seen.append(t) or True)
        )
        self.assertEqual(n, 1)
        self.assertEqual(len(seen), 1)

    def test_iter_sorted_full(self):
        self.assertEqual(
            list(self.db.iter("edge")),
            [(1, 2), (1, 3), (2, 3), (2, 4), (3, 5)],
        )

    def test_iter_bound_and_seek(self):
        it = self.db.iter("edge", (2,))
        self.assertEqual(list(it), [(2, 3), (2, 4)])
        it.seek((1,))
        # seek resets enumeration; a fresh next pass must yield the (1,*) rows
        self.assertEqual(next(it), (1, 2))
        it.close()

    def test_iter_absent_prefix_is_valid_empty(self):
        with self.db.iter("edge", (42,)) as it:
            self.assertEqual(list(it), [])

    def test_count(self):
        self.assertEqual(self.db.count("edge"), 5)

    def test_rank_select_roundtrip(self):
        db = self.db
        for i in range(db.count("edge")):
            t = db.select("edge", i)
            self.assertEqual(db.rank("edge", t), i)

    def test_range_count(self):
        # half-open [lo, hi)
        lo, hi = (1, 0), (3, 0)
        self.assertEqual(self.db.range_count("edge", lo, hi), 4)

    def test_merge_join(self):
        db = self.db
        with db.iter("edge") as l, db.iter("edge", (2,)) as r:
            pairs = []
            # cross-product semantics: 2 left rows with col0==2 x 2 right rows
            n = db.merge_join(l, r, 1, lambda a, b: pairs.append((a, b)) or None)
            self.assertEqual(n, 4)
            self.assertEqual([p[0] for p in pairs], [(2, 3), (2, 3), (2, 4), (2, 4)])

    def test_delete_fact(self):
        self.assertTrue(self.db.delete_fact("edge", (1, 2)))
        self.assertFalse(self.db.lookup("edge", (1, 2)))
        self.assertFalse(self.db.delete_fact("edge", (1, 2)))  # absent -> False

    def test_interner_roundtrip(self):
        db = self.db
        s1 = db.intern("alpha")
        self.assertGreater(s1, 0)
        self.assertEqual(db.intern("alpha"), s1)  # idempotent
        self.assertEqual(db.intern_find("alpha"), s1)
        self.assertEqual(db.intern_find("never-interned"), 0)
        self.assertEqual(db.sym_of(s1), "alpha")
        self.assertIsNone(db.sym_of(1 << 30))

    def test_input_validation(self):
        with self.assertRaises(DlError):
            self.db.lookup("edge", ())          # too few cols
        with self.assertRaises(DlError):
            self.db.lookup("edge", (1,) * 9)    # too many cols
        with self.assertRaises(DlError):
            self.db.lookup("edge", (-1, 2))     # not a u32
        with self.assertRaises(DlError):
            self.db.lookup("edge", ("a", 2))    # not an int


class TestRulesAndQueries(unittest.TestCase):
    TC_RULES = (
        "tc(X,Y) :- edge(X,Y).\n"
        "tc(X,Y) :- edge(X,Z), tc(Z,Y)."
    )

    def setUp(self):
        self.dir = fresh_dir()
        self.db = db = dlb.Db.open(self.dir)
        db.declare_relation("edge", 2)
        # a 40-node chain 1->2->...->40 forces real recursion
        for i in range(1, 40):
            db.add_fact("edge", (i, i + 1))

    def tearDown(self):
        self.db.close()

    def test_query_rules_ro_recursive_tc(self):
        rows = self.db.query_rules_ro(self.TC_RULES, "tc")
        expected = {(i, j) for i in range(1, 41) for j in range(i + 1, 41)}
        self.assertEqual(set(rows), expected)
        self.assertEqual(len(rows), len(expected))  # no duplicates

    def test_query_rules_ro_leaves_db_untouched(self):
        before = self.db.count("edge")
        rels_before = sorted(os.listdir(self.dir))
        self.db.query_rules_ro(self.TC_RULES, "tc")
        self.assertEqual(self.db.count("edge"), before)
        self.assertEqual(sorted(os.listdir(self.dir)), rels_before)

    def test_query_rules_ro_bad_source_raises(self):
        with self.assertRaises(DlError):
            self.db.query_rules_ro("tc(X,Y :- edge(X,Y).", "tc")

    def test_compile_then_query(self):
        db = self.db
        db.load_rules(self.TC_RULES)
        db.compile_rules()
        rows = db.query("tc", collect=True)
        expected = {(i, j) for i in range(1, 41) for j in range(i + 1, 41)}
        self.assertEqual(set(rows), expected)
        # bound form: reachability from node 1
        rows1 = db.query_bound("tc", (1,), collect=True)
        self.assertEqual(sorted(rows1), [(1, j) for j in range(2, 41)])


class TestWritePath(unittest.TestCase):
    def setUp(self):
        self.dir = fresh_dir()
        self.db = dlb.Db.open(self.dir)

    def tearDown(self):
        self.db.close()

    def test_cas_revision_optimistic_concurrency(self):
        db = self.db
        self.assertEqual(db.rev_get("entity-a"), 0)
        db.cas_revision("entity-a", 0, 1)
        self.assertEqual(db.rev_get("entity-a"), 1)
        with self.assertRaises(DlConflictError):
            db.cas_revision("entity-a", 0, 2)  # stale expected
        self.assertEqual(db.rev_get("entity-a"), 1)  # unchanged
        db.cas_revision("entity-a", 1, 2)  # idempotent no-op value? no: 1->2
        self.assertEqual(db.rev_get("entity-a"), 2)

    def test_cas_idempotent_noop(self):
        db = self.db
        db.cas_revision("e", 3, 3)  # expected == new_value: no-op success
        self.assertEqual(db.rev_get("e"), 0)  # never written

    def test_transaction_commit_atomic(self):
        db = self.db
        db.declare_relation("edge", 2)
        with db.transaction() as tx:
            tx.add_fact("edge", (1, 2))
            tx.add_fact("edge", (2, 3))
            tx.cas("doc", 0, 1)
        self.assertTrue(db.lookup("edge", (1, 2)))
        self.assertTrue(db.lookup("edge", (2, 3)))
        self.assertEqual(db.rev_get("doc"), 1)

    def test_transaction_conflict_applies_nothing(self):
        db = self.db
        db.declare_relation("edge", 2)
        db.cas_revision("doc", 0, 1)  # current is now 1
        tx = db.transaction()
        tx.add_fact("edge", (9, 9))
        tx.cas("doc", 0, 2)  # stale expected -> whole txn must abort
        with self.assertRaises(DlConflictError):
            tx.commit()
        self.assertFalse(db.lookup("edge", (9, 9)))  # nothing applied
        self.assertEqual(db.rev_get("doc"), 1)

    def test_transaction_rollback(self):
        db = self.db
        db.declare_relation("edge", 2)
        tx = db.transaction()
        tx.add_fact("edge", (9, 9))
        tx.rollback()
        self.assertFalse(db.lookup("edge", (9, 9)))

    def test_transaction_rejects_nesting(self):
        db = self.db
        db.declare_relation("edge", 2)
        tx = db.transaction()
        with self.assertRaises(DlError):
            db.transaction()
        tx.rollback()

    def test_transaction_exception_rolls_back(self):
        db = self.db
        db.declare_relation("edge", 2)
        try:
            with db.transaction() as tx:
                tx.add_fact("edge", (1, 2))
                raise RuntimeError("boom")
        except RuntimeError:
            pass
        self.assertFalse(db.lookup("edge", (1, 2)))

    def test_ro_handle_rejects_writes(self):
        db = self.db
        db.declare_relation("edge", 2)
        db.add_fact("edge", (1, 2))
        db.close()
        ro = dlb.Db.open_ro(self.dir)
        try:
            with self.assertRaises(DlError):
                ro.add_fact("edge", (3, 4))
            with self.assertRaises(DlError):
                ro.declare_relation("other", 1)
            with self.assertRaises(DlError):
                ro.transaction()
            # reads still work
            self.assertTrue(ro.lookup("edge", (1, 2)))
        finally:
            ro.close()


class TestLocking(unittest.TestCase):
    """Cross-process single-writer semantics via subprocesses (the model the
    architecture uses: CEN is the sole writer, readers are other processes)."""

    def _child(self, code: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-c", code],
            capture_output=True, text=True, timeout=120,
            env={**os.environ, "PYTHONPATH": os.path.join(
                os.path.dirname(__file__), "..")},
        )

    def test_second_writer_gets_locked(self):
        d = fresh_dir()
        with dlb.Db.open(d) as w1:
            # same-process: refused by the binding's registry (fcntl cannot
            # self-conflict; the C library alone would allow it — measured)
            with self.assertRaises(DlLockedError):
                dlb.Db.open(d)
            with self.assertRaises(DlLockedError):
                dlb.Db.open_ro(d)
            r = self._child(
                "import dlb,sys\n"
                "try:\n"
                f"    db=dlb.Db.open({d!r})\n"
                "except dlb.DlLockedError: print('LOCKED'); sys.exit(0)\n"
                "print('OPENED'); sys.exit(1)\n"
            )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("LOCKED", r.stdout)

    def test_registry_releases_on_close(self):
        d = fresh_dir()
        with dlb.Db.open(d) as db:
            db.declare_relation("edge", 2)  # ensures rels.txt exists on close
        db = dlb.Db.open(d)  # re-open after close must succeed
        db.close()
        ro = dlb.Db.open_ro(d)  # RO after RW close must succeed
        ro.close()
        ro2 = dlb.Db.open_ro(d)  # second RO after the first closed
        ro2.close()

    def test_reader_in_other_process_runs_alongside_writer(self):
        """The two-scope lock (<dir>/LOCK byte 0 = writer exclusivity, byte 1 =
        the mutation window a reader holds only across its own open): a live
        writer EXCLUDES WRITERS, NOT READERS.  A reader in another process
        opens while the writer's handle is held and reads the writer's WAL'd
        state.  Pre-fix this was DL_E_LOCKED (a whole-file F_WRLCK blocks
        F_RDLCK in both directions)."""
        d = fresh_dir()
        with dlb.Db.open(d) as w:
            w.declare_relation("edge", 2)
            w.add_fact("edge", (1, 2))
            r = self._child(
                "import dlb,sys\n"
                "try:\n"
                f"    ro=dlb.Db.open_ro({d!r})\n"
                "except dlb.DlLockedError: print('LOCKED'); sys.exit(0)\n"
                "print('OPEN-RO', ro.lookup('edge',(1,2)), ro.count('edge'))\n"
                "ro.close()\n"
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("OPEN-RO True 1", r.stdout)
        # the reader's view is its own open boundary: after the writer closed,
        # a fresh reader sees the same committed state
        r2 = self._child(
            "import dlb\n"
            f"ro=dlb.Db.open_ro({d!r})\n"
            "print('OPEN-RO', ro.lookup('edge',(1,2)), ro.count('edge'))\n"
            "ro.close()\n"
        )
        self.assertEqual(r2.returncode, 0, r2.stderr)
        self.assertIn("OPEN-RO True 1", r2.stdout)

    def test_writer_opens_while_reader_open(self):
        """A live READER no longer blocks the writer: dl_open_ro holds no lock
        after its open returns (it needs the shared window only to replay the
        WAL without racing a truncation), so the sole writer can open and
        write while a read-only handle is live.  Pre-fix the reader's
        whole-lifetime F_RDLCK refused the writer."""
        d = fresh_dir()
        with dlb.Db.open(d) as w:
            w.declare_relation("edge", 2)
            w.add_fact("edge", (1, 2))
        # reader holds an RO handle in a child process
        proc = subprocess.Popen(
            [sys.executable, "-c",
             "import dlb,time\n"
             f"ro=dlb.Db.open_ro({d!r})\n"
             "print('HELD', flush=True)\n"
             "time.sleep(3)\n"
             "ro.close()\n"],
            stdout=subprocess.PIPE, text=True,
            env={**os.environ, "PYTHONPATH": os.path.join(
                os.path.dirname(__file__), "..")},
        )
        try:
            proc.stdout.readline()  # wait until the reader holds its handle
            r = self._child(
                "import dlb,sys\n"
                "try:\n"
                f"    db=dlb.Db.open({d!r})\n"
                "except dlb.DlLockedError: print('LOCKED'); sys.exit(0)\n"
                "db.add_fact('edge',(3,4)); db.close(); print('OPENED'); sys.exit(0)\n"
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("OPENED", r.stdout)
        finally:
            proc.wait(timeout=30)
        with dlb.Db.open(d) as w2:
            self.assertTrue(w2.lookup("edge", (1, 2)))
            self.assertTrue(w2.lookup("edge", (3, 4)))


class TestSnapshot(unittest.TestCase):
    def test_publish_and_asof_query(self):
        d = fresh_dir()
        with dlb.Db.open(d) as db:
            db.declare_relation("edge", 2)
            db.add_fact("edge", (1, 2))
            db.publish_snapshot()
            v1 = db.snapshot_versions()
            self.assertEqual(v1, [1])
            db.add_fact("edge", (2, 3))
            db.publish_snapshot()
            v2 = db.snapshot_versions()
            self.assertEqual(v2, [1, 2])
            # live view (no snapshot routing for lookup) sees both
            self.assertEqual(db.count("edge"), 2)
            # as-of v1 sees one
            self.assertEqual(db.query_version(1, "edge"), [(1, 2)])
            self.assertEqual(db.query_version(2, "edge"), [(1, 2), (2, 3)])
            self.assertEqual(db.query_bound_version(2, "edge", (2,)), [(2, 3)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
