"""Type stubs for the native ``_fastxl`` extension module."""

from typing import Any, Literal, Mapping, NotRequired, Sequence, TypeAlias, TypedDict

CellValue: TypeAlias = int | float | bool | str | None
SheetSelector: TypeAlias = str | int | None
HeaderRow: TypeAlias = int | Literal["auto"]

__version__: str
__build_python__: tuple[int, int]

class Error(RuntimeError):
    """Stable fastxl error; ``code`` carries the machine-readable code."""

    code: str

class InfoSheet(TypedDict):
    index: int
    name: str
    sheet_id: str
    rel_id: str
    part: str
    kind: Literal["worksheet", "chartsheet"]
    state: str
    uncompressed_bytes: int

class InfoResult(TypedDict):
    version: int
    ok: bool
    file: str
    size_bytes: int
    date1904: bool
    shared_strings: int
    sheets: list[InfoSheet]

class NumericStats(TypedDict):
    any: bool
    integers: int
    min: float
    max: float
    mean: float
    sum: float
    non_finite: int
    int_min: NotRequired[int]
    int_max: NotRequired[int]

class TextStats(TypedDict):
    min_len: int
    max_len: int

class TopValue(TypedDict):
    value: str
    count: int

class ColumnProfile(TypedDict):
    index: int
    name: str
    type: str
    nullable: bool
    non_null: int
    nulls: int
    distinct: int
    distinct_capped: bool
    numeric: NumericStats
    text: TextStats
    samples: list[str]
    sample_truncated: bool
    top_values: list[TopValue]
    enum_values: list[str]
    flags: list[str]

class SheetSection(TypedDict):
    name: str
    index: int
    header_row: int
    header_rows: int
    rows_scanned: int
    data_rows: int
    max_row: int
    max_col: int
    empty_rows: int
    ragged_rows: int
    merged_ranges: int
    error_cells: int
    has_formulas: bool
    parallel: bool
    hidden_columns: list[int]
    columns: list[ColumnProfile]
    issues: list[str]

class SchemaResult(TypedDict):
    version: int
    ok: bool
    file: str
    format: str
    header_auto: bool
    sheets: list[SheetSection]

class ProbeResult(TypedDict):
    version: int
    ok: bool
    file: str
    sheet_name: str
    sheet_index: int
    header_row: int
    header_rows: int
    rows_scanned: int
    data_rows: int
    max_row: int
    max_col: int
    empty_rows: int
    ragged_rows: int
    merged_ranges: int
    error_cells: int
    has_formulas: bool
    parallel: bool
    columns: list[ColumnProfile]
    issues: list[str]
    header_auto: bool

class PreviewCell(TypedDict):
    col: int
    letter: str
    ref: str
    type: str
    text: str
    formula: NotRequired[str]

class PreviewRow(TypedDict):
    number: int
    cells: list[PreviewCell]

class HeaderCandidate(TypedDict):
    row: int
    score: float
    reasons: list[str]

class SheetInfo(TypedDict):
    name: str

RowsPayload: TypeAlias = (
    list[dict[str, CellValue]] | list[list[CellValue]] | list[PreviewRow]
)

class PreviewResult(TypedDict):
    version: int
    ok: bool
    file: str
    format: str
    sheet: str
    sheet_info: SheetInfo
    header_row: int
    header_auto: bool
    header_candidates: list[HeaderCandidate]
    columns: list[str]
    rows: RowsPayload
    truncated: bool
    rows_read: int
    extra_cells: int

def info(file: str, format: str = "") -> InfoResult: ...
def schema(
    file: str,
    sheet: SheetSelector = None,
    header_row: HeaderRow = 1,
    format: str = "",
    infer: bool = True,
    delimiter: str = "",
    fill_gaps: bool = False,
    max_rows: int = 0,
    sample: int = 5,
    top: int = 5,
    distinct_cap: int = 10000,
    jobs: int = 0,
) -> SchemaResult: ...
def probe(
    file: str,
    sheet: SheetSelector = None,
    header_row: HeaderRow = 1,
    format: str = "",
    infer: bool = True,
    delimiter: str = "",
    fill_gaps: bool = False,
    max_rows: int = 0,
    sample: int = 5,
    top: int = 5,
    distinct_cap: int = 10000,
    jobs: int = 0,
) -> ProbeResult: ...
def read_rows(
    file: str,
    sheet: SheetSelector = None,
    header_row: HeaderRow = 1,
    format: str = "",
    limit: int = 20,
    offset: int = 0,
    as_dicts: bool = True,
    infer: bool = True,
    delimiter: str = "",
    fill_gaps: bool = False,
    max_columns: int = 16384,
    with_cells: bool = False,
) -> PreviewResult: ...
def preview(
    file: str,
    sheet: SheetSelector = None,
    header_row: HeaderRow = 1,
    format: str = "",
    rows: int = 20,
    offset: int = 0,
    as_dicts: bool = True,
    infer: bool = True,
    delimiter: str = "",
    fill_gaps: bool = False,
    max_columns: int = 16384,
    with_cells: bool = False,
) -> PreviewResult: ...
def write_xlsx(
    file: str,
    rows: Sequence[Mapping[str, CellValue]] | Sequence[Sequence[CellValue]],
    columns: Sequence[str] | None = None,
    sheet_name: str = "Sheet1",
    header: bool = True,
    compression_level: int = 6,
) -> int: ...
