"""Fast native probes for Excel/CSV/JSONL.

The compiled module is ``_fastxl``; this package re-exports its public functions
and ships PEP 561 type information (``_fastxl.pyi`` and ``py.typed``).
"""

import sys

try:
    from . import _fastxl as _native
except ImportError as exc:  # pragma: no cover - depends on the local build
    raise ImportError(
        "fastxl's native module failed to load under Python %d.%d. It must be "
        "built for the interpreter that imports it: `xmake f -y -m release && "
        "xmake build -y fastxl-python`. The CLI fallback still works with "
        "FASTXL_FORCE_CLI=1." % sys.version_info[:2]
    ) from exc

_build_python = getattr(_native, "__build_python__", None)
if _build_python is not None and tuple(_build_python) != sys.version_info[:2]:
    _build_major, _build_minor = _build_python
    raise ImportError(
        "fastxl's native module was built for Python %d.%d but this interpreter "
        "is %d.%d; rebuild it with `xmake f -y -m release && xmake build -y "
        "fastxl-python`."
        % (_build_major, _build_minor, *sys.version_info[:2])
    )

from ._fastxl import (  # noqa: E402
    Error as Error,
    Session as Session,
    info as info,
    preview as preview,
    preview_sheet as preview_sheet,
    probe as probe,
    read_rows as read_rows,
    schema as schema,
    search as search,
    write_xlsx as write_xlsx,
)

__version__: str = "1.1.0"
__license__: str = "MIT"
__all__: list[str] = [
    "Error",
    "Session",
    "info",
    "preview",
    "preview_sheet",
    "probe",
    "read_rows",
    "schema",
    "search",
    "write_xlsx",
]
