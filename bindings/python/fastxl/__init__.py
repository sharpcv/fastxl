"""Fast native probes for Excel/CSV/JSONL.

The compiled module is ``_fastxl``; this package re-exports its public functions
and ships PEP 561 type information (``_fastxl.pyi`` and ``py.typed``).
"""

from ._fastxl import (
    Error as Error,
    info as info,
    preview as preview,
    probe as probe,
    read_rows as read_rows,
    schema as schema,
    write_xlsx as write_xlsx,
)

__version__: str = "1.0.0"
__license__: str = "MIT"
__all__: list[str] = [
    "Error",
    "info",
    "preview",
    "probe",
    "read_rows",
    "schema",
    "write_xlsx",
]
