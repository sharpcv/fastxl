"""fastxl: one Python API over the native pybind11 module or ctypes.

The public surface is backend-independent. Set ``FASTXL_BACKEND=native`` to
require the compiled ``_fastxl`` extension, ``FASTXL_BACKEND=ctypes`` to use the
bundled ``libfastxl.so`` / ``fastxl.dll`` through ctypes, or leave it unset to
prefer the native module and fall back to ctypes. ``fastxl.__backend__`` reports
which one is active.

The ctypes backend needs no per-Python build, so the release wheels are
``py3-none-<plat>`` and work on CPython 3.8-3.15 (free-threaded included) and
PyPy. Native and ctypes expose the same functions, classes and error fields:
``Error.code``, ``Error.exit_code``, ``Error.status``, ``Error.message``.

    import fastxl
    with fastxl.Session() as session:
        schema = session.schema("book.xlsx", header_row="auto")
    for row in fastxl.iter_rows("book.xlsx", shape="objects", batch_size=4096):
        ...
    fastxl.convert("book.xlsx", "book.csv", sheet="Data")
"""

from __future__ import annotations

import json as _json
import os as _os
import sys as _sys
from pathlib import Path as _Path
from typing import Any, Iterable, Iterator, Mapping, Sequence

__version__: str = "1.3.2"
__license__: str = "MIT"

_INFO = ("format", "max_bytes")
_REPORT = (
    "sheet", "header_row", "format", "infer", "delimiter", "fill_gaps",
    "max_rows", "sample", "top", "distinct_cap", "jobs", "regions", "max_bytes",
)
_PREVIEW = (
    "sheet", "header_row", "format", "rows", "range", "columns",
    "max_cell_bytes", "max_bytes",
)
_SEARCH = (
    "regex", "ignore_case", "context", "max_hits", "max_cell_bytes",
    "sheet", "format", "max_bytes",
)
_CONVERT = (
    "from", "format", "to", "sheet", "header_row", "infer", "delimiter",
    "fill_gaps", "limit", "normalize", "columns", "crlf", "bom", "sheet_name",
    "compression_level",
)
_ITER = (
    "sheet", "header_row", "format", "limit", "offset", "infer", "delimiter",
    "fill_gaps", "max_columns", "with_cells",
)


def _pick(options: Mapping[str, Any], keys: Sequence[str]) -> dict[str, Any]:
    return {key: value for key, value in options.items() if key in keys}


def _native_module():
    from . import _fastxl

    build = getattr(_fastxl, "__build_python__", None)
    if build is not None and tuple(build) != _sys.version_info[:2]:
        raise ImportError(
            "fastxl's native module was built for Python %d.%d but this "
            "interpreter is %d.%d" % (*build, *_sys.version_info[:2])
        )
    return _fastxl


_requested = _os.environ.get("FASTXL_BACKEND", "").strip().lower()
if _requested not in ("", "native", "ctypes"):
    raise ValueError(
        "FASTXL_BACKEND must be 'native' or 'ctypes' (got %r)" % _requested
    )
_native: Any = None
_ctype_api: Any = None

if _requested == "ctypes":
    __backend__ = "ctypes"
elif _requested == "native":
    _native = _native_module()
    __backend__ = "native"
else:
    try:
        _native = _native_module()
        __backend__ = "native"
    except ImportError:
        __backend__ = "ctypes"

if __backend__ == "ctypes":
    from . import _ctypes_backend as _ctypes_module

    _ctype_api = _ctypes_module

if __backend__ == "native":
    Error = _native.Error
else:
    Error = _ctype_api.Error


def _native_call(name: str, keys: Sequence[str], *args: Any, **options: Any) -> Any:
    return getattr(_native, name)(*args, **_pick(options, keys))


def _format_name(path: str, explicit: str) -> str:
    if explicit:
        return explicit
    suffix = _Path(path).suffix
    return suffix[1:].lower() if suffix else ""


class RowIterator(Iterator[Any]):
    """Lazy row iterator with the same interface on both backends.

    Rows are ``list`` for ``shape="arrays"`` (default) and ``dict`` for
    ``shape="objects"``. ``close()`` releases the reader deterministically;
    ``closed`` is true after close() and after the stream is exhausted.
    """

    def __init__(self, inner: Any, meta: Mapping[str, Any] | None = None) -> None:
        self._inner = inner
        self._meta = dict(meta) if meta is not None else None
        self._rows_read = 0

    def __iter__(self) -> "RowIterator":
        return self

    def __next__(self) -> Any:
        row = next(self._inner)
        self._rows_read += 1
        return row

    def close(self) -> None:
        self._inner.close()

    def __enter__(self) -> "RowIterator":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:  # interpreter shutdown can clear module globals
            pass

    @property
    def closed(self) -> bool:
        return bool(self._inner.closed)

    @property
    def columns(self) -> list[str]:
        return list(self._inner.columns)

    @property
    def sheet(self) -> str:
        return self._inner.sheet

    @property
    def header_row(self) -> int:
        return int(self._inner.header_row)

    @property
    def rows_read(self) -> int:
        return self._rows_read

    @property
    def truncated(self) -> bool:
        return bool(getattr(self._inner, "truncated", False))

    @property
    def meta(self) -> dict[str, Any]:
        if self._meta is not None:
            return dict(self._meta)
        return dict(self._inner.meta)


class JsonlWriter:
    """Streaming writer: append JSONL batches, then finish (atomically)."""

    def __init__(self, dst: str, **options: Any) -> None:
        self._finished = False
        if __backend__ == "ctypes":
            self._writer = _ctype_api.JsonlWriter(dst, **options)
        else:
            kwargs = _pick(
                options,
                ("columns", "format", "header", "delimiter", "crlf", "bom",
                 "fsync", "sheet_name", "compression_level"),
            )
            if "columns" in kwargs:
                kwargs["columns"] = list(kwargs["columns"])
            self._writer = _native.JsonlWriter(dst, **kwargs)

    def write_jsonl(self, data: str | bytes) -> None:
        if isinstance(data, bytes):
            data = data.decode("utf-8")
        self._writer.write_jsonl(data)

    def write_rows(self, rows: Iterable[Any]) -> int:
        written = 0
        batch: list[str] = []
        for row in rows:
            batch.append(_json.dumps(row, ensure_ascii=False, separators=(",", ":")))
            written += 1
            if len(batch) >= 2048:
                self.write_jsonl("\n".join(batch) + "\n")
                batch.clear()
        if batch:
            self.write_jsonl("\n".join(batch) + "\n")
        return written

    def finish(self) -> None:
        if self._finished:
            return
        self._writer.finish()
        self._finished = True

    def close(self) -> None:
        if __backend__ == "ctypes":
            self._writer.close()

    def __enter__(self) -> "JsonlWriter":
        return self

    def __exit__(self, exc_type: Any, *rest: Any) -> None:
        try:
            if exc_type is None:
                self.finish()
        finally:
            self.close()


class Session:
    """Cache parsed workbooks, sheet profiles and search payloads across calls."""

    def __init__(
        self,
        capacity: int = 8,
        profile_capacity: int = 64,
        search_capacity: int = 16,
        memory_map: bool | None = None,
    ) -> None:
        if __backend__ == "native":
            self._session = _native.Session(
                capacity=capacity,
                memory_map=memory_map,
                profile_capacity=profile_capacity,
                search_capacity=search_capacity,
            )
        else:
            self._session = _ctype_api.Session(
                capacity=capacity,
                profile_capacity=profile_capacity,
                search_capacity=search_capacity,
            )
        self._closed = False

    def _live(self) -> Any:
        if self._closed:
            raise ValueError("session is closed")
        return self._session

    def close(self) -> None:
        if self._closed:
            return
        try:
            self._session.clear()
        finally:
            self._closed = True
            if __backend__ == "ctypes":
                self._session.close()

    @property
    def closed(self) -> bool:
        return self._closed

    def clear(self) -> None:
        self._live().clear()

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def __getattr__(self, name: str) -> Any:
        if name.startswith("_"):
            raise AttributeError(name)
        return getattr(self._session, name)

    def __len__(self) -> int:
        return len(self._live())

    def info(self, file: str, **options: Any) -> dict[str, Any]:
        if __backend__ == "ctypes":
            return self._live().info(file, **options)
        return _native_call_on(self._live(), "info", _INFO, file, options)

    def preview(self, file: str, **options: Any) -> dict[str, Any]:
        if __backend__ == "ctypes":
            return self._live().preview(file, **options)
        return _native_call_on(self._live(), "preview_sheet", _PREVIEW, file, options)

    def schema(self, file: str, **options: Any) -> dict[str, Any]:
        if __backend__ == "ctypes":
            return self._live().schema(file, **options)
        return _native_call_on(self._live(), "schema", _REPORT, file, options)

    def probe(self, file: str, **options: Any) -> dict[str, Any]:
        if __backend__ == "ctypes":
            return self._live().probe(file, **options)
        return _native_call_on(self._live(), "probe", _REPORT, file, options)

    def search(self, file: str, query: str, **options: Any) -> dict[str, Any]:
        if __backend__ == "ctypes":
            return self._live().search(file, query, **options)
        return _native_call_on(self._live(), "search", _SEARCH, file, options,
                               query=query)

    def iter_rows(self, file: str, **options: Any) -> RowIterator:
        return iter_rows(file, **options)


def _native_call_on(target: Any, name: str, keys: Sequence[str], file: str,
                    options: Mapping[str, Any], query: str | None = None) -> Any:
    kwargs = _pick(options, keys)
    if name == "search" and "case_sensitive" in options:
        kwargs["ignore_case"] = not bool(options["case_sensitive"])
    function = getattr(target, name)
    if query is None:
        return function(file, **kwargs)
    return function(file, query, **kwargs)


def _native_iter_options(options: Mapping[str, Any]) -> tuple[dict[str, Any], str]:
    shape = options.get("shape", "arrays")
    if shape not in ("arrays", "objects"):
        raise ValueError("shape must be 'arrays' or 'objects'")
    kwargs = _pick(options, _ITER)
    kwargs["as_dicts"] = shape == "objects"
    kwargs["as_tuples"] = False
    return kwargs, shape


def info(file: str, **options: Any) -> dict[str, Any]:
    if __backend__ == "ctypes":
        return _ctype_api.info(file, **options)
    return _native_call("info", _INFO, file, **options)


def preview(file: str, **options: Any) -> dict[str, Any]:
    if __backend__ == "ctypes":
        return _ctype_api.preview(file, **options)
    return _native_call("preview_sheet", _PREVIEW, file, **options)


def schema(file: str, **options: Any) -> dict[str, Any]:
    if __backend__ == "ctypes":
        return _ctype_api.schema(file, **options)
    return _native_call("schema", _REPORT, file, **options)


def probe(file: str, **options: Any) -> dict[str, Any]:
    if __backend__ == "ctypes":
        return _ctype_api.probe(file, **options)
    return _native_call("probe", _REPORT, file, **options)


def search(file: str, query: str, **options: Any) -> dict[str, Any]:
    if __backend__ == "ctypes":
        return _ctype_api.search(file, query, **options)
    kwargs = _pick(options, _SEARCH)
    if "case_sensitive" in options:
        kwargs["ignore_case"] = not bool(options["case_sensitive"])
    return _native.search(file, query, **kwargs)


def convert(src: str, dst: str, **options: Any) -> None:
    if __backend__ == "ctypes":
        _ctype_api.convert(src, dst, **options)
        return
    _native.convert(src, dst, **_pick(options, _CONVERT))


def iter_rows(file: str, **options: Any) -> RowIterator:
    if __backend__ == "ctypes":
        return RowIterator(_ctype_api.iter_rows(file, **options))
    kwargs, _shape = _native_iter_options(options)
    inner = _native.iter_rows(file, **kwargs)
    meta = {
        "version": 1,
        "ok": True,
        "file": file,
        "format": _format_name(file, options.get("format", "")),
        "sheet": inner.sheet,
        "header_row": inner.header_row,
        "header_auto": options.get("header_row") == "auto",
        "columns": list(inner.columns),
    }
    return RowIterator(inner, meta)


def write_rows(dst: str, rows: Iterable[Any], columns: Sequence[str] | None = None,
               **options: Any) -> int:
    if columns is not None:
        options["columns"] = list(columns)
    if __backend__ == "ctypes":
        return _ctype_api.write_rows(dst, rows, **options)
    with JsonlWriter(dst, **options) as writer:
        return writer.write_rows(rows)


def _native_extra(name: str) -> Any:
    if __backend__ == "native" and hasattr(_native, name):
        return getattr(_native, name)
    raise AttributeError("fastxl has no attribute %r (native backend only)" % name)


def version() -> str:
    if __backend__ == "ctypes":
        return _ctype_api.version()
    return str(_native.__version__)


def abi_version() -> int:
    if __backend__ == "ctypes":
        return _ctype_api.abi_version()
    return 1


def __getattr__(name: str) -> Any:
    if name.startswith("_"):
        raise AttributeError(name)
    return _native_extra(name)


__all__ = [
    "Error",
    "JsonlWriter",
    "RowIterator",
    "Session",
    "abi_version",
    "convert",
    "info",
    "iter_rows",
    "preview",
    "probe",
    "schema",
    "search",
    "version",
    "write_rows",
]
