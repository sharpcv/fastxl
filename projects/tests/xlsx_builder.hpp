#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "zip_writer.hpp"

namespace fxltest {

// Builds a minimal single-sheet OOXML package for parser-level tests.
inline void make_xlsx(const std::filesystem::path& path, const std::string& sheet_xml,
                      const std::vector<std::pair<std::string, std::string>>& extra = {}) {
  fastxl::detail::zip::ZipWriter zip =
      fastxl::detail::zip::ZipWriter::create(path, 0);
  zip.add_buffer("[Content_Types].xml",
                 "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/"
                 "package/2006/content-types\">"
                 "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-"
                 "package.relationships+xml\"/>"
                 "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                 "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd."
                 "openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                 "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/"
                 "vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
                 "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd."
                 "openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
                 "</Types>");
  zip.add_buffer("_rels/.rels",
                 "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas."
                 "openxmlformats.org/package/2006/relationships\">"
                 "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
                 "officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
                 "</Relationships>");
  zip.add_buffer("xl/workbook.xml",
                 "<?xml version=\"1.0\"?><workbook xmlns=\"http://schemas.openxmlformats.org/"
                 "spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/"
                 "officeDocument/2006/relationships\"><sheets><sheet name=\"Raw\" sheetId=\"1\" "
                 "r:id=\"rId1\"/></sheets></workbook>");
  zip.add_buffer("xl/_rels/workbook.xml.rels",
                 "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas."
                 "openxmlformats.org/package/2006/relationships\">"
                 "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
                 "officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
                 "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/"
                 "officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                 "</Relationships>");
  zip.add_buffer("xl/styles.xml",
                 "<?xml version=\"1.0\"?><styleSheet xmlns=\"http://schemas.openxmlformats.org/"
                 "spreadsheetml/2006/main\"><fonts count=\"1\"><font/></fonts>"
                 "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\"/></cellXfs></styleSheet>");
  zip.add_buffer("xl/worksheets/sheet1.xml", sheet_xml);
  for (const auto& [name, content] : extra) zip.add_buffer(name, content);
  zip.close();
}

} // namespace fxltest
