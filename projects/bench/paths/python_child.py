#!/usr/bin/env python3
"""Child process for compare_paths.py: one pybind11 or ctypes operation.

Prints one JSON line with the operation time, peak RSS and row count. Kept in a
separate process so imports and allocator state never leak between samples.
"""

import argparse
import importlib
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def peak_mb():
    for line in open("/proc/self/status", encoding="utf-8"):
        if line.startswith("VmHWM:"):
            return float(line.split()[1]) / 1024.0
    return 0.0


def load(impl):
    if impl == "ctypes":
        os.environ["FASTXL_BACKEND"] = "ctypes"
    for candidate in (ROOT / "build" / "python", ROOT / "bindings" / "python"):
        if (candidate / "fastxl").is_dir():
            sys.path.insert(0, str(candidate))
            break
    return importlib.import_module("fastxl")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("impl", choices=["pybind", "ctypes"])
    parser.add_argument("op")
    parser.add_argument("file")
    parser.add_argument("extra", nargs="?")
    args = parser.parse_args()

    fastxl = load(args.impl)
    op = args.op
    warm = op.startswith("warm_")
    base = op[5:] if warm else op
    rows = 0
    payload = None
    session = fastxl.Session() if warm else None

    def invoke():
        nonlocal rows, payload
        if base == "info":
            payload = session.info(args.file) if session is not None else fastxl.info(args.file)
        elif base == "preview":
            payload = (session.preview(args.file, rows=2000)
                       if session is not None
                       else fastxl.preview(args.file, rows=2000))
        elif base == "schema":
            payload = (session.schema(args.file) if session is not None
                       else fastxl.schema(args.file))
        elif base == "probe":
            payload = (session.probe(args.file) if session is not None
                       else fastxl.probe(args.file))
        elif base == "search":
            payload = (session.search(args.file, "invoice", max_hits=100)
                       if session is not None
                       else fastxl.search(args.file, "invoice", max_hits=100))
        elif base == "read_rows":
            if args.impl == "ctypes":
                raise SystemExit("read_rows is only implemented by the native backend")
            payload = fastxl.read_rows(args.file, limit=0)
            rows = len(payload["rows"])
        elif base == "iter":
            with fastxl.iter_rows(args.file, shape="arrays") as iterator:
                for _ in iterator:
                    rows += 1
        elif base == "convert":
            fastxl.convert(args.file, args.extra)
        else:
            raise SystemExit("unknown op " + base)

    if warm:
        invoke()
    started = time.perf_counter()
    invoke()
    ms = (time.perf_counter() - started) * 1000.0
    size = 0
    if payload is not None:
        size = len(
            json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        )
    print(
        json.dumps(
            {
                "ms": round(ms, 3),
                "rss_mb": round(peak_mb(), 1),
                "rows": rows,
                "bytes": size,
            }
        )
    )


if __name__ == "__main__":
    main()
