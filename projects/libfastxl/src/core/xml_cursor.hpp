#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "simd_scan.hpp"

namespace fastxl::detail::xml {

struct XmlAttrView {
  std::string_view name;  // local name, prefix stripped
  std::string_view value; // raw, entity undecoded
};

// Pulls bytes from a stream for the incremental XML cursor.
class XmlSource {
public:
  virtual ~XmlSource() = default;
  virtual size_t read(char* dst, size_t size) = 0;
};

// Zero-allocation XML scanner for OOXML parts. Tokens are views into the input
// buffer (whole-buffer mode) or into a sliding window (streaming mode); callers
// must not retain them beyond the next call to next().
class XmlCursor {
public:
  explicit XmlCursor(std::string_view buffer);
  explicit XmlCursor(XmlSource& source);
  XmlCursor(const XmlCursor&) = delete;
  XmlCursor& operator=(const XmlCursor&) = delete;

  // Advances to the next token. Text tokens are already entity-decoded and have
  // CRLF/CR normalised. Returns false at end of document.
  bool next();

  bool is_text() const noexcept { return token_ == Token::Text; }
  bool is_tag() const noexcept { return token_ == Token::StartTag; }
  bool is_end_tag() const noexcept { return token_ == Token::EndTag; }
  std::string_view text() const noexcept { return text_; }
  // Local name of the current start/end tag (namespace prefix stripped).
  std::string_view tag_name() const noexcept { return name_; }
  bool tag_is(std::string_view local) const noexcept {
    return token_ == Token::StartTag && name_ == local;
  }
  // True for either a start or an end tag with this local name.
  bool name_is(std::string_view local) const noexcept {
    return (token_ == Token::StartTag || token_ == Token::EndTag) && name_ == local;
  }
  bool self_closing() const noexcept { return self_closing_; }
  // Raw attribute value (entities still encoded); empty when absent.
  std::string_view attr_raw(std::string_view local) const noexcept;
  // Entity-decoded attribute value; `scratch` holds the result when decoding is
  // needed. Returns an empty view when the attribute is absent.
  std::string_view attr(std::string_view local, std::string& scratch) const;

  size_t offset() const noexcept { return consumed_ + pos_; }
  // Included in error messages, e.g. "xl/worksheets/sheet1.xml".
  void set_context(std::string context) { context_ = std::move(context); }

  [[noreturn]] void fail(const std::string& message) const;
  [[noreturn]] void fail_utf8() const;

private:
  enum class Token { None, StartTag, EndTag, Text, End };

  std::string_view window() const noexcept {
    return streaming_ ? std::string_view(buffer_) : whole_;
  }
  bool ensure(size_t count);
  bool pull();
  bool scan_until(std::string_view terminator, bool quote_aware, bool consume,
                  std::string_view* out, bool brackets = false);
  bool parse_start_tag(std::string_view content);
  bool parse_end_tag(std::string_view content);
  void skip_comment();
  void skip_pi();
  void skip_doctype();
  bool parse_cdata();
  std::string_view decode_text(std::string_view raw);
  simd::XmlTextScan validate_xml_bytes(std::string_view raw) const;
  void validate_utf8(std::string_view text) const;
  void check_encoding_bom();

  std::string_view whole_;
  bool streaming_ = false;
  XmlSource* source_ = nullptr;
  std::string buffer_;
  std::string staging_;
  size_t pos_ = 0;
  size_t consumed_ = 0;
  bool eof_ = false;
  bool started_ = false;

  Token token_ = Token::None;
  std::string_view name_;
  std::string_view text_;
  bool self_closing_ = false;
  std::vector<XmlAttrView> attrs_;

  std::string scratch_; // gathered token bytes
  std::string decode_;  // decoded text/attribute output
  std::vector<std::string> stack_;
  std::string context_;
};

} // namespace fastxl::detail::xml
