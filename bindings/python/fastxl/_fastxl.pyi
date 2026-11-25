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

class DefinedName(TypedDict):
    name: str
    ref: str
    local_sheet_index: NotRequired[int]
    hidden: NotRequired[bool]

class TableInfo(TypedDict):
    name: str
    ref: str
    columns: list[str]
    header_rows: int
    columns_truncated: NotRequired[bool]

class CommentItem(TypedDict):
    ref: str
    author: str
    text: str

class CommentsInfo(TypedDict):
    count: int
    items: list[CommentItem]
    truncated: bool

class PivotTableInfo(TypedDict):
    name: str
    part: str
    cache_id: int
    source_sheet: NotRequired[str]
    source_ref: NotRequired[str]

class ChartRef(TypedDict):
    sheet: str
    ref: str

class ChartInfo(TypedDict):
    part: str
    refs: list[ChartRef]

class FilterCondition(TypedDict):
    operator: str
    value: str

class FilterColumn(TypedDict):
    column: int
    values: list[str]
    custom: list[FilterCondition]

class FormulaRef(TypedDict):
    sheet: str
    ref: str

class FormulaLink(TypedDict):
    cell: str
    formula: str
    refs: list[FormulaRef]

class JoinKey(TypedDict):
    left_sheet: str
    left_column: int
    left_name: str
    right_sheet: str
    right_column: int
    right_name: str
    reason: Literal["same_name", "value_overlap"]
    overlap: NotRequired[float]

class InfoSheet(TypedDict):
    index: int
    name: str
    sheet_id: int
    rel_id: str
    part: str
    kind: Literal["worksheet", "chartsheet"]
    state: str
    uncompressed_bytes: int
    tables: NotRequired[list[TableInfo]]
    tables_truncated: NotRequired[bool]
    comments_count: NotRequired[int]
    pivot_tables_count: NotRequired[int]
    charts_count: NotRequired[int]

class InfoResult(TypedDict):
    version: int
    ok: bool
    file: str
    size_bytes: int
    date1904: bool
    shared_strings: int
    sheets: list[InfoSheet]
    defined_names: NotRequired[list[DefinedName]]
    defined_names_truncated: NotRequired[bool]

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
    enum_values_truncated: NotRequired[bool]
    num_fmt: NotRequired[str]
    style_flags: NotRequired[list[str]]
    formula_cells: NotRequired[int]

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
    range: NotRequired[str]
    merged_cells: NotRequired[list[str]]
    merged_cells_truncated: NotRequired[bool]
    hidden_rows: NotRequired[list[int]]
    hidden_rows_truncated: NotRequired[bool]
    filter_columns: NotRequired[list[FilterColumn]]
    uncached_formulas: NotRequired[int]
    formula_links: NotRequired[list[FormulaLink]]
    formula_links_truncated: NotRequired[bool]
    tables: NotRequired[list[TableInfo]]
    tables_truncated: NotRequired[bool]
    comments: NotRequired[CommentsInfo]
    pivot_tables: NotRequired[list[PivotTableInfo]]
    charts: NotRequired[list[ChartInfo]]
    regions: NotRequired[list["SheetSection"]]

class SchemaResult(TypedDict):
    version: int
    ok: bool
    file: str
    format: str
    header_auto: bool
    sheets: list[SheetSection]
    defined_names: NotRequired[list[DefinedName]]
    defined_names_truncated: NotRequired[bool]
    join_keys: NotRequired[list[JoinKey]]

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
    defined_names: NotRequired[list[DefinedName]]
    merged_cells: NotRequired[list[str]]
    merged_cells_truncated: NotRequired[bool]
    hidden_rows: NotRequired[list[int]]
    hidden_rows_truncated: NotRequired[bool]
    filter_columns: NotRequired[list[FilterColumn]]
    uncached_formulas: NotRequired[int]
    formula_links: NotRequired[list[FormulaLink]]
    formula_links_truncated: NotRequired[bool]
    tables: NotRequired[list[TableInfo]]
    tables_truncated: NotRequired[bool]
    comments: NotRequired[CommentsInfo]
    pivot_tables: NotRequired[list[PivotTableInfo]]
    charts: NotRequired[list[ChartInfo]]
    range: NotRequired[str]
    regions: NotRequired[list[SheetSection]]

class PreviewCell(TypedDict):
    col: int
    letter: str
    ref: str
    type: str
    text: str
    formula: NotRequired[str]
    truncated: NotRequired[bool]
    num_fmt: NotRequired[str]
    style_flags: NotRequired[list[str]]
    font_color: NotRequired[str]
    fill_color: NotRequired[str]
    needs_recalc: NotRequired[bool]

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

class SearchContextCell(TypedDict):
    ref: str
    text: str

class SearchMatch(TypedDict):
    sheet: str
    sheet_index: int
    ref: str
    row: int
    col: int
    type: str
    field: Literal["text", "formula"]
    text: str
    context: NotRequired[list[SearchContextCell]]

class SearchResult(TypedDict):
    version: int
    ok: bool
    file: str
    query: str
    regex: bool
    ignore_case: bool
    matches: list[SearchMatch]
    match_count: int
    truncated: bool
    searched_cells: int

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
    regions: bool = False,
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
    regions: bool = False,
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
def search(
    file: str,
    query: str,
    regex: bool = False,
    ignore_case: bool = True,
    context: int = 1,
    max_hits: int = 100,
    max_cell_bytes: int = 256,
    sheet: SheetSelector = None,
    format: str = "",
) -> SearchResult: ...
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
