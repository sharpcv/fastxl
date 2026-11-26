// C ABI implementation. The report builders, readers and writers are the same
// C++ code the CLI uses, so the SDK and the CLI cannot drift apart.

#include "fastxl_c.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <simdutf.h>

#include "args.hpp"
#include "common.hpp"
#include "fastxl/detail/json_budget.hpp"
#include "fastxl/fastxl.hpp"
#include "headers.hpp"
#include "jsonl_writer.hpp"
#include "reports.hpp"

namespace {

using namespace fastxl;
using fastxl_cli::Args;
using fastxl_cli::OutputFormat;
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;

constexpr int32_t kStatusOk = 0;

int32_t status_for(ErrorCode code) {
  return static_cast<int32_t>(code) + 1;
}

// ------------------------------------------------------------------ buffers

void zero_buffer(fastxl_buffer_t* buffer) {
  if (buffer == nullptr) return;
  buffer->data = nullptr;
  buffer->size = 0;
}

void release_buffer(fastxl_buffer_t* buffer) {
  if (buffer == nullptr) return;
  std::free(buffer->data);
  zero_buffer(buffer);
}

// Overwrites the output struct without touching its previous contents: callers
// routinely pass uninitialised stack structs, so a free() here would corrupt
// the heap. fastxl_buffer_free() owns releasing anything the library returned.
bool store_buffer(fastxl_buffer_t* buffer, std::string_view text) {
  if (buffer == nullptr) return true;
  char* data = static_cast<char*>(std::malloc(text.size() + 1));
  if (data == nullptr) return false;
  if (!text.empty()) std::memcpy(data, text.data(), text.size());
  data[text.size()] = '\0';
  buffer->data = data;
  buffer->size = text.size();
  return true;
}

int32_t fail(fastxl_buffer_t* out_err, int32_t status, std::string_view message) {
  store_buffer(out_err, message);
  return status;
}

template <class Function>
int32_t guarded(fastxl_buffer_t* out_err, Function&& function) noexcept {
  try {
    function();
    zero_buffer(out_err);
    return kStatusOk;
  } catch (const fastxl_cli::UsageError& error) {
    return fail(out_err, status_for(ErrorCode::InvalidValue), error.what());
  } catch (const Exception& error) {
    return fail(out_err, status_for(error.code()), error.what());
  } catch (const std::exception& error) {
    return fail(out_err, status_for(ErrorCode::Internal), error.what());
  } catch (...) {
    return fail(out_err, status_for(ErrorCode::Internal), "unknown error");
  }
}

void emit_json(const Json& root, size_t max_bytes, fastxl_buffer_t* out_json) {
  Json budgeted = root;
  if (max_bytes != 0) fastxl::detail::apply_output_budget(budgeted, max_bytes);
  const std::string payload = budgeted.dump();
  if (!store_buffer(out_json, payload))
    throw Exception(ErrorCode::Internal, "cannot allocate the JSON buffer");
}

// ----------------------------------------------------------------- options

Json parse_options(const char* text) {
  if (text == nullptr || *text == '\0') return Json::object();
  Json value;
  try {
    value = Json::parse(text);
  } catch (const std::exception& error) {
    throw Exception(ErrorCode::InvalidValue,
                    std::string("options_json is not valid JSON: ") + error.what());
  }
  if (!value.is_object())
    throw Exception(ErrorCode::InvalidValue, "options_json must be a JSON object");
  return value;
}

bool has(const Json& options, const char* key) { return options.contains(key); }

std::string opt_string(const Json& options, const char* key, std::string fallback = {}) {
  if (!options.contains(key)) return fallback;
  const Json& value = options.at(key);
  if (!value.is_string())
    throw Exception(ErrorCode::InvalidValue,
                    std::string("option '") + key + "' must be a string");
  return value.get<std::string>();
}

uint64_t opt_uint(const Json& options, const char* key, uint64_t fallback = 0) {
  if (!options.contains(key)) return fallback;
  const Json& value = options.at(key);
  if (value.is_number_unsigned()) return value.get<uint64_t>();
  if (value.is_number_integer()) {
    const int64_t signed_value = value.get<int64_t>();
    if (signed_value < 0)
      throw Exception(ErrorCode::InvalidValue,
                      std::string("option '") + key + "' must not be negative");
    return static_cast<uint64_t>(signed_value);
  }
  throw Exception(ErrorCode::InvalidValue,
                  std::string("option '") + key + "' must be an integer");
}

bool opt_bool(const Json& options, const char* key, bool fallback = false) {
  if (!options.contains(key)) return fallback;
  const Json& value = options.at(key);
  if (!value.is_boolean())
    throw Exception(ErrorCode::InvalidValue,
                    std::string("option '") + key + "' must be a boolean");
  return value.get<bool>();
}

char opt_delimiter(const Json& options) {
  const std::string text = opt_string(options, "delimiter");
  if (text.empty()) return '\0';
  if (text == "\\t" || text == "\t") return '\t';
  if (text.size() != 1)
    throw Exception(ErrorCode::InvalidValue, "option 'delimiter' must be one character");
  return text[0];
}

std::pair<uint32_t, bool> header_from_options(const Json& options) {
  if (!options.contains("header_row")) return {1, false};
  const Json& value = options.at("header_row");
  if (value.is_string() && value.get<std::string>() == "auto") return {1, true};
  if (value.is_number_unsigned()) {
    const uint64_t row = value.get<uint64_t>();
    if (row > 1048576)
      throw Exception(ErrorCode::InvalidValue, "option 'header_row' is out of range");
    return {static_cast<uint32_t>(row), false};
  }
  if (value.is_number_integer()) {
    const int64_t row = value.get<int64_t>();
    if (row < 0 || row > 1048576)
      throw Exception(ErrorCode::InvalidValue, "option 'header_row' is out of range");
    return {static_cast<uint32_t>(row), false};
  }
  throw Exception(ErrorCode::InvalidValue,
                  "option 'header_row' must be a row number or \"auto\"");
}

void apply_header(Args& args, const Json& options) {
  const auto [row, automatic] = header_from_options(options);
  args.header_row = row;
  args.header_auto = automatic;
}

// Probe/preview/search shared read options.
void apply_report_options(Args& args, const Json& options) {
  args.from = opt_string(options, "format");
  args.sheet = opt_string(options, "sheet");
  apply_header(args, options);
  args.infer = opt_bool(options, "infer", true);
  args.fill_gaps = opt_bool(options, "fill_gaps", false);
  args.delimiter = opt_delimiter(options);
  args.max_rows = static_cast<size_t>(opt_uint(options, "max_rows", 0));
  args.sample = static_cast<size_t>(opt_uint(options, "sample", 5));
  args.top = static_cast<size_t>(opt_uint(options, "top", 5));
  args.distinct_cap = static_cast<size_t>(opt_uint(options, "distinct_cap", 10000));
  args.jobs = static_cast<unsigned>(opt_uint(options, "jobs", 0));
  args.regions = opt_bool(options, "regions", false);
  args.max_bytes = static_cast<size_t>(opt_uint(options, "max_bytes", 0));
}

size_t max_bytes_of(const Json& options) {
  return static_cast<size_t>(opt_uint(options, "max_bytes", 0));
}

void key_field(std::string& key, std::string_view value) {
  key += std::to_string(value.size());
  key += ':';
  key.append(value.data(), value.size());
}

std::string utf8_path_text(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string file_stamp_key(const std::string& file) {
  std::error_code ec;
  const fs::path path = fastxl_cli::path_from_utf8(file).lexically_normal();
  const uintmax_t size = fs::file_size(path, ec);
  std::string key;
  key_field(key, utf8_path_text(path));
  key_field(key, ec ? std::string("?") : std::to_string(size));
  ec.clear();
  const auto mtime = fs::last_write_time(path, ec);
  key_field(key, ec ? std::string("?") : std::to_string(mtime.time_since_epoch().count()));
  return key;
}

std::string cache_key(const char* kind, const std::string& file, const Json& options) {
  // Sort option keys so {"a":1,"b":2} and {"b":2,"a":1} share one entry.
  nlohmann::json relevant = options;
  relevant.erase("max_bytes");
  std::string key;
  key_field(key, kind);
  key_field(key, file_stamp_key(file));
  key_field(key, relevant.dump());
  return key;
}

// ------------------------------------------------------------------- caches

class LruCache {
public:
  void set_capacity(size_t capacity) noexcept { capacity_ = capacity == 0 ? 1 : capacity; }

  bool get(const std::string& key, Json& out) {
    const auto found = items_.find(key);
    if (found == items_.end()) return false;
    order_.splice(order_.begin(), order_, found->second.position);
    out = found->second.value;
    return true;
  }

  void put(const std::string& key, const Json& value) {
    const auto found = items_.find(key);
    if (found != items_.end()) {
      found->second.value = value;
      order_.splice(order_.begin(), order_, found->second.position);
      return;
    }
    while (items_.size() >= capacity_ && !order_.empty()) {
      items_.erase(order_.back());
      order_.pop_back();
    }
    order_.push_front(key);
    items_.emplace(key, Entry{value, order_.begin()});
  }

  void clear() {
    items_.clear();
    order_.clear();
  }

private:
  struct Entry {
    Json value;
    std::list<std::string>::iterator position;
  };
  size_t capacity_ = 8;
  std::list<std::string> order_;
  std::unordered_map<std::string, Entry> items_;
};

} // namespace

struct fastxl_session {
  mutable std::mutex mutex;
  LruCache info;
  LruCache previews;
  LruCache profiles;
  LruCache searches;
};

namespace {

Json cached_report(fastxl_session_t* session, LruCache* cache, const std::string& key,
                   const std::function<Json()>& build) {
  if (session != nullptr) {
    std::lock_guard<std::mutex> lock(session->mutex);
    Json hit;
    if (cache->get(key, hit)) return hit;
  }
  Json root = build();
  if (session != nullptr) {
    std::lock_guard<std::mutex> lock(session->mutex);
    cache->put(key, root);
  }
  return root;
}

} // namespace

// ---------------------------------------------------------- exported surface

extern "C" {

uint32_t fastxl_abi_version(void) { return FASTXL_C_ABI_VERSION; }

const char* fastxl_version_string(void) { return FASTXL_VERSION; }

const char* fastxl_error_name(fastxl_status_t status) {
  if (status == kStatusOk) return "OK";
  if (status < 1 || status > static_cast<int32_t>(ErrorCode::Count)) return "Unknown";
  return fastxl::to_string(static_cast<ErrorCode>(status - 1)).data();
}

int32_t fastxl_error_exit_code(fastxl_status_t status) {
  if (status == kStatusOk) return 0;
  if (status < 1 || status > static_cast<int32_t>(ErrorCode::Count)) return 1;
  return fastxl::exit_code(static_cast<ErrorCode>(status - 1));
}

void fastxl_buffer_free(fastxl_buffer_t* buf) { release_buffer(buf); }

fastxl_status_t fastxl_session_create(const char* options_json,
                                      fastxl_session_t** out_session,
                                      fastxl_buffer_t* out_err) {
  if (out_session != nullptr) *out_session = nullptr;
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (out_session == nullptr)
      throw Exception(ErrorCode::InvalidValue, "out_session must not be NULL");
    const Json options = parse_options(options_json);
    auto session = std::make_unique<fastxl_session>();
    session->info.set_capacity(static_cast<size_t>(opt_uint(options, "capacity", 8)));
    session->previews.set_capacity(
        static_cast<size_t>(opt_uint(options, "preview_capacity", 4)));
    session->profiles.set_capacity(
        static_cast<size_t>(opt_uint(options, "profile_capacity", 64)));
    session->searches.set_capacity(
        static_cast<size_t>(opt_uint(options, "search_capacity", 16)));
    *out_session = session.release();
  });
}

void fastxl_session_clear(fastxl_session_t* session) {
  if (session == nullptr) return;
  std::lock_guard<std::mutex> lock(session->mutex);
  session->info.clear();
  session->previews.clear();
  session->profiles.clear();
  session->searches.clear();
}

void fastxl_session_destroy(fastxl_session_t* session) { delete session; }

fastxl_status_t fastxl_info_json(fastxl_session_t* session, const char* file_utf8,
                                 const char* options_json, fastxl_buffer_t* out_json,
                                 fastxl_buffer_t* out_err) {
  zero_buffer(out_json);
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (file_utf8 == nullptr || *file_utf8 == '\0')
      throw Exception(ErrorCode::InvalidValue, "file_utf8 must not be empty");
    const std::string file = file_utf8;
    const Json options = parse_options(options_json);
    Args args;
    args.command = "info";
    args.positional = {file};
    args.format = OutputFormat::Jsonl;
    args.from = opt_string(options, "format");
    const size_t max_bytes = max_bytes_of(options);
    LruCache* cache = session != nullptr ? &session->info : nullptr;
    Json root = cached_report(
        session, cache, cache_key("info", file, options),
        [&] { return fastxl_cli::info_report(args); });
    emit_json(root, max_bytes, out_json);
  });
}

fastxl_status_t fastxl_preview_json(fastxl_session_t* session, const char* file_utf8,
                                    const char* options_json, fastxl_buffer_t* out_json,
                                    fastxl_buffer_t* out_err) {
  zero_buffer(out_json);
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (file_utf8 == nullptr || *file_utf8 == '\0')
      throw Exception(ErrorCode::InvalidValue, "file_utf8 must not be empty");
    const std::string file = file_utf8;
    const Json options = parse_options(options_json);
    Args args;
    args.command = "preview";
    args.positional = {file};
    args.format = OutputFormat::Jsonl;
    apply_report_options(args, options);
    args.preview_rows = static_cast<size_t>(opt_uint(options, "rows", 20));
    args.range = opt_string(options, "range");
    args.columns = opt_string(options, "columns");
    args.max_cell_bytes = static_cast<size_t>(opt_uint(options, "max_cell_bytes", 256));
    const size_t max_bytes = max_bytes_of(options);
    LruCache* cache = session != nullptr ? &session->previews : nullptr;
    Json root = cached_report(
        session, cache, cache_key("preview", file, options),
        [&] { return fastxl_cli::preview_report(args); });
    emit_json(root, max_bytes, out_json);
  });
}

fastxl_status_t fastxl_schema_json(fastxl_session_t* session, const char* file_utf8,
                                   const char* options_json, fastxl_buffer_t* out_json,
                                   fastxl_buffer_t* out_err) {
  zero_buffer(out_json);
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (file_utf8 == nullptr || *file_utf8 == '\0')
      throw Exception(ErrorCode::InvalidValue, "file_utf8 must not be empty");
    const std::string file = file_utf8;
    const Json options = parse_options(options_json);
    Args args;
    args.command = "schema";
    args.positional = {file};
    args.format = OutputFormat::Jsonl;
    apply_report_options(args, options);
    const size_t max_bytes = max_bytes_of(options);
    LruCache* cache = session != nullptr ? &session->profiles : nullptr;
    Json root = cached_report(
        session, cache, cache_key("schema", file, options),
        [&] { return fastxl_cli::schema_report(args); });
    emit_json(root, max_bytes, out_json);
  });
}

fastxl_status_t fastxl_probe_json(fastxl_session_t* session, const char* file_utf8,
                                  const char* options_json, fastxl_buffer_t* out_json,
                                  fastxl_buffer_t* out_err) {
  zero_buffer(out_json);
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (file_utf8 == nullptr || *file_utf8 == '\0')
      throw Exception(ErrorCode::InvalidValue, "file_utf8 must not be empty");
    const std::string file = file_utf8;
    const Json options = parse_options(options_json);
    Args args;
    args.command = "probe";
    args.positional = {file};
    args.format = OutputFormat::Jsonl;
    apply_report_options(args, options);
    const size_t max_bytes = max_bytes_of(options);
    LruCache* cache = session != nullptr ? &session->profiles : nullptr;
    Json root = cached_report(
        session, cache, cache_key("probe", file, options),
        [&] { return fastxl_cli::build_probe_report(args).root; });
    emit_json(root, max_bytes, out_json);
  });
}

fastxl_status_t fastxl_search_json(fastxl_session_t* session, const char* file_utf8,
                                   const char* query_utf8, const char* options_json,
                                   fastxl_buffer_t* out_json, fastxl_buffer_t* out_err) {
  zero_buffer(out_json);
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (file_utf8 == nullptr || *file_utf8 == '\0')
      throw Exception(ErrorCode::InvalidValue, "file_utf8 must not be empty");
    const std::string file = file_utf8;
    const Json options = parse_options(options_json);
    Args args;
    args.command = "search";
    args.positional = {file};
    args.format = OutputFormat::Jsonl;
    args.from = opt_string(options, "format");
    args.sheet = opt_string(options, "sheet");
    args.query = query_utf8 != nullptr ? query_utf8 : "";
    if (args.query.empty())
      throw Exception(ErrorCode::InvalidValue, "query_utf8 must not be empty");
    args.regex = opt_bool(options, "regex", false);
    if (has(options, "ignore_case"))
      args.case_sensitive = !opt_bool(options, "ignore_case", true);
    else
      args.case_sensitive = opt_bool(options, "case_sensitive", false);
    args.context = static_cast<uint32_t>(opt_uint(options, "context", 1));
    args.max_hits = static_cast<size_t>(opt_uint(options, "max_hits", 100));
    args.max_cell_bytes = static_cast<size_t>(opt_uint(options, "max_cell_bytes", 256));
    args.infer = opt_bool(options, "infer", true);
    args.delimiter = opt_delimiter(options);
    args.fill_gaps = opt_bool(options, "fill_gaps", false);
    const size_t max_bytes = max_bytes_of(options);
    Json key_options = options;
    key_options["query"] = args.query;
    LruCache* cache = session != nullptr ? &session->searches : nullptr;
    Json root = cached_report(
        session, cache, cache_key("search", file, key_options),
        [&] { return fastxl_cli::search_report(args); });
    emit_json(root, max_bytes, out_json);
  });
}

fastxl_status_t fastxl_convert(const char* src_utf8, const char* dst_utf8,
                               const char* options_json, fastxl_buffer_t* out_err) {
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (src_utf8 == nullptr || *src_utf8 == '\0' || dst_utf8 == nullptr ||
        *dst_utf8 == '\0')
      throw Exception(ErrorCode::InvalidValue, "src_utf8 and dst_utf8 must not be empty");
    const Json options = parse_options(options_json);
    Args args;
    args.command = "convert";
    args.positional = {src_utf8};
    args.out = dst_utf8;
    args.from = opt_string(options, "from", opt_string(options, "format"));
    args.to = opt_string(options, "to");
    args.sheet = opt_string(options, "sheet");
    apply_header(args, options);
    args.infer = opt_bool(options, "infer", true);
    args.delimiter = opt_delimiter(options);
    args.fill_gaps = opt_bool(options, "fill_gaps", false);
    args.limit = static_cast<size_t>(opt_uint(options, "limit", 0));
    args.normalize = opt_bool(options, "normalize", false);
    args.columns = opt_string(options, "columns");
    args.crlf = opt_bool(options, "crlf", false);
    args.bom = opt_bool(options, "bom", false);
    args.sheet_name = opt_string(options, "sheet_name", "Sheet1");
    args.compression_level =
        static_cast<int>(opt_uint(options, "compression_level", 6));
    fastxl_cli::convert_report(args);
  });
}

} // extern "C"

// ------------------------------------------------------------------ iter/writer

namespace {

Format resolve_named_format(const std::string& name, const std::string& path,
                           const char* option) {
  if (!name.empty()) {
    const Format format = format_from_string(name);
    if (format == Format::Auto)
      throw Exception(ErrorCode::UnsupportedFormat,
                      std::string("unknown ") + option + ": " + name);
    return format;
  }
  const Format format = format_from_path(fastxl_cli::path_from_utf8(path));
  if (format == Format::Auto)
    throw Exception(ErrorCode::UnsupportedFormat,
                    "cannot detect the format of " + path + "; pass " + option);
  return format;
}

using fastxl_cli::unique_names;

// Appends `text` as a JSON string literal. Escaping and validation mirror
// nlohmann's dump(): the five short control escapes, \u00XX for the remaining
// C0 controls, `\"`/`\\`, raw UTF-8 otherwise, and an error on invalid UTF-8.
void append_json_string(std::string& out, std::string_view text) {
  if (!simdutf::validate_utf8(text.data(), text.size()))
    // nlohmann::dump() reported invalid UTF-8 as a plain std::exception, which
    // guarded() maps to Internal; keep that status stable.
    throw Exception(ErrorCode::Internal, "cannot encode invalid UTF-8 text as JSON");
  out += '"';
  size_t run = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char byte = static_cast<unsigned char>(text[i]);
    if (byte >= 0x20 && byte != '"' && byte != '\\') continue;
    out.append(text.data() + run, i - run);
    run = i + 1;
    switch (byte) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: {
      char escaped[7];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
      out.append(escaped, sizeof(escaped) - 1);
      break;
    }
    }
  }
  out.append(text.data() + run, text.size() - run);
  out += '"';
}

void append_json_cell(std::string& out, const CellView& cell, std::string& scratch) {
  switch (cell.type) {
  case CellType::Null:
  case CellType::Invalid: out += "null"; return;
  case CellType::Int: {
    char digits[32];
    const auto end = std::to_chars(digits, digits + sizeof(digits), cell.int_value).ptr;
    out.append(digits, end);
    return;
  }
  case CellType::Number:
    if (!std::isfinite(cell.number)) {
      append_json_string(out, to_text(cell, scratch));
      return;
    }
    // json::dump() formats doubles with nlohmann::detail::to_chars(); calling
    // it directly keeps the exact spelling (1.0, not 1) without building a
    // throwaway json value and output adapter per cell.
    {
      char digits[32];
      const char* end =
          nlohmann::detail::to_chars(digits, digits + sizeof(digits), cell.number);
      out.append(digits, static_cast<size_t>(end - digits));
    }
    return;
  case CellType::Boolean: out += cell.boolean ? "true" : "false"; return;
  case CellType::SharedString:
  case CellType::Text: append_json_string(out, cell.text); return;
  case CellType::Date:
  case CellType::DateTime:
  case CellType::Time:
  case CellType::Error: append_json_string(out, to_text(cell, scratch)); return;
  }
  out += "null";
}

} // namespace

struct fastxl_iter {
  std::unique_ptr<fastxl::Reader> reader;
  std::vector<std::string> columns;
  std::string file;
  std::string format_name;
  std::string sheet_name;
  uint32_t header_row = 1;
  bool header_auto = false;
  bool objects = false;
  bool closed = false;
  bool exhausted = false;
  bool truncated = false;
  size_t offset = 0;
  size_t limit = 0;
  size_t skipped = 0;
  size_t emitted = 0;
  RowView row;
  std::string scratch;

  void close() {
    reader.reset();
    closed = true;
  }
};

extern "C" {

fastxl_status_t fastxl_iter_open(const char* file_utf8, const char* options_json,
                                 fastxl_iter_t** out_iter, fastxl_buffer_t* out_meta_json,
                                 fastxl_buffer_t* out_err) {
  if (out_iter != nullptr) *out_iter = nullptr;
  zero_buffer(out_meta_json);
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (file_utf8 == nullptr || *file_utf8 == '\0')
      throw Exception(ErrorCode::InvalidValue, "file_utf8 must not be empty");
    if (out_iter == nullptr)
      throw Exception(ErrorCode::InvalidValue, "out_iter must not be NULL");
    const Json options = parse_options(options_json);

    auto iter = std::make_unique<fastxl_iter>();
    iter->file = file_utf8;
    iter->offset = static_cast<size_t>(opt_uint(options, "offset", 0));
    iter->limit = static_cast<size_t>(opt_uint(options, "limit", 0));
    const std::string shape = opt_string(options, "shape", "arrays");
    if (shape != "arrays" && shape != "objects")
      throw Exception(ErrorCode::InvalidValue,
                      "option 'shape' must be \"arrays\" or \"objects\"");
    iter->objects = shape == "objects";

    const std::string sheet = opt_string(options, "sheet");
    const std::string format = opt_string(options, "format");
    const Format resolved = resolve_named_format(format, iter->file, "format");
    iter->format_name = std::string(to_string(resolved));

    ReadOptions read;
    read.format = resolved;
    read.sheet = sheet;
    read.infer_types = opt_bool(options, "infer", true);
    read.delimiter = opt_delimiter(options);
    read.fill_gaps = opt_bool(options, "fill_gaps", false);
    read.max_rows = 0;
    read.lenient_columns = true;
    read.max_columns =
        static_cast<uint32_t>(opt_uint(options, "max_columns", 16384));
    const auto [header_row, header_auto] = header_from_options(options);
    read.header_row = header_row;
    iter->header_row = header_row;
    iter->header_auto = header_auto;

    const fs::path path = fastxl_cli::path_from_utf8(iter->file);
    if (header_auto) {
      if (resolved == Format::Xlsx) {
        WorkBook workbook = WorkBook::open(path);
        read.header_row =
            fastxl_cli::detect_header_xlsx(workbook,
                                           fastxl_cli::resolve_sheet_index(workbook, sheet))
                .row;
      } else if (resolved == Format::Csv) {
        ReadOptions scan = read;
        scan.header_row = 0;
        scan.max_rows = 0;
        char delimiter = read.delimiter;
        if (delimiter == '\0' && path.extension() == ".tsv") delimiter = '\t';
        if (delimiter == '\0') delimiter = ',';
        scan.column_count = fastxl_cli::csv_column_count(path, delimiter, 20);
        auto scan_reader = open_reader(path, scan);
        read.header_row = fastxl_cli::detect_header_table(*scan_reader).row;
        read.column_count = scan.column_count;
      } else {
        read.header_row = 0;
      }
    }
    if (resolved == Format::Jsonl) read.header_row = 0;
    iter->header_row = read.header_row;
    iter->reader = open_reader(path, read);
    const std::string_view sheet_name = iter->reader->sheet_name();
    iter->sheet_name = sheet_name.empty() ? "table" : std::string(sheet_name);
    iter->columns = unique_names(iter->reader->columns());

    Json meta;
    meta["version"] = 1;
    meta["ok"] = true;
    meta["file"] = iter->file;
    meta["format"] = iter->format_name;
    meta["sheet"] = iter->sheet_name;
    meta["header_row"] = read.header_row;
    meta["header_auto"] = iter->header_auto;
    meta["columns"] = iter->columns;
    if (!store_buffer(out_meta_json, meta.dump()))
      throw Exception(ErrorCode::Internal, "cannot allocate the meta buffer");
    // Only hand over ownership after every fallible step finished; otherwise a
    // throwing dump()/store_buffer() leaks the reader and its file descriptor.
    *out_iter = iter.release();
  });
}

fastxl_status_t fastxl_iter_next_batch(fastxl_iter_t* iter, size_t max_rows,
                                       fastxl_buffer_t* out_chunk, size_t* out_rows_read,
                                       fastxl_buffer_t* out_err) {
  zero_buffer(out_chunk);
  zero_buffer(out_err);
  if (out_rows_read != nullptr) *out_rows_read = 0;
  if (iter == nullptr) {
    return fail(out_err, status_for(ErrorCode::InvalidValue), "iter must not be NULL");
  }
  return guarded(out_err, [&] {
    if (iter->closed || iter->exhausted || iter->reader == nullptr) {
      iter->close();
      if (!store_buffer(out_chunk, std::string_view{}))
        throw Exception(ErrorCode::Internal, "cannot allocate the chunk buffer");
      return;
    }
    const size_t wanted = max_rows == 0 ? 4096 : max_rows;
    std::string chunk;
    size_t produced = 0;
    while (produced < wanted) {
      if (!iter->reader->next(iter->row)) {
        iter->exhausted = true;
        iter->close();
        break;
      }
      if (iter->skipped < iter->offset) {
        ++iter->skipped;
        continue;
      }
      if (iter->limit != 0 && iter->emitted >= iter->limit) {
        iter->truncated = true;
        iter->exhausted = true;
        iter->close();
        break;
      }
      if (iter->objects) {
        chunk += '{';
        for (size_t i = 0; i < iter->row.size() && i < iter->columns.size(); ++i) {
          if (i != 0) chunk += ',';
          append_json_string(chunk, iter->columns[i]);
          chunk += ':';
          append_json_cell(chunk, iter->row[i], iter->scratch);
        }
        chunk += '}';
      } else {
        chunk += '[';
        for (size_t i = 0; i < iter->row.size(); ++i) {
          if (i != 0) chunk += ',';
          append_json_cell(chunk, iter->row[i], iter->scratch);
        }
        chunk += ']';
      }
      chunk += '\n';
      ++iter->emitted;
      ++produced;
    }
    if (!store_buffer(out_chunk, chunk))
      throw Exception(ErrorCode::Internal, "cannot allocate the chunk buffer");
    if (out_rows_read != nullptr) *out_rows_read = produced;
  });
}

void fastxl_iter_close(fastxl_iter_t* iter) {
  if (iter != nullptr) iter->close();
}

void fastxl_iter_destroy(fastxl_iter_t* iter) {
  if (iter == nullptr) return;
  iter->close();
  delete iter;
}

} // extern "C"

struct fastxl_writer {
  explicit fastxl_writer(fastxl_cli::JsonlWriterOptions options)
      : stream(std::move(options)) {}
  fastxl_cli::JsonlWriter stream;
};

extern "C" {

fastxl_status_t fastxl_writer_open(const char* dst_utf8, const char* options_json,
                                   fastxl_writer_t** out_writer,
                                   fastxl_buffer_t* out_err) {
  if (out_writer != nullptr) *out_writer = nullptr;
  zero_buffer(out_err);
  return guarded(out_err, [&] {
    if (dst_utf8 == nullptr || *dst_utf8 == '\0')
      throw Exception(ErrorCode::InvalidValue, "dst_utf8 must not be empty");
    if (out_writer == nullptr)
      throw Exception(ErrorCode::InvalidValue, "out_writer must not be NULL");
    const Json options = parse_options(options_json);

    fastxl_cli::JsonlWriterOptions config;
    config.dst = dst_utf8;
    config.format = resolve_named_format(opt_string(options, "format"), config.dst, "format");
    config.header = opt_bool(options, "header", true);
    config.delimiter = opt_delimiter(options);
    config.crlf = opt_bool(options, "crlf", false);
    config.bom = opt_bool(options, "bom", false);
    config.fsync = opt_bool(options, "fsync", true);
    config.sheet_name = opt_string(options, "sheet_name", "Sheet1");
    config.compression_level = static_cast<int>(opt_uint(options, "compression_level", 6));

    if (options.contains("columns")) {
      const Json& list = options.at("columns");
      if (!list.is_array() || list.empty())
        throw Exception(ErrorCode::InvalidValue,
                        "option 'columns' must be a non-empty array of strings");
      std::vector<std::string> names;
      names.reserve(list.size());
      for (const Json& item : list) {
        if (!item.is_string())
          throw Exception(ErrorCode::InvalidValue,
                          "option 'columns' must contain strings");
        names.push_back(item.get<std::string>());
      }
      std::vector<std::string_view> views;
      views.reserve(names.size());
      for (const std::string& name : names) views.emplace_back(name);
      config.columns = unique_names(views);
    }
    auto writer = std::make_unique<fastxl_writer>(std::move(config));
    *out_writer = writer.release();
  });
}

fastxl_status_t fastxl_writer_write_jsonl(fastxl_writer_t* writer, const char* jsonl_data,
                                          size_t jsonl_len, fastxl_buffer_t* out_err) {
  zero_buffer(out_err);
  if (writer == nullptr)
    return fail(out_err, status_for(ErrorCode::InvalidValue), "writer must not be NULL");
  return guarded(out_err, [&] {
    if (jsonl_data == nullptr) return;
    const std::string_view payload =
        jsonl_len == 0 ? std::string_view(jsonl_data) : std::string_view(jsonl_data, jsonl_len);
    writer->stream.write_jsonl(payload);
  });
}

fastxl_status_t fastxl_writer_finish(fastxl_writer_t* writer, fastxl_buffer_t* out_err) {
  zero_buffer(out_err);
  if (writer == nullptr)
    return fail(out_err, status_for(ErrorCode::InvalidValue), "writer must not be NULL");
  return guarded(out_err, [&] { writer->stream.finish(); });
}

void fastxl_writer_destroy(fastxl_writer_t* writer) { delete writer; }

} // extern "C"
