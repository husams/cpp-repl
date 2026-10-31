#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "cpp_interpreter.hpp"

namespace example {
namespace {

bool has(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

bool has_signature(const CppInterpreter::Completions& result, const std::string& fragment) {
  return std::any_of(result.signature_items.begin(), result.signature_items.end(),
                     [&](const auto& item) {
                       return item.label.find(fragment) != std::string::npos;
                     });
}

bool has_token(const std::vector<SemanticToken>& tokens, const std::string& input,
               const std::string& spelling, SemanticKind kind) {
  return std::any_of(tokens.begin(), tokens.end(), [&](const auto& token) {
    if (token.kind != kind) {
      return false;
    }
    return input.substr(token.begin, token.end - token.begin).find(spelling) != std::string::npos;
  });
}

struct ScratchTree {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("cpp-repl-editor-pch-" + std::to_string(getpid()));

  ScratchTree() { std::filesystem::create_directories(root); }
  ~ScratchTree() { std::filesystem::remove_all(root); }
};

TEST(EditorPchTest, SignatureAndCompletionCachesDoNotCrossContaminate) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int editor_pch_cached(int); double editor_pch_cached(double);"));

  const std::string input = "editor_pch_cached(";
  const auto signatures_first = cpp->signatures(input);
  ASSERT_GE(signatures_first.signature_items.size(), 2u);
  const auto full_after_signatures = cpp->complete(input);
  EXPECT_FALSE(full_after_signatures.names.empty());
  EXPECT_GE(full_after_signatures.signature_items.size(), 2u);

  const auto full_first = cpp->complete(input);
  ASSERT_GE(full_first.signature_items.size(), 2u);
  const auto signatures_after_full = cpp->signatures(input);
  EXPECT_GE(signatures_after_full.signature_items.size(), 2u);
  EXPECT_TRUE(has_signature(signatures_after_full, "int"));
  EXPECT_TRUE(has_signature(signatures_after_full, "double"));

  ASSERT_TRUE(cpp->execute("long editor_pch_cached(long);"));
  const auto after_session_mutation = cpp->signatures(input);
  EXPECT_GE(after_session_mutation.signature_items.size(), 3u);
  EXPECT_TRUE(has_signature(after_session_mutation, "long"));
}

TEST(EditorPchTest, GlobalUsingAnonymousAndOperatorDeclarationsStayVisible) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute(
      "namespace editor_pch_imported { struct Tag {}; int editor_pch_over(int); "
      "int editor_pch_over(double); } "
      "using namespace editor_pch_imported; "
      "namespace { struct EditorPchLocal { int editor_pch_member; }; "
      "EditorPchLocal editor_pch_local; } "
      "int operator+(editor_pch_imported::Tag, int) { return 7; } "
      "long double operator\"\"_editor_pch(long double value) { return value; }"));

  EXPECT_TRUE(has(cpp->complete("editor_pch_ov").names, "editor_pch_over"));
  EXPECT_TRUE(has(cpp->complete("editor_pch_local.").names, "editor_pch_member"));
  const auto operator_signatures = cpp->signatures("operator+(");
  EXPECT_TRUE(has_signature(operator_signatures, "Tag"));
  const std::string operator_use = "editor_pch_imported::Tag{} + 1";
  EXPECT_FALSE(cpp->signatures("operator\"\"_editor_pch(").signature_items.empty());
  EXPECT_TRUE(cpp->evaluate(operator_use).has_value());
  EXPECT_TRUE(cpp->evaluate("1.0_editor_pch").has_value());
}

TEST(EditorPchTest, ImportingOneOverloadDependencyKeepsSiblingOverloadVisible) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int editor_pch_dependency(int x) { return x; } "
                           "double editor_pch_dependency(double x) { return x; } "
                           "int editor_pch_caller() { return editor_pch_dependency(1); }"));

  const auto result = cpp->signatures("editor_pch_caller(); editor_pch_dependency(");
  EXPECT_GE(result.signature_items.size(), 2u);
  EXPECT_TRUE(has_signature(result, "int"));
  EXPECT_TRUE(has_signature(result, "double"));
}

TEST(EditorPchTest, PreparedHeaderAndRetainedSessionDeclarationsRemainUsable) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_header.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n"
           "struct EditorPchHeaderType { int editor_pch_header_member; };\n"
           "int editor_pch_header_call(int);\n"
           "double editor_pch_header_call(double);\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_header.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  ASSERT_TRUE(cpp->execute("EditorPchHeaderType editor_pch_header_object; "
                           "struct EditorPchSessionType { int editor_pch_session_member; }; "
                           "EditorPchSessionType editor_pch_session_object; "
                           "void editor_pch_session_call(int); "
                           "void editor_pch_session_call(double);"));

  EXPECT_TRUE(has(cpp->complete("editor_pch_header_object.").names,
                  "editor_pch_header_member"));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  EXPECT_TRUE(has(cpp->complete("editor_pch_session_object.").names,
                  "editor_pch_session_member"));
  EXPECT_GE(cpp->signatures("editor_pch_header_call(").signature_items.size(), 2u);
  EXPECT_GE(cpp->signatures("editor_pch_session_call(").signature_items.size(), 2u);
}

TEST(EditorPchTest, HasIncludeChangeDisablesPchAndPreservesLiveHeaderDeclarations) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_has_include.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n"
           "#if __has_include(\"editor_pch_optional.hpp\")\n"
           "#include \"editor_pch_optional.hpp\"\n"
           "#else\n"
           "struct EditorPchOptionalType { int editor_pch_old_member; };\n"
           "typedef EditorPchOptionalType EditorPchOptionalAlias;\n"
           "#endif\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_has_include.hpp\""));
  {
    std::ofstream out(tree.root / "editor_pch_optional.hpp");
    out << "#pragma once\nstruct EditorPchOptionalType { int editor_pch_new_member; };\n";
  }
  cpp->prepare_editor();
  EXPECT_TRUE(cpp->editor_pch_path().empty());
  ASSERT_TRUE(cpp->execute("EditorPchOptionalAlias editor_pch_optional_object;"));
  const auto members = cpp->complete("editor_pch_optional_object.");
  EXPECT_TRUE(has(members.names, "editor_pch_old_member"));
  EXPECT_FALSE(has(members.names, "editor_pch_new_member"));
  EXPECT_TRUE(has(cpp->complete("EditorPchOptionalAl").names, "EditorPchOptionalAlias"));
}

TEST(EditorPchTest, HeaderMacroGeneratedOverloadsRemainDistinctWithActivePch) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_macro_overloads.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n"
           "struct EditorPchInt { int int_member; };\n"
           "struct EditorPchDouble { int double_member; };\n"
           "#define EDITOR_PCH_DECLARE(T, R) R editor_pch_macro_overload(T)\n"
           "#define EDITOR_PCH_DECLARE_BOTH EDITOR_PCH_DECLARE(int, EditorPchInt); "
           "EDITOR_PCH_DECLARE(double, EditorPchDouble);\n"
           "EDITOR_PCH_DECLARE_BOTH\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_macro_overloads.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  ASSERT_TRUE(cpp->execute(
      "struct EditorPchLater { decltype(editor_pch_macro_overload(0.0)) result; }; "
      "EditorPchLater editor_pch_later;"));

  const auto result = cpp->signatures("editor_pch_macro_overload(");
  EXPECT_GE(result.signature_items.size(), 2u);
  EXPECT_TRUE(has_signature(result, "int"));
  EXPECT_TRUE(has_signature(result, "double"));
  EXPECT_TRUE(has(cpp->complete("editor_pch_macro_overload(0.0).").names, "double_member"));
  EXPECT_TRUE(has(cpp->complete("editor_pch_later.result.").names, "double_member"));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, LaterRecordDefinitionDoesNotChangeOldOrNewObjectCompletion) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_shadow.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n"
           "struct EditorPchHeaderAnchor {};\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_shadow.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  ASSERT_TRUE(cpp->execute("struct EditorPchShadow { int editor_pch_old_field; }; "
                           "EditorPchShadow editor_pch_old_object;"));
  ASSERT_TRUE(cpp->execute("struct EditorPchShadow { int editor_pch_new_field; }; "
                           "EditorPchShadow editor_pch_new_object;"));

  const auto old_object = cpp->complete("editor_pch_old_object.");
  const auto new_object = cpp->complete("editor_pch_new_object.");
  EXPECT_TRUE(has(old_object.names, "editor_pch_old_field"));
  EXPECT_FALSE(has(old_object.names, "editor_pch_new_field"));
  EXPECT_TRUE(has(new_object.names, "editor_pch_new_field"));
  EXPECT_FALSE(has(new_object.names, "editor_pch_old_field"));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, FreshInterpreterReusesSamePrefixPchWithoutRewritingIt) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_reuse.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\nstruct EditorPchReuseType { int editor_pch_reuse_member; };\n";
  }

  const std::vector<std::string> args = {"-I" + tree.root.string()};
  std::string error;
  std::string first_path;
  std::filesystem::file_time_type first_write_time;
  {
    auto cpp = CppInterpreter::create(args, error);
    ASSERT_NE(cpp, nullptr) << error;
    ASSERT_TRUE(cpp->execute("#include \"editor_pch_reuse.hpp\""));
    cpp->prepare_editor();
    ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
    first_path = cpp->editor_pch_path();
    first_write_time = std::filesystem::last_write_time(first_path);
    EXPECT_TRUE(has(cpp->complete("EditorPchReuseType").names, "EditorPchReuseType"));
    EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  }

  auto cpp = CppInterpreter::create(args, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_reuse.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  EXPECT_EQ(cpp->editor_pch_path(), first_path);
  EXPECT_EQ(std::filesystem::last_write_time(cpp->editor_pch_path()), first_write_time);
  EXPECT_TRUE(has(cpp->complete("EditorPchReuseType").names, "EditorPchReuseType"));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, StandardLibraryTemplatesCombineWithLaterSessionDeclarations) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include <vector>\n#include <string>"));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  ASSERT_TRUE(cpp->execute("std::vector<std::string> editor_pch_words;"));

  EXPECT_TRUE(has(cpp->complete("editor_pch_words.").names, "push_back"));
  EXPECT_TRUE(has(cpp->complete("editor_pch_words.front().").names, "size"));
  EXPECT_FALSE(cpp->signatures("editor_pch_words.push_back(").signature_items.empty());
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, ExplicitPolicySpecializationKeepsTemplateMembersAvailable) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_template_policy.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n"
           "namespace editor_pch_template_policy {\n"
           "template <typename T> struct DestroyPolicy {\n"
           "  template <typename U> static int destroy(U* value) { return value != nullptr; }\n"
           "};\n"
           "template <> struct DestroyPolicy<int> {\n"
           "  template <typename U> static int destroy(U* value) { return value != nullptr; }\n"
           "};\n"
           "template <typename T> struct Retained {\n"
           "  int field = 37;\n"
           "  ~Retained() { DestroyPolicy<T>::template destroy<T>(static_cast<T*>(nullptr)); }\n"
           "};\n"
           "}\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_template_policy.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  ASSERT_TRUE(cpp->execute(
      "editor_pch_template_policy::Retained<int> editor_pch_template_retained;"));

  EXPECT_TRUE(has(cpp->complete("editor_pch_template_retained.fi").names, "field"));
  const auto signatures = cpp->signatures(
      "editor_pch_template_policy::DestroyPolicy<int>::destroy(");
  EXPECT_FALSE(signatures.signature_items.empty());
  EXPECT_EQ(cpp->evaluate("editor_pch_template_retained.field"), "(int) 37\n");
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, SemanticColoringVisitsMainFileRootsWithLargePreparedPch) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_semantic_colors.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n"
           "#define EDITOR_PCH_COLOR_MACRO(value) (value)\n"
           "template <typename T> struct EditorPchColorBox { T value; };\n";
    for (int i = 0; i < 20000; ++i) {
      out << "int editor_pch_unused_" << i << "(int);\n";
    }
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_semantic_colors.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();

  const std::string input =
      "namespace editor_color_outer { struct Widget { int field; "
      "int method(int value) { return value; } }; }\n"
      "template <typename T> T editor_color_identity(T value) { return value; }\n"
      "int editor_color_function() { EditorPchColorBox<int> box; "
      "editor_color_outer::Widget object; "
      "box.value = editor_color_identity(EDITOR_PCH_COLOR_MACRO(object.field)); "
      "return object.method(box.value); }\n"
      "editor_color_outer::Widget editor_color_global;\n"
      "editor_color_global.field;";
  const auto tokens = cpp->semantic_tokens(input);

  EXPECT_TRUE(has_token(tokens, input, "editor_color_outer", SemanticKind::Namespace));
  EXPECT_TRUE(has_token(tokens, input, "Widget", SemanticKind::Type));
  EXPECT_TRUE(has_token(tokens, input, "field", SemanticKind::Field));
  EXPECT_TRUE(has_token(tokens, input, "method", SemanticKind::Method));
  EXPECT_TRUE(has_token(tokens, input, "editor_color_function", SemanticKind::Function));
  EXPECT_TRUE(has_token(tokens, input, "editor_color_identity", SemanticKind::Function));
  EXPECT_TRUE(has_token(tokens, input, "EditorPchColorBox", SemanticKind::Type));
  EXPECT_TRUE(has_token(tokens, input, "EDITOR_PCH_COLOR_MACRO", SemanticKind::Macro));
  const auto global_occurrences = std::count_if(
      tokens.begin(), tokens.end(), [&](const SemanticToken& token) {
        return input.substr(token.begin, token.end - token.begin) == "editor_color_global" &&
               token.kind == SemanticKind::Variable;
      });
  EXPECT_EQ(global_occurrences, 2u);
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, NestedHeaderChangeInvalidatesCacheAndPreservesSessionAst) {
  ScratchTree tree;
  const auto dependency = tree.root / "editor_pch_nested.hpp";
  const auto header = tree.root / "editor_pch_parent.hpp";
  {
    std::ofstream out(dependency);
    out << "#pragma once\nstruct EditorPchNested { int editor_pch_old_field; };\n";
  }
  {
    std::ofstream out(header);
    out << "#pragma once\n#include \"editor_pch_nested.hpp\"\n"
           "EditorPchNested editor_pch_nested_global;\n";
  }

  const std::vector<std::string> args = {"-I" + tree.root.string()};
  std::string error;
  auto cpp = CppInterpreter::create(args, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_parent.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  const auto before_change = cpp->complete("editor_pch_nested_global.");
  EXPECT_TRUE(has(before_change.names, "editor_pch_old_field"));
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();

  {
    std::ofstream out(dependency, std::ios::trunc);
    out << "#pragma once\nstruct EditorPchNested { int editor_pch_new_field; };\n";
  }
  const auto after_change = cpp->complete("editor_pch_nested_global.editor_pch_");
  EXPECT_TRUE(cpp->editor_pch_path().empty());
  EXPECT_TRUE(has(after_change.names, "editor_pch_old_field"));
  EXPECT_FALSE(has(after_change.names, "editor_pch_new_field"));

  auto refreshed = CppInterpreter::create(args, error);
  ASSERT_NE(refreshed, nullptr) << error;
  ASSERT_TRUE(refreshed->execute("#include \"editor_pch_parent.hpp\""));
  refreshed->prepare_editor();
  ASSERT_FALSE(refreshed->editor_pch_path().empty()) << refreshed->editor_pch_status();
  const auto refreshed_completion =
      refreshed->complete("editor_pch_nested_global.editor_pch_");
  EXPECT_TRUE(has(refreshed_completion.names, "editor_pch_new_field"));
  EXPECT_FALSE(refreshed->editor_pch_path().empty()) << refreshed->editor_pch_status();
}

TEST(EditorPchTest, MacroChangesAfterPreparationAreNotRetainedAsStaleState) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_macro_header.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n#define EDITOR_PCH_HEADER_MACRO 17\n"
           "struct EditorPchFromHeader { int editor_pch_from_header; };\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_macro_header.hpp\""));
  ASSERT_TRUE(cpp->execute("#define EDITOR_PCH_SESSION_MACRO 23"));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  ASSERT_TRUE(cpp->execute("#undef EDITOR_PCH_SESSION_MACRO"));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();

  const std::string header_macro_input = "#if EDITOR_PCH_HEADER_MACRO";
  const auto header_macro_tokens = cpp->semantic_tokens(header_macro_input);
  EXPECT_TRUE(has_token(header_macro_tokens, header_macro_input, "EDITOR_PCH_HEADER_MACRO",
                        SemanticKind::Macro));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  const std::string removed_macro_input = "#if EDITOR_PCH_SESSION_MACRO";
  const auto removed_macro_tokens = cpp->semantic_tokens(removed_macro_input);
  EXPECT_FALSE(has_token(removed_macro_tokens, removed_macro_input, "EDITOR_PCH_SESSION_MACRO",
                         SemanticKind::Macro));
  EXPECT_TRUE(has(cpp->complete("EditorPchFromHe").names, "EditorPchFromHeader"));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, PchMacroReplayAppliesOnlyPostSnapshotChanges) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_macro_delta.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n"
           "#define EDITOR_PCH_DELTA_HEADER 17\n"
           "#define EDITOR_PCH_DELTA_PUSH 23\n"
           "#define EDITOR_PCH_DELTA_FUNCTION(x) ((x) + 1)\n"
           "struct EditorPchMacroDeltaAnchor {};\n";
  }
  {
    std::ofstream out(tree.root / "editor_pch_macro_delta_later.hpp");
    out << "#pragma once\n#define EDITOR_PCH_DELTA_NEW 43\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create(
      {"-std=c++20", "-DEDITOR_PCH_DELTA_CMDLINE=31", "-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_macro_delta.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();

  // Unchanged PCH macros stay available without replaying their directives.
  const std::string unchanged = "#if EDITOR_PCH_DELTA_HEADER";
  EXPECT_TRUE(has_token(cpp->semantic_tokens(unchanged), unchanged,
                        "EDITOR_PCH_DELTA_HEADER", SemanticKind::Macro));
  EXPECT_TRUE(has(cpp->complete("EDITOR_PCH_DELTA_HEADER").names,
                  "EDITOR_PCH_DELTA_HEADER"));
  EXPECT_TRUE(has(cpp->complete("EDITOR_PCH_DELTA_FUNCTION").names,
                  "EDITOR_PCH_DELTA_FUNCTION"));
  EXPECT_TRUE(has(cpp->complete("EDITOR_PCH_DELTA_CMDLINE").names,
                  "EDITOR_PCH_DELTA_CMDLINE"));
  EXPECT_TRUE(has(cpp->complete("#if EDITOR_PCH_DELTA_HE").names,
                  "EDITOR_PCH_DELTA_HEADER"));
  EXPECT_TRUE(has(cpp->complete("#ifdef EDITOR_PCH_DELTA_HE").names,
                  "EDITOR_PCH_DELTA_HEADER"));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();

  ASSERT_TRUE(cpp->execute("#undef EDITOR_PCH_DELTA_HEADER\n"
                           "#define EDITOR_PCH_DELTA_HEADER 41\n"
                           "#include \"editor_pch_macro_delta_later.hpp\"\n"
                           "#undef EDITOR_PCH_DELTA_CMDLINE"));
  EXPECT_FALSE(has(cpp->complete("EDITOR_PCH_DELTA_CM").names,
                   "EDITOR_PCH_DELTA_CMDLINE"));
  EXPECT_TRUE(has(cpp->complete("#define EDITOR_PCH_DELTA_BUFFER_LOCAL 43\n"
                                "#if EDITOR_PCH_DELTA_BUFFER_").names,
                  "EDITOR_PCH_DELTA_BUFFER_LOCAL"));
  EXPECT_FALSE(has(cpp->complete("#undef EDITOR_PCH_DELTA_HEADER\n"
                                 "EDITOR_PCH_DELTA_HEADER").names,
                   "EDITOR_PCH_DELTA_HEADER"));
  const std::string redefined = "#if EDITOR_PCH_DELTA_HEADER";
  EXPECT_TRUE(has_token(cpp->semantic_tokens(redefined), redefined,
                        "EDITOR_PCH_DELTA_HEADER", SemanticKind::Macro));
  const std::string added = "#if EDITOR_PCH_DELTA_NEW";
  EXPECT_TRUE(has_token(cpp->semantic_tokens(added), added, "EDITOR_PCH_DELTA_NEW",
                        SemanticKind::Macro));
  const std::string removed_command_line = "#if EDITOR_PCH_DELTA_CMDLINE";
  EXPECT_FALSE(has_token(cpp->semantic_tokens(removed_command_line), removed_command_line,
                         "EDITOR_PCH_DELTA_CMDLINE", SemanticKind::Macro));

  ASSERT_TRUE(cpp->execute("#pragma push_macro(\"EDITOR_PCH_DELTA_PUSH\")\n"
                           "#undef EDITOR_PCH_DELTA_PUSH\n"
                           "#define EDITOR_PCH_DELTA_PUSH 47\n"
                           "#pragma pop_macro(\"EDITOR_PCH_DELTA_PUSH\")"));
  const std::string restored = "#if EDITOR_PCH_DELTA_PUSH";
  EXPECT_TRUE(has_token(cpp->semantic_tokens(restored), restored,
                        "EDITOR_PCH_DELTA_PUSH", SemanticKind::Macro));
  const std::string replayed_values =
      "#if EDITOR_PCH_DELTA_HEADER == 41 && EDITOR_PCH_DELTA_NEW == 43 && "
      "EDITOR_PCH_DELTA_PUSH == 23 && !defined(EDITOR_PCH_DELTA_CMDLINE)\n"
      "int editor_pch_macro_delta_expected;\n"
      "#else\n"
      "int editor_pch_macro_delta_stale;\n"
      "#endif";
  const auto replayed_tokens = cpp->semantic_tokens(replayed_values);
  EXPECT_TRUE(has_token(replayed_tokens, replayed_values,
                        "editor_pch_macro_delta_expected", SemanticKind::Variable));
  EXPECT_FALSE(has_token(replayed_tokens, replayed_values,
                         "editor_pch_macro_delta_stale", SemanticKind::Variable));
  ASSERT_TRUE(cpp->execute("#if EDITOR_PCH_DELTA_HEADER == 41 && "
                           "EDITOR_PCH_DELTA_NEW == 43 && "
                           "EDITOR_PCH_DELTA_PUSH == 23\n"
                           "int editor_pch_macro_delta_value = 1;\n"
                           "#else\n"
                           "int editor_pch_macro_delta_value = 0;\n"
                           "#endif"));
  EXPECT_EQ(cpp->evaluate("editor_pch_macro_delta_value"), "(int) 1\n");
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, SameHeaderReincludeImportsNewNamespaceMembersAfterPchPrefix) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_reinclude_namespace.hpp";
  {
    std::ofstream out(header);
    out << "#if !defined(EDITOR_PCH_REINCLUDE_PHASE)\n"
           "#define EDITOR_PCH_REINCLUDE_PHASE 1\n"
           "namespace EditorPchReinclude { struct EditorPchBefore {}; }\n"
           "#else\n"
           "namespace EditorPchReinclude { struct EditorPchAfter {}; }\n"
           "#endif\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_reinclude_namespace.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();

  ASSERT_TRUE(cpp->execute("#undef EDITOR_PCH_REINCLUDE_PHASE\n"
                           "#define EDITOR_PCH_REINCLUDE_PHASE 2\n"
                           "#include \"editor_pch_reinclude_namespace.hpp\""));
  const auto before = cpp->complete("EditorPchReinclude::EditorPchBef");
  const auto after = cpp->complete("EditorPchReinclude::EditorPchAft");
  EXPECT_TRUE(has(before.names, "EditorPchBefore"));
  EXPECT_TRUE(has(after.names, "EditorPchAfter"));
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, RecursiveStandardLibraryImportDoesNotCrashEditorAnalysis) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_heavy_global.hpp";
  {
    std::ofstream out(header);
    out << "#pragma once\n"
           "#include <algorithm>\n#include <array>\n"
           "#include <chrono>\n#include <deque>\n#include <filesystem>\n"
           "#include <functional>\n#include <iostream>\n#include <list>\n"
           "#include <map>\n#include <memory>\n#include <numeric>\n"
           "#include <optional>\n#include <queue>\n#include <regex>\n"
           "#include <set>\n#include <sstream>\n#include <string>\n"
           "#include <tuple>\n#include <unordered_map>\n"
           "#include <unordered_set>\n#include <variant>\n#include <vector>\n";
    for (int i = 0; i < 1000; ++i) {
      out << "struct GlobalRecord_" << i << " { int value = " << i
          << "; inline int get() const { return value; } "
             "inline void set(int v) { value = v; } };\n"
          << "inline int global_profile_function_" << i << "(int x) { return x + " << i
          << "; }\n";
    }
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-std=c++20", "-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_heavy_global.hpp\""));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();

  const std::string incomplete = " std::vector<int> profile_value_1; profile_value_1";
  const auto tokens = cpp->semantic_tokens(incomplete);
  EXPECT_TRUE(has_token(tokens, incomplete, "profile_value_1", SemanticKind::Variable));
  ASSERT_TRUE(cpp->execute("std::vector<int> editor_pch_profile_value; "
                           "editor_pch_profile_value.push_back(42);"));
  EXPECT_TRUE(has(cpp->complete("editor_pch_profile_value.").names, "push_back"));
  EXPECT_FALSE(cpp->signatures("editor_pch_profile_value.push_back(").signature_items.empty());
  EXPECT_EQ(cpp->evaluate("editor_pch_profile_value.front()"), "(int) 42\n");
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

TEST(EditorPchTest, StandardLibraryNameCompletionSurvivesRequiresExpressionImports) {
  ScratchTree tree;
  const auto header = tree.root / "editor_pch_stdlib_bundle.hpp";
  {
    std::ofstream out(header);
    out << "#include <algorithm>\n#include <array>\n#include <chrono>\n"
           "#include <deque>\n#include <filesystem>\n#include <functional>\n"
           "#include <iostream>\n#include <list>\n#include <map>\n#include <memory>\n"
           "#include <numeric>\n#include <optional>\n#include <queue>\n#include <regex>\n"
           "#include <set>\n#include <sstream>\n#include <string>\n#include <tuple>\n"
           "#include <unordered_map>\n#include <unordered_set>\n#include <variant>\n"
           "#include <vector>\n";
  }

  std::string error;
  auto cpp = CppInterpreter::create({"-std=c++20", "-I" + tree.root.string()}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("#include \"editor_pch_stdlib_bundle.hpp\""));
  ASSERT_TRUE(cpp->execute(
      "namespace bench { inline int twice(int x) { return x * 2; } }\n"
      "std::vector<int> values = {1, 2, 3}; std::string label = \"profile\";"));
  cpp->prepare_editor();
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();

  EXPECT_TRUE(has(cpp->complete(" std::vec").names, "vector"));
  ASSERT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
  EXPECT_TRUE(has(cpp->complete("values.").names, "push_back"));
  EXPECT_EQ(cpp->evaluate("values.front()"), "(int) 1\n");
  EXPECT_FALSE(cpp->editor_pch_path().empty()) << cpp->editor_pch_status();
}

}  // namespace
}  // namespace example
