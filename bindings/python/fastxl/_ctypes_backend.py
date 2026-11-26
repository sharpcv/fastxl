"""ctypes backend for the fastxl C ABI.

The router in ``fastxl/__init__.py`` selects this backend when the native
pybind11 module is unavailable (or ``FASTXL_BACKEND=ctypes`` is set). Works on
any CPython 3.8+ (including free-threaded builds), PyPy and any other FFI host:
the library is loaded at runtime, so no per-Python-version rebuild is needed.
ctypes releases the GIL around each C call, so methods on one Session can run
concurrently from several threads.

    import fastxl
    with fastxl.Session() as session:
        schema = session.schema("book.xlsx", header_row="auto")
    for row in fastxl.iter_rows("book.xlsx", batch_size=4096):
        ...

Buffers returned by the library are always released with fastxl_buffer_free.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import json
import os
from collections import deque
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping, Sequence

try:  # optional accelerator: parsing dominates per-batch JSONL decoding
    import orjson as _orjson
except ImportError:  # pragma: no cover - depends on the environment
    _orjson = None


def _loads(payload: str) -> Any:
    if _orjson is not None:
        return _orjson.loads(payload)
    return json.loads(payload)

__all__ = [
    "Error",
    "Session",
    "RowIterator",
    "JsonlWriter",
    "abi_version",
    "version",
    "info",
    "preview",
    "schema",
    "probe",
    "search",
    "convert",
    "iter_rows",
    "write_rows",
]

_LIB_NAMES = ("libfastxl.so", "libfastxl.dylib", "fastxl.dll")


class Error(RuntimeError):
    """A non-zero fastxl status.

    Attributes match the native backend: `code`/`name` (stable ErrorCode name),
    `status` (1 + ErrorCode), `exit_code` (CLI process exit code) and
    `message`. `status`/`name`/`message` are kept for older callers.
    """

    def __init__(self, status: int, name: str, message: str, exit_code: int = 1) -> None:
        super().__init__(message or name)
        self.status = status
        self.name = name
        self.code = name
        self.exit_code = exit_code
        self.message = message


class _Buffer(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("size", ctypes.c_size_t)]


def _candidate_paths() -> Iterator[Path]:
    explicit = os.environ.get("FASTXL_LIBRARY")
    if explicit:
        yield Path(explicit)
    here = Path(__file__).resolve().parent
    roots = [
        here,
        here.parent / "lib",
        here.parent.parent / "lib",
        here.parent.parent / "build" / "lib",
        Path.cwd() / "build" / "lib",
    ]
    for root in roots:
        for name in _LIB_NAMES:
            yield root / name


def _load_library() -> ctypes.CDLL:
    for candidate in _candidate_paths():
        if candidate.is_file():
            return ctypes.CDLL(str(candidate))
    found = ctypes.util.find_library("fastxl")
    if found:
        return ctypes.CDLL(found)
    raise OSError(
        "cannot locate libfastxl.so/fastxl.dll; set FASTXL_LIBRARY or place the "
        "library next to fastxl_ctypes"
    )


_lib = _load_library()

_STATUS = ctypes.c_int32
_SESSION = ctypes.c_void_p
_ITER = ctypes.c_void_p
_WRITER = ctypes.c_void_p
_BUFFER = ctypes.POINTER(_Buffer)
_CSTR = ctypes.c_char_p


def _declare(name: str, restype: Any, *argtypes: Any) -> None:
    function = getattr(_lib, name)
    function.restype = restype
    function.argtypes = list(argtypes)


_declare("fastxl_abi_version", ctypes.c_uint32)
_declare("fastxl_version_string", _CSTR)
_declare("fastxl_error_name", _CSTR, _STATUS)
_declare("fastxl_error_exit_code", _STATUS, _STATUS)
_declare("fastxl_buffer_free", None, _BUFFER)

_declare(
    "fastxl_session_create",
    _STATUS,
    _CSTR,
    ctypes.POINTER(_SESSION),
    _BUFFER,
)
_declare("fastxl_session_clear", None, _SESSION)
_declare("fastxl_session_destroy", None, _SESSION)

for _name in ("info", "preview", "schema", "probe"):
    _declare(
        "fastxl_%s_json" % _name,
        _STATUS,
        _SESSION,
        _CSTR,
        _CSTR,
        _BUFFER,
        _BUFFER,
    )

_declare(
    "fastxl_search_json",
    _STATUS,
    _SESSION,
    _CSTR,
    _CSTR,
    _CSTR,
    _BUFFER,
    _BUFFER,
)

_declare("fastxl_convert", _STATUS, _CSTR, _CSTR, _CSTR, _BUFFER)

_declare(
    "fastxl_iter_open",
    _STATUS,
    _CSTR,
    _CSTR,
    ctypes.POINTER(_ITER),
    _BUFFER,
    _BUFFER,
)
_declare(
    "fastxl_iter_next_batch",
    _STATUS,
    _ITER,
    ctypes.c_size_t,
    _BUFFER,
    ctypes.POINTER(ctypes.c_size_t),
    _BUFFER,
)
_declare("fastxl_iter_close", None, _ITER)
_declare("fastxl_iter_destroy", None, _ITER)

_declare("fastxl_writer_open", _STATUS, _CSTR, _CSTR, ctypes.POINTER(_WRITER), _BUFFER)
_declare(
    "fastxl_writer_write_jsonl",
    _STATUS,
    _WRITER,
    ctypes.c_char_p,
    ctypes.c_size_t,
    _BUFFER,
)
_declare("fastxl_writer_finish", _STATUS, _WRITER, _BUFFER)
_declare("fastxl_writer_destroy", None, _WRITER)


def _encode(text: str | None) -> bytes | None:
    if text is None:
        return None
    return text.encode("utf-8")


def _options_json(options: Mapping[str, Any] | None, **kwargs: Any) -> bytes | None:
    merged: dict[str, Any] = {}
    if options:
        merged.update(options)
    merged.update({key: value for key, value in kwargs.items() if value is not None})
    if not merged:
        return None
    return json.dumps(merged, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def _take(buffer: _Buffer) -> str:
    try:
        if not buffer.data:
            return ""
        return ctypes.string_at(buffer.data, buffer.size).decode("utf-8")
    finally:
        _lib.fastxl_buffer_free(ctypes.byref(buffer))


def _error(status: int, buffer: _Buffer) -> Error:
    message = _take(buffer)
    name = "Internal"
    raw = _lib.fastxl_error_name(status)
    if raw:
        name = raw.decode("utf-8", "replace")
    return Error(status, name, message, int(_lib.fastxl_error_exit_code(status)))


def _json_result(status: int, out: _Buffer, err: _Buffer) -> dict[str, Any]:
    payload = _take(out)
    if status != 0:
        raise _error(status, err)
    _lib.fastxl_buffer_free(ctypes.byref(err))
    return _loads(payload)


def abi_version() -> int:
    return int(_lib.fastxl_abi_version())


def version() -> str:
    raw = _lib.fastxl_version_string()
    return raw.decode("utf-8") if raw else ""


def _call_report(
    function: Any,
    session: "_SessionHandle | None",
    file: str,
    options: Mapping[str, Any] | None,
    query: str | None = None,
) -> dict[str, Any]:
    out = _Buffer()
    err = _Buffer()
    handle = session.handle if session is not None else None
    path = _encode(file)
    opts = _options_json(options)
    if query is None:
        status = function(handle, path, opts, ctypes.byref(out), ctypes.byref(err))
    else:
        status = function(
            handle, path, _encode(query), opts, ctypes.byref(out), ctypes.byref(err)
        )
    return _json_result(status, out, err)


class _SessionHandle:
    def __init__(self, handle: int) -> None:
        self.handle = handle

    def close(self) -> None:
        if self.handle:
            _lib.fastxl_session_destroy(self.handle)
            self.handle = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:  # interpreter shutdown can clear module globals
            pass


class Session:
    """Caches parsed workbooks, profiles and search payloads across calls."""

    def __init__(
        self,
        capacity: int = 8,
        profile_capacity: int = 64,
        search_capacity: int = 16,
    ) -> None:
        options = _options_json(
            {
                "capacity": capacity,
                "profile_capacity": profile_capacity,
                "search_capacity": search_capacity,
            }
        )
        handle = _SESSION()
        err = _Buffer()
        status = _lib.fastxl_session_create(options, ctypes.byref(handle), ctypes.byref(err))
        if status != 0:
            raise _error(status, err)
        self._session = _SessionHandle(int(handle.value or 0))

    def close(self) -> None:
        self._session.close()

    @property
    def closed(self) -> bool:
        return self._session.handle is None

    def _live(self) -> "_SessionHandle":
        if self._session.handle is None:
            raise ValueError("session is closed")
        return self._session

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def clear(self) -> None:
        _lib.fastxl_session_clear(self._live().handle)

    def info(self, file: str, **options: Any) -> dict[str, Any]:
        return _call_report(_lib.fastxl_info_json, self._live(), file, options)

    def preview(self, file: str, **options: Any) -> dict[str, Any]:
        return _call_report(_lib.fastxl_preview_json, self._live(), file, options)

    def schema(self, file: str, **options: Any) -> dict[str, Any]:
        return _call_report(_lib.fastxl_schema_json, self._live(), file, options)

    def probe(self, file: str, **options: Any) -> dict[str, Any]:
        return _call_report(_lib.fastxl_probe_json, self._live(), file, options)

    def search(self, file: str, query: str, **options: Any) -> dict[str, Any]:
        return _call_report(_lib.fastxl_search_json, self._live(), file, options, query)

    def iter_rows(self, file: str, **options: Any) -> "RowIterator":
        # Row streams own a reader; they are not cached by the session.
        return RowIterator(file, options)


def info(file: str, **options: Any) -> dict[str, Any]:
    return _call_report(_lib.fastxl_info_json, None, file, options)


def preview(file: str, **options: Any) -> dict[str, Any]:
    return _call_report(_lib.fastxl_preview_json, None, file, options)


def schema(file: str, **options: Any) -> dict[str, Any]:
    return _call_report(_lib.fastxl_schema_json, None, file, options)


def probe(file: str, **options: Any) -> dict[str, Any]:
    return _call_report(_lib.fastxl_probe_json, None, file, options)


def search(file: str, query: str, **options: Any) -> dict[str, Any]:
    return _call_report(_lib.fastxl_search_json, None, file, options, query)


def convert(src: str, dst: str, **options: Any) -> None:
    err = _Buffer()
    status = _lib.fastxl_convert(
        _encode(src), _encode(dst), _options_json(options), ctypes.byref(err)
    )
    if status != 0:
        raise _error(status, err)
    _lib.fastxl_buffer_free(ctypes.byref(err))


class RowIterator(Iterator[Any]):
    """Lazy batched row iterator over the C ABI JSONL stream.

    Rows are lists for shape="arrays" (default) and dicts for shape="objects".
    `close()` releases the reader deterministically; iteration stops afterwards
    and `closed` is true once the stream is exhausted or closed.
    """

    def __init__(self, file: str, options: Mapping[str, Any]) -> None:
        self._options = dict(options)
        self._batch_size = int(self._options.pop("batch_size", 4096))
        if "shape" not in self._options:
            self._options["shape"] = "arrays"
        self._objects = self._options["shape"] == "objects"
        iter_handle = _ITER()
        meta = _Buffer()
        err = _Buffer()
        status = _lib.fastxl_iter_open(
            _encode(file),
            _options_json(self._options),
            ctypes.byref(iter_handle),
            ctypes.byref(meta),
            ctypes.byref(err),
        )
        payload = _take(meta)
        if status != 0:
            raise _error(status, err)
        _lib.fastxl_buffer_free(ctypes.byref(err))
        self._handle = iter_handle
        self._meta = _loads(payload)
        self._buffer: deque[Any] = deque()
        self._exhausted = False

    @property
    def closed(self) -> bool:
        return not self._handle

    def close(self) -> None:
        if self._handle:
            _lib.fastxl_iter_close(self._handle)
            _lib.fastxl_iter_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "RowIterator":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    @property
    def meta(self) -> dict[str, Any]:
        return dict(self._meta)

    @property
    def columns(self) -> list[str]:
        return list(self._meta.get("columns", []))

    @property
    def sheet(self) -> str:
        return self._meta.get("sheet", "")

    @property
    def header_row(self) -> int:
        return int(self._meta.get("header_row", 0))

    def _fill(self) -> None:
        if self._exhausted or not self._handle:
            self._exhausted = True
            return
        chunk = _Buffer()
        err = _Buffer()
        rows = ctypes.c_size_t()
        status = _lib.fastxl_iter_next_batch(
            self._handle,
            self._batch_size,
            ctypes.byref(chunk),
            ctypes.byref(rows),
            ctypes.byref(err),
        )
        payload = _take(chunk)
        if status != 0:
            raise _error(status, err)
        _lib.fastxl_buffer_free(ctypes.byref(err))
        if rows.value == 0:
            self._exhausted = True
            self.close()
            return
        # Only split on "\n": str.splitlines() also breaks on U+2028/U+2029,
        # which are legal unescaped characters inside JSON strings.
        self._buffer.extend(_loads(line) for line in payload.split("\n") if line)

    def __iter__(self) -> "RowIterator":
        return self

    def __next__(self) -> Any:
        while not self._buffer:
            if self._exhausted:
                raise StopIteration
            self._fill()
        return self._buffer.popleft()


def iter_rows(file: str, **options: Any) -> RowIterator:
    return RowIterator(file, options)


class JsonlWriter:
    """Streaming writer: append JSONL batches, then finish (atomically)."""

    def __init__(self, dst: str, **options: Any) -> None:
        handle = _WRITER()
        err = _Buffer()
        status = _lib.fastxl_writer_open(
            _encode(dst), _options_json(options), ctypes.byref(handle), ctypes.byref(err)
        )
        if status != 0:
            raise _error(status, err)
        _lib.fastxl_buffer_free(ctypes.byref(err))
        self._handle = handle
        self._finished = False

    def write_jsonl(self, data: str | bytes) -> None:
        if isinstance(data, str):
            data = data.encode("utf-8")
        err = _Buffer()
        status = _lib.fastxl_writer_write_jsonl(
            self._handle, data, len(data), ctypes.byref(err)
        )
        if status != 0:
            raise _error(status, err)
        _lib.fastxl_buffer_free(ctypes.byref(err))

    def write_rows(self, rows: Iterable[Any]) -> int:
        written = 0
        batch: list[str] = []
        for row in rows:
            batch.append(json.dumps(row, ensure_ascii=False, separators=(",", ":")))
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
        err = _Buffer()
        status = _lib.fastxl_writer_finish(self._handle, ctypes.byref(err))
        if status != 0:
            raise _error(status, err)
        _lib.fastxl_buffer_free(ctypes.byref(err))
        self._finished = True

    def close(self) -> None:
        if self._handle:
            _lib.fastxl_writer_destroy(self._handle)
            self._handle = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:  # interpreter shutdown can clear module globals
            pass

    def __enter__(self) -> "JsonlWriter":
        return self

    def __exit__(self, exc_type: Any, *rest: Any) -> None:
        try:
            if exc_type is None:
                self.finish()
        finally:
            self.close()


def write_rows(dst: str, rows: Iterable[Any], columns: Sequence[str] | None = None,
               **options: Any) -> int:
    """Write dicts or sequences to xlsx/csv/jsonl in streaming batches."""
    if columns is not None:
        options["columns"] = list(columns)
    with JsonlWriter(dst, **options) as writer:
        return writer.write_rows(rows)
