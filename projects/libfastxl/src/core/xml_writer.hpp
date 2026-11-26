#pragma once

#include <string>
#include <string_view>

#include "byte_sink.hpp"

namespace fastxl::detail::xml {

using io::ByteSink;

// Minimal streaming XML writer for OOXML parts. Text is escaped for XML, CR is
// normalised to LF, invalid C0 controls are dropped and literal `_xHHHH_` sequences
// are protected with Excel's `_x005F_` convention.
class XmlWriter {
public:
  explicit XmlWriter(ByteSink& sink) : sink_(sink) {}

  void declaration();
  void start(std::string_view name);
  void attr(std::string_view name, std::string_view value);
  void end_start();      // ">"
  void end_start_self(); // "/>"
  void text(std::string_view value);
  void end(std::string_view name);

  ByteSink& sink() { return sink_; }

private:
  void write_escaped(std::string_view value, bool excel_escape, bool normalize_cr);

  ByteSink& sink_;
  std::string scratch_;
};

} // namespace fastxl::detail::xml
