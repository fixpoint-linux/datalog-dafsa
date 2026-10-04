"""dlb — in-process Python binding for datalog-dafsa (libdatalog.so)."""

from ._binding import (
    DL_E_CONFLICT,
    DL_E_LOCKED,
    SUPPORTED_ABI,
    DlConflictError,
    DlError,
    DlLibraryError,
    DlLockedError,
    VEC_D,
    VEC_IVEC_WORDS,
    VEC_M,
    VEC_SIG_WORDS,
    abi_version,
)
from .api import Db, Iter, Transaction

__all__ = [
    "Db",
    "Iter",
    "Transaction",
    "DlError",
    "DlLockedError",
    "DlConflictError",
    "DlLibraryError",
    "DL_E_LOCKED",
    "DL_E_CONFLICT",
    "SUPPORTED_ABI",
    "abi_version",
    "VEC_D",
    "VEC_M",
    "VEC_SIG_WORDS",
    "VEC_IVEC_WORDS",
]

__version__ = "0.1.0"
