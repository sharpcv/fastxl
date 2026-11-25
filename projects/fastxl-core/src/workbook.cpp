#include "fastxl/workbook.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "fastxl/dom.hpp"
#include "fastxl/error.hpp"
#include "flat_map.hpp"
#include "sheet_parser.hpp"
#include "utf8.hpp"
#include "workbook_access.hpp"
#include "xml_cursor.hpp"
#include "xml_part_parsers.hpp"
#include "zip_archive.hpp"

namespace fastxl {

using detail::FlatMap;
using detail::ooxml::FormatKind;
using detail::ooxml::Relationship;
using detail::ooxml::SharedStrings;
using detail::ooxml::StylesheetData;
using detail::ooxml::WorkbookMeta;
using detail::ooxml::parse_relationships;
using detail::ooxml::parse_shared_strings;
using detail::ooxml::parse_stylesheet;
using detail::ooxml::parse_workbook_meta;
using detail::ooxml::parse_table;
using detail::ooxml::parse_comments;
using detail::ooxml::parse_pivot_table;
using detail::ooxml::parse_pivot_cache;
using detail::ooxml::parse_drawing_charts;
using detail::ooxml::parse_chart_refs;
using detail::sheet::SaxHandlerSink;
using detail::sheet::SheetSink;
using detail::sheet::parse_sheet;
using detail::xml::XmlSource;
using detail::zip::ZipArchive;
using detail::zip::ZipEntryInfo;
using detail::zip::ZipEntryStream;

namespace {

std::string percent_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int high = hex(text[i + 1]);
      int low = hex(text[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::string resolve_part(std::string_view base_dir, std::string_view target) {
  std::string path = percent_decode(target);
  std::replace(path.begin(), path.end(), '\\', '/');
  if (!path.empty() && path.front() == '/') {
    path.erase(0, 1);
  } else if (!base_dir.empty()) {
    path = std::string(base_dir) + "/" + path;
  }

  std::vector<std::string> segments;
  size_t start = 0;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    if (slash == std::string::npos) slash = path.size();
    std::string_view segment(path.data() + start, slash - start);
    if (segment.empty() || segment == ".") {
      // skip
    } else if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
    } else {
      segments.emplace_back(segment);
    }
    start = slash + 1;
  }

  std::string out;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i) out.push_back('/');
    out += segments[i];
  }
  return out;
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string dir_name(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string base_name(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

class ZipSource : public XmlSource {
public:
  explicit ZipSource(std::unique_ptr<ZipEntryStream> stream) : stream_(std::move(stream)) {}
  size_t read(char* dst, size_t size) override { return stream_->read(dst, size); }

private:
  std::unique_ptr<ZipEntryStream> stream_;
};

struct RelsIndex {
  FlatMap<std::string, size_t> by_id;
  std::vector<Relationship> rels;
};

RelsIndex index_rels(std::vector<Relationship> rels) {
  RelsIndex index;
  index.rels = std::move(rels);
  for (size_t i = 0; i < index.rels.size(); ++i) index.by_id[index.rels[i].id] = i;
  return index;
}

const Relationship* find_rel_type(const RelsIndex& index, std::string_view suffix) {
  for (const auto& rel : index.rels) {
    if (ends_with(rel.type, suffix)) return &rel;
  }
  return nullptr;
}

constexpr size_t kMaxTables = 64;
constexpr size_t kMaxCommentItems = 50;
constexpr size_t kMaxPivotTables = 32;
constexpr size_t kMaxCharts = 64;

// Reads the table/comment/pivot/chart parts referenced by one worksheet's
// relationship list. Missing parts are skipped; malformed XML still throws.
void resolve_sheet_metadata(const ZipArchive& archive, std::string_view part_dir,
                            const std::vector<Relationship>& relationships,
                            size_t max_metadata_bytes, SheetMetadata* metadata) {
  for (const Relationship& rel : relationships) {
    if (ends_with(rel.type, "/table")) {
      const std::string part = resolve_part(part_dir, rel.target);
      if (part.empty() || !archive.has(part)) continue;
      TableInfo table = parse_table(archive.read_all(part, max_metadata_bytes));
      if (table.name.empty() || table.ref.empty()) continue;
      if (metadata->tables.size() < kMaxTables) {
        metadata->tables.push_back(std::move(table));
      } else {
        metadata->tables_truncated = true;
      }
      continue;
    }

    if (ends_with(rel.type, "/comments")) {
      const std::string part = resolve_part(part_dir, rel.target);
      if (part.empty() || !archive.has(part)) continue;
      detail::ooxml::CommentList list =
          parse_comments(archive.read_all(part, max_metadata_bytes));
      metadata->comments_total += list.total;
      for (auto& item : list.items) {
        if (metadata->comments.size() >= kMaxCommentItems) break;
        metadata->comments.push_back(std::move(item));
      }
      if (list.truncated || list.total > list.items.size() ||
          metadata->comments_total > metadata->comments.size())
        metadata->comments_truncated = true;
      continue;
    }

    if (ends_with(rel.type, "/pivotTable")) {
      const std::string part = resolve_part(part_dir, rel.target);
      if (part.empty() || !archive.has(part)) continue;
      PivotTableInfo pivot = parse_pivot_table(archive.read_all(part, max_metadata_bytes));
      if (pivot.name.empty()) continue;
      pivot.part = part;
      ++metadata->pivot_tables_total;
      if (metadata->pivot_tables.size() < kMaxPivotTables)
        metadata->pivot_tables.push_back(std::move(pivot));
      continue;
    }

    if (!ends_with(rel.type, "/drawing")) continue;
    const std::string drawing = resolve_part(part_dir, rel.target);
    if (drawing.empty() || !archive.has(drawing)) continue;
    const std::string drawing_dir = dir_name(drawing);
    const std::string drawing_rels_path =
        (drawing_dir.empty() ? std::string("_rels/") : drawing_dir + "/_rels/") +
        base_name(drawing) + ".rels";
    std::vector<Relationship> chart_rels;
    if (archive.has(drawing_rels_path))
      chart_rels =
          parse_relationships(archive.read_all(drawing_rels_path, max_metadata_bytes));
    const std::vector<std::string> chart_ids =
        parse_drawing_charts(archive.read_all(drawing, max_metadata_bytes));
    for (const std::string& id : chart_ids) {
      const Relationship* chart_rel = nullptr;
      for (const Relationship& candidate : chart_rels) {
        if (candidate.id == id && ends_with(candidate.type, "/chart")) {
          chart_rel = &candidate;
          break;
        }
      }
      if (!chart_rel) continue;
      const std::string chart_part = resolve_part(drawing_dir, chart_rel->target);
      if (chart_part.empty() || !archive.has(chart_part)) continue;
      ++metadata->charts_total;
      if (metadata->charts.size() >= kMaxCharts) continue;
      ChartInfo chart;
      chart.part = chart_part;
      chart.refs = parse_chart_refs(archive.read_all(chart_part, max_metadata_bytes));
      metadata->charts.push_back(std::move(chart));
    }
  }
}

// Adapts lambda callbacks to the virtual SAX surface.
class CallbackSaxHandler : public SaxHandler {
public:
  explicit CallbackSaxHandler(const SaxCallbacks& callbacks) : callbacks_(callbacks) {}

  void on_sheet_begin(const WorkSheet& sheet) override {
    if (callbacks_.sheet_begin) callbacks_.sheet_begin(sheet);
  }
  void on_dimension(std::string_view ref) override {
    if (callbacks_.dimension) callbacks_.dimension(ref);
  }
  void on_column(const ColumnView& column) override {
    if (callbacks_.column) callbacks_.column(column);
  }
  void on_row_info(uint32_t row, const RowInfo& info) override {
    if (callbacks_.row_info) callbacks_.row_info(row, info);
  }
  void on_pane(const PaneView& pane) override {
    if (callbacks_.pane) callbacks_.pane(pane);
  }
  void on_auto_filter(std::string_view ref) override {
    if (callbacks_.auto_filter) callbacks_.auto_filter(ref);
  }
  void on_hyperlink(const HyperlinkView& link) override {
    if (callbacks_.hyperlink) callbacks_.hyperlink(link);
  }
  void on_data_validation(const DataValidationView& info) override {
    if (callbacks_.data_validation) callbacks_.data_validation(info);
  }
  bool on_row_begin(uint32_t row) override {
    return !callbacks_.row_begin || callbacks_.row_begin(row);
  }
  bool on_cell(const CellView& cell) override {
    return !callbacks_.cell || callbacks_.cell(cell);
  }
  void on_row_end() override {
    if (callbacks_.row_end) callbacks_.row_end();
  }
  void on_merge_cell(const MergeRange& range) override {
    if (callbacks_.merge_cell) callbacks_.merge_cell(range);
  }
  void on_sheet_end() override {
    if (callbacks_.sheet_end) callbacks_.sheet_end();
  }

private:
  const SaxCallbacks& callbacks_;
};

} // namespace

namespace detail {

struct SheetStyleImpl {
  std::vector<std::string> codes;
  std::vector<FormatKind> kinds;
  std::vector<CellStyleFlags> flags;
};

struct SheetRecord {
  std::string name;
  uint32_t sheet_id = 0;
  std::string rel_id;
  std::string part_name;
  uint64_t uncompressed_bytes = 0;
  SheetState state = SheetState::Visible;
  SheetKind kind = SheetKind::Worksheet;
  FlatMap<std::string, std::string> relationships;
  SheetMetadata metadata;
};

struct WorkBookImpl {
  std::filesystem::path path;
  OpenOptions options;
  ZipArchive archive;
  std::vector<SheetRecord> records;
  std::vector<WorkSheet> handles;
  bool date_1904 = false;
  SheetStyle style;
  SharedStrings shared_strings;
  size_t shared_string_total = 0;
  bool shared_strings_loaded = false;
  std::vector<DefinedName> names;
  bool names_truncated = false;

  ReadStats read_sheet(size_t index, SaxHandler& handler);
  ReadStats read_sheet(size_t index, const SaxCallbacks& callbacks);
  ReadStats read_with(size_t index, SheetSink& sink);
  ReadStats read_record(size_t index, SheetSink& sink);
};

ReadStats WorkBookImpl::read_sheet(size_t index, SaxHandler& handler) {
  SaxHandlerSink sink(handler);
  return read_with(index, sink);
}

ReadStats WorkBookImpl::read_with(size_t index, SheetSink& sink) {
  if (index >= records.size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  const SheetRecord& record = records[index];
  if (record.kind == SheetKind::Chartsheet)
    throw_error(ErrorCode::InvalidSheetRef, "sheet '" + record.name + "' is a chartsheet");
  return read_record(index, sink);
}

ReadStats WorkBookImpl::read_record(size_t index, SheetSink& sink) {
  const SheetRecord& record = records[index];
  const SharedStrings* shared = shared_strings_loaded ? &shared_strings : nullptr;
  auto entry = archive.locate(record.part_name);
  if (entry.method == 0) {
    // STORE entries can be parsed straight from the read-only mapping.
    if (auto mapped = archive.mapped_store_entry(record.part_name); mapped) {
      const std::string_view xml(reinterpret_cast<const char*>(mapped->data()),
                                 mapped->size());
      return parse_sheet(xml, handles[index], style, shared, options.lenient, date_1904, sink,
                         &record.relationships);
    }
  }
  if (entry.uncompressed_size <= options.in_memory_threshold) {
    std::string xml = archive.read_all(record.part_name);
    return parse_sheet(xml, handles[index], style, shared, options.lenient, date_1904, sink,
                       &record.relationships);
  }
  ZipSource source(archive.open_entry(record.part_name));
  return parse_sheet(source, handles[index], style, shared, options.lenient, date_1904, sink,
                     &record.relationships);
}

ReadStats WorkBookImpl::read_sheet(size_t index, const SaxCallbacks& callbacks) {
  CallbackSaxHandler handler(callbacks);
  return read_sheet(index, handler);
}

} // namespace detail

// ---------------------------------------------------------------- SheetStyle

size_t SheetStyle::xf_count() const noexcept { return impl_ ? impl_->codes.size() : 0; }

std::string_view SheetStyle::format_code(uint32_t xf) const noexcept {
  if (!impl_ || xf >= impl_->codes.size()) return {};
  return impl_->codes[xf];
}

bool SheetStyle::is_date(uint32_t xf) const noexcept {
  return impl_ && xf < impl_->kinds.size() && impl_->kinds[xf] == FormatKind::Date;
}

bool SheetStyle::is_datetime(uint32_t xf) const noexcept {
  return impl_ && xf < impl_->kinds.size() && impl_->kinds[xf] == FormatKind::DateTime;
}

bool SheetStyle::is_time(uint32_t xf) const noexcept {
  return impl_ && xf < impl_->kinds.size() && impl_->kinds[xf] == FormatKind::Time;
}

CellStyleInfo SheetStyle::info(uint32_t xf) const noexcept {
  CellStyleInfo info;
  if (!impl_ || xf >= impl_->codes.size()) return info;
  info.num_fmt = impl_->codes[xf];
  if (xf < impl_->flags.size()) info.flags = impl_->flags[xf];
  return info;
}

// ----------------------------------------------------------------- WorkSheet

std::string_view WorkSheet::name() const noexcept {
  return impl_ ? std::string_view(impl_->records[index_].name) : std::string_view();
}

uint32_t WorkSheet::sheet_id() const noexcept {
  return impl_ ? impl_->records[index_].sheet_id : 0;
}

std::string_view WorkSheet::rel_id() const noexcept {
  return impl_ ? std::string_view(impl_->records[index_].rel_id) : std::string_view();
}

std::string_view WorkSheet::part_name() const noexcept {
  return impl_ ? std::string_view(impl_->records[index_].part_name) : std::string_view();
}

uint64_t WorkSheet::uncompressed_bytes() const noexcept {
  return impl_ ? impl_->records[index_].uncompressed_bytes : 0;
}

SheetState WorkSheet::state() const noexcept {
  return impl_ ? impl_->records[index_].state : SheetState::Visible;
}

SheetKind WorkSheet::kind() const noexcept {
  return impl_ ? impl_->records[index_].kind : SheetKind::Worksheet;
}

size_t WorkSheet::index() const noexcept { return index_; }

bool WorkSheet::is_worksheet() const noexcept { return kind() == SheetKind::Worksheet; }

std::string_view WorkSheet::relationship(std::string_view rel_id) const {
  if (!impl_ || index_ >= impl_->records.size()) return {};
  const auto& map = impl_->records[index_].relationships;
  auto found = map.find(std::string(rel_id));
  if (found == map.end()) return {};
  return found->second;
}

ReadStats WorkSheet::read(SaxHandler& handler) const {
  if (!impl_) throw_error(ErrorCode::Internal, "empty worksheet handle");
  return impl_->read_sheet(index_, handler);
}

ReadStats WorkSheet::read(const SaxCallbacks& callbacks) const {
  if (!impl_) throw_error(ErrorCode::Internal, "empty worksheet handle");
  return impl_->read_sheet(index_, callbacks);
}

// ------------------------------------------------------------ WorkSheetReader

struct WorkSheetReader::Impl {
  detail::WorkBookImpl* workbook = nullptr;
  WorkSheet sheet;
  size_t index = 0;
  bool finished = false;

  ReadStats read(SaxHandler& handler) {
    if (finished) throw_error(ErrorCode::Internal, "WorkSheetReader reused");
    finished = true;
    SaxHandlerSink sink(handler);
    return workbook->read_record(index, sink);
  }
};

// ------------------------------------------------------------------- WorkBook

WorkBook::WorkBook() = default;
WorkBook::~WorkBook() = default;
WorkBook::WorkBook(WorkBook&&) noexcept = default;
WorkBook& WorkBook::operator=(WorkBook&&) noexcept = default;

WorkBook::WorkBook(std::unique_ptr<detail::WorkBookImpl> impl) : impl_(std::move(impl)) {}

size_t WorkBook::sheet_count() const noexcept { return impl_->records.size(); }

WorkSheet WorkBook::sheet(size_t index) const {
  if (index >= impl_->handles.size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  return impl_->handles[index];
}

std::optional<WorkSheet> WorkBook::find_sheet(std::string_view name) const {
  for (size_t i = 0; i < impl_->records.size(); ++i) {
    if (impl_->records[i].name == name) return impl_->handles[i];
  }
  return std::nullopt;
}

std::span<const WorkSheet> WorkBook::sheets() const noexcept { return impl_->handles; }

bool WorkBook::date_1904() const noexcept { return impl_->date_1904; }

const SheetStyle& WorkBook::style() const noexcept { return impl_->style; }

size_t WorkBook::shared_string_count() const noexcept { return impl_->shared_string_total; }

std::string_view WorkBook::shared_string(uint32_t index) const {
  if (!impl_->shared_strings_loaded)
    throw_error(ErrorCode::SharedStringsNotLoaded, "shared strings were not loaded");
  if (index >= impl_->shared_strings.count())
    throw_error(ErrorCode::SharedStringOutOfRange,
                "shared string index " + std::to_string(index) + " out of range");
  return impl_->shared_strings.at(index);
}

std::span<const DefinedName> WorkBook::defined_names() const noexcept {
  return impl_->names;
}

bool WorkBook::defined_names_truncated() const noexcept { return impl_->names_truncated; }

const SheetMetadata& WorkBook::sheet_metadata(size_t sheet) const {
  if (sheet >= impl_->records.size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  return impl_->records[sheet].metadata;
}

const std::filesystem::path& WorkBook::path() const noexcept { return impl_->path; }

WorkBook WorkBook::open(const std::filesystem::path& path, const OpenOptions& options) {
  auto impl = std::make_unique<detail::WorkBookImpl>();
  impl->path = path;
  impl->options = options;
  impl->archive = ZipArchive::open(path, options.memory_map);

  std::string workbook_part = "xl/workbook.xml";
  if (impl->archive.has("_rels/.rels")) {
    auto rels = parse_relationships(
        impl->archive.read_all("_rels/.rels", options.max_metadata_bytes));
    for (const auto& rel : rels) {
      if (ends_with(rel.type, "/officeDocument")) {
        std::string resolved = resolve_part(std::string(), rel.target);
        if (!resolved.empty()) workbook_part = resolved;
        break;
      }
    }
  }

  const std::string workbook_dir = dir_name(workbook_part);
  const std::string rels_path =
      (workbook_dir.empty() ? std::string("_rels/") : workbook_dir + "/_rels/") +
      base_name(workbook_part) + ".rels";

  RelsIndex rels;
  if (impl->archive.has(rels_path))
    rels = index_rels(parse_relationships(
        impl->archive.read_all(rels_path, options.max_metadata_bytes)));

  WorkbookMeta meta = parse_workbook_meta(
      impl->archive.read_all(workbook_part, options.max_metadata_bytes));
  impl->date_1904 = meta.date_1904;
  impl->names_truncated = meta.defined_names.size() > 1024;
  impl->names = std::move(meta.defined_names);
  if (impl->names.size() > 1024) impl->names.resize(1024);

  FlatMap<std::string, const ZipEntryInfo*> entry_sizes;
  std::vector<ZipEntryInfo> entries = impl->archive.entries();
  for (const auto& entry : entries) entry_sizes[entry.name] = &entry;

  for (size_t i = 0; i < meta.sheets.size(); ++i) {
    const auto& parsed = meta.sheets[i];
    detail::SheetRecord record;
    record.name = parsed.name;
    record.sheet_id = parsed.sheet_id;
    record.rel_id = parsed.rel_id;
    record.state = parsed.state;

    std::string part;
    const Relationship* rel = nullptr;
    auto found = rels.by_id.find(parsed.rel_id);
    if (found != rels.by_id.end()) rel = &rels.rels[found->second];
    if (rel && ends_with(rel->type, "/chartsheet")) {
      record.kind = SheetKind::Chartsheet;
      part = resolve_part(workbook_dir, rel->target);
    } else if (rel && ends_with(rel->type, "/worksheet")) {
      part = resolve_part(workbook_dir, rel->target);
    } else {
      part = (workbook_dir.empty() ? std::string("xl/worksheets") : workbook_dir + "/worksheets") +
             "/sheet" + std::to_string(i + 1) + ".xml";
    }
    if (part.empty() || !impl->archive.has(part))
      throw_error(ErrorCode::MissingPart,
                  "worksheet part for '" + parsed.name + "' not found: " + part);
    auto size = entry_sizes.find(part);
    if (size != entry_sizes.end() && size->second)
      record.uncompressed_bytes = size->second->uncompressed_size;
    record.part_name = part;

    const std::string part_dir = dir_name(part);
    const std::string sheet_rels_path =
        (part_dir.empty() ? std::string("_rels/") : part_dir + "/_rels/") + base_name(part) +
        ".rels";
    std::vector<Relationship> sheet_relationships;
    if (impl->archive.has(sheet_rels_path)) {
      sheet_relationships = parse_relationships(
          impl->archive.read_all(sheet_rels_path, options.max_metadata_bytes));
      for (const auto& item : sheet_relationships) {
        std::string target = item.target;
        if (target.find("://") == std::string::npos && (target.empty() || target.front() != '#'))
          target = resolve_part(part_dir, target);
        record.relationships.emplace(item.id, std::move(target));
      }
    }
    resolve_sheet_metadata(impl->archive, part_dir, sheet_relationships,
                           options.max_metadata_bytes, &record.metadata);
    impl->records.push_back(std::move(record));
  }

  // Pivot caches live at workbook scope; map cacheId to its worksheet source.
  if (!meta.pivot_caches.empty()) {
    FlatMap<uint32_t, std::pair<std::string, std::string>> sources;
    for (const auto& cache : meta.pivot_caches) {
      auto found = rels.by_id.find(cache.rel_id);
      if (found == rels.by_id.end()) continue;
      const Relationship& rel = rels.rels[found->second];
      if (!ends_with(rel.type, "/pivotCacheDefinition")) continue;
      const std::string part = resolve_part(workbook_dir, rel.target);
      if (part.empty() || !impl->archive.has(part)) continue;
      auto source = parse_pivot_cache(impl->archive.read_all(part, options.max_metadata_bytes));
      if (source.first.empty() && source.second.empty()) continue;
      sources.try_emplace(cache.cache_id, std::move(source));
    }
    for (auto& record : impl->records) {
      for (auto& pivot : record.metadata.pivot_tables) {
        auto found = sources.find(pivot.cache_id);
        if (found == sources.end()) continue;
        pivot.source_sheet = found->second.first;
        pivot.source_ref = found->second.second;
      }
    }
  }

  std::string styles_part;
  if (const Relationship* rel = find_rel_type(rels, "/styles"))
    styles_part = resolve_part(workbook_dir, rel->target);
  if (styles_part.empty() || !impl->archive.has(styles_part)) {
    std::string fallback = (workbook_dir.empty() ? std::string("xl") : workbook_dir) + "/styles.xml";
    styles_part = impl->archive.has(fallback) ? fallback : std::string();
  }
  auto style = std::make_shared<detail::SheetStyleImpl>();
  if (!styles_part.empty()) {
    StylesheetData data = parse_stylesheet(
        impl->archive.read_all(styles_part, options.max_metadata_bytes));
    style->codes = std::move(data.format_codes);
    style->kinds = std::move(data.kinds);
    style->flags = std::move(data.flags);
  }
  impl->style.impl_ = std::move(style);

  if (options.shared_strings != SharedStringMode::Skip) {
    std::string sst_part;
    if (const Relationship* rel = find_rel_type(rels, "/sharedStrings"))
      sst_part = resolve_part(workbook_dir, rel->target);
    if (sst_part.empty() || !impl->archive.has(sst_part)) {
      std::string fallback =
          (workbook_dir.empty() ? std::string("xl") : workbook_dir) + "/sharedStrings.xml";
      sst_part = impl->archive.has(fallback) ? fallback : std::string();
    }
    if (!sst_part.empty()) {
      const size_t sst_limit =
          std::max(options.max_metadata_bytes, options.max_shared_strings_bytes);
      impl->shared_string_total = parse_shared_strings(
          impl->archive.read_all(sst_part, sst_limit), &impl->shared_strings,
          options.max_shared_strings_bytes, options.shared_strings == SharedStringMode::Load);
      impl->shared_strings_loaded = options.shared_strings == SharedStringMode::Load;
    }
  }

  impl->handles.reserve(impl->records.size());
  for (size_t i = 0; i < impl->records.size(); ++i)
    impl->handles.emplace_back(WorkSheet(impl.get(), static_cast<uint32_t>(i)));

  return WorkBook(std::move(impl));
}

Result<WorkBook> WorkBook::try_open(const std::filesystem::path& path,
                                    const OpenOptions& options) {
  return detail::capture_result([&] { return WorkBook::open(path, options); });
}

WorkSheetReader::WorkSheetReader(Impl* impl) noexcept : impl_(impl) {}

WorkSheetReader::~WorkSheetReader() { delete impl_; }

WorkSheetReader::WorkSheetReader(WorkSheetReader&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

WorkSheetReader& WorkSheetReader::operator=(WorkSheetReader&& other) noexcept {
  if (this != &other) {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

ReadStats WorkSheetReader::read(SaxHandler& handler) { return impl_->read(handler); }

ReadStats WorkSheetReader::read(const SaxCallbacks& callbacks) {
  CallbackSaxHandler handler(callbacks);
  return impl_->read(handler);
}

WorkSheetReader WorkBook::open_sheet(size_t index) const {
  if (index >= impl_->records.size())
    throw_error(ErrorCode::InvalidSheetRef, "sheet index out of range");
  const auto& record = impl_->records[index];
  if (record.kind == SheetKind::Chartsheet)
    throw_error(ErrorCode::InvalidSheetRef, "sheet '" + record.name + "' is a chartsheet");
  auto* reader = new WorkSheetReader::Impl();
  reader->workbook = impl_.get();
  reader->sheet = impl_->handles[index];
  reader->index = index;
  return WorkSheetReader(reader);
}

ReadStats WorkBook::read_sheet(size_t index, SaxHandler& handler) const {
  return impl_->read_sheet(index, handler);
}

ReadStats WorkBook::read_sheet(size_t index, const SaxCallbacks& callbacks) const {
  return impl_->read_sheet(index, callbacks);
}

std::vector<ReadStats> WorkBook::read_all(SaxHandler& handler) const {
  std::vector<ReadStats> stats;
  for (size_t i = 0; i < impl_->records.size(); ++i) {
    if (impl_->records[i].kind != SheetKind::Worksheet) continue;
    stats.push_back(read_sheet(i, handler));
  }
  return stats;
}

std::vector<ReadStats> WorkBook::read_all(const SaxCallbacks& callbacks) const {
  CallbackSaxHandler handler(callbacks);
  return read_all(handler);
}

uint64_t WorkBook::for_each_cell(
    size_t sheet_index, const std::function<bool(const CellView&)>& callback) const {
  uint64_t count = 0;
  SaxCallbacks callbacks;
  callbacks.cell = [&](const CellView& cell) {
    ++count;
    return callback(cell);
  };
  read_sheet(sheet_index, callbacks);
  return count;
}

uint64_t WorkBook::for_each_row(
    size_t sheet_index, const std::function<bool(const CellRowView&)>& callback) const {
  // CellView text/formula are borrowed from parser scratch and are only valid
  // during the callback, so row collection keeps owned Cells.
  class RowCollector : public SheetSink {
  public:
    explicit RowCollector(const std::function<bool(const CellRowView&)>& callback)
        : callback_(callback) {}
    bool row(uint32_t number) override {
      if (stopped_) return false;
      row_ = number;
      cells_.clear();
      views_.clear();
      return true;
    }
    bool cell(const CellView& cell) override {
      if (stopped_) return false;
      Cell& owned = cells_.emplace_back();
      owned.row = cell.row;
      owned.col = cell.col;
      owned.type = cell.type;
      owned.issue = cell.issue;
      owned.number = cell.number;
      owned.boolean = cell.boolean;
      owned.error = cell.error;
      owned.datetime = cell.datetime;
      owned.style = cell.style;
      owned.text.assign(cell.text.data(), cell.text.size());
      owned.formula.assign(cell.formula.data(), cell.formula.size());
      owned.has_formula = cell.has_formula;
      owned.format = cell.format;
      return true;
    }
    bool end_row() override {
      if (stopped_) return false;
      views_.reserve(cells_.size());
      for (const auto& owned : cells_) views_.push_back(owned.view());
      CellRowView view;
      view.number = row_;
      view.cells = views_;
      ++rows_;
      if (!callback_(view)) stopped_ = true;
      return !stopped_;
    }

    uint64_t rows() const noexcept { return rows_; }

  private:
    const std::function<bool(const CellRowView&)>& callback_;
    std::vector<Cell> cells_;
    std::vector<CellView> views_;
    uint32_t row_ = 0;
    uint64_t rows_ = 0;
    bool stopped_ = false;
  };

  RowCollector collector(callback);
  impl_->read_with(sheet_index, collector);
  return collector.rows();
}

} // namespace fastxl

namespace fastxl::detail {

const OpenOptions& WorkbookAccessor::options(const WorkBook& workbook) {
  return workbook.impl_->options;
}

const WorkSheet& WorkbookAccessor::sheet(const WorkBook& workbook, size_t index) {
  return workbook.impl_->handles[index];
}

bool WorkbookAccessor::date_1904(const WorkBook& workbook) {
  return workbook.impl_->date_1904;
}

const SheetStyle& WorkbookAccessor::styles(const WorkBook& workbook) {
  return workbook.impl_->style;
}

const SharedStrings* WorkbookAccessor::shared_strings(const WorkBook& workbook) {
  return workbook.impl_->shared_strings_loaded ? &workbook.impl_->shared_strings : nullptr;
}

std::string WorkbookAccessor::read_part(const WorkBook& workbook, std::string_view part) {
  return workbook.impl_->archive.read_all(part);
}

std::unique_ptr<zip::ZipEntryStream> WorkbookAccessor::open_stream(const WorkBook& workbook,
                                                                   std::string_view part) {
  return workbook.impl_->archive.open_entry(part);
}

} // namespace fastxl::detail
