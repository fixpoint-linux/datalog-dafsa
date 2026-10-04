"""_binding.py — raw ctypes layer over libdatalog.so (datalog-dafsa).

Every function used gets exact argtypes/restype. Error codes are mapped to
exceptions — nothing silently returns 0. Callbacks (dl_tuple_cb, dl_vec_cb,
dl_join_cb) are wrapped so a Python callable can be passed as `user` directly.

Library resolution order (first hit wins, overridable via DLBLIB env var):
  1. $DLBLIB
  2. <repo>/zig-out/lib/libdatalog.so        (canonical Zig release build)
  3. ~/fixpoint-linux/datalog-dafsa/zig-out/lib/libdatalog.so
  4. libdatalog.so on the default loader path

Loading is cached per-process by resolved path.
"""

from __future__ import annotations

import ctypes
import os
from ctypes import (
    CFUNCTYPE,
    POINTER,
    byref,
    c_char_p,
    c_int,
    c_long,
    c_uint,
    c_uint8,
    c_uint32,
    c_uint64,
    c_void_p,
)

__all__ = [
    "DlError",
    "DlLockedError",
    "DlConflictError",
    "DlLibraryError",
    "load_library",
    "DlRaw",
    "DL_E_LOCKED",
    "DL_E_CONFLICT",
    "SUPPORTED_ABI",
    "abi_version",
]

# Error codes from dl.h
DL_E_LOCKED = 1    # database is locked by another writer
DL_E_CONFLICT = 2  # CAS: expected revision != current revision

# The C ABI version this binding is written against (dafsa_abi_version()).
SUPPORTED_ABI = 1

# Vector-tier layout constants (src/vector.h) — the Python side must match.
VEC_D = 384          # embedding dim (bge-small)
VEC_C = 256          # ITQ bit-code length
VEC_M = 16           # MIH bands (__sig0__..__sig15__)
VEC_SIG_WORDS = 8    # 256-bit signature, u32 words, MSB-first
VEC_IVEC_WORDS = 96  # int8 query vector, 4 int8 packed per u32


class DlError(Exception):
    """Base error for the binding. `code` is the C error code where meaningful."""

    def __init__(self, msg: str, code: int | None = None):
        super().__init__(msg if code is None else f"{msg} (code={code})")
        self.code = code


class DlLockedError(DlError):
    """dl_open returned DL_E_LOCKED: another writer holds the lock."""


class DlConflictError(DlError):
    """A CAS/txn commit found expected revision != current revision."""


class DlLibraryError(DlError):
    """The library could not be loaded or its ABI is unsupported."""


# ─── C callback prototypes ─────────────────────────────────────────────

# typedef int (*dl_tuple_cb)(const uint32_t *cols, uint8_t arity, void *user);
TUPLE_CB = CFUNCTYPE(c_int, POINTER(c_uint32), c_uint8, c_void_p)
# typedef int (*dl_join_cb)(const uint32_t *l, uint8_t la,
#                           const uint32_t *r, uint8_t ra, void *user);
JOIN_CB = CFUNCTYPE(c_int, POINTER(c_uint32), c_uint8, POINTER(c_uint32), c_uint8, c_void_p)
# typedef int (*dl_vec_cb)(uint32_t entity_sym, int score, void *user);
VEC_CB = CFUNCTYPE(c_int, c_uint32, c_int, c_void_p)
# typedef int (*dl_traverse_cb)(uint32_t node_sym, uint8_t depth, void *user);
TRAVERSE_CB = CFUNCTYPE(c_int, c_uint32, c_uint8, c_void_p)
# typedef int (*dl_str_cb)(const char *s, void *user);
STR_CB = CFUNCTYPE(c_int, c_char_p, c_void_p)
# typedef int (*dl_relation_cb)(const char *name, uint8_t arity, int idb, void *user);
RELATION_CB = CFUNCTYPE(c_int, c_char_p, c_uint8, c_int, c_void_p)


# NOTE on callbacks: every trampoline below closes over the Python callable,
# so the C `void *user` argument is never dereferenced — pass c_void_p(0).
# Each factory returns (c_callback, keepalive) where keepalive must be held
# for the duration of the C call (it usually is the c_callback itself).


def make_tuple_cb(py_cb):
    """Wrap callable(cols: tuple[int, ...]) -> truthy-stop as a dl_tuple_cb."""
    stop = [0]

    @TUPLE_CB
    def tramp(cols, arity, user):
        if stop[0]:
            return 1
        if py_cb(tuple(cols[i] for i in range(arity))):
            stop[0] = 1
            return 1
        return 0

    return tramp


def make_vec_cb(py_cb):
    """Wrap callable(entity_sym: int, score: int) -> truthy-stop as dl_vec_cb."""
    stop = [0]

    @VEC_CB
    def tramp(entity_sym, score, user):
        if stop[0]:
            return 1
        if py_cb(entity_sym, score):
            stop[0] = 1
            return 1
        return 0

    return tramp


def make_str_cb(py_cb):
    """Wrap callable(s: bytes) -> truthy-stop as dl_str_cb."""
    stop = [0]

    @STR_CB
    def tramp(s, user):
        if stop[0]:
            return 1
        if py_cb(s):
            stop[0] = 1
            return 1
        return 0

    return tramp


def make_join_cb(py_cb):
    """Wrap callable(l, r) -> truthy-stop as dl_join_cb (l, r are tuples)."""
    stop = [0]

    @JOIN_CB
    def tramp(l, la, r, ra, user):
        if stop[0]:
            return 1
        if py_cb(tuple(l[i] for i in range(la)), tuple(r[i] for i in range(ra))):
            stop[0] = 1
            return 1
        return 0

    return tramp


class DlRaw:
    """Loaded libdatalog.so with exact prototypes on every symbol used.

    Calling these functions directly is possible but discouraged — use
    dlb.api, which maps error codes to exceptions. This layer exists so the
    ergonomic layer never guesses a signature.
    """

    def __init__(self, path: str):
        self.path = path
        self.lib = lib = ctypes.CDLL(path)

        # ABI gate — refuse to run against an unknown C ABI.
        lib.dafsa_abi_version.argtypes = []
        lib.dafsa_abi_version.restype = c_uint
        self.abi = int(lib.dafsa_abi_version())
        if self.abi != SUPPORTED_ABI:
            raise DlLibraryError(
                f"{path}: dafsa_abi_version()={self.abi}, binding supports {SUPPORTED_ABI}"
            )

        # ── lifecycle ──────────────────────────────────────────────
        lib.dl_open.argtypes = [c_char_p]
        lib.dl_open.restype = c_void_p
        lib.dl_open2.argtypes = [c_char_p, POINTER(c_int)]
        lib.dl_open2.restype = c_void_p
        lib.dl_open_ro.argtypes = [c_char_p, POINTER(c_int)]
        lib.dl_open_ro.restype = c_void_p
        lib.dl_close.argtypes = [c_void_p]
        lib.dl_close.restype = None

        # ── schema / facts ─────────────────────────────────────────
        lib.dl_declare_relation.argtypes = [c_void_p, c_char_p, c_uint8]
        lib.dl_declare_relation.restype = c_int
        lib.dl_declare_relation_variadic.argtypes = [c_void_p, c_char_p]
        lib.dl_declare_relation_variadic.restype = c_int
        lib.dl_load_facts.argtypes = [c_void_p, c_char_p, c_char_p]
        lib.dl_load_facts.restype = c_int
        lib.dl_add_fact.argtypes = [c_void_p, c_char_p, POINTER(c_uint32), c_uint8]
        lib.dl_add_fact.restype = c_int
        lib.dl_delete_fact.argtypes = [c_void_p, c_char_p, POINTER(c_uint32), c_uint8]
        lib.dl_delete_fact.restype = c_int

        # ── CAS revision ───────────────────────────────────────────
        lib.dl_cas_revision.argtypes = [c_void_p, c_char_p, c_uint32, c_uint32]
        lib.dl_cas_revision.restype = c_int
        lib.dl_rev_get.argtypes = [c_void_p, c_char_p, POINTER(c_uint32)]
        lib.dl_rev_get.restype = c_int

        # ── transactions ───────────────────────────────────────────
        lib.dl_txn_begin.argtypes = [c_void_p]
        lib.dl_txn_begin.restype = c_int
        lib.dl_txn_cas.argtypes = [c_void_p, c_char_p, c_uint32, c_uint32]
        lib.dl_txn_cas.restype = c_int
        lib.dl_txn_add_fact.argtypes = [c_void_p, c_char_p, POINTER(c_uint32), c_uint8]
        lib.dl_txn_add_fact.restype = c_int
        lib.dl_txn_delete_fact.argtypes = [c_void_p, c_char_p, POINTER(c_uint32), c_uint8]
        lib.dl_txn_delete_fact.restype = c_int
        lib.dl_txn_commit.argtypes = [c_void_p]
        lib.dl_txn_commit.restype = c_int
        lib.dl_txn_rollback.argtypes = [c_void_p]
        lib.dl_txn_rollback.restype = c_int

        # ── query primitives ───────────────────────────────────────
        lib.dl_lookup.argtypes = [c_void_p, c_char_p, POINTER(c_uint32), c_uint8]
        lib.dl_lookup.restype = c_int
        lib.dl_prefix.argtypes = [
            c_void_p, c_char_p, POINTER(c_uint32), c_uint8, TUPLE_CB, c_void_p,
        ]
        lib.dl_prefix.restype = c_long
        lib.dl_iter_open.argtypes = [c_void_p, c_char_p, POINTER(c_uint32), c_uint8]
        lib.dl_iter_open.restype = c_void_p
        lib.dl_iter_seek.argtypes = [c_void_p, POINTER(c_uint32), c_uint8]
        lib.dl_iter_seek.restype = c_int
        lib.dl_iter_next.argtypes = [c_void_p, POINTER(c_uint32)]
        lib.dl_iter_next.restype = c_int
        lib.dl_iter_arity.argtypes = [c_void_p]
        lib.dl_iter_arity.restype = c_uint8
        lib.dl_iter_close.argtypes = [c_void_p]
        lib.dl_iter_close.restype = None
        lib.dl_merge_join.argtypes = [c_void_p, c_void_p, c_uint8, JOIN_CB, c_void_p]
        lib.dl_merge_join.restype = c_long

        # ── order statistics ───────────────────────────────────────
        lib.dl_rank.argtypes = [c_void_p, c_char_p, POINTER(c_uint32), c_uint8]
        lib.dl_rank.restype = c_uint64
        lib.dl_select.argtypes = [c_void_p, c_char_p, c_uint64, POINTER(c_uint32), c_uint8]
        lib.dl_select.restype = c_int
        lib.dl_range_count.argtypes = [
            c_void_p, c_char_p, POINTER(c_uint32), POINTER(c_uint32), c_uint8,
        ]
        lib.dl_range_count.restype = c_uint64
        lib.dl_count.argtypes = [c_void_p, c_char_p]
        lib.dl_count.restype = c_uint64

        # ── rules / evaluation ─────────────────────────────────────
        lib.dl_load_rules.argtypes = [c_void_p, c_char_p]
        lib.dl_load_rules.restype = c_int
        lib.dl_compile.argtypes = [c_void_p]
        lib.dl_compile.restype = c_int
        lib.dl_query.argtypes = [c_void_p, c_char_p, TUPLE_CB, c_void_p]
        lib.dl_query.restype = c_long
        lib.dl_query_rules_ro.argtypes = [c_void_p, c_char_p, c_char_p, TUPLE_CB, c_void_p]
        lib.dl_query_rules_ro.restype = c_long
        lib.dl_query_bound.argtypes = [
            c_void_p, c_char_p, POINTER(c_uint32), c_uint8, TUPLE_CB, c_void_p,
        ]
        lib.dl_query_bound.restype = c_long
        lib.dl_query_magic.argtypes = [
            c_void_p, c_char_p, POINTER(c_uint32), c_uint8, TUPLE_CB, c_void_p,
        ]
        lib.dl_query_magic.restype = c_long
        lib.dl_query_magic_adorn.argtypes = [
            c_void_p, c_char_p, c_char_p, POINTER(c_uint32), c_uint8, TUPLE_CB, c_void_p,
        ]
        lib.dl_query_magic_adorn.restype = c_long
        lib.dl_query_topdown.argtypes = [
            c_void_p, c_char_p, POINTER(c_uint32), c_uint8, TUPLE_CB, c_void_p,
        ]
        lib.dl_query_topdown.restype = c_long

        # ── snapshot ───────────────────────────────────────────────
        lib.dl_publish_snapshot.argtypes = [c_void_p]
        lib.dl_publish_snapshot.restype = c_int
        lib.dl_snapshot_versions.argtypes = [c_void_p, POINTER(c_uint32), ctypes.c_size_t]
        lib.dl_snapshot_versions.restype = c_long
        lib.dl_query_version.argtypes = [
            c_void_p, c_uint32, c_char_p, TUPLE_CB, c_void_p,
        ]
        lib.dl_query_version.restype = c_long
        lib.dl_query_bound_version.argtypes = [
            c_void_p, c_uint32, c_char_p, POINTER(c_uint32), c_uint8, TUPLE_CB, c_void_p,
        ]
        lib.dl_query_bound_version.restype = c_long

        # ── interner ───────────────────────────────────────────────
        lib.dl_intern_str.argtypes = [c_void_p, c_char_p]
        lib.dl_intern_str.restype = c_uint32
        lib.dl_intern_str_find.argtypes = [c_void_p, c_char_p]
        lib.dl_intern_str_find.restype = c_uint32
        lib.dl_intern_str_of.argtypes = [c_void_p, c_uint32]
        lib.dl_intern_str_of.restype = c_char_p

        # ── vector tier ────────────────────────────────────────────
        # struct dl_vec_corpus (vector.h): corpus-parameterized search.
        # The production memory store indexes OBSERVATION CONTENT
        # (__obssig*__/__vec_obs__/observation), not entity names, so the
        # plain dl_vector_search (hardcoded ENTITY corpus) returns -1 there.
        class DlVecCorpus(ctypes.Structure):
            _fields_ = [
                ("filter_rel", c_char_p),      # liveness relation
                ("filter_col", c_uint8),       # which col holds the sym
                ("sig_rel_fmt", c_char_p),     # "__sig%d__" / "__obssig%d__"
                ("vec_rel", c_char_p),         # "__vec_q__" / "__vec_obs__"
                ("basis_suffix", c_char_p),    # "" / "_obs"
            ]

        self.DlVecCorpus = DlVecCorpus
        self.CORPUS_ENTITY = DlVecCorpus(
            filter_rel=b"entity", filter_col=0,
            sig_rel_fmt=b"__sig%d__", vec_rel=b"__vec_q__", basis_suffix=b"",
        )
        self.CORPUS_OBSERVATION_CONTENT = DlVecCorpus(
            filter_rel=b"observation", filter_col=1,
            sig_rel_fmt=b"__obssig%d__", vec_rel=b"__vec_obs__",
            basis_suffix=b"_obs",
        )

        lib.dl_vector_search.argtypes = [
            c_void_p, POINTER(c_uint32), c_int, c_int, VEC_CB, c_void_p,
        ]
        lib.dl_vector_search.restype = c_long
        lib.dl_vector_search_corpus.argtypes = [
            c_void_p, ctypes.POINTER(DlVecCorpus), POINTER(c_uint32),
            c_int, c_int, VEC_CB, c_void_p,
        ]
        lib.dl_vector_search_corpus.restype = c_long
        lib.dl_vector_search_version.argtypes = [
            c_void_p, c_uint32, POINTER(c_uint32), c_int, c_int, VEC_CB, c_void_p,
        ]
        lib.dl_vector_search_version.restype = c_long
        lib.dl_vector_search_corpus_version.argtypes = [
            c_void_p, c_uint32, ctypes.POINTER(DlVecCorpus), POINTER(c_uint32),
            c_int, c_int, VEC_CB, c_void_p,
        ]
        lib.dl_vector_search_corpus_version.restype = c_long
        lib.dl_vector_rerank.argtypes = [
            c_void_p, POINTER(c_uint32), POINTER(c_uint32), c_int, c_int, VEC_CB, c_void_p,
        ]
        lib.dl_vector_rerank.restype = c_long
        lib.dl_vector_rerank_corpus.argtypes = [
            c_void_p, ctypes.POINTER(DlVecCorpus), POINTER(c_uint32),
            POINTER(c_uint32), c_int, c_int, VEC_CB, c_void_p,
        ]
        lib.dl_vector_rerank_corpus.restype = c_long

        # Expose prototypes for direct use by the ergonomic layer.
        self.dl_open = lib.dl_open
        self.dl_open2 = lib.dl_open2
        self.dl_open_ro = lib.dl_open_ro
        self.dl_close = lib.dl_close
        for nm in (
            "dl_declare_relation dl_declare_relation_variadic dl_load_facts "
            "dl_add_fact dl_delete_fact dl_cas_revision dl_rev_get dl_txn_begin "
            "dl_txn_cas dl_txn_add_fact dl_txn_delete_fact dl_txn_commit "
            "dl_txn_rollback dl_lookup dl_prefix dl_iter_open dl_iter_seek "
            "dl_iter_next dl_iter_arity dl_iter_close dl_merge_join dl_rank "
            "dl_select dl_range_count dl_count dl_load_rules dl_compile dl_query "
            "dl_query_rules_ro dl_query_bound dl_query_magic dl_query_magic_adorn "
            "dl_query_topdown dl_publish_snapshot dl_snapshot_versions "
            "dl_query_version dl_query_bound_version dl_intern_str "
            "dl_intern_str_find dl_intern_str_of dl_vector_search "
            "dl_vector_search_corpus dl_vector_search_version "
            "dl_vector_search_corpus_version "
            "dl_vector_rerank dl_vector_rerank_corpus"
        ).split():
            setattr(self, nm, getattr(lib, nm))

    def close(self) -> None:
        """Drop references (the OS unloads when refcount hits zero)."""
        self.lib = None


_LIB_CACHE: dict[str, DlRaw] = {}

_CANONICAL = os.path.expanduser(
    "~/fixpoint-linux/datalog-dafsa/zig-out/lib/libdatalog.so"
)


def resolve_path() -> str:
    env = os.environ.get("DLBLIB")
    if env:
        return env
    # REALPATH, not abspath: this module can be reached through a SYMLINKED import
    # root (e.g. ~/thing/dlb -> <repo>/python), and the kernel resolves ".." against
    # the symlink TARGET while abspath resolves it textually. Mixing the two makes
    # os.path.exists() true for a path the loader cannot open — measured: a
    # symlinked root returned "<other-tree>/zig-out/lib/..." while exists() had
    # followed the link to this repo's own zig-out. Resolving first makes the
    # existence check and the returned path agree in every layout.
    here = os.path.dirname(os.path.realpath(__file__))
    candidate = os.path.join(here, "..", "..", "zig-out", "lib", "libdatalog.so")
    if os.path.exists(candidate):
        return os.path.realpath(candidate)
    if os.path.exists(_CANONICAL):
        return _CANONICAL
    return "libdatalog.so"  # loader default search


def load_library(path: str | None = None) -> DlRaw:
    """Load (and cache per resolved path) libdatalog.so, gating on ABI."""
    p = path or resolve_path()
    real = os.path.realpath(p) if os.path.exists(p) else p
    raw = _LIB_CACHE.get(real)
    if raw is None:
        raw = DlRaw(real)
        _LIB_CACHE[real] = raw
    return raw


def abi_version() -> int:
    return load_library().abi
