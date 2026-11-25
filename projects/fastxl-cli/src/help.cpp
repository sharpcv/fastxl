#include "args.hpp"

#include <string>
#include <string_view>

namespace fastxl_cli {

namespace {

constexpr std::string_view kExitSummary =
    "Exit codes: 0 success, 1 usage, 2 input/format, 3 output, 4 internal.\n"
    "Run `fastxl-cli errors` for every error code and its meaning.\n";

} // namespace

std::string help_text() {
  return
      "fastxl-cli — XLSX/CSV/JSONL conversion and probing\n"
      "\n"
      "Usage:\n"
      "  fastxl-cli <command> [options]\n"
      "  fastxl-cli <command> --help\n"
      "  fastxl-cli --help | --version\n"
      "  fastxl-cli errors [--format text|table|jsonl]\n"
      "\n"
      "Commands:\n"
      "  info <file.xlsx>           workbook metadata (xlsx only)\n"
      "  probe <file>               per-column profile: types, nulls, stats, samples\n"
      "  schema <file>              column schema for code generation\n"
      "  preview <file>             rows with A1 refs and header candidates\n"
      "  search <file> --query Q    keyword/regex search with A1 hits and context\n"
      "  convert <input> --out P    stream xlsx/csv/jsonl to another format\n"
      "  errors                     list error codes, exit codes and meanings\n"
      "\n"
      "Run `fastxl-cli <command> --help` for the options and examples of one command.\n"
      "\n"
      "Input and output:\n"
      "  <file> is a path, or `-` for stdin; `convert --out -` writes stdout.\n"
      "  `-` needs --from (and --to for convert) because stdin/stdout have no extension.\n"
      "  Reading is bounded and streaming; xlsx output is written atomically.\n"
      "  Legacy .xls, encrypted workbooks and bzip2/lzma/zstd ZIP entries are rejected.\n"
      "\n"
      "Common options (run `fastxl-cli <command> --help` for the exact set):\n"
      "      --sheet <name|index>   worksheet to read (default: first worksheet)\n"
      "      --header-row N|auto    header row, 1-based; `auto` detects it (default 1)\n"
      "      --no-header            treat every row as data (header_row = 0)\n"
      "      --max-rows N           stop after N data rows (0 = no limit)\n"
      "      --from FMT             input format: xlsx, csv or jsonl\n"
      "      --delimiter C          CSV delimiter: `,` `;` or `tab`\n"
      "      --no-infer             keep CSV/JSONL values as text\n"
      "      --fill-gaps            emit empty cells between populated cells\n"
      "      --format [FMT]         output style: text (also when omitted), table or jsonl\n"
      "      --json                 alias for --format jsonl\n"
      "  -h, --help                 show help; with a command, show command help\n"
      "      --version              print fastxl and linked library versions, then exit\n"
      "\n" +
      std::string(kExitSummary) +
      "\n"
      "Examples:\n"
      "  fastxl-cli info book.xlsx\n"
      "  fastxl-cli schema book.xlsx --sheet Data --header-row auto --format table\n"
      "  fastxl-cli probe book.xlsx --format jsonl --jobs 4 --max-rows 200000\n"
      "  fastxl-cli preview book.xlsx --rows 20 --columns 1,3-5\n"
      "  fastxl-cli convert data.csv --out book.xlsx --header-row 1\n";
}

std::string command_help(const std::string& command) {
  if (command == "info")
    return
        "fastxl-cli info — workbook metadata\n"
        "\n"
        "Usage:\n"
        "  fastxl-cli info <file.xlsx> [--from xlsx] [--format text|table|jsonl]\n"
        "\n"
        "Reads the OPC package without parsing cell data: sheet order, names, ids,\n"
        "relationships, part names, visibility, uncompressed sizes and the shared-string\n"
        "count. Only ZIP/OPC workbooks (.xlsx/.xlsm) are accepted.\n"
        "\n"
        "Options:\n"
        "      --from xlsx     explicit input format (xlsx is the only supported one)\n"
        "      --format [FMT]  text (default), table or jsonl\n"
        "      --json          alias for --format jsonl\n"
        "  -h, --help          show this help\n"
        "\n"
        "Output:\n"
        "  Key/value text by default; --format table renders the sheet list, --format\n"
        "  jsonl emits {\"version\":1,\"ok\":true,...}.\n"
        "\n" +
        std::string(kExitSummary) +
        "\n"
        "Example:\n"
        "  fastxl-cli info book.xlsx --format table\n";

  if (command == "probe")
    return
        "fastxl-cli probe — per-column profile\n"
        "\n"
        "Usage:\n"
        "  fastxl-cli probe <file> [options]\n"
        "  fastxl-cli probe - --from csv|jsonl|xlsx [options]\n"
        "\n"
        "Streams one worksheet and reports, per column: inferred type, nulls, distinct\n"
        "count (capped), top values, numeric statistics, samples, empty/ragged rows and\n"
        "formula/error flags. Memory stays bounded; --jobs N parallelizes xlsx profiling\n"
        "for both buffered and streamed (large) worksheet parts.\n"
        "\n"
        "Options:\n"
        "      --sheet <name|index>   worksheet (default: first worksheet)\n"
        "      --header-row N|auto    header row, 1-based (default 1)\n"
        "      --no-header            no header; every row is data\n"
        "      --from FMT             input format: xlsx, csv or jsonl (required for `-`)\n"
        "      --delimiter C          CSV delimiter: `,` `;` or tab\n"
        "      --max-rows N           scan at most N data rows (0 = all)\n"
        "      --sample N             sample values per column (default 5)\n"
        "      --top N                top values per column, 0..1000 (default 5)\n"
        "      --distinct-cap N       distinct-value cap (default 10000)\n"
        "      --jobs N               0 = auto, 1 = single thread, max 1024\n"
        "      --regions              profile each detected data island separately\n"
        "      --max-bytes N          JSONL output budget in bytes (0 = unlimited)\n"
        "      --no-infer             keep CSV/JSONL values as text\n"
        "      --fill-gaps            emit empty cells between populated cells\n"
        "      --format [FMT]         text (default), table or jsonl\n"
        "      --json                 alias for --format jsonl\n"
        "  -h, --help                 show this help\n"
        "\n" +
        std::string(kExitSummary) +
        "\n"
        "Example:\n"
        "  fastxl-cli probe book.xlsx --sheet Data --format table --jobs 4\n";

  if (command == "schema")
    return
        "fastxl-cli schema — column schema for code generation\n"
        "\n"
        "Usage:\n"
        "  fastxl-cli schema <file> [options]\n"
        "  fastxl-cli schema - --from csv|jsonl|xlsx [options]\n"
        "\n"
        "Profiles one sheet and emits columns with name, inferred type, nullability,\n"
        "statistics and sample values, plus header detection metadata. The default text\n"
        "layout is an aligned table; --format jsonl emits the compact machine-facing\n"
        "contract for code generation.\n"
        "\n"
        "Options:\n"
        "      --sheet <name|index>   worksheet (default: first worksheet)\n"
        "      --header-row N|auto    header row, 1-based (default 1)\n"
        "      --no-header            no header; every row is data\n"
        "      --from FMT             input format: xlsx, csv or jsonl (required for `-`)\n"
        "      --delimiter C          CSV delimiter: `,` `;` or tab\n"
        "      --max-rows N           scan at most N data rows (0 = all)\n"
        "      --sample N             sample values per column (default 5)\n"
        "      --top N                top values per column, 0..1000 (default 5)\n"
        "      --distinct-cap N       distinct-value cap (default 10000)\n"
        "      --jobs N               0 = auto, 1 = single thread, max 1024\n"
        "      --regions              split each sheet into data islands and profile them\n"
        "      --max-bytes N          JSONL output budget in bytes (0 = unlimited)\n"
        "      --no-infer             keep CSV/JSONL values as text\n"
        "      --fill-gaps            emit empty cells between populated cells\n"
        "      --format [FMT]         text (default), table or jsonl\n"
        "      --json                 alias for --format jsonl\n"
        "  -h, --help                 show this help\n"
        "\n" +
        std::string(kExitSummary) +
        "\n"
        "Example:\n"
        "  fastxl-cli schema book.xlsx --sheet Data --header-row auto --format jsonl\n";

  if (command == "preview")
    return
        "fastxl-cli preview — rows with A1 references\n"
        "\n"
        "Usage:\n"
        "  fastxl-cli preview <file> [--rows N] [--range A1:F20] [--columns 1,3-5]\n"
        "  fastxl-cli preview - --from csv|jsonl|xlsx [options]\n"
        "\n"
        "Returns the first rows as written, each cell carrying ref, column, type and\n"
        "canonical text, plus derived column names, header candidates and sheet metadata\n"
        "(used range, auto filter, frozen pane, hidden columns). The default text layout\n"
        "is an aligned table with at most 12 columns; --format jsonl emits the full\n"
        "JSON payload.\n"
        "\n"
        "Options:\n"
        "      --sheet <name|index>   worksheet (default: first worksheet)\n"
        "      --header-row N|auto    header row, 1-based (default 1)\n"
        "      --no-header            no header; every row is data\n"
        "      --rows N               displayed rows, 1..1048576 (default 20)\n"
        "      --range A1:F20         explicit used range (start/end cell)\n"
        "      --columns 1,3-5        1-based column projection, in the given order\n"
        "      --max-cell-bytes N     truncate cell text, 1..1048576 (default 256)\n"
        "      --from FMT             input format: xlsx, csv or jsonl (required for `-`)\n"
        "      --delimiter C          CSV delimiter: `,` `;` or tab\n"
        "      --no-infer             keep CSV/JSONL values as text\n"
        "      --fill-gaps            emit empty cells between populated cells\n"
        "      --format [FMT]         text (default), table or jsonl\n"
        "      --json                 alias for --format jsonl\n"
        "  -h, --help                 show this help\n"
        "\n" +
        std::string(kExitSummary) +
        "\n"
        "Example:\n"
        "  fastxl-cli preview book.xlsx --rows 20 --columns 1,3-5\n";

  if (command == "search")
    return
        "fastxl-cli search — keyword and regex search across a workbook\n"
        "\n"
        "Usage:\n"
        "  fastxl-cli search <file> --query TEXT [options]\n"
        "  fastxl-cli search - --from csv|jsonl|xlsx --query TEXT [options]\n"
        "\n"
        "Scans cell text (and formula text when the cached text does not match) and\n"
        "returns sheet, A1 ref, row/column, type and a small context window. Text\n"
        "output prints one line per hit; --format jsonl emits the machine contract.\n"
        "\n"
        "Options:\n"
        "      --query TEXT           required search text\n"
        "      --regex                ECMAScript regular expression\n"
        "      --case-sensitive       disable ASCII case folding (default: ignore case)\n"
        "      --context N            rows/columns of context, 0..10 (default 1)\n"
        "      --max-hits N            stop after N matches, 0 = unlimited (default 100)\n"
        "      --max-cell-bytes N     truncate matched/context text (default 256)\n"
        "      --sheet <name|index>   restrict to one worksheet\n"
        "      --from FMT             input format: xlsx, csv or jsonl (required for `-`)\n"
        "      --delimiter C          CSV delimiter: `,` `;` or tab\n"
        "      --no-infer             keep CSV/JSONL values as text\n"
        "      --format [FMT]         text (default), table or jsonl\n"
        "      --json                 alias for --format jsonl\n"
        "  -h, --help                 show this help\n"
        "\n" +
        std::string(kExitSummary) +
        "\n"
        "Example:\n"
        "  fastxl-cli search book.xlsx --query invoice --context 2 --format jsonl\n";

  if (command == "convert")
    return
        "fastxl-cli convert — stream between formats\n"
        "\n"
        "Usage:\n"
        "  fastxl-cli convert <input> --out <output> [options]\n"
        "  fastxl-cli convert - --from csv --out book.xlsx\n"
        "  fastxl-cli convert book.xlsx --out - --to csv\n"
        "\n"
        "Reads one table and writes another format. xlsx -> xlsx copies every worksheet\n"
        "(or the selected one) and preserves cell types; other pairs stream rows. xlsx\n"
        "output is written atomically (temporary file, then replace).\n"
        "\n"
        "Options:\n"
        "      --out, -o PATH         output path or `-` (required)\n"
        "      --from FMT             input format: xlsx, csv or jsonl\n"
        "      --to FMT               output format: xlsx, csv or jsonl\n"
        "      --sheet <name|index>   worksheet to read (xlsx input only)\n"
        "      --header-row N         header row, 1-based; 0 = no header (default 1)\n"
        "      --delimiter C          CSV delimiter: `,` `;` or tab\n"
        "      --no-infer             keep CSV/JSONL values as text\n"
        "      --normalize            write the canonical text form of every value\n"
        "      --fill-gaps            emit empty cells between populated cells\n"
        "      --columns 1,3-5        1-based column projection, in the given order\n"
        "      --limit N              convert at most N data rows (0 = all)\n"
        "      --crlf                 CSV output uses CRLF line endings\n"
        "      --bom                  CSV output starts with a UTF-8 BOM\n"
        "      --sheet-name NAME      xlsx output sheet name (default Sheet1)\n"
        "      --compress-level N     xlsx ZIP level, 0..9 (default 6)\n"
        "  -h, --help                 show this help\n"
        "\n" +
        std::string(kExitSummary) +
        "\n"
        "Examples:\n"
        "  fastxl-cli convert data.csv --out book.xlsx --header-row 1\n"
        "  fastxl-cli convert book.xlsx --out data.jsonl --to jsonl --sheet Data\n";

  if (command == "errors")
    return
        "fastxl-cli errors — exit and error code reference\n"
        "\n"
        "Usage:\n"
        "  fastxl-cli errors [--format text|table|jsonl]\n"
        "\n"
        "Lists every exit code and every stable error code with its exit code and a\n"
        "one-line explanation. `UsageError` covers command-line mistakes; the other names\n"
        "are fastxl::ErrorCode values from the library.\n"
        "\n"
        "Options:\n"
        "      --format [FMT] text (default), table or jsonl\n"
        "      --json        alias for --format jsonl\n"
        "  -h, --help        show this help\n"
        "\n"
        "Example:\n"
        "  fastxl-cli errors --format jsonl\n";

  return {};
}

} // namespace fastxl_cli
