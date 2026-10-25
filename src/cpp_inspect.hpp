#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace clang {
class ASTContext;
class CXXRecordDecl;
}  // namespace clang

namespace example {

enum class MemberKind { Methods, Fields };
enum class AccessFilter { All, Public, Private, Protected };

struct MemberOptions {
  AccessFilter access = AccessFilter::All;
  bool include_parents = false;
};

// Session introspection over the interpreter's AST; nothing here runs code.

// Classes, structs and unions defined by user code (the inputs, %add-ed files
// and their non-system headers), as qualified names, sorted.
std::vector<std::string> user_class_names(clang::ASTContext& ctx);

// The class definition a (qualified) class or typedef name refers to, or
// nullptr.
const clang::CXXRecordDecl* find_class(clang::ASTContext& ctx, std::string_view name);

// A C++-like synopsis of `record`: bases and members as declared, with the
// layout (size, alignment, field offsets) in comments.
std::string describe_class(clang::ASTContext& ctx, const clang::CXXRecordDecl& record);

// Explicitly declared members, grouped by declaring record. Access is the
// declared access, independent of the access used to inherit a base.
std::string describe_members(clang::ASTContext& ctx, const clang::CXXRecordDecl& record,
                             MemberKind kind, MemberOptions options = {});

// Data members and named methods, inherited ones included, of the object an
// access chain names (see trailing_member_access()); empty if the chain does
// not lead from a variable through fields to a class.
std::vector<std::string> member_names(clang::ASTContext& ctx,
                                      const std::vector<std::string>& names,
                                      const std::vector<bool>& arrows);

}  // namespace example
