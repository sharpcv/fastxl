#include "xml_writer.hpp"

#include "excel_escape.hpp"
#include "utf8.hpp"

namespace fastxl::detail::xml {

using text::decode_utf8;
using text::encode_excel_escapes;
using text::is_valid_utf8;
using text::is_xml_char;

void XmlWriter::declaration() {
  sink_.write("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
}

void XmlWriter::start(std::string_view name) {
  sink_.put('<');
  sink_.write(name);
}

void XmlWriter::attr(std::string_view name, std::string_view value) {
  sink_.put(' ');
  sink_.write(name);
  sink_.write("=\"");
  write_escaped(value, false, false);
  sink_.put('"');
}

void XmlWriter::end_start() { sink_.put('>'); }

void XmlWriter::end_start_self() { sink_.write("/>"); }

void XmlWriter::text(std::string_view value) { write_escaped(value, true, true); }

void XmlWriter::end(std::string_view name) {
  sink_.write("</");
  sink_.write(name);
  sink_.put('>');
}

void XmlWriter::write_escaped(std::string_view value, bool excel_escape, bool normalize_cr) {
  if (value.empty()) return;
  std::string protected_text;
  if (excel_escape) value = encode_excel_escapes(value, protected_text);

  scratch_.clear();
  scratch_.reserve(value.size() + 16);
  for (size_t i = 0; i < value.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(value[i]);
    switch (c) {
    case '&': scratch_.append("&amp;"); continue;
    case '<': scratch_.append("&lt;"); continue;
    case '>': scratch_.append("&gt;"); continue;
    case '"': scratch_.append("&quot;"); continue;
    case '\'': scratch_.append("&apos;"); continue;
    case '\r':
      if (normalize_cr) {
        scratch_.push_back('\n');
        if (i + 1 < value.size() && value[i + 1] == '\n') ++i;
        continue;
      }
      break;
    case '\t':
    case '\n':
      break;
    default:
      if (c < 0x20) continue; // not representable in XML 1.0
      break;
    }
    scratch_.push_back(static_cast<char>(c));
  }
  if (!is_valid_utf8(scratch_)) {
    // Drop invalid sequences rather than emitting a corrupt document.
    std::string cleaned;
    cleaned.reserve(scratch_.size());
    size_t pos = 0;
    while (pos < scratch_.size()) {
      size_t next = pos;
      uint32_t code_point = 0;
      if (decode_utf8(scratch_, next, code_point) && is_xml_char(code_point)) {
        cleaned.append(scratch_, pos, next - pos);
        pos = next;
      } else {
        ++pos;
      }
    }
    scratch_.swap(cleaned);
  }
  sink_.write(scratch_);
}

} // namespace fastxl::detail::xml
