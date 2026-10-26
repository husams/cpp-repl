#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cpp_interpreter.hpp"

namespace example {
namespace {

bool has(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

struct IncludeTree {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("cpp-repl-editor-context-" + std::to_string(getpid()));
  std::filesystem::path quoted = root / "quoted";
  std::filesystem::path system = root / "system";

  IncludeTree() {
    std::filesystem::create_directories(quoted / "nested");
    std::filesystem::create_directories(system / "nested");
    std::ofstream(quoted / "alpha_header.hpp") << "#pragma once\n";
    std::ofstream(quoted / "__detail.hpp") << "#pragma once\n";
    std::ofstream(quoted / "nested" / "beta_header.hpp") << "#pragma once\n";
    std::ofstream(quoted / "editor_edge_header.hpp") << "#pragma once\n";
    std::ofstream(system / "system_header.hpp") << "#pragma once\n";
    std::ofstream(system / "nested" / "system_nested.hpp") << "#pragma once\n";
  }

  ~IncludeTree() { std::filesystem::remove_all(root); }
};

TEST(EditorContextTest, IncludePathCompletionReportsReplacementPrefix) {
  IncludeTree tree;
  std::string error;
  auto cpp = CppInterpreter::create({"-I" + tree.quoted.string(), "-isystem", tree.system.string()},
                                    error);
  ASSERT_NE(cpp, nullptr) << error;

  const auto quoted = cpp->complete("#include \"alpha_he");
  EXPECT_TRUE(has(quoted.names, "alpha_header.hpp"));
  EXPECT_EQ(quoted.prefix, "alpha_he");
  EXPECT_TRUE(has(cpp->complete("#include \"__d").names, "__detail.hpp"));

  const auto extension = cpp->complete("#include \"alpha_header.h");
  EXPECT_TRUE(has(extension.names, "alpha_header.hpp"));
  EXPECT_EQ(extension.prefix, "alpha_header.h");

  const auto directory = cpp->complete("#include \"ne");
  EXPECT_TRUE(has(directory.names, "nested/"));
  EXPECT_EQ(directory.prefix, "ne");

  const auto nested = cpp->complete("#include \"nested/beta_he");
  EXPECT_TRUE(has(nested.names, "nested/beta_header.hpp"));
  EXPECT_EQ(nested.prefix, "nested/beta_he");

  const auto repeated_separator = cpp->complete("#include \"nested//beta_he");
  EXPECT_TRUE(has(repeated_separator.names, "nested//beta_header.hpp"));
  EXPECT_EQ(repeated_separator.prefix, "nested//beta_he");

  const auto nested_directory = cpp->complete("#include \"nested/");
  EXPECT_TRUE(has(nested_directory.names, "nested/beta_header.hpp"));
  EXPECT_EQ(nested_directory.prefix, "nested/");

  const auto system = cpp->complete("#include <system_he");
  EXPECT_TRUE(has(system.names, "system_header.hpp"));
  EXPECT_EQ(system.prefix, "system_he");

  const auto system_nested = cpp->complete("#include <nested/system_ne");
  EXPECT_TRUE(has(system_nested.names, "nested/system_nested.hpp"));
  EXPECT_EQ(system_nested.prefix, "nested/system_ne");

  const auto block_comment = cpp->complete("#include /* comment */ \"nested/beta_he");
  EXPECT_TRUE(has(block_comment.names, "nested/beta_header.hpp"));
  EXPECT_EQ(block_comment.prefix, "nested/beta_he");

  const auto continued = cpp->complete("#include \\\n  \"nested/beta_he");
  EXPECT_TRUE(has(continued.names, "nested/beta_header.hpp"));
  EXPECT_EQ(continued.prefix, "nested/beta_he");

  const auto spliced_path = cpp->complete("#include \"editor_edge_\\\nhe");
  EXPECT_TRUE(has(spliced_path.names, "editor_edge_header.hpp"));
  EXPECT_EQ(spliced_path.prefix, "editor_edge_\\\nhe");

  EXPECT_FALSE(has(cpp->complete("int value; // #include \"nested/beta_he").names,
                   "nested/beta_header.hpp"));
}

TEST(EditorContextTest, MemberCompletionRespectsTypeAndAccessContext) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute(
      "struct EditorBase { int inherited_field; }; "
      "struct EditorObject : EditorBase { int field; int method() const; int mutable_method(); "
      "private: int hidden_field; }; "
      "EditorObject editor_object; EditorObject* editor_pointer; "
      "int EditorObjectValue; const EditorObject editor_const{}; "
      "EditorObject editor_factory() { return {}; }"));

  EXPECT_TRUE(has(cpp->complete("editor_object.fi").names, "field"));
  EXPECT_TRUE(has(cpp->complete("editor_object.inherited_").names, "inherited_field"));
  EXPECT_TRUE(has(cpp->complete("editor_pointer->fi").names, "field"));
  EXPECT_TRUE(has(cpp->complete("editor_factory().me").names, "method"));
  EXPECT_TRUE(has(cpp->complete("editor_const.me").names, "method"));
  EXPECT_FALSE(has(cpp->complete("editor_const.mutable_").names, "mutable_method"));
  EXPECT_FALSE(has(cpp->complete("editor_object.hidden_").names, "hidden_field"));
  const auto type_context = cpp->complete("using EditorAlias = EditorO");
  EXPECT_TRUE(has(type_context.names, "EditorObject"));
  EXPECT_FALSE(has(type_context.names, "EditorObjectValue"));
  EXPECT_FALSE(has(cpp->complete("// editor_").names, "editor_object"));
  const auto function = cpp->complete("editor_fac");
  const auto factory = std::find_if(function.items.begin(), function.items.end(),
                                    [](const auto& i) { return i.text == "editor_factory"; });
  ASSERT_NE(factory, function.items.end());
  EXPECT_NE(factory->label.find("editor_factory()"), std::string::npos);

  ASSERT_TRUE(cpp->execute("namespace editor_scope { int editor_function(int); "
                           "struct EditorCallable { int method(int); }; }"));
  const auto ordinary = cpp->complete("editor_scope::editor_fun");
  const auto ordinary_function =
      std::find_if(ordinary.items.begin(), ordinary.items.end(),
                   [](const auto& item) { return item.text == "editor_function"; });
  ASSERT_NE(ordinary_function, ordinary.items.end());
  EXPECT_TRUE(ordinary_function->insert_call);

  ASSERT_TRUE(cpp->execute("int editor_target(int);"));
  const auto typed_pointer = cpp->complete("int (*editor_typed_pointer)(int) = editor_tar");
  const auto pointer_function =
      std::find_if(typed_pointer.items.begin(), typed_pointer.items.end(),
                   [](const auto& item) { return item.text == "editor_target"; });
  ASSERT_NE(pointer_function, typed_pointer.items.end());
  EXPECT_FALSE(pointer_function->insert_call);

  const auto typed_reference = cpp->complete("int (&editor_typed_reference)(int) = editor_tar");
  const auto reference_function =
      std::find_if(typed_reference.items.begin(), typed_reference.items.end(),
                   [](const auto& item) { return item.text == "editor_target"; });
  ASSERT_NE(reference_function, typed_reference.items.end());
  EXPECT_FALSE(reference_function->insert_call);

  const auto value_initializer = cpp->complete("int editor_value = editor_tar");
  const auto value_function =
      std::find_if(value_initializer.items.begin(), value_initializer.items.end(),
                   [](const auto& item) { return item.text == "editor_target"; });
  ASSERT_NE(value_function, value_initializer.items.end());
  EXPECT_TRUE(value_function->insert_call);

  const auto address = cpp->complete("auto editor_function_pointer = &editor_scope::editor_fun");
  const auto address_function =
      std::find_if(address.items.begin(), address.items.end(),
                   [](const auto& item) { return item.text == "editor_function"; });
  ASSERT_NE(address_function, address.items.end());
  EXPECT_FALSE(address_function->insert_call);

  const auto parenthesized_address =
      cpp->complete("auto editor_parenthesized_pointer = &(editor_scope::editor_fun");
  const auto parenthesized_function =
      std::find_if(parenthesized_address.items.begin(), parenthesized_address.items.end(),
                   [](const auto& item) { return item.text == "editor_function"; });
  ASSERT_NE(parenthesized_function, parenthesized_address.items.end());
  EXPECT_FALSE(parenthesized_function->insert_call);

  const auto commented_address =
      cpp->complete("auto editor_commented_pointer = & /* address */ editor_scope::editor_fun");
  const auto commented_function =
      std::find_if(commented_address.items.begin(), commented_address.items.end(),
                   [](const auto& item) { return item.text == "editor_function"; });
  ASSERT_NE(commented_function, commented_address.items.end());
  EXPECT_FALSE(commented_function->insert_call);

  const auto method_address =
      cpp->complete("auto editor_method_pointer = &editor_scope::EditorCallable::me");
  const auto address_method = std::find_if(method_address.items.begin(), method_address.items.end(),
                                           [](const auto& item) { return item.text == "method"; });
  ASSERT_NE(address_method, method_address.items.end());
  EXPECT_FALSE(address_method->insert_call);

  const auto using_decl = cpp->complete("using editor_scope::editor_fun");
  const auto using_function =
      std::find_if(using_decl.items.begin(), using_decl.items.end(),
                   [](const auto& item) { return item.text == "editor_function"; });
  ASSERT_NE(using_function, using_decl.items.end());
  EXPECT_FALSE(using_function->insert_call);
}

TEST(EditorContextTest, CompletionItemsCarryStructuredParameterMetadata) {
  std::string error;
  auto cpp = CppInterpreter::create({}, error);
  ASSERT_NE(cpp, nullptr) << error;
  ASSERT_TRUE(cpp->execute("int editor_signature(int count, const char* label = nullptr); "
                           "int editor_empty(); "
                           "int editor_unnamed(int, double); "
                           "int editor_variadic(const char* format, ...); "
                           "struct EditorSignature { int method(double amount) const &; }; "
                           "template<class... Ts> int editor_pack(Ts... values);"));

  const auto completion = cpp->complete("editor_sign");
  const auto item =
      std::find_if(completion.items.begin(), completion.items.end(),
                   [](const auto& candidate) { return candidate.text == "editor_signature"; });
  ASSERT_NE(item, completion.items.end());
  EXPECT_EQ(item->identity.substr(0, std::string("editor_signature").size()), "editor_signature");
  ASSERT_EQ(item->parameters.size(), 2u);
  EXPECT_EQ(item->parameters[0].name, "count");
  EXPECT_EQ(item->parameters[0].type, "int");
  EXPECT_FALSE(item->parameters[0].has_default);
  EXPECT_EQ(item->parameters[1].name, "label");
  EXPECT_EQ(item->parameters[1].type, "const char *");
  EXPECT_TRUE(item->parameters[1].has_default);
  EXPECT_FALSE(item->variadic);

  const auto empty = cpp->complete("editor_empty");
  const auto empty_item =
      std::find_if(empty.items.begin(), empty.items.end(),
                   [](const auto& candidate) { return candidate.text == "editor_empty"; });
  ASSERT_NE(empty_item, empty.items.end());
  EXPECT_TRUE(empty_item->parameters.empty());
  EXPECT_FALSE(empty_item->variadic);

  const auto unnamed = cpp->complete("editor_unnamed");
  const auto unnamed_item =
      std::find_if(unnamed.items.begin(), unnamed.items.end(),
                   [](const auto& candidate) { return candidate.text == "editor_unnamed"; });
  ASSERT_NE(unnamed_item, unnamed.items.end());
  ASSERT_EQ(unnamed_item->parameters.size(), 2u);
  EXPECT_TRUE(unnamed_item->parameters[0].name.empty());
  EXPECT_EQ(unnamed_item->parameters[1].type, "double");

  const auto variadic = cpp->complete("editor_variadic");
  const auto variadic_item =
      std::find_if(variadic.items.begin(), variadic.items.end(),
                   [](const auto& candidate) { return candidate.text == "editor_variadic"; });
  ASSERT_NE(variadic_item, variadic.items.end());
  EXPECT_TRUE(variadic_item->variadic);

  const auto packed = cpp->complete("editor_pack");
  const auto pack_item =
      std::find_if(packed.items.begin(), packed.items.end(),
                   [](const auto& candidate) { return candidate.text == "editor_pack"; });
  ASSERT_NE(pack_item, packed.items.end());
  ASSERT_EQ(pack_item->parameters.size(), 1u);
  EXPECT_TRUE(pack_item->parameters[0].is_pack);

  const auto method = cpp->complete("EditorSignature::method");
  const auto method_item =
      std::find_if(method.items.begin(), method.items.end(),
                   [](const auto& candidate) { return candidate.text == "method"; });
  ASSERT_NE(method_item, method.items.end());
  EXPECT_EQ(method_item->parameters.size(), 1u);
  EXPECT_EQ(method_item->parameters[0].type, "double");
  EXPECT_NE(method_item->label.find("const &"), std::string::npos);

  const auto typed_call = cpp->complete("editor_signature(");
  EXPECT_FALSE(typed_call.signature_items.empty());
  const auto active_signature =
      std::find_if(typed_call.signature_items.begin(), typed_call.signature_items.end(),
                   [](const auto& candidate) { return candidate.text == "editor_signature"; });
  ASSERT_NE(active_signature, typed_call.signature_items.end());
  EXPECT_FALSE(active_signature->identity.empty());
  ASSERT_EQ(active_signature->parameters.size(), 2u);
  EXPECT_EQ(active_signature->parameters[1].name, "label");
  EXPECT_TRUE(active_signature->parameters[1].has_default);
}

TEST(EditorContextTest, IncompleteContextCompletionReturnsWithoutSessionMutation) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::string error;
    auto cpp = CppInterpreter::create({}, error);
    if (!cpp) {
      _exit(2);
    }
    for (const auto* partial :
         {"struct EditorPartial { int value =", "struct EditorPartial { int value = 1; int next =",
          "void editor_partial() { int local =", "int editor_partial_expression() { return 1 +"}) {
      cpp->complete(partial);
    }
    _exit(cpp->evaluate("20 + 22") == std::optional<std::string>("(int) 42\n") ? 0 : 3);
  }
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  pid_t waited = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    waited = waitpid(child, &status, WNOHANG);
    if (waited != 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (waited != child) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    FAIL() << "editor completion exceeded its 10 second deadline";
  }
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace
} // namespace example
