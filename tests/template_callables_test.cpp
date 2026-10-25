#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cpp_interpreter.hpp"

namespace example {
namespace {

class TemplateCallableDefinitions : public ::testing::Test {
 protected:
  void SetUp() override {
    cpp = CppInterpreter::create({"-fvisibility=hidden"}, error);
    ASSERT_NE(cpp, nullptr) << error;
    const auto header =
        std::filesystem::path(__FILE__).parent_path() / "fixtures/template_callables.hpp";
    // Native publication is process-wide. Give each test distinct symbols when
    // Google Test runs this whole suite in one process; the native ABI test
    // deliberately uses the unchanged header's namespace.
    const std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    const bool native = name == "PublishesAllThreeTemplateCasesToNativeLibrary";
    std::string input;
    if (!native) input = "#define template_callables template_callables_" + name + "\n";
    input += "#include \"" + header.string() + "\"\n";
    if (!native)
      input += "#undef template_callables\nnamespace template_callables = template_callables_" +
               name + ";";
    ASSERT_TRUE(cpp->execute(input));
  }
  std::string error;
  std::unique_ptr<CppInterpreter> cpp;
};

TEST_F(TemplateCallableDefinitions, DefinesMethodForFreshClassTemplateInstance) {
  ASSERT_TRUE(
      cpp->define_callable("std::string template_callables::Person<int>::name() const & noexcept",
                           "return std::to_string(this->value_);", error))
      << error;
  ASSERT_TRUE(cpp->execute("template_callables::Person<int> person{42};"));
  EXPECT_TRUE(cpp->evaluate("person.name()").has_value());
  EXPECT_EQ(cpp->evaluate("person.name() == \"42\""), "(bool) true\n");
}

TEST_F(TemplateCallableDefinitions, DefinesPreviouslyDeclaredMethodSpecialization) {
  ASSERT_TRUE(
      cpp->define_callable("std::string template_callables::Person<long>::name() const & noexcept",
                           "return \"long \" + std::to_string(value_);", error))
      << error;
  ASSERT_TRUE(cpp->execute("template_callables::Person<long> person{7};"));
  EXPECT_EQ(cpp->evaluate("person.name() == \"long 7\""), "(bool) true\n");
}

TEST_F(TemplateCallableDefinitions, DefinesFunctionTemplateInstancesIndependently) {
  ASSERT_TRUE(cpp->define_callable("std::string template_callables::describe<int>(int value)",
                                   "return std::to_string(value);", error))
      << error;
  ASSERT_TRUE(cpp->define_callable("std::string template_callables::describe<long>(long value)",
                                   "return \"long \" + std::to_string(value);", error))
      << error;
  EXPECT_EQ(cpp->evaluate("template_callables::describe<int>(42) == \"42\""), "(bool) true\n");
  EXPECT_EQ(cpp->evaluate("template_callables::describe<long>(7) == \"long 7\""), "(bool) true\n");
}

TEST_F(TemplateCallableDefinitions, InfersReturnTypesAndKeepsClassInstancesIndependent) {
  ASSERT_TRUE(cpp->define_callable("template_callables::Person<int>::name() const & noexcept",
                                   "return \"int\";", error))
      << error;
  ASSERT_TRUE(cpp->define_callable("template_callables::Person<double>::name() const & noexcept",
                                   "return \"double\";", error))
      << error;
  ASSERT_TRUE(cpp->define_callable("template_callables::describe<int>(int value)",
                                   "return \"function\";", error))
      << error;
  ASSERT_TRUE(
      cpp->execute("template_callables::Person<int> a{1}; "
                   "template_callables::Person<double> b{2};"));
  EXPECT_EQ(cpp->evaluate("a.name() == \"int\" && b.name() == \"double\" && "
                          "template_callables::describe<int>(0) == \"function\""),
            "(bool) true\n");
}

TEST_F(TemplateCallableDefinitions, ResolvesMemberAndFunctionOverloadsByExactParameters) {
  ASSERT_TRUE(cpp->define_callable("int template_callables::Person<int>::overloaded(int x) const",
                                   "return x + value_;", error))
      << error;
  ASSERT_TRUE(
      cpp->define_callable("int template_callables::Person<int>::overloaded(double x) const",
                           "return int(x) * value_;", error))
      << error;
  ASSERT_TRUE(cpp->define_callable("int template_callables::overloaded<int>(int x)",
                                   "return x + 10;", error))
      << error;
  ASSERT_TRUE(cpp->define_callable("int template_callables::overloaded<int>(int* x)",
                                   "return *x + 20;", error))
      << error;
  ASSERT_TRUE(cpp->execute("template_callables::Person<int> person{3}; int value = 2;"));
  EXPECT_EQ(cpp->evaluate("person.overloaded(4)"), "(int) 7\n");
  EXPECT_EQ(cpp->evaluate("person.overloaded(4.0)"), "(int) 12\n");
  EXPECT_EQ(cpp->evaluate("template_callables::overloaded<int>(value)"), "(int) 12\n");
  EXPECT_EQ(cpp->evaluate("template_callables::overloaded<int>(&value)"), "(int) 22\n");
}

TEST_F(TemplateCallableDefinitions, SupportsStaticMethodsNestedTypesAndNonTypeArguments) {
  ASSERT_TRUE(cpp->define_callable("int template_callables::Person<int>::scale(int x)",
                                   "return x * 3;", error))
      << error;
  ASSERT_TRUE(cpp->define_callable(
      "std::string template_callables::Person<std::pair<int, long>>::name() const & noexcept",
      "return std::to_string(value_.first + value_.second);", error))
      << error;
  ASSERT_TRUE(
      cpp->define_callable("int template_callables::count<(2 + 3)>(int x)", "return x + 5;", error))
      << error;
  ASSERT_TRUE(cpp->define_callable("int template_callables::Outer<int>::Inner<long>::value() const",
                                   "return 42;", error))
      << error;
  ASSERT_TRUE(cpp->execute("template_callables::Person<std::pair<int, long>> pair{{2, 3}};"));
  EXPECT_EQ(cpp->evaluate("template_callables::Person<int>::scale(14)"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("pair.name() == \"5\""), "(bool) true\n");
  EXPECT_EQ(cpp->evaluate("template_callables::count<5>(7)"), "(int) 12\n");
  EXPECT_EQ(cpp->evaluate("template_callables::Outer<int>::Inner<long>{}.value()"), "(int) 42\n");
}

TEST_F(TemplateCallableDefinitions, DefinesMemberOfFullySpecializedClassAndMemberTemplate) {
  ASSERT_TRUE(cpp->define_callable("int template_callables::Specialized<int>::value() const",
                                   "return 42;", error))
      << error;
  ASSERT_TRUE(
      cpp->define_callable("std::string template_callables::Formatter::format<int>(int x) const",
                           "return std::to_string(x);", error))
      << error;
  EXPECT_EQ(cpp->evaluate("template_callables::Specialized<int>{}.value()"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("template_callables::Formatter{}.format<int>(7) == \"7\""),
            "(bool) true\n");
}

TEST_F(TemplateCallableDefinitions, RecoversFromRejectedBodiesAndRejectsDuplicateDefinitions) {
  const std::string signature = "std::string template_callables::describe<int>(int value)";
  EXPECT_FALSE(cpp->define_callable(signature, "return nonexistent;", error));
  EXPECT_FALSE(cpp->define_callable(signature, "return absent(value);", error));
  ASSERT_TRUE(cpp->define_callable(signature, "return \"ready\";", error)) << error;
  EXPECT_FALSE(cpp->define_callable(signature, "return \"replaced\";", error));
  EXPECT_EQ(cpp->evaluate("template_callables::describe<int>(0) == \"ready\""), "(bool) true\n");
  const std::string method = "std::string template_callables::Person<int>::name() const & noexcept";
  EXPECT_FALSE(cpp->define_callable(method, "return unknown_member;", error));
  ASSERT_TRUE(cpp->define_callable(method, "return \"method\";", error)) << error;
  ASSERT_TRUE(cpp->execute("template_callables::Person<int> person{1};"));
  EXPECT_EQ(cpp->evaluate("person.name() == \"method\""), "(bool) true\n");
}

TEST_F(TemplateCallableDefinitions, WrongTypesAndQualifiersDoNotPoisonLaterDefinitions) {
  EXPECT_FALSE(cpp->define_callable("int template_callables::describe<int>(int value)",
                                    "return value;", error));
  EXPECT_FALSE(cpp->define_callable("std::string template_callables::describe<int>(double value)",
                                    "return \"bad\";", error));
  EXPECT_FALSE(cpp->define_callable("std::string template_callables::Person<int>::name()",
                                    "return \"bad\";", error));
  EXPECT_FALSE(cpp->define_callable(
      "std::string template_callables::describe<int>(int value) const", "return \"bad\";", error));
  EXPECT_FALSE(cpp->define_callable("int template_callables::Person<int>::scale(int x) &",
                                    "return x;", error));
  EXPECT_FALSE(cpp->define_callable("std::string template_callables::describe<int>(int value, ...)",
                                    "return \"bad\";", error));
  ASSERT_TRUE(cpp->define_callable("std::string ::template_callables::describe<int>(int value)",
                                   "return \"good\";", error))
      << error;
  ASSERT_TRUE(
      cpp->define_callable("std::string template_callables::Person<int>::name() const & noexcept",
                           "return \"good\";", error))
      << error;
  EXPECT_EQ(cpp->evaluate("template_callables::describe<int>(0) == \"good\""), "(bool) true\n");
}

TEST_F(TemplateCallableDefinitions, UsesReturnTypeToDisambiguateFunctionTemplates) {
  ASSERT_TRUE(cpp->define_callable("int template_callables::same_parameters<int>(int x)",
                                   "return x + 1;", error))
      << error;
  ASSERT_TRUE(cpp->define_callable("double template_callables::same_parameters<int>(int x)",
                                   "return x + 0.5;", error))
      << error;
  ASSERT_TRUE(
      cpp->execute("int (*integer)(int) = &template_callables::same_parameters<int>; "
                   "double (*floating)(int) = &template_callables::same_parameters<int>;"));
  EXPECT_EQ(cpp->evaluate("integer(41)"), "(int) 42\n");
  EXPECT_EQ(cpp->evaluate("floating(1) == 1.5"), "(bool) true\n");
}

TEST_F(TemplateCallableDefinitions, DefinesPrivateAndProtectedMethodsWithoutChangingAccessChecks) {
  ASSERT_TRUE(cpp->define_callable("int template_callables::Person<int>::hidden(int x) const",
                                   "return x * 2;", error))
      << error;
  ASSERT_TRUE(
      cpp->define_callable("int template_callables::Person<int>::protected_step(int x) const",
                           "return value_ + x;", error))
      << error;
  ASSERT_TRUE(cpp->execute("template_callables::Person<int> person{10};"));
  EXPECT_FALSE(cpp->execute("person.hidden(4);"));
  EXPECT_FALSE(cpp->execute("person.protected_step(4);"));
  EXPECT_EQ(cpp->evaluate("person.call_hidden(4)"), "(int) 22\n");
}

TEST_F(TemplateCallableDefinitions, PublishesAllThreeTemplateCasesToNativeLibrary) {
  ASSERT_TRUE(
      cpp->define_callable("std::string template_callables::Person<int>::name() const & noexcept",
                           "return \"int\" + std::to_string(value_);", error))
      << error;
  ASSERT_TRUE(
      cpp->define_callable("std::string template_callables::Person<long>::name() const & noexcept",
                           "return \"long\" + std::to_string(value_);", error))
      << error;
  ASSERT_TRUE(cpp->define_callable("std::string template_callables::describe<int>(int value)",
                                   "return \"function\" + std::to_string(value);", error))
      << error;
  ASSERT_TRUE(cpp->load_library(TEMPLATE_CALLABLES_LIBRARY).empty());
  ASSERT_TRUE(cpp->execute("extern \"C\" int template_callable_native_probe();"));
  EXPECT_EQ(cpp->evaluate("template_callable_native_probe()"), "(int) 1\n");
}

}  // namespace
}  // namespace example
