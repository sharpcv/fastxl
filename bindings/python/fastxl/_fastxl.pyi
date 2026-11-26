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
    exit_code: int

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

class BudgetColumn(TypedDict):
    reason: Literal["empty", "constant", "budget"]
    index: NotRequired[int]
    name: NotRequired[str]
    letter: NotRequired[str]

class Budget(TypedDict):
    """Report emitted when ``max_bytes`` is set; describes what was folded."""

    max_bytes: int
    applied: bool
    bytes: int
    within_budget: bool
    stages: NotRequired[dict[str, int]]
    stages_count: NotRequired[int]
    folded_columns: NotRequired[list[BudgetColumn]]
    folded_columns_count: NotRequired[int]
    dropped_rows: NotRequired[int]
    dropped_matches: NotRequired[int]

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
    budget: NotRequired[Budget]

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
    budget: NotRequired[Budget]

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
    budget: NotRequired[Budget]

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
    list[dict[str, CellValue]]
    | list[list[CellValue]]
    | list[tuple[CellValue, ...]]
    | list[PreviewRow]
)

class RowIterator:
    """Lazy typed-row iterator returned by ``iter_rows()``.

    Decodes one row per ``next()`` call, so memory stays bounded for very large
    sheets. The reader (and its file handle) is released when the iterator is
    exhausted, garbage collected, or explicitly closed. Use it as a context
    manager (``with fastxl.iter_rows(...) as rows:``) or call :meth:`close` for
    deterministic release, which matters on Windows when breaking out of the
    loop early. Iterating a closed iterator stops.
    """

    def close(self) -> None:
        """Release the reader and close the file handle now (idempotent)."""
    @property
    def closed(self) -> bool: ...
    @property
    def columns(self) -> list[str]: ...
    @property
    def header_row(self) -> int: ...
    @property
    def sheet(self) -> str: ...
    @property
    def rows_read(self) -> int: ...
    @property
    def truncated(self) -> bool: ...
    def __iter__(self) -> "RowIterator": ...
    def __next__(self) -> dict[str, CellValue] | list[CellValue] | tuple[CellValue, ...] | PreviewRow: ...
    def __enter__(self) -> "RowIterator": ...
    def __exit__(self, *args: object) -> None: ...

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
    budget: NotRequired[Budget]

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
    budget: NotRequired[Budget]

def preview_sheet(
    file: str,
    sheet: SheetSelector = None,
    header_row: HeaderRow = 1,
    format: str = "",
    rows: int = 20,
    range: str = "",
    columns: str = "",
    max_cell_bytes: int = 256,
    max_bytes: int = 0,
) -> dict[str, Any]: ...
def info(file: str, format: str = "", max_bytes: int = 0) -> InfoResult: ...
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
    max_bytes: int = 0,
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
    max_bytes: int = 0,
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
    max_bytes: int = 0,
    as_tuples: bool = False,
) -> PreviewResult: ...
def iter_rows(
    file: str,
    sheet: SheetSelector = None,
    header_row: HeaderRow = 1,
    format: str = "",
    limit: int = 0,
    offset: int = 0,
    as_dicts: bool = True,
    as_tuples: bool = False,
    infer: bool = True,
    delimiter: str = "",
    fill_gaps: bool = False,
    max_columns: int = 16384,
    with_cells: bool = False,
) -> RowIterator: ...
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
    max_bytes: int = 0,
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
    max_bytes: int = 0,
    as_tuples: bool = False,
) -> PreviewResult: ...
class Session:
    """Caches workbooks, sheet profiles and search payloads for repeated probes.

    Workbook entries are keyed by path and invalidated when the file's mtime or
    size changes; the least recently used entry is evicted past ``capacity``.
    ``schema``/``probe`` reuse cached sheet profiles (``profile_capacity``), and
    ``search`` reuses the untrimmed payload for an identical query
    (``search_capacity``) so changing ``max_bytes`` never rescans the sheet.
    """

    def __init__(
        self,
        capacity: int = 8,
        memory_map: bool | None = None,
        profile_capacity: int = 64,
        search_capacity: int = 16,
    ) -> None: ...
    def preview_sheet(
        self,
        file: str,
        sheet: SheetSelector = None,
        header_row: HeaderRow = 1,
        format: str = "",
        rows: int = 20,
        range: str = "",
        columns: str = "",
        max_cell_bytes: int = 256,
        max_bytes: int = 0,
    ) -> dict[str, Any]: ...
    def info(
        self, file: str, format: str = "", max_bytes: int = 0
    ) -> InfoResult: ...
    def schema(
        self,
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
        max_bytes: int = 0,
    ) -> SchemaResult: ...
    def probe(
        self,
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
        max_bytes: int = 0,
    ) -> ProbeResult: ...
    def search(
        self,
        file: str,
        query: str,
        regex: bool = False,
        ignore_case: bool = True,
        context: int = 1,
        max_hits: int = 100,
        max_cell_bytes: int = 256,
        sheet: SheetSelector = None,
        format: str = "",
        max_bytes: int = 0,
    ) -> SearchResult: ...
    def read_rows(
        self,
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
        max_bytes: int = 0,
        as_tuples: bool = False,
    ) -> PreviewResult: ...
    def iter_rows(
        self,
        file: str,
        sheet: SheetSelector = None,
        header_row: HeaderRow = 1,
        format: str = "",
        limit: int = 0,
        offset: int = 0,
        as_dicts: bool = True,
        as_tuples: bool = False,
        infer: bool = True,
        delimiter: str = "",
        fill_gaps: bool = False,
        max_columns: int = 16384,
        with_cells: bool = False,
    ) -> RowIterator: ...
    def preview(
        self,
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
        max_bytes: int = 0,
        as_tuples: bool = False,
    ) -> PreviewResult: ...
    def clear(self) -> None: ...
    def stats(self) -> dict[str, int]: ...
    @property
    def capacity(self) -> int: ...
    @property
    def profile_capacity(self) -> int: ...
    @property
    def search_capacity(self) -> int: ...
    @property
    def memory_map(self) -> bool: ...
    def __len__(self) -> int: ...
    def __enter__(self) -> "Session": ...
    def __exit__(self, *args: object) -> None: ...

def write_xlsx(
    file: str,
    rows: Sequence[Mapping[str, CellValue]] | Sequence[Sequence[CellValue]],
    columns: Sequence[str] | None = None,
    sheet_name: str = "Sheet1",
    header: bool = True,
    compression_level: int = 6,
) -> int: ...

def convert(src: str, dst: str, **options: Any) -> None: ...

class JsonlWriter:
    def __init__(
        self,
        dst: str,
        columns: Sequence[str] | None = None,
        format: str = "",
        header: bool = True,
        delimiter: str = "",
        crlf: bool = False,
        bom: bool = False,
        fsync: bool = True,
        sheet_name: str = "Sheet1",
        compression_level: int = 6,
    ) -> None: ...
    def write_jsonl(self, payload: str) -> None: ...
    def finish(self) -> None: ...
