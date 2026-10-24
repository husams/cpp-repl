#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "cpp_lexer.hpp"
#include "highlight_cpp.hpp"

namespace example {
namespace {

using Pieces = std::vector<std::pair<TokenKind, std::string>>;

Pieces lex(std::string_view src) {
  Pieces out;
  for (const Token& t : lex_cpp(src).tokens) {
    out.emplace_back(t.kind, std::string(src.substr(t.begin, t.end - t.begin)));
  }
  return out;
}

TokenKind kind_of(std::string_view src) {
  const auto p = lex(src);
  EXPECT_EQ(p.size(), 1u) << src;
  return p.empty() ? TokenKind::Other : p.front().first;
}

TEST(Lexer, KeywordsTypesAndIdentifiers) {
  EXPECT_EQ(kind_of("while"), TokenKind::Keyword);
  EXPECT_EQ(kind_of("constexpr"), TokenKind::Keyword);
  EXPECT_EQ(kind_of("nullptr"), TokenKind::Keyword);
  EXPECT_EQ(kind_of("int"), TokenKind::Type);
  EXPECT_EQ(kind_of("uint64_t"), TokenKind::Type);
  EXPECT_EQ(kind_of("vector"), TokenKind::Type);
  EXPECT_EQ(kind_of("my_var1"), TokenKind::Identifier);
  EXPECT_EQ(kind_of("iffy"), TokenKind::Identifier);
}

TEST(Lexer, NumericLiterals) {
  for (const char* n : {"42", "0x1F", "0XdeadBEEF", "0b1010", "017", "3.14",
                        ".5", "1e10", "1.5e-3", "2.f", "10ull", "0xFFu",
                        "1'000'000", "0b1010'0101", "0x1.8p+3", "5_km"}) {
    EXPECT_EQ(kind_of(n), TokenKind::Number) << n;
  }
}

TEST(Lexer, SignAfterExponentStaysInNumber) {
  EXPECT_EQ(lex("1e+5").size(), 1u);
  const auto p = lex("0xe+1");  // hex digit 'e' is not an exponent
  ASSERT_EQ(p.size(), 3u);
  EXPECT_EQ(p[1].first, TokenKind::Operator);
}

TEST(Lexer, StringsAndChars) {
  EXPECT_EQ(kind_of("\"hello world\""), TokenKind::String);
  EXPECT_EQ(kind_of("u8\"x\""), TokenKind::String);
  EXPECT_EQ(kind_of("L\"x\""), TokenKind::String);
  EXPECT_EQ(kind_of("'a'"), TokenKind::Char);
  EXPECT_EQ(kind_of("L'a'"), TokenKind::Char);
}

TEST(Lexer, EscapesAreSeparateTokens) {
  const auto p = lex("\"a\\n\\x41\\u00e9\\101\\\"b\"");
  ASSERT_EQ(p.size(), 7u);
  EXPECT_EQ(p[0], (std::pair<TokenKind, std::string>{TokenKind::String, "\"a"}));
  EXPECT_EQ(p[1], (std::pair<TokenKind, std::string>{TokenKind::Escape, "\\n"}));
  EXPECT_EQ(p[2].second, "\\x41");
  EXPECT_EQ(p[3].second, "\\u00e9");
  EXPECT_EQ(p[4].second, "\\101");
  EXPECT_EQ(p[5].second, "\\\"");
  EXPECT_EQ(p[6], (std::pair<TokenKind, std::string>{TokenKind::String, "b\""}));
}

TEST(Lexer, CharEscape) {
  const auto p = lex(R"('\'')");
  ASSERT_EQ(p.size(), 3u);
  EXPECT_EQ(p[0].first, TokenKind::Char);
  EXPECT_EQ(p[1].first, TokenKind::Escape);
  EXPECT_EQ(p[2].first, TokenKind::Char);
}

TEST(Lexer, RawStrings) {
  EXPECT_EQ(kind_of(R"T(R"(a "quoted" \n )")T"), TokenKind::String);
  EXPECT_EQ(lex(R"T(R"x(a)" b)x")T").size(), 1u);
  EXPECT_EQ(lex("R\"(line1\nline2)\"").size(), 1u);
  const auto p = lex(R"T(u8R"(x)" y)T");
  ASSERT_EQ(p.size(), 2u);
  EXPECT_EQ(p[0].first, TokenKind::String);
  EXPECT_EQ(p[1].first, TokenKind::Identifier);
}

TEST(Lexer, MalformedRawDelimiterFallsBackToPlainString) {
  const auto p = lex(R"(R"a b")");
  ASSERT_GE(p.size(), 2u);
  EXPECT_EQ(p[0].first, TokenKind::Identifier);
  EXPECT_EQ(p[1].first, TokenKind::String);
}

TEST(Lexer, Comments) {
  EXPECT_EQ(kind_of("// hi \"not a string\""), TokenKind::Comment);
  EXPECT_EQ(kind_of("/* a\nb */"), TokenKind::Comment);
  const auto p = lex("x // c\ny");
  ASSERT_EQ(p.size(), 3u);
  EXPECT_EQ(p[1].second, "// c");
  EXPECT_EQ(p[2].first, TokenKind::Identifier);
}

TEST(Lexer, PreprocessorLines) {
  EXPECT_EQ(kind_of("#include <vector>"), TokenKind::Preprocessor);
  const auto p = lex("#define X \\\n  1\nint y");
  ASSERT_EQ(p.size(), 3u);
  EXPECT_EQ(p[0].first, TokenKind::Preprocessor);
  EXPECT_EQ(p[0].second, "#define X \\\n  1");
  EXPECT_EQ(p[1].first, TokenKind::Type);
}

TEST(Lexer, HashMidLineIsOperator) {
  const auto p = lex("a # b");
  ASSERT_EQ(p.size(), 3u);
  EXPECT_EQ(p[1].first, TokenKind::Operator);
}

TEST(Lexer, OperatorsUseLongestMatch) {
  EXPECT_EQ(lex("a<<=b").at(1).second, "<<=");
  EXPECT_EQ(lex("a->b").at(1).second, "->");
  EXPECT_EQ(lex("a::b").at(1).second, "::");
  EXPECT_EQ(lex("a<=>b").at(1).second, "<=>");
  EXPECT_EQ(lex("a&&b").at(1).second, "&&");
  EXPECT_EQ(kind_of("..."), TokenKind::Operator);
}

TEST(Lexer, BracketDepthAndMatching) {
  const auto r = lex_cpp("f(a[0]){}");
  std::vector<std::pair<TokenKind, int>> brackets;
  for (const Token& t : r.tokens) {
    if (t.kind == TokenKind::Bracket || t.kind == TokenKind::UnmatchedBracket)
      brackets.emplace_back(t.kind, t.depth);
  }
  const std::vector<std::pair<TokenKind, int>> expected = {
      {TokenKind::Bracket, 0}, {TokenKind::Bracket, 1}, {TokenKind::Bracket, 1},
      {TokenKind::Bracket, 0}, {TokenKind::Bracket, 0}, {TokenKind::Bracket, 0}};
  EXPECT_EQ(brackets, expected);
  EXPECT_TRUE(r.open_brackets.empty());
}

TEST(Lexer, UnmatchedClosers) {
  EXPECT_EQ(kind_of(")"), TokenKind::UnmatchedBracket);
  const auto p = lex("(]");
  ASSERT_EQ(p.size(), 2u);
  EXPECT_EQ(p[1].first, TokenKind::UnmatchedBracket);
  EXPECT_EQ(lex_cpp("(]").open_brackets, "(");
}

TEST(Lexer, TokensDoNotOverlap) {
  const std::string src = "int main() { return \"a\\n\" + 0x1f; } // done";
  std::size_t prev_end = 0;
  for (const Token& t : lex_cpp(src).tokens) {
    EXPECT_GE(t.begin, prev_end);
    EXPECT_LT(t.begin, t.end);
    prev_end = t.end;
  }
}

TEST(Lexer, Utf8InsideStringsAndIdentifiers) {
  EXPECT_EQ(kind_of("\"h\xc3\xa9llo\""), TokenKind::String);
  EXPECT_EQ(kind_of("caf\xc3\xa9"), TokenKind::Identifier);
}

TEST(Incomplete, UnbalancedBrackets) {
  EXPECT_TRUE(is_incomplete("int main() {"));
  EXPECT_TRUE(is_incomplete("f("));
  EXPECT_TRUE(is_incomplete("a[1"));
  EXPECT_TRUE(is_incomplete("{ ( ) "));
  EXPECT_FALSE(is_incomplete("int main() { return 0; }"));
  EXPECT_FALSE(is_incomplete("f(a[1], {2})"));
}

TEST(Incomplete, BracketsInsideLiteralsAndCommentsAreIgnored) {
  EXPECT_FALSE(is_incomplete("\"(\""));
  EXPECT_FALSE(is_incomplete("'{'"));
  EXPECT_FALSE(is_incomplete("x // {"));
  EXPECT_FALSE(is_incomplete("/* ( */ x"));
  EXPECT_FALSE(is_incomplete("R\"({)\""));
}

TEST(Incomplete, StraySurplusClosersAreNotIncomplete) {
  EXPECT_FALSE(is_incomplete(")"));
  EXPECT_FALSE(is_incomplete("}"));
}

TEST(Incomplete, UnterminatedLiterals) {
  EXPECT_TRUE(is_incomplete("\"abc"));
  EXPECT_TRUE(is_incomplete("'a"));
  EXPECT_TRUE(is_incomplete("\"abc\\\""));
  EXPECT_TRUE(is_incomplete("R\"(abc"));
  EXPECT_TRUE(is_incomplete("R\"x(abc)\""));
  EXPECT_TRUE(is_incomplete("R\"del"));
  EXPECT_FALSE(is_incomplete("R\"x(abc)x\""));
  EXPECT_FALSE(is_incomplete("\"abc\""));
}

TEST(Incomplete, UnclosedBlockComment) {
  EXPECT_TRUE(is_incomplete("/* abc"));
  EXPECT_TRUE(is_incomplete("int x; /*"));
  EXPECT_FALSE(is_incomplete("/* abc */"));
  EXPECT_FALSE(is_incomplete("// abc"));
}

TEST(Incomplete, TrailingOperatorOrComma) {
  EXPECT_TRUE(is_incomplete("int x ="));
  EXPECT_TRUE(is_incomplete("a +   "));
  EXPECT_TRUE(is_incomplete("f(1,"));
  EXPECT_TRUE(is_incomplete("a &&"));
  EXPECT_TRUE(is_incomplete("cout <<"));
  EXPECT_TRUE(is_incomplete("a ? b"  " ?"));
  EXPECT_TRUE(is_incomplete("x = 1 + // note"));
  EXPECT_FALSE(is_incomplete("x = 1;"));
  EXPECT_FALSE(is_incomplete("i++"));
  EXPECT_FALSE(is_incomplete("vector<int>"));
  EXPECT_FALSE(is_incomplete("x = 1 + 2"));
}

TEST(Incomplete, PreprocessorLineContinuation) {
  EXPECT_TRUE(is_incomplete("#define X \\"));
  EXPECT_TRUE(is_incomplete("#define X \\\n"));
  EXPECT_FALSE(is_incomplete("#define X 1"));
  EXPECT_FALSE(is_incomplete("#include <vector>"));
}

TEST(Incomplete, EmptyAndBlank) {
  EXPECT_FALSE(is_incomplete(""));
  EXPECT_FALSE(is_incomplete("  \n "));
}

TEST(Incomplete, MultilineBlockBecomesCompleteWhenClosed) {
  EXPECT_TRUE(is_incomplete("int main() {\n  return 0;"));
  EXPECT_FALSE(is_incomplete("int main() {\n  return 0; }"));
}

using replxx::Replxx;

Replxx::colors_t colors_for(const std::string& input, size_t cps) {
  Replxx::colors_t colors(cps, Replxx::Color::DEFAULT);
  highlight_cpp(input, colors);
  return colors;
}

TEST(Highlight, ColorsKeywordStringAndComment) {
  const std::string src = "if \"s\" // c";
  const auto c = colors_for(src, src.size());
  EXPECT_EQ(c[0], token_color(TokenKind::Keyword));
  EXPECT_EQ(c[1], token_color(TokenKind::Keyword));
  EXPECT_EQ(c[2], Replxx::Color::DEFAULT);
  EXPECT_EQ(c[3], token_color(TokenKind::String));
  EXPECT_EQ(c[5], token_color(TokenKind::String));
  EXPECT_EQ(c[7], token_color(TokenKind::Comment));
  EXPECT_EQ(c[10], token_color(TokenKind::Comment));
}

TEST(Highlight, SpansEmbeddedNewlines) {
  const std::string src = "/* a\nb */ int\nx";
  const auto c = colors_for(src, src.size());
  EXPECT_EQ(c[4], token_color(TokenKind::Comment));  // the '\n' inside
  EXPECT_EQ(c[5], token_color(TokenKind::Comment));  // 'b'
  EXPECT_EQ(c[10], token_color(TokenKind::Type));    // "int" on the next token
  EXPECT_EQ(c[13], Replxx::Color::DEFAULT);          // '\n' between tokens
  EXPECT_EQ(c[14], token_color(TokenKind::Identifier));
}

TEST(Highlight, IndexesByCodePointNotByte) {
  // "é" is two bytes but a single code point.
  const std::string src = "\"\xc3\xa9\" int";
  const auto c = colors_for(src, 6);
  EXPECT_EQ(c[0], token_color(TokenKind::String));
  EXPECT_EQ(c[1], token_color(TokenKind::String));
  EXPECT_EQ(c[2], token_color(TokenKind::String));
  EXPECT_EQ(c[4], token_color(TokenKind::Type));
}

TEST(Highlight, BracketColorCyclesWithDepth) {
  const std::string src = "(((())))";
  const auto c = colors_for(src, src.size());
  EXPECT_EQ(c[0], token_color(TokenKind::Bracket, 0));
  EXPECT_EQ(c[1], token_color(TokenKind::Bracket, 1));
  EXPECT_EQ(c[7], c[0]);
  EXPECT_NE(c[0], c[1]);
  EXPECT_NE(c[1], c[2]);
}

TEST(TopLevelReturn, OnlyOutsideBraces) {
  EXPECT_TRUE(has_top_level_return("return 1;"));
  EXPECT_TRUE(has_top_level_return("if (x)\n  return;"));
  EXPECT_FALSE(has_top_level_return("int f() { return 1; }"));
  EXPECT_FALSE(has_top_level_return("{ return; }"));
  EXPECT_FALSE(has_top_level_return("[] { return 2; }()"));
  EXPECT_FALSE(has_top_level_return("int returned = 1; // return"));
  EXPECT_FALSE(has_top_level_return("const char* s = \"return\";"));
}

TEST(TrailingMemberAccess, Chains) {
  auto a = trailing_member_access("int n = p.na");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->names, (std::vector<std::string>{"p"}));
  EXPECT_EQ(a->arrows, (std::vector<bool>{false}));
  EXPECT_EQ(a->member_prefix, "na");

  a = trailing_member_access("a.b->");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->names, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(a->arrows, (std::vector<bool>{false, true}));
  EXPECT_EQ(a->member_prefix, "");
}

TEST(TrailingMemberAccess, RejectsOtherText) {
  for (const char* text : {"abc", "f().x", "3.", "ns::x.", "x.1", "", "."}) {
    EXPECT_FALSE(trailing_member_access(text).has_value()) << text;
  }
  EXPECT_EQ(trailing_identifier("std::vec"), "vec");
  EXPECT_EQ(trailing_identifier("x."), "");
}

}  // namespace
}  // namespace example
