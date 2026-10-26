#include <utility>

#include <gtest/gtest.h>

#include "editor_input.hpp"

namespace example {
namespace {

TEST(EditorInputTest, OpeningDelimitersPairInCppPayload) {
  for (const auto [key, close] :
       {std::pair<char32_t, char32_t>{'(', ')'}, {'[', ']'}, {'{', '}'}}) {
    const auto decision = delimiter_decision("value", 5, key);
    EXPECT_EQ(decision.action, DelimiterAction::InsertPair);
    EXPECT_EQ(decision.closing, close);
  }
}

TEST(EditorInputTest, ClosingDelimiterSkipsExistingMatch) {
  EXPECT_EQ(delimiter_decision("call()", 5, ')').action, DelimiterAction::SkipClosing);
  EXPECT_EQ(delimiter_decision("items]", 5, ']').action, DelimiterAction::SkipClosing);
  EXPECT_EQ(delimiter_decision("block}", 5, '}').action, DelimiterAction::SkipClosing);
}

TEST(EditorInputTest, DelimiterPairBackspaceIsRecognized) {
  EXPECT_EQ(delimiter_decision("call()", 5, '\b').action, DelimiterAction::DeletePair);
  EXPECT_EQ(delimiter_decision("\"\"", 1, 127).action, DelimiterAction::DeletePair);
}

TEST(EditorInputTest, ShellInputUsesLiteralTypingAndSingleLineSubmission) {
  const std::string shell = "! printf ";
  EXPECT_FALSE(is_cpp_editor_payload(shell, shell.size()));
  EXPECT_EQ(delimiter_decision(shell, shell.size(), '\"').action, DelimiterAction::None);
  EXPECT_EQ(delimiter_decision(shell, shell.size(), '(').action, DelimiterAction::None);
  EXPECT_FALSE(editor_wants_more_input("! echo '", 8));
}

TEST(EditorInputTest, ExistingQuoteIsSkipped) {
  EXPECT_EQ(delimiter_decision("\"value\"", 6, '\"').action, DelimiterAction::SkipClosing);
  EXPECT_EQ(delimiter_decision("#include \"header\"", 16, '\"').action,
            DelimiterAction::SkipClosing);
}

TEST(EditorInputTest, QuoteAtExistingLiteralOpeningIsNotSkipped) {
  EXPECT_EQ(delimiter_decision("\"value\"", 0, '"').action, DelimiterAction::InsertPair);
}

TEST(EditorInputTest, DoesNotPairInsideStringsCommentsOrDigitSeparators) {
  EXPECT_EQ(delimiter_decision("\"text", 5, '"').action, DelimiterAction::None);
  EXPECT_EQ(delimiter_decision("// comment", 10, '(').action, DelimiterAction::None);
  EXPECT_EQ(delimiter_decision("R\"tag(raw", 9, '"').action, DelimiterAction::None);
  EXPECT_EQ(delimiter_decision("1", 1, '\'').action, DelimiterAction::None);
  EXPECT_EQ(delimiter_decision("0xFA", 4, '\'').action, DelimiterAction::None);
}

TEST(EditorInputTest, DoesNotBreakRawStringPrefixWhileTyping) {
  for (const std::string prefix : {"R", "u8R", "uR", "UR", "LR"}) {
    EXPECT_EQ(delimiter_decision(prefix, prefix.size(), '"').action, DelimiterAction::None)
        << prefix;
  }
}

TEST(EditorInputTest, HandlesEscapedQuotes) {
  EXPECT_EQ(delimiter_decision("\"escaped\\", 9, '"').action, DelimiterAction::None);
  const std::string text = "\"a\\\"b\"";
  EXPECT_EQ(delimiter_decision(text, text.size() - 1, '"').action, DelimiterAction::SkipClosing);
}

TEST(EditorInputTest, PairingResumesAfterClosedLiteralsAndBlockComments) {
  EXPECT_EQ(delimiter_decision("\"text\"", 6, '(').action, DelimiterAction::InsertPair);
  EXPECT_EQ(delimiter_decision("'x'", 3, '"').action, DelimiterAction::InsertPair);
  EXPECT_EQ(delimiter_decision("/* text */", 10, '(').action, DelimiterAction::InsertPair);
}

TEST(EditorInputTest, RecognizesCppPayloadButNotCommandOperands) {
  EXPECT_FALSE(is_cpp_editor_payload("%shell echo", 11));
  EXPECT_FALSE(is_cpp_editor_payload("%add-path /tmp", 14));
  const std::string command_body = "%mock editor_f() cpp`";
  EXPECT_TRUE(is_cpp_editor_payload(command_body, command_body.size()));
  EXPECT_EQ(delimiter_decision(command_body, command_body.size(), '{').action,
            DelimiterAction::InsertPair);
  EXPECT_TRUE(is_cpp_editor_payload("int f() { ", 10));
}

TEST(EditorInputTest, CursorOffsetCountsUnicodeCodepoints) {
  EXPECT_EQ(utf8_byte_offset("a🧪b", 2), 5);
}

TEST(EditorInputTest, IncludeAngleBracketsPairAtHeaderStart) {
  for (const std::string text : {"#include ", "  # include\t", "#include_next ", "#include",
                                 "int x;\n#include ", "#include /* header */ ", "#include \\\n "}) {
    const auto decision = delimiter_decision(text, text.size(), '<');
    EXPECT_EQ(decision.action, DelimiterAction::InsertPair) << text;
    EXPECT_EQ(decision.closing, '>') << text;
  }
}

TEST(EditorInputTest, IncludeAngleCloserSkipsExistingMatch) {
  for (const std::string text :
       {"#include <>", "#include <nested/header.hpp>", "# include_next <header.hpp>"}) {
    EXPECT_EQ(delimiter_decision(text, text.size() - 1, '>').action, DelimiterAction::SkipClosing)
        << text;
  }
}

TEST(EditorInputTest, EmptyIncludeAnglePairDeletesTogether) {
  const std::string text = "#include <>";
  for (const char32_t key : {char32_t{'\b'}, char32_t{127}}) {
    EXPECT_EQ(delimiter_decision(text, text.size() - 1, key).action, DelimiterAction::DeletePair);
  }
}

TEST(EditorInputTest, AnglePairingRequiresAnIncludeHeaderContext) {
  for (const std::string text :
       {"a ", "std::vector", "#if VALUE ", "#define LESS ", "#include HEADER ",
        "#include \"nested/", "#include <nested/", "#include <header> ", "// #include ",
        "/*\n#include ", "R\"tag(\n#include ", "#include // comment ", "%shell #include "}) {
    EXPECT_EQ(delimiter_decision(text, text.size(), '<').action, DelimiterAction::None) << text;
  }
  EXPECT_EQ(delimiter_decision("a > b", 2, '>').action, DelimiterAction::None);
  EXPECT_EQ(delimiter_decision("std::vector<>", 12, '\b').action, DelimiterAction::None);
  EXPECT_EQ(delimiter_decision("#include \"x>\"", 11, '>').action, DelimiterAction::None);
}

TEST(EditorInputTest, EnterContinuesInsideAnEmptyPairAfterAnIncompleteOperator) {
  EXPECT_TRUE(editor_wants_more_input("21 * ()", 6));
  EXPECT_TRUE(editor_wants_more_input("21 * ( )", 6));
  EXPECT_TRUE(editor_wants_more_input("21 * ( )", 7));
  EXPECT_TRUE(editor_wants_more_input("21 *", 4));
  EXPECT_TRUE(editor_wants_more_input("()", 1));
  const std::string commented_empty_pair = " /* note */ ()";
  EXPECT_TRUE(editor_wants_more_input(commented_empty_pair, commented_empty_pair.size() - 1));
  EXPECT_FALSE(editor_wants_more_input("f()", 2));
  EXPECT_FALSE(editor_wants_more_input("f( )", 2));
  EXPECT_FALSE(editor_wants_more_input("f( )", 3));
  EXPECT_FALSE(editor_wants_more_input("f(42)", 4));
  EXPECT_FALSE(editor_wants_more_input("f(20,22)", 7));
  EXPECT_FALSE(editor_wants_more_input("// call(\n", 9));
  EXPECT_FALSE(editor_wants_more_input("%hooks", 6));
  EXPECT_FALSE(editor_wants_more_input("x++", 3));
  EXPECT_FALSE(editor_wants_more_input("x;", 2));
  EXPECT_TRUE(editor_wants_more_input("if (x) {}", 8));
  EXPECT_FALSE(editor_wants_more_input("#include \"header\"", 16));
  const std::string continued_macro = "#define MSG \"abc\\\ndef\"";
  EXPECT_FALSE(editor_wants_more_input(continued_macro, continued_macro.size() - 1));
  const std::string unicode_call = "f(\"λ\")";
  EXPECT_FALSE(editor_wants_more_input(unicode_call, unicode_call.size()));
}

TEST(EditorInputTest, EnterContinuesAnEscapedPreprocessorQuote) {
  EXPECT_TRUE(editor_wants_more_input("#define MSG \"abc\\\"", 17));
  const std::string continued_quote = "#define MSG \"abc\\\ndef\\\"";
  EXPECT_TRUE(editor_wants_more_input(continued_quote, continued_quote.size() - 1));
  EXPECT_EQ(delimiter_decision(continued_quote, continued_quote.size() - 1, '"').action,
            DelimiterAction::None);
  const std::string continued_string = "#define MSG \"abc\\\ndef";
  EXPECT_EQ(delimiter_decision(continued_string, continued_string.size(), '(').action,
            DelimiterAction::None);
  const std::string continued_quote_with_comment = "#define MSG /* \"ignored\" */ \"abc\\\ndef\\\"";
  EXPECT_TRUE(editor_wants_more_input(continued_quote_with_comment,
                                      continued_quote_with_comment.size() - 1));
  const std::string complete_two_line_macro = "#define MSG \"abc\\\ndef\\\nghi\"";
  EXPECT_FALSE(
      editor_wants_more_input(complete_two_line_macro, complete_two_line_macro.size() - 1));
  EXPECT_EQ(
      delimiter_decision(complete_two_line_macro, complete_two_line_macro.size() - 1, '"').action,
      DelimiterAction::SkipClosing);
  EXPECT_FALSE(editor_wants_more_input("#define MSG \"abc\"", 17));
  EXPECT_FALSE(editor_wants_more_input("%info foo(\"x\")", 14));
}

TEST(EditorInputTest, AngleHeaderTreatsOrdinaryDelimitersAsFilenameCharacters) {
  const std::string header = "#include <dir/name";
  for (const char32_t key :
       {char32_t{'('}, char32_t{'['}, char32_t{'{'}, char32_t{'\"'}, char32_t{'\''}}) {
    EXPECT_EQ(delimiter_decision(header, header.size(), key).action, DelimiterAction::None);
  }
  for (const char32_t key : {char32_t{')'}, char32_t{']'}, char32_t{'}'}}) {
    EXPECT_EQ(delimiter_decision(header + ")", header.size(), key).action, DelimiterAction::None);
  }
  EXPECT_EQ(delimiter_decision("#include <header>", 16, '>').action, DelimiterAction::SkipClosing);
  EXPECT_EQ(delimiter_decision("#include <>", 10, '\b').action, DelimiterAction::DeletePair);
}

} // namespace
} // namespace example
