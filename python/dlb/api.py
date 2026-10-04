"""api.py — thin ergonomic layer over dlb._binding.

Design points (from the binding brief):
- Open once, hold the handle: `Db.open()` / `Db.open_ro()`.
- Errors are raised, never returned as silent zeros. DL_E_LOCKED ->
  DlLockedError, DL_E_CONFLICT -> DlConflictError.
- One writer, many readers: RW handles must be the CEN's sole-writer handle;
  RO handles belong in OTHER processes (same-process RW+RO is forbidden by
  dl.h — fcntl locks are per-process).
- Facts are tuples of u32 column values (interned symbol ids or raw ints).
  Strings go through intern()/sym_of().
- A single RLock guards handle use from multiple Python threads; see
  ENVELOPE.md for the measured thread-safety result.

Rule syntax for query_rules/compile_rules: Datalog with upper-case variables,
e.g. "tc(X,Y) :- edge(X,Y).\\ntc(X,Y) :- edge(X,Z), tc(Z,Y)."
"""

from __future__ import annotations

import os
import ctypes
import threading
from ctypes import (
    POINTER,
    byref,
    c_char_p,
    c_int,
    c_long,
    c_uint32,
    c_uint8,
    c_void_p,
    create_string_buffer,
)
from ctypes.util import find_library  # noqa: F401  (documented fallback)

from ._binding import (
    DL_E_CONFLICT,
    DL_E_LOCKED,
    DlConflictError,
    DlError,
    DlLockedError,
    VEC_M,
    VEC_SIG_WORDS,
    load_library,
    make_join_cb,
    make_tuple_cb,
    make_vec_cb,
)

__all__ = ["Db", "Iter", "Transaction", "VEC_M", "VEC_SIG_WORDS"]

_U32P = POINTER(c_uint32)
_MAX_ARITY = 8


def _cols(values, what: str) -> tuple:
    vals = tuple(values)
    if not (1 <= len(vals) <= _MAX_ARITY):
        raise DlError(f"{what}: need 1..{_MAX_ARITY} column values, got {len(vals)}")
    for v in vals:
        if not isinstance(v, int) or not (0 <= v <= 0xFFFFFFFF):
            raise DlError(f"{what}: column values are u32, got {v!r}")
    return vals


def _arr(vals: tuple):
    return (c_uint32 * len(vals))(*vals) if vals else None


class Iter:
    """Resumable sorted cursor from dl_iter_open. Must not outlive its Db."""

    def __init__(self, db: "Db", rel: str, leading: tuple | None, raw_ptr):
        self._db = db
        self._raw = db._raw
        self._it = raw_ptr
        self._arity = int(self._raw.dl_iter_arity(self._it))
        self._buf = (c_uint32 * max(1, self._arity))()

    def __iter__(self):
        return self

    def __next__(self) -> tuple:
        r = self._raw.dl_iter_next(self._it, self._buf)
        if r == 1:
            return tuple(self._buf[i] for i in range(self._arity))
        if r == 0:
            raise StopIteration
        raise DlError("dl_iter_next failed", r)

    def seek(self, leading: tuple) -> None:
        vals = _cols(leading, "seek")
        arr = _arr(vals)
        with self._db._lock:
            r = self._raw.dl_iter_seek(self._it, arr, c_uint8(len(vals)))
        if r != 0:
            raise DlError("dl_iter_seek failed", r)

    def arity(self) -> int:
        return self._arity

    def close(self) -> None:
        if self._it is not None:
            self._raw.dl_iter_close(self._it)
            self._it = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class Transaction:
    """Buffered atomic write (dl_txn_begin .. dl_txn_commit)."""

    def __init__(self, db: "Db"):
        self._db = db
        r = db._call(db._raw.dl_txn_begin)
        if r != 0:
            raise DlError("dl_txn_begin failed", r)
        self._open = True

    def add_fact(self, rel: str, cols) -> None:
        self._op(db_op := self._db._raw.dl_txn_add_fact, rel, cols, "txn_add_fact")

    def delete_fact(self, rel: str, cols) -> None:
        self._op(self._db._raw.dl_txn_delete_fact, rel, cols, "txn_delete_fact")

    def cas(self, entity: str, expected: int, new_value: int) -> None:
        with self._db._lock:
            r = self._db._raw.dl_txn_cas(
                self._db._h, entity.encode(), expected, new_value
            )
        if r != 0:
            raise DlError("dl_txn_cas failed", r)

    def _op(self, fn, rel: str, cols, what: str) -> None:
        vals = _cols(cols, what)
        arr = _arr(vals)
        with self._db._lock:
            r = fn(self._db._h, rel.encode(), arr, c_uint8(len(vals)))
        if r != 0:
            raise DlError(f"{what} failed", r)

    def commit(self) -> None:
        with self._db._lock:
            r = self._db._raw.dl_txn_commit(self._db._h)
        self._open = False
        if r == DL_E_CONFLICT:
            raise DlConflictError("txn commit: CAS conflict")
        if r != 0:
            raise DlError("dl_txn_commit failed", r)

    def rollback(self) -> None:
        with self._db._lock:
            r = self._db._raw.dl_txn_rollback(self._db._h)
        self._open = False
        if r != 0:
            raise DlError("dl_txn_rollback failed", r)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        if self._open:
            if exc_type is None:
                self.commit()
            else:
                self.rollback()
        return False


class Db:
    """A dl_db handle. open() = read-write (sole writer); open_ro() = shared
    read-only (use from other processes; never alongside an RW handle of the
    same directory in THIS process).

    Per-process handle registry: fcntl locks never conflict within one
    process, so the C library CANNOT enforce (and does not enforce — measured)
    the single-writer invariant against a second same-process open. This
    binding enforces it: at most one open handle per database directory per
    process, and never an RO handle alongside an RW one (dl.h same-process
    caveat). Cross-process exclusion IS enforced by the C lock."""

    _open_handles: dict[str, "Db"] = {}  # realpath -> live handle
    _registry_lock = threading.Lock()

    def __init__(self, handle, raw, read_only: bool, path: str):
        self._h = handle  # c_void_p; None after close
        self._raw = raw
        self.read_only = read_only
        self._path = path
        self._lock = threading.RLock()
        self._closed = False

    # ── lifecycle ──────────────────────────────────────────────

    @classmethod
    def _claim(cls, directory: str, read_only: bool) -> None:
        real = os.path.realpath(directory)
        with cls._registry_lock:
            live = cls._open_handles.get(real)
            if live is not None:
                kind = "read-only" if live.read_only else "read-write"
                new = "read-only" if read_only else "read-write"
                raise DlLockedError(
                    f"{directory}: this process already holds a {kind} handle "
                    f"for that database (a {new} open is refused — fcntl "
                    "locks cannot arbitrate within one process)"
                )
            cls._open_handles[real] = None  # reserved; set on success

    @classmethod
    def _register(cls, directory: str, db: "Db") -> None:
        with cls._registry_lock:
            cls._open_handles[os.path.realpath(directory)] = db

    @classmethod
    def open(cls, directory: str, *, lib_path: str | None = None) -> "Db":
        cls._claim(directory, read_only=False)
        raw = load_library(lib_path)
        err = c_int(0)
        h = raw.dl_open2(directory.encode(), byref(err))
        if not h:
            with cls._registry_lock:
                cls._open_handles.pop(os.path.realpath(directory), None)
            if err.value == DL_E_LOCKED:
                raise DlLockedError(f"{directory}: locked by another writer")
            raise DlError(f"dl_open2({directory}) failed", err.value)
        db = cls(h, raw, read_only=False, path=directory)
        cls._register(directory, db)
        return db

    @classmethod
    def open_ro(cls, directory: str, *, lib_path: str | None = None) -> "Db":
        cls._claim(directory, read_only=True)
        raw = load_library(lib_path)
        err = c_int(0)
        h = raw.dl_open_ro(directory.encode(), byref(err))
        if not h:
            with cls._registry_lock:
                cls._open_handles.pop(os.path.realpath(directory), None)
            if err.value == DL_E_LOCKED:
                raise DlLockedError(
                    f"{directory}: a writer holds the lock (DL_E_LOCKED)"
                )
            raise DlError(f"dl_open_ro({directory}) failed", err.value)
        db = cls(h, raw, read_only=True, path=directory)
        cls._register(directory, db)
        return db

    def close(self) -> None:
        if self._h is not None:
            with self._lock:
                h, self._h = self._h, None
            self._raw.dl_close(h)
        with Db._registry_lock:
            if Db._open_handles.get(os.path.realpath(self._path)) is self:
                del Db._open_handles[os.path.realpath(self._path)]
        self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _call(self, fn, *args):
        with self._lock:
            if self._h is None:
                raise DlError("handle is closed")
            return fn(self._h, *args)

    def _require_rw(self, what: str) -> None:
        if self.read_only:
            raise DlError(f"{what}: read-only handle")

    # ── schema / facts ─────────────────────────────────────────

    def declare_relation(self, name: str, arity: int) -> None:
        self._require_rw("declare_relation")
        if not (0 <= arity <= _MAX_ARITY):
            raise DlError(f"declare_relation: arity must be 0..{_MAX_ARITY}")
        r = self._call(self._raw.dl_declare_relation, name.encode(), c_uint8(arity))
        if r != 0:
            raise DlError(f"declare_relation({name},{arity}) failed", r)

    def declare_relation_variadic(self, name: str) -> None:
        self._require_rw("declare_relation_variadic")
        r = self._call(self._raw.dl_declare_relation_variadic, name.encode())
        if r != 0:
            raise DlError(f"declare_relation_variadic({name}) failed", r)

    def add_fact(self, rel: str, cols) -> bool:
        """Returns True if added, False if it was already present."""
        self._require_rw("add_fact")
        vals = _cols(cols, "add_fact")
        arr = _arr(vals)
        r = self._call(self._raw.dl_add_fact, rel.encode(), arr, c_uint8(len(vals)))
        if r < 0:
            raise DlError(f"add_fact({rel}, {vals}) failed", r)
        return r == 1

    def delete_fact(self, rel: str, cols) -> bool:
        self._require_rw("delete_fact")
        vals = _cols(cols, "delete_fact")
        arr = _arr(vals)
        r = self._call(self._raw.dl_delete_fact, rel.encode(), arr, c_uint8(len(vals)))
        if r < 0:
            raise DlError(f"delete_fact({rel}, {vals}) failed", r)
        return r == 1

    def load_facts(self, rel: str, csv_path: str) -> int:
        self._require_rw("load_facts")
        r = self._call(self._raw.dl_load_facts, rel.encode(), csv_path.encode())
        if r < 0:
            raise DlError(f"load_facts({rel},{csv_path}) failed", r)
        return r

    # ─── CAS revision ──────────────────────────────────────────

    def rev_get(self, entity: str) -> int:
        out = c_uint32(0)
        r = self._call(self._raw.dl_rev_get, entity.encode(), byref(out))
        if r != 0:
            raise DlError(f"rev_get({entity}) failed", r)
        return out.value

    def cas_revision(self, entity: str, expected: int, new_value: int) -> None:
        self._require_rw("cas_revision")
        r = self._call(self._raw.dl_cas_revision, entity.encode(), expected, new_value)
        if r == DL_E_CONFLICT:
            raise DlConflictError(
                f"cas_revision({entity}): expected {expected} != current"
            )
        if r != 0:
            raise DlError(f"cas_revision({entity}) failed", r)

    def transaction(self) -> Transaction:
        self._require_rw("transaction")
        return Transaction(self)

    # ─── interner ──────────────────────────────────────────────

    def intern(self, s: str) -> int:
        r = self._call(self._raw.dl_intern_str, s.encode())
        if r == 0:
            raise DlError(f"intern({s!r}) failed (OOM?)")
        return r

    def intern_find(self, s: str) -> int:
        return self._call(self._raw.dl_intern_str_find, s.encode())

    def sym_of(self, sym: int) -> str | None:
        p = self._call(self._raw.dl_intern_str_of, sym)
        return p.decode() if p is not None else None

    # ─── reads ─────────────────────────────────────────────────

    def lookup(self, rel: str, cols) -> bool:
        """Exact membership: True iff the fact exists."""
        vals = _cols(cols, "lookup")
        arr = _arr(vals)
        return self._call(self._raw.dl_lookup, rel.encode(), arr, c_uint8(len(vals))) == 1

    def prefix(self, rel: str, leading: tuple, cb) -> int:
        """Enumerate tuples whose first len(leading) columns match; cb(tuple)
        per tuple (return truthy to stop early). Returns count visited."""
        vals = _cols(leading, "prefix")
        arr = _arr(vals)
        trap = make_tuple_cb(cb)
        n = self._call(
            self._raw.dl_prefix, rel.encode(), arr, c_uint8(len(vals)), trap, c_void_p(0)
        )
        if n < 0:
            raise DlError(f"prefix({rel}, {vals}) failed", n)
        return n

    def iter(self, rel: str, leading: tuple = ()) -> Iter:
        vals = tuple(leading)
        if vals and len(vals) > _MAX_ARITY:
            raise DlError("iter: too many leading values")
        arr = _arr(vals)
        with self._lock:
            p = self._raw.dl_iter_open(self._h, rel.encode(), arr, c_uint8(len(vals)))
        if not p:
            raise DlError(f"iter({rel}, {vals}) failed")
        return Iter(self, rel, vals, p)

    def count(self, rel: str) -> int:
        n = self._call(self._raw.dl_count, rel.encode())
        if n == 0xFFFFFFFFFFFFFFFF:
            raise DlError(f"count({rel}) failed")
        return n

    def rank(self, rel: str, cols) -> int:
        vals = _cols(cols, "rank")
        arr = _arr(vals)
        n = self._call(self._raw.dl_rank, rel.encode(), arr, c_uint8(len(vals)))
        if n == 0xFFFFFFFFFFFFFFFF:
            raise DlError(f"rank({rel}) failed")
        return n

    def select(self, rel: str, k: int) -> tuple:
        # arity is needed for the out buffer; discover via an empty iter
        with self.iter(rel) as it:
            arity = it.arity()
        buf = (c_uint32 * max(1, arity))()
        r = self._call(self._raw.dl_select, rel.encode(), k, buf, c_uint8(arity))
        if r != 0:
            raise DlError(f"select({rel},{k}) failed", r)
        return tuple(buf[i] for i in range(arity))

    def range_count(self, rel: str, lo: tuple, hi: tuple) -> int:
        lov, hiv = _cols(lo, "range_count lo"), _cols(hi, "range_count hi")
        if len(lov) != len(hiv):
            raise DlError("range_count: lo/hi arity mismatch")
        loa, hia = _arr(lov), _arr(hiv)
        n = self._call(
            self._raw.dl_range_count, rel.encode(), loa, hia, c_uint8(len(lov))
        )
        if n == 0xFFFFFFFFFFFFFFFF:
            raise DlError(f"range_count({rel}) failed")
        return n

    def merge_join(self, l: Iter, r: Iter, jcols: int, cb) -> int:
        """Equi-join two sorted iterators on their first jcols columns; cb(l, r)
        per emitted pair (return truthy to stop early). Both iterators are
        left exhausted. Returns pairs emitted."""
        trap = make_join_cb(cb)
        with self._lock:
            n = self._raw.dl_merge_join(
                l._it, r._it, c_uint8(jcols), trap, c_void_p(0)
            )
        if n < 0:
            raise DlError("merge_join failed", n)
        return n

    # keep the earlier join_rows alias working
    join_rows = merge_join

    # ─── rules / queries ───────────────────────────────────────

    def load_rules(self, source: str) -> None:
        self._require_rw("load_rules")
        r = self._call(self._raw.dl_load_rules, source.encode())
        if r != 0:
            raise DlError(f"load_rules failed: {source[:80]!r}")

    def compile_rules(self) -> None:
        self._require_rw("compile_rules")
        r = self._call(self._raw.dl_compile)
        if r != 0:
            raise DlError("dl_compile failed")

    def query(self, goal_rel: str, cb=None, *, collect: bool = False) -> list | int:
        """Evaluate the compiled program; collect rows if collect=True."""
        rows: list = []

        def sink(t):
            rows.append(t)
            return None

        trap = make_tuple_cb(sink)
        n = self._call(self._raw.dl_query, goal_rel.encode(), trap, c_void_p(0))
        if n < 0:
            raise DlError(f"query({goal_rel}) failed", n)
        return rows if collect else n

    def query_rules_ro(self, source: str, goal_rel: str) -> list:
        """Self-contained read-only rule query; db is untouched. Returns rows."""
        rows: list = []

        def sink(t):
            rows.append(t)
            return None

        trap = make_tuple_cb(sink)
        n = self._call(
            self._raw.dl_query_rules_ro, source.encode(), goal_rel.encode(),
            trap, c_void_p(0),
        )
        if n < 0:
            raise DlError(f"query_rules_ro({goal_rel}) failed: {source[:80]!r}")
        return rows

    def query_bound(self, goal_rel: str, leading: tuple, cb=None, *, collect: bool = False):
        vals = _cols(leading, "query_bound") if leading else ()
        arr = _arr(vals)
        rows: list = []

        def sink(t):
            rows.append(t)
            return None

        trap = make_tuple_cb(cb if cb is not None else sink)
        n = self._call(
            self._raw.dl_query_bound, goal_rel.encode(), arr, c_uint8(len(vals)),
            trap, c_void_p(0),
        )
        if n < 0:
            raise DlError(f"query_bound({goal_rel}) failed", n)
        return rows if collect else n

    def query_magic(self, goal_rel: str, leading: tuple) -> list:
        vals = tuple(leading)
        arr = _arr(vals)
        rows: list = []

        def sink(t):
            rows.append(t)
            return None

        trap = make_tuple_cb(sink)
        n = self._call(
            self._raw.dl_query_magic, goal_rel.encode(), arr, c_uint8(len(vals)),
            trap, c_void_p(0),
        )
        if n < 0:
            raise DlError(f"query_magic({goal_rel}) failed", n)
        return rows

    # ─── snapshot / as-of ──────────────────────────────────────

    def publish_snapshot(self) -> None:
        self._require_rw("publish_snapshot")
        r = self._call(self._raw.dl_publish_snapshot)
        if r != 0:
            raise DlError("publish_snapshot failed")

    def snapshot_versions(self) -> list:
        n = self._call(self._raw.dl_snapshot_versions, None, 0)
        if n < 0:
            raise DlError("snapshot_versions failed", n)
        if n == 0:
            return []
        buf = (c_uint32 * n)()
        total = self._call(self._raw.dl_snapshot_versions, buf, n)
        return [buf[i] for i in range(min(total, n))]

    def query_version(self, version: int, goal_rel: str) -> list:
        rows: list = []

        def sink(t):
            rows.append(t)
            return None

        trap = make_tuple_cb(sink)
        n = self._call(
            self._raw.dl_query_version, version, goal_rel.encode(), trap, c_void_p(0)
        )
        if n < 0:
            raise DlError(f"query_version({version},{goal_rel}) failed", n)
        return rows

    def query_bound_version(self, version: int, goal_rel: str, leading: tuple) -> list:
        vals = tuple(leading)
        arr = _arr(vals)
        rows: list = []

        def sink(t):
            rows.append(t)
            return None

        trap = make_tuple_cb(sink)
        n = self._call(
            self._raw.dl_query_bound_version, version, goal_rel.encode(), arr,
            c_uint8(len(vals)), trap, c_void_p(0),
        )
        if n < 0:
            raise DlError(f"query_bound_version({version},{goal_rel}) failed", n)
        return rows

    # ─── vector tier ───────────────────────────────────────────

    def vector_search(self, q_sig, k: int, r: int, *, corpus=None) -> list:
        """MIH candidate search over LIVE sig relations. q_sig is the
        pre-encoded 256-bit signature as VEC_SIG_WORDS (8) u32, MSB-first.
        corpus: None -> ENTITY (dl_vector_search), "obs" -> OBSERVATION_CONTENT
        (dl_vector_search_corpus; the production memory store's index).
        Returns [(entity_sym, band_match_count), ...] best-first."""
        sig = tuple(q_sig)
        if len(sig) != VEC_SIG_WORDS:
            raise DlError(f"vector_search: q_sig needs {VEC_SIG_WORDS} u32 values")
        out: list = []

        def sink(sym, score):
            out.append((sym, score))
            return None

        trap = make_vec_cb(sink)
        arr = (c_uint32 * VEC_SIG_WORDS)(*sig)
        if corpus is None:
            n = self._call(self._raw.dl_vector_search, arr, k, r, trap, c_void_p(0))
        else:
            c = self._raw.CORPUS_OBSERVATION_CONTENT if corpus == "obs" else corpus
            if not isinstance(c, self._raw.DlVecCorpus):
                raise DlError("vector_search: corpus must be None, 'obs', or a DlVecCorpus")
            n = self._call(self._raw.dl_vector_search_corpus, ctypes.byref(c), arr,
                           k, r, trap, c_void_p(0))
        if n < 0:
            raise DlError("vector_search failed", n)
        return out

    def vector_search_version(self, version: int, q_sig, k: int, r: int, *, corpus=None) -> list:
        sig = tuple(q_sig)
        if len(sig) != VEC_SIG_WORDS:
            raise DlError(f"vector_search_version: q_sig needs {VEC_SIG_WORDS} u32")
        out: list = []

        def sink(sym, score):
            out.append((sym, score))
            return None

        trap = make_vec_cb(sink)
        arr = (c_uint32 * VEC_SIG_WORDS)(*sig)
        if corpus is None:
            n = self._call(
                self._raw.dl_vector_search_version, version, arr, k, r, trap, c_void_p(0)
            )
        else:
            c = self._raw.CORPUS_OBSERVATION_CONTENT if corpus == "obs" else corpus
            if not isinstance(c, self._raw.DlVecCorpus):
                raise DlError("vector_search_version: corpus must be None, 'obs', or DlVecCorpus")
            n = self._call(
                self._raw.dl_vector_search_corpus_version, version, ctypes.byref(c),
                arr, k, r, trap, c_void_p(0),
            )
        if n < 0:
            raise DlError("vector_search_version failed", n)
        return out

    def vector_rerank(self, q_int8, cand_syms, k: int, *, corpus=None) -> list:
        """Exact int8-cosine re-rank. q_int8: 96 u32 (4 packed int8 each).
        corpus: None -> __vec_q__ (entity), "obs" -> __vec_obs__."""
        q = tuple(q_int8)
        if len(q) != 96:
            raise DlError("vector_rerank: q_int8 needs 96 u32 values")
        cands = tuple(cand_syms)
        if not cands:
            return []
        qa = (c_uint32 * 96)(*q)
        ca = (c_uint32 * len(cands))(*cands)
        out: list = []

        def sink(sym, score):
            out.append((sym, score))
            return None

        trap = make_vec_cb(sink)
        if corpus is None:
            n = self._call(
                self._raw.dl_vector_rerank, qa, ca, len(cands), k, trap, c_void_p(0)
            )
        else:
            c = self._raw.CORPUS_OBSERVATION_CONTENT if corpus == "obs" else corpus
            if not isinstance(c, self._raw.DlVecCorpus):
                raise DlError("vector_rerank: corpus must be None, 'obs', or a DlVecCorpus")
            n = self._call(
                self._raw.dl_vector_rerank_corpus, ctypes.byref(c), qa, ca,
                len(cands), k, trap, c_void_p(0),
            )
        if n < 0:
            raise DlError("vector_rerank failed", n)
        return out
