#include "excel_escape.hpp"
#include "fastxl/error.hpp"
#include "test.hpp"
#include "xml_cursor.hpp"
#include "xml_writer.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace fastxl;
using namespace fastxl::detail::io;
using namespace fastxl::detail::text;
using namespace fastxl::detail::xml;

namespace {

// Feeds a buffer in fixed-size chunks to exercise token gathering.
class ChunkSource : public XmlSource {
public:
  ChunkSource(std::string data, size_t chunk) : data_(std::move(data)), chunk_(chunk) {}
  size_t read(char* dst, size_t size) override {
    size_t n = std::min({size, chunk_, data_.size() - pos_});
    if (n == 0) return 0;
    std::memcpy(dst, data_.data() + pos_, n);
    pos_ += n;
    return n;
  }

private:
  std::string data_;
  size_t chunk_;
  size_t pos_ = 0;
};

struct Token {
  enum Kind { Text, Start, End } kind;
  std::string name;
  std::string text;
  bool self_closing = false;
  std::vector<std::pair<std::string, std::string>> attrs;
};

std::vector<Token> collect(XmlCursor& cursor) {
  std::vector<Token> tokens;
  while (cursor.next()) {
    if (cursor.is_text()) {
      tokens.push_back({Token::Text, {}, std::string(cursor.text())});
    } else if (cursor.is_tag()) {
      Token token{Token::Start, std::string(cursor.tag_name()), {}, cursor.self_closing(), {}};
      static const char* names[] = {"r", "t", "s", "name", "val", "xmlns", "a"};
      for (const char* name : names) {
        std::string scratch;
        auto value = cursor.attr(name, scratch);
        if (!value.empty()) token.attrs.emplace_back(name, std::string(value));
      }
      tokens.push_back(std::move(token));
    } else if (cursor.is_end_tag()) {
      tokens.push_back({Token::End, std::string(cursor.tag_name()), {}});
    }
  }
  return tokens;
}

std::string token_signature(const std::vector<Token>& tokens) {
  std::string out;
  for (const auto& token : tokens) {
    switch (token.kind) {
    case Token::Text: out += "T(" + token.text + ")"; break;
    case Token::Start:
      out += "S(" + token.name;
      if (token.self_closing) out += "/";
      for (const auto& [key, value] : token.attrs) out += " " + key + "=" + value;
      out += ")";
      break;
    case Token::End: out += "E(" + token.name + ")"; break;
    }
  }
  return out;
}

} // namespace

TEST(xml_basic_tokens) {
  std::string xml = "<?xml version=\"1.0\"?><root a=\"1\"><x:row r=\"2\"/><row><c>hi</c></row></root>";
  XmlCursor cursor(xml);
  auto tokens = collect(cursor);
  CHECK_EQ(token_signature(tokens),
           "S(root a=1)S(row/ r=2)S(row)S(c)T(hi)E(c)E(row)E(root)");
}

TEST(xml_entities_and_cdata) {
  std::string xml = "<a>&lt;&amp;&#65;&#x1F600;</a><b><![CDATA[x<y&z]]></b>";
  XmlCursor cursor(xml);
  auto tokens = collect(cursor);
  CHECK_EQ(tokens.size(), 6u);
  CHECK_EQ(tokens[1].text, "<&A\xF0\x9F\x98\x80");
  CHECK_EQ(tokens[4].text, "x<y&z");

  std::string bad = "<a>&nope;</a>";
  XmlCursor cursor2(bad);
  CHECK_THROWS_CODE(cursor2.next() && cursor2.next(), ErrorCode::MalformedXml);

  std::string zero = "<a>&#0;</a>";
  XmlCursor cursor3(zero);
  CHECK_THROWS_CODE(cursor3.next() && cursor3.next(), ErrorCode::MalformedXml);
}

TEST(xml_comments_and_declarations) {
  std::string xml =
      "<!-- c --><r><!-- inner --><v>1</v><?pi test?><w><![CDATA[z]]></w></r>";
  XmlCursor cursor(xml);
  auto tokens = collect(cursor);
  CHECK_EQ(token_signature(tokens), "S(r)S(v)T(1)E(v)S(w)T(z)E(w)E(r)");
}

TEST(xml_doctype_with_gt_in_quotes) {
  std::string xml = "<!DOCTYPE x [ <!ENTITY e \"a>b\"> ]><r a=\"x>y\">t</r>";
  XmlCursor cursor(xml);
  auto tokens = collect(cursor);
  CHECK_EQ(tokens.size(), 3u);
  CHECK_EQ(tokens[0].name, "r");
  CHECK_EQ(tokens[0].attrs.at(0).second, "x>y");
  CHECK_EQ(tokens[1].text, "t");
}

TEST(xml_cr_normalization) {
  std::string xml = "<a>l1\r\nl2\rl3</a>";
  XmlCursor cursor(xml);
  auto tokens = collect(cursor);
  CHECK_EQ(tokens[1].text, "l1\nl2\nl3");
}

TEST(xml_errors) {
  {
    std::string xml = "<a><b></a>";
    XmlCursor cursor(xml);
    CHECK_THROWS_CODE(collect(cursor), ErrorCode::MalformedXml);
  }
  {
    std::string xml = "<a><b>";
    XmlCursor cursor(xml);
    CHECK_THROWS_CODE(collect(cursor), ErrorCode::MalformedXml);
  }
  {
    std::string xml = "<a b=c/>";
    XmlCursor cursor(xml);
    CHECK_THROWS_CODE(collect(cursor), ErrorCode::MalformedXml);
  }
  {
    std::string xml = "\xFF\xFE<\x00a\x00/\x00>\x00";
    CHECK_THROWS_CODE((void)XmlCursor{xml}, ErrorCode::UnsupportedEncoding);
  }
  {
    std::string xml = "<a>\xFF</a>";
    CHECK_THROWS_CODE((void)XmlCursor{xml}, ErrorCode::InvalidUtf8);
  }
  {
    std::string xml = "<a>\x01</a>";
    XmlCursor cursor(xml);
    CHECK_THROWS_CODE(collect(cursor), ErrorCode::MalformedXml);
  }
}

TEST(xml_streaming_validates_attribute_utf8) {
  std::string xml = "<a b=\"\xFF\">x</a>";
  CHECK_THROWS_CODE((void)XmlCursor{xml}, ErrorCode::InvalidUtf8); // whole buffer
  for (size_t chunk : {size_t(1), size_t(2)}) {
    ChunkSource source(xml, chunk);
    XmlCursor cursor(source);
    CHECK_THROWS_CODE(cursor.next(), ErrorCode::InvalidUtf8);
  }
}

TEST(xml_depth_limit) {
  std::string xml;
  for (int i = 0; i < 257; ++i) xml += "<a>";
  for (int i = 0; i < 257; ++i) xml += "</a>";
  XmlCursor cursor(xml);
  CHECK_THROWS_CODE(collect(cursor), ErrorCode::MalformedXml);
}

TEST(xml_streaming_matches_buffer) {
  std::string xml;
  xml.reserve(400000);
  xml += "<root>";
  for (int i = 0; i < 100; ++i) {
    xml += "<row r=\"" + std::to_string(i + 1) + "\"><c>v" + std::to_string(i) + "</c></row>";
  }
  xml += "<big>";
  xml.append(300000, 'x');
  xml += "&amp;</big></root>";

  XmlCursor buffered(xml);
  auto expected = token_signature(collect(buffered));

  for (size_t chunk : {size_t(1), size_t(7), size_t(64), size_t(4096)}) {
    ChunkSource source(xml, chunk);
    XmlCursor streaming(source);
    CHECK_EQ(token_signature(collect(streaming)), expected);
  }
  CHECK(expected.find("T(" + std::string(300000, 'x') + "&)") != std::string::npos);
}

TEST(xml_writer_roundtrip) {
  std::string buffer;
  BufferSink sink(buffer);
  XmlWriter writer(sink);
  writer.declaration();
  writer.start("root");
  writer.attr("name", "a<b&c\"d");
  writer.end_start();
  writer.text("line1\r\nline2 <tag> & _x0041_ ");
  writer.start("child");
  writer.end_start_self();
  writer.end("root");
  sink.flush();

  XmlCursor cursor(buffer);
  auto tokens = collect(cursor);
  REQUIRE(tokens.size() == 5u); // declaration newline, root, text, child, /root
  CHECK_EQ(tokens[0].kind, Token::Text);
  CHECK_EQ(tokens[1].name, "root");
  CHECK_EQ(tokens[1].attrs.at(0).second, "a<b&c\"d");
  CHECK_EQ(tokens[2].text, "line1\nline2 <tag> & _x005F_x0041_ ");
  CHECK(tokens[3].self_closing);
}

TEST(xml_excel_escapes) {
  std::string scratch;
  CHECK_EQ(decode_excel_escapes("a_x000D_b", scratch), "a\rb");
  CHECK_EQ(decode_excel_escapes("_xD83D__xDE00_", scratch), "\xF0\x9F\x98\x80");
  CHECK_EQ(decode_excel_escapes("plain", scratch), "plain");

  CHECK_EQ(encode_excel_escapes("_x0041_", scratch), "_x005F_x0041_");
  CHECK_EQ(encode_excel_escapes("_x00zz_", scratch), "_x00zz_");
  CHECK_EQ(encode_excel_escapes("nothing", scratch), "nothing");
}
