"""dlb — in-process Python binding for datalog-dafsa (libdatalog.so)."""

from ._binding import (
    DL_E_CONFLICT,
    DL_E_LOCKED,
    SUPPORTED_ABI,
    DlConflictError,
    DlError,
    DlLibraryError,
    DlLockedError,
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
]

__version__ = "0.1.0"
