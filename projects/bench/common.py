#!/usr/bin/env python3
"""Shared helpers for the fastxl benchmark scripts."""

import ctypes
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


# --------------------------------------------------------------------------- RSS

if sys.platform == "win32":
    from ctypes import wintypes

    class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    _psapi = ctypes.WinDLL("psapi", use_last_error=True)
    _psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(PROCESS_MEMORY_COUNTERS),
        wintypes.DWORD,
    ]
    _psapi.GetProcessMemoryInfo.restype = wintypes.BOOL

    def peak_rss_bytes():
        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(counters)
        handle = _kernel32.GetCurrentProcess()
        if not _psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return counters.PeakWorkingSetSize

else:
    import resource

    def peak_rss_bytes():
        usage = resource.getrusage(resource.RUSAGE_SELF)
        return int(usage.ru_maxrss) * 1024


# ----------------------------------------------------------------------- helpers

BENCH_CANDIDATES = (
    "build/windows/x64/release/fastxl-bench.exe",
    "build/windows/x64/debug/fastxl-bench.exe",
    "build/linux/x86_64/release/fastxl-bench",
    "build/linux/x86_64/debug/fastxl-bench",
)


def find_bin(explicit, candidates, label):
    if explicit:
        return Path(explicit)
    for candidate in candidates:
        path = Path(candidate)
        if not path.is_absolute():
            path = ROOT / path
        if path.exists():
            return path
    raise SystemExit("%s not found: %s" % (label, ", ".join(candidates)))


def find_bench(explicit=None):
    return find_bin(explicit, BENCH_CANDIDATES, "fastxl-bench")


def run_json(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(
            "command failed (%d): %s\n%s"
            % (result.returncode, " ".join(map(str, command)), result.stderr)
        )
    line = result.stdout.strip().splitlines()[-1]
    return json.loads(line)


def measure(command, runs):
    run_json(command)  # warm-up
    return [run_json(command) for _ in range(max(1, runs))]


def mib(value):
    return value / (1024.0 * 1024.0)


def speedup(slow, fast):
    return slow / fast if fast else float("inf")


def median(values, key):
    return statistics.median(item[key] for item in values)


def rss_span(values):
    peaks = [item["peak_rss_bytes"] for item in values]
    return min(peaks), max(peaks)
