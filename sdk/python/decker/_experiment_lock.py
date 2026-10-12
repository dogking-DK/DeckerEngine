"""Windows process leases for cooperating experiment writers and snapshot readers."""

from contextlib import contextmanager
import ctypes
from ctypes import wintypes
from functools import lru_cache
import hashlib
import os
import threading

from .experiments import require

_owned = set()
_guard = threading.Lock()


def path_identity(path):
    # Roots reject links. Do not open the target to resolve its name before taking
    # the lock: even a Windows metadata handle can obstruct the concurrent rename.
    return hashlib.sha256(os.path.normcase(os.path.abspath(path)).encode("utf-8")).hexdigest()


@contextmanager
def exclusive_path(kind, path):
    """Process-lifetime writer lease, including rejection of same-thread recursion."""
    name = kind + "-" + path_identity(path)
    with _guard:
        require(name not in _owned, "Experiment directory already has a writer")
        _owned.add(name)
    try:
        with named_mutex(name, 0, "Experiment directory already has a writer"):
            yield
    finally:
        with _guard:
            _owned.remove(name)


@lru_cache(maxsize=1)
def _kernel():
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    api.CreateMutexW.restype = wintypes.HANDLE
    api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    api.WaitForSingleObject.restype = wintypes.DWORD
    api.ReleaseMutex.argtypes = [wintypes.HANDLE]
    api.ReleaseMutex.restype = wintypes.BOOL
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.CloseHandle.restype = wintypes.BOOL
    return api


@contextmanager
def named_mutex(name, timeout_ms, message):
    api = _kernel()
    handle = api.CreateMutexW(None, False, "Local\\DeckerExperiment-" + name)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    owned = False
    try:
        result = api.WaitForSingleObject(handle, timeout_ms)
        require(result in (0, 0x80), message)
        owned = True
        yield
    finally:
        if owned:
            api.ReleaseMutex(handle)
        api.CloseHandle(handle)


@contextmanager
def snapshot_access(path):
    if os.name != "nt":
        yield
        return
    identity = path_identity(path)
    with named_mutex("snapshot-" + identity, 5000, "Snapshot reader/writer lock timed out"):
        yield
