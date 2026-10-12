"""Windows process leases for cooperating experiment writers and snapshot readers."""

from contextlib import contextmanager
import ctypes
from ctypes import wintypes
from functools import lru_cache
import hashlib
import os

from .experiments import require


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
    identity = hashlib.sha256(os.path.normcase(os.path.realpath(path)).encode("utf-8")).hexdigest()
    with named_mutex("snapshot-" + identity, 5000, "Snapshot reader/writer lock timed out"):
        yield
