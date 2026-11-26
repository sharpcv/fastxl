# fastxl 1.3.0 performance report — C++ core vs C ABI vs Python

Date: 2026-11-26 · Version: 1.3.0 · Executed on the local Linux workstation.

This report measures the same workloads through the three supported entry
paths and separates core compute from binding cost:

| path | what it uses |
|---|---|
| **C++ native** | `fastxl-core` directly (`WorkBook`, `profile_sheet`, `open_reader`, `search_workbook`) |
| **C ABI** | `libfastxl.so` through `fastxl_c.h` (no Python) |
| **pybind11** | the existing native extension module (`build/python/fastxl`) |
| **ctypes** | `fastxl_ctypes` over the same `libfastxl.so` |

## Environment

| item | value |
|---|---|
| CPU | Intel Core i5-13420H (12 threads) |
| RAM | 15 GiB |
| OS | Linux x86_64 (kernel 7.3.6) |
| Compiler | GCC 16.2.1, C++20, release (`-O3`) |
| Python | CPython 3.14.10 |
| SIMD | AVX2 (`simdutf` / `fast_float` / fastxl scanner) |
| Build | `xmake f -y -m release`, commit `fd90f84` + benchmark/report changes |

## Method

- Every sample is a **fresh process**; the reported `ms` is the operation only
  (imports and process startup are outside the timer), and peak memory is the
  child's `VmHWM`.
- Probe layer: 7 measured runs per path, medians. Streaming cases: 3 runs.
- Fixtures:
  - `projects/tests/fixtures/large.xlsx` — 15,000 × 10, 1 worksheet
    (6.1 MiB uncompressed part).
  - `build/fixtures/big-200000x8.xlsx` — 200,000 × 8, 8.5 MiB zip,
    199,999 data rows.
- `Session` warm rows are the second call in the same process with the same
  options (workbook/profile/search cache hit).
- The C ABI and ctypes rows include JSON construction/parsing; pybind11 rows
  include dict/tuple construction. The native rows do not build a payload
  (except info, which opens the workbook). This is the cost each surface
  actually exposes to its caller.

## Probe layer — large.xlsx (15,000 × 10)

| workload | C++ native ms | C ABI ms | pybind11 ms | ctypes ms |
|---|---:|---:|---:|---:|
| info | 0.6 | 0.9 | 0.5 | 0.6 |
| preview_sheet 2000 | — | 149.6 | 148.9 | 178.7 |
| schema cold | 88.6 | 84.5 | 86.4 | 86.1 |
| schema warm (Session) | — | **0.1** | 0.2 | 0.3 |
| probe cold | 89.7 | 86.9 | 85.9 | 85.6 |
| search cold | 118.0 | 118.3 | 116.2 | 118.6 |
| search warm (Session) | — | **0.0** | 0.1 | 0.1 |

Peak RSS (MiB, whole child process):

| workload | C++ native | C ABI | pybind11 | ctypes |
|---|---:|---:|---:|---:|
| info | 5.8 | 6.1 | 21.9 | 23.6 |
| preview_sheet 2000 | — | **36.4** | 63.7 | 60.9 |
| schema cold | 25.1 | 25.3 | 40.8 | 42.8 |
| schema warm (Session) | — | 25.4 | 41.0 | 42.8 |
| probe cold | 25.1 | 25.4 | 40.9 | 42.9 |
| search cold | 12.3 | 12.6 | 28.3 | 30.1 |
| search warm (Session) | — | 12.6 | 28.5 | 30.2 |

## Streaming — big-200000x8.xlsx (199,999 data rows × 8)

| workload | C++ native ms | C ABI ms | pybind11 ms | ctypes ms |
|---|---:|---:|---:|---:|
| read all rows (stream, 4096-row batches) | 1195.3 | 1227.6 | 1206.3 | 1807.9 |
| convert xlsx → csv | 1246.9 | 1270.4 | — | 1243.2 |

Peak RSS (MiB, whole child process):

| workload | C++ native | C ABI | pybind11 | ctypes |
|---|---:|---:|---:|---:|
| read all rows (stream) | 67.6 | 68.6 | 83.5 | 88.5 |
| convert xlsx → csv | 68.3 | 68.3 | — | 85.5 |

Materialised all rows (`pybind11 read_rows(limit=0)`):

| ms | peak RSS MiB | rows |
|---:|---:|---:|
| 1235.0 | 185.7 | 199,999 |

## Analysis

**The C ABI boundary is effectively free.** Against direct core calls:

| case | C ABI vs native |
|---|---:|
| schema cold | −4.6% (within noise) |
| probe cold | −3.1% (within noise) |
| search cold | +0.3% |
| read all rows | +2.7% |
| convert | +1.9% |

The JSON payload for schema is 5.7 KiB; serialising it costs less than the
measurement noise of the sheet scan. The streaming case pays for JSONL
formatting of 199,999 rows, which is the +2.7%.

**pybind11 and the C ABI are equivalent.** schema 86.4 vs 84.5 ms, preview_sheet
148.9 vs 149.6 ms, streaming read 1206.3 vs 1227.6 ms. Dict/tuple construction
and JSON construction roughly cancel out.

**The ctypes gap is Python-side parsing, not the ABI.** at the same contract:

- single-call conversions: 1243.2 ms vs 1270.4 ms for the C ABI (no penalty);
- probe JSON: +1.6 ms (schema) to +29 ms (preview_sheet, 1.39 MB payload);
- `iter_rows`: +47% (1807.9 vs 1227.6 ms), from `json.loads` per row
  (~0.55 s over 200k rows).

**Memory follows the same split.** The ABI adds 0.2 MiB over the native core
(25.3 vs 25.1 MiB schema; 68.6 vs 67.6 MiB streaming). Python paths pay the
interpreter floor (40.8 MiB pybind11 / 42.8 MiB ctypes for schema) and, for
`preview_sheet`, dict materialisation (63.7 MiB vs 36.4 MiB ABI). Materialising
all 200k rows costs 185.7 MiB versus 67.6 MiB streaming, as documented.

**Warm caches collapse probe latency.** A cached schema/probe/search is
0.0–0.3 ms on every path; the ctypes +0.1–0.2 ms is JSON parsing.

### Recommendations

- Maximum row-stream throughput with Python: keep using **pybind11**.
- One library across Python 3.8–3.15 (including free-threaded builds), or
  non-Python hosts: use the **C ABI / fastxl_ctypes**. Probe and convert are at
  parity; only per-row ctypes iteration pays the JSON decode cost.
- A future ctypes batch format (compact typed blocks) would close the
  `iter_rows` gap without changing the C ABI.

## Reproduce

```sh
xmake f -y -m release
xmake build -y fastxl-bench-paths-native fastxl-bench-paths-abi fastxl-python fastxl-c
python projects/bench/compare_paths.py --runs 7 --big-runs 3 \
  --write build/reports/paths.md
```

Sources: `projects/bench/paths/native.cpp`,
`projects/bench/paths/abi.cpp`, `projects/bench/paths/python_child.py`,
`projects/bench/compare_paths.py`. The 200k×8 fixture is generated on demand.

## Caveats

- Numbers are a single-machine snapshot, not a cross-platform guarantee.
- The native harness has no JSON preview builder, so `preview_sheet` has no
  native column; its native cost is covered by the `preview` (SAX row count)
  mode in the harness, which is not the same contract and is not reported.
- Windows peak working set and Linux `VmHWM` are different metrics; do not
  compare those RSS numbers across platforms.
- `Session` warm numbers depend on the cache being populated by the same
  process and file identity (path, size, mtime).
