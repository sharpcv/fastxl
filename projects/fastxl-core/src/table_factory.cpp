#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "fastxl/error.hpp"
#include "fastxl/table.hpp"
#include "table_internal.hpp"
#include "utf8.hpp"

namespace fastxl {

using detail::table::InputTarget;
using detail::table::OutputTarget;
using detail::table::open_csv_table_reader;
using detail::table::open_csv_table_writer;
using detail::table::open_jsonl_table_reader;
using detail::table::open_jsonl_table_writer;
using detail::table::open_xlsx_table_reader;
using detail::table::open_xlsx_table_writer;
using detail::table::validate_columns;
using detail::text::to_utf8;

namespace {

Format resolve_requested(const std::filesystem::path& path, Format requested) {
  if (requested != Format::Auto) return requested;
  Format detected = format_from_path(path);
  if (detected == Format::Auto)
    throw_error(ErrorCode::UnsupportedFormat,
                "cannot detect the table format from " + to_utf8(path) + "; pass --from/--to");
  return detected;
}

bool is_tsv(const std::filesystem::path& path) {
  std::string extension = to_utf8(path.extension());
  for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return extension == ".tsv";
}

OutputTarget file_output(const std::filesystem::path& path, const WriteOptions& options) {
  return OutputTarget::to_file(path, options.fsync,
                               options.temp_dir.value_or(std::filesystem::path()));
}

} // namespace

std::unique_ptr<Reader> open_reader(const std::filesystem::path& path,
                                    const ReadOptions& options) {
  const Format format = resolve_requested(path, options.format);
  switch (format) {
  case Format::Xlsx: return open_xlsx_table_reader(path, options);
  case Format::Csv:
    return open_csv_table_reader(InputTarget::from_file(path), options, is_tsv(path));
  case Format::Jsonl: return open_jsonl_table_reader(InputTarget::from_file(path), options);
  case Format::Auto: break;
  }
  throw_error(ErrorCode::UnsupportedFormat, "unsupported table format");
}

std::unique_ptr<Writer> open_writer(const std::filesystem::path& path,
                                    std::span<const std::string_view> columns,
                                    const WriteOptions& options) {
  validate_columns(columns);
  const Format format = resolve_requested(path, options.format);
  switch (format) {
  case Format::Xlsx: return open_xlsx_table_writer(path, columns, options);
  case Format::Csv:
    return open_csv_table_writer(file_output(path, options), columns, options, is_tsv(path));
  case Format::Jsonl:
    return open_jsonl_table_writer(file_output(path, options), columns, options);
  case Format::Auto: break;
  }
  throw_error(ErrorCode::UnsupportedFormat, "unsupported table format");
}

std::unique_ptr<Reader> open_reader(std::istream& stream, Format format,
                                    const ReadOptions& options) {
  if (format == Format::Auto)
    throw_error(ErrorCode::UnsupportedFormat, "a stream reader needs an explicit format");
  if (format == Format::Xlsx) return open_xlsx_table_reader(stream, options);
  if (format == Format::Csv)
    return open_csv_table_reader(InputTarget::from_stream(stream), options, false);
  if (format == Format::Jsonl)
    return open_jsonl_table_reader(InputTarget::from_stream(stream), options);
  throw_error(ErrorCode::UnsupportedFormat, "unsupported table format");
}

std::unique_ptr<Writer> open_writer(std::ostream& stream, Format format,
                                    std::span<const std::string_view> columns,
                                    const WriteOptions& options) {
  if (format == Format::Auto)
    throw_error(ErrorCode::UnsupportedFormat, "a stream writer needs an explicit format");
  validate_columns(columns);
  if (format == Format::Xlsx) return open_xlsx_table_writer(stream, columns, options);
  if (format == Format::Csv)
    return open_csv_table_writer(OutputTarget::to_stream(stream), columns, options, false);
  if (format == Format::Jsonl)
    return open_jsonl_table_writer(OutputTarget::to_stream(stream), columns, options);
  throw_error(ErrorCode::UnsupportedFormat, "unsupported table format");
}

} // namespace fastxl
