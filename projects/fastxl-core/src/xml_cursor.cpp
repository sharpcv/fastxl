#include "xml_cursor.hpp"

#include "fastxl/error.hpp"
#include "utf8.hpp"

namespace fastxl::detail::xml {

using text::append_utf8;
using text::is_valid_utf8;
using text::is_xml_char;

namespace {

constexpr size_t kChunk = 256 * 1024;
constexpr size_t kMaxToken = 64ull << 20;
constexpr size_t kMaxDepth = 256;

inline bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool is_name_start(unsigned char c) noexcept {
  return c == '_' || c == ':' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80;
}

inline bool is_name_char(unsigned char c) noexcept {
  return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

} // namespace

XmlCursor::XmlCursor(std::string_view buffer) : whole_(buffer) {
  if (whole_.size() >= 2) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(whole_.data());
    if ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF))
      throw_error(ErrorCode::UnsupportedEncoding, "UTF-16 XML is not supported");
  }
  if (whole_.size() >= 3 && whole_.compare(0, 3, "\xEF\xBB\xBF") == 0) {
    whole_.remove_prefix(3);
    if (!is_valid_utf8(whole_)) fail_utf8();
  } else if (!is_valid_utf8(whole_)) {
    fail_utf8();
  }
}

XmlCursor::XmlCursor(XmlSource& source) : streaming_(true), source_(&source) { buffer_.reserve(kChunk); }

void XmlCursor::fail(const std::string& message) const {
  std::string text = context_.empty() ? "" : context_ + ":";
  text += "offset " + std::to_string(offset()) + ": " + message;
  throw_error(ErrorCode::MalformedXml, std::move(text));
}

void XmlCursor::fail_utf8() const {
  std::string text = context_.empty() ? "" : context_ + ":";
  text += "offset " + std::to_string(offset()) + ": invalid UTF-8";
  throw_error(ErrorCode::InvalidUtf8, std::move(text));
}

void XmlCursor::check_encoding_bom() {
  if (started_) return;
  if (buffer_.size() >= 2) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(buffer_.data());
    if ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF))
      throw_error(ErrorCode::UnsupportedEncoding, "UTF-16 XML is not supported");
    if (buffer_.size() >= 3 && buffer_.compare(0, 3, "\xEF\xBB\xBF") == 0) {
      buffer_.erase(0, 3);
      consumed_ += 3;
    }
  }
  started_ = true;
}

bool XmlCursor::pull() {
  if (!streaming_ || eof_) return false;
  if (pos_ > 0 && (pos_ == buffer_.size() || pos_ > kChunk)) {
    buffer_.erase(0, pos_);
    consumed_ += pos_;
    pos_ = 0;
  }
  if (staging_.size() < kChunk) staging_.resize(kChunk);
  size_t got = source_->read(staging_.data(), kChunk);
  if (got == 0) {
    eof_ = true;
    return false;
  }
  buffer_.append(staging_.data(), got);
  return true;
}

bool XmlCursor::ensure(size_t count) {
  if (!streaming_) return whole_.size() - pos_ >= count;
  for (;;) {
    if (!started_) {
      // The UTF-8 BOM is 3 bytes; wait for them (or for end of input) before
      // deciding that no BOM is present.
      if (buffer_.size() - pos_ >= 3 || eof_) check_encoding_bom();
    }
    if (buffer_.size() - pos_ >= count) return true;
    if (eof_) return false;
    pull();
  }
}

std::string_view XmlCursor::attr_raw(std::string_view local) const noexcept {
  for (const auto& attr : attrs_) {
    if (attr.name == local) return attr.value;
  }
  return {};
}

std::string_view XmlCursor::attr(std::string_view local, std::string& scratch) const {
  std::string_view raw = attr_raw(local);
  if (raw.find('&') == std::string_view::npos) return raw;
  scratch.clear();
  size_t index = 0;
  while (index < raw.size()) {
    char c = raw[index];
    if (c != '&') {
      scratch.push_back(c);
      ++index;
      continue;
    }
    size_t semi = raw.find(';', index + 1);
    if (semi == std::string_view::npos) fail("unterminated entity in attribute");
    std::string_view entity = raw.substr(index + 1, semi - index - 1);
    if (entity == "lt") {
      scratch.push_back('<');
    } else if (entity == "gt") {
      scratch.push_back('>');
    } else if (entity == "amp") {
      scratch.push_back('&');
    } else if (entity == "quot") {
      scratch.push_back('"');
    } else if (entity == "apos") {
      scratch.push_back('\'');
    } else if (entity.size() > 1 && entity[0] == '#') {
      uint32_t code_point = 0;
      bool ok = false;
      if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
        for (size_t i = 2; i < entity.size(); ++i) {
          int digit = hex_digit(entity[i]);
          if (digit < 0) {
            ok = false;
            break;
          }
          code_point = code_point * 16 + static_cast<uint32_t>(digit);
          ok = true;
        }
      } else {
        for (size_t i = 1; i < entity.size(); ++i) {
          if (entity[i] < '0' || entity[i] > '9') {
            ok = false;
            break;
          }
          code_point = code_point * 10 + static_cast<uint32_t>(entity[i] - '0');
          ok = true;
        }
      }
      if (!ok || !is_xml_char(code_point)) fail("invalid character entity");
      append_utf8(scratch, code_point);
    } else {
      fail("unknown entity '" + std::string(entity) + "'");
    }
    index = semi + 1;
  }
  return scratch;
}

void XmlCursor::skip_comment() {
  pos_ += 4; // "<!--"
  std::string_view ignored;
  if (!scan_until("-->", false, true, &ignored)) fail("unterminated comment");
}

void XmlCursor::skip_pi() {
  pos_ += 2; // "<?"
  std::string_view ignored;
  if (!scan_until("?>", false, true, &ignored)) fail("unterminated processing instruction");
}

void XmlCursor::skip_doctype() {
  pos_ += 2; // "<!"
  std::string_view ignored;
  if (!scan_until(">", true, true, &ignored, true)) fail("unterminated declaration");
}

bool XmlCursor::parse_cdata() {
  pos_ += 9; // "<![CDATA["
  std::string_view raw;
  if (!scan_until("]]>", false, true, &raw)) fail("unterminated CDATA section");
  validate_utf8(raw);
  // CDATA content is literal: only CR normalisation applies.
  decode_.clear();
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '\r') {
      decode_.push_back('\n');
      if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
      continue;
    }
    decode_.push_back(c);
  }
  text_ = decode_;
  token_ = Token::Text;
  return true;
}

void XmlCursor::validate_utf8(std::string_view text) const {
  if (!streaming_ || text.empty()) return;
  if (simd::all_ascii(text.data(), text.size())) return;
  if (!is_valid_utf8(text)) fail_utf8();
}

simd::XmlTextScan XmlCursor::validate_xml_bytes(std::string_view raw) const {
  simd::XmlTextScan scan = simd::scan_xml_text(raw.data(), raw.size());
  if (scan.bad_offset != static_cast<size_t>(-1)) {
    if (static_cast<unsigned char>(raw[scan.bad_offset]) == 0xEF)
      fail("non-character code point in text");
    fail("control character in text");
  }
  return scan;
}

std::string_view XmlCursor::decode_text(std::string_view raw) {
  const simd::XmlTextScan scan = validate_xml_bytes(raw);
  if (streaming_ && scan.has_non_ascii && !is_valid_utf8(raw)) fail_utf8();
  if (!scan.has_entity && !scan.has_carriage_return) return raw;

  decode_.clear();
  size_t index = 0;
  while (index < raw.size()) {
    char c = raw[index];
    if (c == '\r') {
      decode_.push_back('\n');
      ++index;
      if (index < raw.size() && raw[index] == '\n') ++index;
      continue;
    }
    if (c != '&') {
      decode_.push_back(c);
      ++index;
      continue;
    }
    size_t semi = raw.find(';', index + 1);
    if (semi == std::string_view::npos) fail("unterminated entity");
    std::string_view entity = raw.substr(index + 1, semi - index - 1);
    if (entity == "lt") {
      decode_.push_back('<');
    } else if (entity == "gt") {
      decode_.push_back('>');
    } else if (entity == "amp") {
      decode_.push_back('&');
    } else if (entity == "quot") {
      decode_.push_back('"');
    } else if (entity == "apos") {
      decode_.push_back('\'');
    } else if (entity.size() > 1 && entity[0] == '#') {
      uint32_t code_point = 0;
      bool ok = false;
      if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
        for (size_t i = 2; i < entity.size(); ++i) {
          int digit = hex_digit(entity[i]);
          if (digit < 0) {
            ok = false;
            break;
          }
          code_point = code_point * 16 + static_cast<uint32_t>(digit);
          ok = true;
        }
      } else {
        for (size_t i = 1; i < entity.size(); ++i) {
          if (entity[i] < '0' || entity[i] > '9') {
            ok = false;
            break;
          }
          code_point = code_point * 10 + static_cast<uint32_t>(entity[i] - '0');
          ok = true;
        }
      }
      if (!ok || !is_xml_char(code_point)) fail("invalid character entity");
      append_utf8(decode_, code_point);
    } else {
      fail("unknown entity '" + std::string(entity) + "'");
    }
    index = semi + 1;
  }
  return decode_;
}

bool XmlCursor::scan_until(std::string_view terminator, bool quote_aware, bool consume,
                           std::string_view* out, bool brackets) {
  scratch_.clear();
  bool quoted = false;
  char quote_char = 0;
  int bracket_depth = 0;
  const size_t keep = terminator.size() > 1 ? terminator.size() - 1 : 0;

  for (;;) {
    std::string_view view = this->window();
    auto finish = [&](size_t at) {
      if (scratch_.empty()) {
        *out = view.substr(pos_, at - pos_);
      } else {
        scratch_.append(view.substr(pos_, at - pos_));
        *out = scratch_;
      }
      pos_ = at + (consume ? terminator.size() : 0);
      return true;
    };

    size_t scan = pos_;
    while (scan < view.size()) {
      const size_t remaining = view.size() - scan;
      if (!quote_aware && !brackets && terminator.size() == 1) {
        const size_t found = simd::find_byte(view.data() + scan, remaining, terminator[0]);
        if (found == remaining) {
          scan = view.size();
          break;
        }
        scan += found;
        return finish(scan);
      }
      if (quote_aware && !brackets && terminator.size() == 1 && terminator[0] == '>') {
        const size_t found = simd::find_tag_delimiter(view.data() + scan, remaining);
        if (found == remaining) {
          scan = view.size();
          break;
        }
        scan += found;
        const char c = view[scan];
        if (quoted) {
          if (c == quote_char) quoted = false;
          ++scan;
          continue;
        }
        if (c == '"' || c == '\'') {
          quoted = true;
          quote_char = c;
          ++scan;
          continue;
        }
        return finish(scan);
      }

      const char c = view[scan];
      if (quote_aware) {
        if (quoted) {
          if (c == quote_char) quoted = false;
          ++scan;
          continue;
        }
        if (c == '"' || c == '\'') {
          quoted = true;
          quote_char = c;
          ++scan;
          continue;
        }
      }
      if (brackets) {
        if (c == '[') {
          ++bracket_depth;
          ++scan;
          continue;
        }
        if (c == ']') {
          if (bracket_depth > 0) --bracket_depth;
          ++scan;
          continue;
        }
        if (bracket_depth > 0) {
          ++scan;
          continue;
        }
      }
      const bool match = terminator.size() == 1
                             ? c == terminator[0]
                             : view.compare(scan, terminator.size(), terminator) == 0;
      if (match) return finish(scan);
      ++scan;
    }

    // Terminator not in the window: carry the bytes over and pull more.
    size_t end = view.size();
    size_t append_end = end > keep ? end - keep : pos_;
    if (append_end > pos_) {
      if (append_end - pos_ > kMaxToken) fail("token exceeds 64 MiB");
      scratch_.append(view.substr(pos_, append_end - pos_));
      if (scratch_.size() > kMaxToken) fail("token exceeds 64 MiB");
    }
    pos_ = append_end;
    if (!streaming_) {
      if (terminator == "<") {
        std::string_view window_end = this->window();
        if (pos_ < window_end.size()) {
          scratch_.append(window_end.substr(pos_));
          pos_ = window_end.size();
        }
        *out = scratch_;
        return true;
      }
      return false;
    }
    if (!pull()) {
      // End of input: hand back whatever is left (text) or report failure.
      if (terminator == "<") {
        std::string_view window_end = this->window();
        if (pos_ < window_end.size()) {
          scratch_.append(window_end.substr(pos_));
          pos_ = window_end.size();
        }
        *out = scratch_;
        return true;
      }
      return false;
    }
  }
}

bool XmlCursor::parse_start_tag(std::string_view content) {
  token_ = Token::StartTag;
  self_closing_ = false;
  attrs_.clear();
  size_t size = content.size();
  if (size > 0 && content[size - 1] == '/') {
    self_closing_ = true;
    --size;
  }
  size_t index = 0;
  while (index < size && is_space(content[index])) ++index;
  size_t name_begin = index;
  if (index >= size || !is_name_start(static_cast<unsigned char>(content[index])))
    fail("invalid tag name");
  while (index < size && is_name_char(static_cast<unsigned char>(content[index]))) ++index;
  std::string_view full_name = content.substr(name_begin, index - name_begin);
  validate_utf8(full_name);
  size_t colon = full_name.rfind(':');
  name_ = colon == std::string_view::npos ? full_name : full_name.substr(colon + 1);

  for (;;) {
    while (index < size && is_space(content[index])) ++index;
    if (index >= size) break;
    size_t attr_begin = index;
    while (index < size && !is_space(content[index]) && content[index] != '=') ++index;
    if (index == attr_begin) fail("malformed attribute");
    std::string_view attr_name = content.substr(attr_begin, index - attr_begin);
    while (index < size && is_space(content[index])) ++index;
    if (index >= size || content[index] != '=') fail("attribute without value");
    ++index;
    while (index < size && is_space(content[index])) ++index;
    if (index >= size) fail("attribute without value");
    char quote = content[index];
    if (quote != '"' && quote != '\'') fail("attribute value must be quoted");
    ++index;
    size_t value_begin = index;
    const size_t quote_at = simd::find_byte(content.data() + index, size - index, quote);
    if (quote_at == size - index) fail("unterminated attribute value");
    index += quote_at;
    std::string_view value = content.substr(value_begin, index - value_begin);
    ++index;
    validate_utf8(attr_name);
    validate_utf8(value);
    size_t attr_colon = attr_name.rfind(':');
    if (attr_colon != std::string_view::npos) attr_name = attr_name.substr(attr_colon + 1);
    attrs_.push_back({attr_name, value});
  }

  if (!self_closing_) {
    if (stack_.size() >= kMaxDepth) fail("element nesting deeper than 256");
    stack_.emplace_back(name_);
  }
  return true;
}

bool XmlCursor::parse_end_tag(std::string_view content) {
  size_t index = 0;
  while (index < content.size() && is_space(content[index])) ++index;
  size_t begin = index;
  while (index < content.size() && is_name_char(static_cast<unsigned char>(content[index])))
    ++index;
  std::string_view full_name = content.substr(begin, index - begin);
  validate_utf8(full_name);
  size_t colon = full_name.rfind(':');
  name_ = colon == std::string_view::npos ? full_name : full_name.substr(colon + 1);
  if (name_.empty()) fail("empty end tag");
  while (index < content.size() && is_space(content[index])) ++index;
  if (index != content.size()) fail("malformed end tag");
  if (stack_.empty()) fail("unexpected end tag </" + std::string(name_) + ">");
  if (stack_.back() != name_)
    fail("mismatched end tag </" + std::string(name_) + ">, expected </" + stack_.back() + ">");
  stack_.pop_back();
  token_ = Token::EndTag;
  return true;
}

bool XmlCursor::next() {
  if (token_ == Token::End) return false;

  for (;;) {
    if (!streaming_) {
      if (pos_ >= whole_.size()) {
        if (!stack_.empty()) fail("unclosed element <" + stack_.back() + ">");
        token_ = Token::End;
        return false;
      }
    } else {
      if (!ensure(1)) {
        if (!stack_.empty()) fail("unclosed element <" + stack_.back() + ">");
        token_ = Token::End;
        return false;
      }
    }

    std::string_view view = this->window();
    if (view[pos_] != '<') {
      std::string_view raw;
      if (!scan_until("<", false, false, &raw)) {
        token_ = Token::End;
        return false;
      }
      if (raw.empty()) continue; // e.g. "<a></a>" produced no text
      text_ = decode_text(raw);
      token_ = Token::Text;
      return true;
    }

    // Markup. Each probe re-reads the window because ensure() may pull and
    // invalidate an earlier view.
    if (ensure(9) && window().compare(pos_, 9, "<![CDATA[") == 0) {
      if (parse_cdata()) return true;
      continue;
    }
    if (ensure(4) && window().compare(pos_, 4, "<!--") == 0) {
      skip_comment();
      continue;
    }
    if (ensure(2) && window().compare(pos_, 2, "<?") == 0) {
      skip_pi();
      continue;
    }
    if (ensure(2) && window().compare(pos_, 2, "</") == 0) {
      pos_ += 2;
      std::string_view content;
      if (!scan_until(">", false, true, &content)) fail("unterminated end tag");
      if (!parse_end_tag(content)) return false;
      return true;
    }
    if (ensure(2) && window()[pos_ + 1] == '!') {
      skip_doctype();
      continue;
    }

    ++pos_; // '<'
    std::string_view content;
    if (!scan_until(">", true, true, &content)) fail("unterminated start tag");
    if (!parse_start_tag(content)) return false;
    return true;
  }
}

} // namespace fastxl::detail::xml
