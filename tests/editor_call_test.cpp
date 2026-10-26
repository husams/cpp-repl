#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "editor_call.hpp"

namespace example {
namespace {

EditorCall at(std::string_view text, std::string_view needle) {
  const std::size_t cursor = text.find(needle);
  EXPECT_NE(cursor, std::string_view::npos) << needle;
  const auto call = editor_call_at_cursor(text, cursor);
  EXPECT_TRUE(call.has_value()) << text;
  return call.value_or(EditorCall{});
}

TEST(EditorCall, FindsInnermostCallAndActiveArgument) {
  constexpr std::string_view text = "prefix + outer(first, inner(1, 2), last) + suffix";
  const EditorCall inner = at(text, "2");
  EXPECT_EQ(text.substr(inner.open, 1), "(");
  EXPECT_EQ(inner.arguments.size(), 2u);
  EXPECT_EQ(inner.active_argument, 1u);

  const EditorCall outer = at(text, "last");
  EXPECT_EQ(outer.arguments.size(), 3u);
  EXPECT_EQ(outer.active_argument, 2u);
}

TEST(EditorCall, IgnoresCommasAndParenthesesInLiteralsAndComments) {
  constexpr std::string_view text =
      R"call(call("comma, )", 'x', /* , ( */ value // , )
  , other))call";
  const EditorCall call = at(text, "other");
  ASSERT_EQ(call.arguments.size(), 4u);
  EXPECT_EQ(call.active_argument, 3u);
  EXPECT_FALSE(editor_call_at_cursor(text, text.find("comma") + 1).has_value());
  EXPECT_FALSE(editor_call_at_cursor(text, text.find("/*") + 2).has_value());
}

TEST(EditorCall, KeepsTemplateArgumentCommasInsideOneCallArgument) {
  constexpr std::string_view text = "invoke(snippet_template<int, int>(first), "
                                    "std::vector<std::pair<long, short>>{}, last)";
  const EditorCall call = at(text, "last");
  ASSERT_EQ(call.arguments.size(), 3u);
  EXPECT_EQ(call.active_argument, 2u);
  EXPECT_EQ(text.substr(call.arguments[1].begin, call.arguments[1].end - call.arguments[1].begin),
            "std::vector<std::pair<long, short>>{}");
}

TEST(EditorCall, ComparisonCommasStillSplitArguments) {
  constexpr std::string_view text = "call(a < b, c, d)";
  const EditorCall call = at(text, "d");
  ASSERT_EQ(call.arguments.size(), 3u);
  EXPECT_EQ(call.active_argument, 2u);
}

TEST(EditorCall, IgnoresRawStringContents) {
  constexpr std::string_view text = R"(call(R"tag(, ) ignored)tag", value))";
  const EditorCall call = at(text, "value");
  ASSERT_EQ(call.arguments.size(), 2u);
  EXPECT_FALSE(editor_call_at_cursor(text, text.find("ignored")).has_value());
}

TEST(EditorCall, UsesByteOffsetsAndHandlesMidLineCalls) {
  constexpr std::string_view text = "const char* s = \"λ\"; use(α, β); trailing";
  const std::size_t beta = text.find("β");
  const auto call = editor_call_at_cursor(text, beta);
  ASSERT_TRUE(call.has_value());
  ASSERT_EQ(call->arguments.size(), 2u);
  EXPECT_EQ(call->active_argument, 1u);
  EXPECT_EQ(
      text.substr(call->arguments[0].begin, call->arguments[0].end - call->arguments[0].begin),
      "α");
}

TEST(EditorCall, MissingCloserAndWhitespaceProduceUsableSlots) {
  constexpr std::string_view incomplete = "before(); call(  first ,   ";
  const EditorCall call = at(incomplete, "first");
  EXPECT_FALSE(call.has_close);
  EXPECT_EQ(call.close, incomplete.size());
  ASSERT_EQ(call.arguments.size(), 2u);
  EXPECT_EQ(
      incomplete.substr(call.arguments[0].begin, call.arguments[0].end - call.arguments[0].begin),
      "first");
  EXPECT_EQ(call.arguments[1].begin, call.arguments[1].end);

  constexpr std::string_view empty = "call(   )";
  const EditorCall empty_call = at(empty, ")");
  ASSERT_EQ(empty_call.arguments.size(), 1u);
  EXPECT_EQ(empty_call.arguments[0].begin, empty_call.arguments[0].end);
  EXPECT_EQ(empty_call.active_argument, 0u);

  constexpr std::string_view comments_only = "call(/* comment, ( ) */)";
  const auto comment_call = editor_call_at_cursor(comments_only, comments_only.rfind(')'));
  ASSERT_TRUE(comment_call.has_value());
  ASSERT_EQ(comment_call->arguments.size(), 1u);
  EXPECT_EQ(comment_call->arguments[0].begin, comment_call->arguments[0].end);
}

TEST(EditorCall, CursorAfterCommaSelectsNextEmptyArgument) {
  constexpr std::string_view text = "call(first, )";
  const std::size_t cursor = text.find(")");
  const auto call = editor_call_at_cursor(text, cursor);
  ASSERT_TRUE(call.has_value());
  ASSERT_EQ(call->arguments.size(), 2u);
  EXPECT_EQ(call->active_argument, 1u);
}

} // namespace
} // namespace example
