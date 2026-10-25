#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "commands.hpp"
#include "cpp_interpreter.hpp"
#include "library_manager.hpp"
#include "session_commands.hpp"

namespace example {
namespace {

class RecordMembers : public ::testing::Test {
protected:
  void SetUp() override {
    std::string error;
    cpp = CppInterpreter::create({}, error);
    ASSERT_NE(cpp, nullptr) << error;
    ASSERT_TRUE(cpp->execute(R"cpp(
namespace members {
struct Value {};
class Root {
  int secret;
  void hidden();
protected:
  double guarded;
  void protect();
public:
  Value value;
  static int count;
  Root() = default;
  ~Root() = default;
  virtual int run(Value) const;
  int run(int);
  static bool ready();
  template <class T> T convert(T);
  Root& operator=(int);
  using Alias = int;
  struct Nested { int nested; };
};
struct Left : virtual Root { int left; void left_method(); };
struct Right : virtual Root { int right; void right_method(); };
class Leaf : private Left, public Right {
  int own;
  void own_method();
public:
  int run(int);
};
union Choice { int i; double d; void clear(); private: char c; };
struct Empty {};
struct Forward;
using Alias = Leaf;
template <class T> struct Generic { T value; T get() const; };
struct Concrete : Generic<int> {};
}
)cpp"));
  }

  std::string query(const std::string& command, bool expected_ok = true) {
    LibraryManager libs;
    std::ostringstream out;
    const auto result = execute_command(libs, *cpp, parse_command(command), out);
    EXPECT_EQ(result.ok, expected_ok) << out.str();
    EXPECT_TRUE(result.keep_running);
    return out.str();
  }

  std::unique_ptr<CppInterpreter> cpp;
};

TEST_F(RecordMembers, DefaultsToDirectMembersAndDeclaredAccess) {
  EXPECT_EQ(query("%fields members::Leaf"), "members::Leaf:\n  private members::Leaf::own : int\n");
  const auto methods = query("%methods members::Leaf");
  EXPECT_NE(methods.find("private members::Leaf::own_method : void ()"), std::string::npos);
  EXPECT_NE(methods.find("public members::Leaf::run : int (int)"), std::string::npos);
  EXPECT_EQ(methods.find("members::Root"), std::string::npos);
  EXPECT_EQ(methods.find("operator="), std::string::npos); // implicit methods are omitted
  EXPECT_EQ(query("%fields members::Root access private"),
            "members::Root:\n  private members::Root::secret : int\n");
  EXPECT_EQ(query("%methods members::Root access protected"),
            "members::Root:\n  protected members::Root::protect : void ()\n");
}

TEST_F(RecordMembers, GroupsAndDeduplicatesAncestorsIncludingPrivateBases) {
  const auto fields = query("%fields ::members::Alias include-parents true");
  EXPECT_LT(fields.find("members::Leaf:"), fields.find("members::Left:"));
  EXPECT_LT(fields.find("members::Left:"), fields.find("members::Root:"));
  EXPECT_LT(fields.find("members::Root:"), fields.find("members::Right:"));
  EXPECT_EQ(fields.find("members::Root:\n", fields.find("members::Root:\n") + 1),
            std::string::npos);
  EXPECT_NE(fields.find("private members::Root::secret : int"), std::string::npos);
  const auto public_fields = query("%fields members::Leaf access public include-parents true");
  EXPECT_EQ(public_fields.find("::own"), std::string::npos);
  EXPECT_EQ(public_fields.find("::secret"), std::string::npos);
  EXPECT_EQ(public_fields.find("::guarded"), std::string::npos);
  EXPECT_NE(public_fields.find("public static members::Root::count : int"), std::string::npos);
  EXPECT_EQ(query("%fields members::Leaf include-parents false"), query("%fields members::Leaf"));
}

TEST_F(RecordMembers, ListsOverloadsTemplatesOperatorsAndQualifiedTypes) {
  const auto methods = query("%methods members::Root access public");
  for (const auto* text :
       {"members::Root::Root : void ()", "members::Root::~Root : void ()",
        "members::Root::run : int (members::Value) const", "members::Root::run : int (int)",
        "static members::Root::ready : bool ()", "members::Root::convert : T (T)",
        "members::Root::operator= : members::Root &(int)"}) {
    EXPECT_NE(methods.find(text), std::string::npos) << text << '\n' << methods;
  }
  EXPECT_EQ(methods.find("::hidden"), std::string::npos);
  const auto fields = query("%fields members::Root");
  EXPECT_NE(fields.find("members::Root::value : members::Value"), std::string::npos);
  EXPECT_EQ(fields.find("::Alias"), std::string::npos);
  EXPECT_EQ(fields.find("::Nested"), std::string::npos);
  EXPECT_EQ(fields.find("::run"), std::string::npos);
  EXPECT_EQ(query("%fields members::Concrete include-parents true"),
            "members::Concrete:\n  (no matching fields)\nmembers::Generic<int>:\n"
            "  public members::Generic<int>::value : int\n");
}

TEST_F(RecordMembers, SupportsUnionsEmptyResultsAndMissingRecords) {
  EXPECT_EQ(query("%fields members::Choice access private"),
            "members::Choice:\n  private members::Choice::c : char\n");
  EXPECT_EQ(query("%methods members::Choice"),
            "members::Choice:\n  public members::Choice::clear : void ()\n");
  EXPECT_EQ(query("%methods members::Empty"), "members::Empty:\n  (no matching methods)\n");
  for (const auto* name : {"members::Forward", "members::Missing", "members"}) {
    EXPECT_NE(query(std::string("%fields ") + name, false).find("no complete record named"),
              std::string::npos);
  }
  EXPECT_NE(query("%methods", false).find("usage: %methods"), std::string::npos);
  EXPECT_TRUE(cpp->execute("int members_session_alive = 42;"));
  EXPECT_EQ(cpp->evaluate("members_session_alive"), "(int) 42\n");
}

TEST_F(RecordMembers, PreservesComplexTypesAndReadsLatestDefinitions) {
  ASSERT_TRUE(cpp->execute(R"cpp(
namespace members {
struct Complex {
  Value values[2];
  Value (*callback)(const Value&);
  const Value& inspect(const Value&) const & noexcept;
};
}
)cpp"));
  const auto fields = query("%fields members::Complex");
  EXPECT_NE(fields.find("members::Value[2]"), std::string::npos) << fields;
  EXPECT_NE(fields.find("members::Value (*)(const members::Value &)"), std::string::npos) << fields;
  const auto methods = query("%methods members::Complex");
  EXPECT_NE(methods.find("const members::Value &(const members::Value &) const & noexcept"),
            std::string::npos)
      << methods;
  ASSERT_TRUE(cpp->execute("namespace members { struct Complex { double replacement; }; }"));
  EXPECT_EQ(query("%fields members::Complex"),
            "members::Complex:\n  public members::Complex::replacement : double\n");
  EXPECT_EQ(query("%methods members::Complex"), "members::Complex:\n  (no matching methods)\n");
}

} // namespace
} // namespace example
