#pragma once

#include <cstdint>
#include <string_view>

#include "fastxl/sax.hpp"
#include "fastxl/types.hpp"
#include "fastxl/workbook.hpp"
#include "flat_map.hpp"
#include "string_table.hpp"
#include "xml_cursor.hpp"

namespace fastxl::detail::sheet {

using ooxml::SharedStrings;
using xml::XmlSource;

// Callback interface used by the sheet scanner. Returning false from row() or
// cell() stops parsing immediately.
class SheetSink {
public:
  virtual ~SheetSink() = default;
  virtual void begin(const WorkSheet&) {}
  virtual void dimension(std::string_view) {}
  virtual void column(const ColumnView&) {}
  virtual void row_info(uint32_t, const RowInfo&) {}
  virtual void pane(const PaneView&) {}
  virtual void auto_filter(std::string_view) {}
  virtual void filter_column(const FilterColumnView&) {}
  virtual void hyperlink(const HyperlinkView&) {}
  virtual void data_validation(const DataValidationView&) {}
  virtual bool row(uint32_t) { return true; }
  virtual bool cell(const CellView&) { return true; }
  virtual bool end_row() { return true; }
  virtual void merge(const MergeRange&) {}
  virtual void end() {}
};

using SheetRelationshipMap = FlatMap<std::string, std::string>;

ReadStats parse_sheet(std::string_view xml, const WorkSheet& info, const SheetStyle& styles,
                      const SharedStrings* shared_strings, bool lenient, bool date_1904,
                      SheetSink& sink, const SheetRelationshipMap* relationships = nullptr);
ReadStats parse_sheet(XmlSource& source, const WorkSheet& info, const SheetStyle& styles,
                      const SharedStrings* shared_strings, bool lenient, bool date_1904,
                      SheetSink& sink, const SheetRelationshipMap* relationships = nullptr);

// Parses a fragment consisting of complete <row> elements (used by the parallel
// profiler). `seed_row` supplies the row number used when a row omits r="".
ReadStats parse_sheet_fragment(std::string_view xml, const WorkSheet& info,
                               const SheetStyle& styles, const SharedStrings* shared_strings,
                               bool lenient, bool date_1904, uint32_t seed_row, SheetSink& sink);

// Adapts the internal SheetSink to the public SaxHandler.
class SaxHandlerSink : public SheetSink {
public:
  explicit SaxHandlerSink(SaxHandler& handler) : handler_(handler) {}
  void begin(const WorkSheet& info) override { handler_.on_sheet_begin(info); }
  void dimension(std::string_view ref) override { handler_.on_dimension(ref); }
  void column(const ColumnView& info) override { handler_.on_column(info); }
  void row_info(uint32_t row, const RowInfo& info) override { handler_.on_row_info(row, info); }
  void pane(const PaneView& info) override { handler_.on_pane(info); }
  void auto_filter(std::string_view ref) override { handler_.on_auto_filter(ref); }
  void filter_column(const FilterColumnView& info) override {
    handler_.on_filter_column(info);
  }
  void hyperlink(const HyperlinkView& info) override { handler_.on_hyperlink(info); }
  void data_validation(const DataValidationView& info) override {
    handler_.on_data_validation(info);
  }
  bool row(uint32_t number) override { return handler_.on_row_begin(number); }
  bool cell(const CellView& cell) override { return handler_.on_cell(cell); }
  bool end_row() override {
    handler_.on_row_end();
    return true;
  }
  void merge(const MergeRange& range) override { handler_.on_merge_cell(range); }
  void end() override { handler_.on_sheet_end(); }

private:
  SaxHandler& handler_;
};

} // namespace fastxl::detail::sheet
