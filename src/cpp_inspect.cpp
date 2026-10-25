#include "cpp_inspect.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <utility>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/RecordLayout.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "definition_shadowing.hpp"
#include "llvm/Support/raw_ostream.h"

namespace example {

namespace {

// User code: the inputs (virtual files named input_line_N), %add-ed files
// and the headers they include, but not system headers (the standard library,
// the SDK) or Clang's built-in declarations.
bool from_user_code(const clang::ASTContext& ctx, const clang::Decl& d) {
  const clang::SourceLocation loc = d.getLocation();
  if (loc.isInvalid()) return false;
  const clang::SourceManager& sm = ctx.getSourceManager();
  const clang::SourceLocation file_loc = sm.getExpansionLoc(loc);
  if (sm.isInSystemHeader(file_loc)) return false;
  llvm::StringRef file = sm.getFilename(file_loc);
  if (file.empty()) file = sm.getBufferName(file_loc);  // Clang 21's in-memory inputs.
  return !file.empty() && !file.starts_with("<");  // e.g. <built-in>
}

void collect_classes(const clang::ASTContext& ctx, const clang::DeclContext& dc,
                     std::set<std::string>& out) {
  for (const clang::Decl* d : dc.decls()) {
    if (const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(d)) {
      collect_classes(ctx, *ns, out);
      continue;
    }
    const auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(d);
    // Names starting with "__" are reserved, e.g. the interpreter's runtime
    // __clang_Interpreter_NewTag.
    if (rd == nullptr || rd->isImplicit() || rd->getIdentifier() == nullptr ||
        rd->getName().starts_with("__") || !rd->isThisDeclarationADefinition() ||
        !from_user_code(ctx, *rd) || !is_current_definition(*rd)) {
      continue;
    }
    out.insert(unversioned_name(rd->getQualifiedNameAsString()));
    collect_classes(ctx, *rd, out);  // nested classes
  }
}

const clang::CXXRecordDecl* definition_of(clang::QualType type) {
  const clang::CXXRecordDecl* rd = type->getAsCXXRecordDecl();
  return rd != nullptr ? rd->getDefinition() : nullptr;
}

const clang::FieldDecl* find_field(const clang::CXXRecordDecl& rd, llvm::StringRef name) {
  for (const clang::FieldDecl* f : rd.fields()) {
    if (f->getName() == name) return f;
  }
  for (const clang::CXXBaseSpecifier& b : rd.bases()) {
    if (const clang::CXXRecordDecl* base = definition_of(b.getType())) {
      if (const clang::FieldDecl* f = find_field(*base, name)) return f;
    }
  }
  return nullptr;
}

void collect_members(const clang::CXXRecordDecl& rd, std::set<std::string>& out) {
  for (const clang::Decl* d : rd.decls()) {
    const auto* nd = llvm::dyn_cast<clang::NamedDecl>(d);
    if (nd == nullptr || nd->isImplicit() || nd->getIdentifier() == nullptr) continue;
    // Skip constructors/destructors/operators (no identifier) and library
    // internals such as std::vector's __begin_.
    if (nd->getName().starts_with("__")) continue;
    if (llvm::isa<clang::FieldDecl, clang::VarDecl, clang::CXXMethodDecl,
                  clang::FunctionTemplateDecl>(nd)) {
      out.insert(nd->getName().str());
    }
  }
  for (const clang::CXXBaseSpecifier& b : rd.bases()) {
    if (const clang::CXXRecordDecl* base = definition_of(b.getType())) collect_members(*base, out);
  }
}

std::string bytes(clang::CharUnits size) {
  const auto n = size.getQuantity();
  return std::to_string(n) + (n == 1 ? " byte" : " bytes");
}

std::string one_line(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

clang::QualType qualified_member_type(clang::ASTContext& ctx, clang::QualType type) {
  // Clang's qualification helper does not recurse through function types.
  // Rebuild signatures with qualified results and parameters first, retaining
  // cv/ref qualifiers, calling convention, variadic and exception information.
  if (const auto* fn = type->getAs<clang::FunctionProtoType>()) {
    std::vector<clang::QualType> params;
    for (auto param : fn->param_types()) {
      params.push_back(qualified_member_type(ctx, param));
    }
    return ctx.getFunctionType(qualified_member_type(ctx, fn->getReturnType()), params,
                               fn->getExtProtoInfo());
  }
  if (type->isPointerType()) {
    return ctx.getQualifiedType(
        ctx.getPointerType(qualified_member_type(ctx, type->getPointeeType())),
        type.getLocalQualifiers());
  }
  if (type->isLValueReferenceType()) {
    return ctx.getLValueReferenceType(qualified_member_type(ctx, type->getPointeeType()));
  }
  if (type->isRValueReferenceType()) {
    return ctx.getRValueReferenceType(qualified_member_type(ctx, type->getPointeeType()));
  }
  if (const auto* array = ctx.getAsConstantArrayType(type)) {
    return ctx.getConstantArrayType(qualified_member_type(ctx, array->getElementType()),
                                    array->getSize(), array->getSizeExpr(),
                                    array->getSizeModifier(), array->getIndexTypeCVRQualifiers());
  }
  if (const auto* array = ctx.getAsIncompleteArrayType(type)) {
    return ctx.getIncompleteArrayType(qualified_member_type(ctx, array->getElementType()),
                                      array->getSizeModifier(), array->getIndexTypeCVRQualifiers());
  }
  return clang::TypeName::getFullyQualifiedType(type, ctx);
}

}  // namespace

std::vector<std::string> user_class_names(clang::ASTContext& ctx) {
  std::set<std::string> names;
  // Each input is a partial translation unit with its own TranslationUnitDecl.
  for (const clang::TranslationUnitDecl* tu : ctx.getTranslationUnitDecl()->redecls()) {
    collect_classes(ctx, *tu, names);
  }
  return {names.begin(), names.end()};
}

const clang::CXXRecordDecl* find_class(clang::ASTContext& ctx, std::string_view name) {
  llvm::StringRef rest(name.data(), name.size());
  rest.consume_front("::");
  const clang::DeclContext* dc = ctx.getTranslationUnitDecl();
  while (!rest.empty()) {
    const auto [part, tail] = rest.split("::");
    rest = tail;
    const bool last = rest.empty();
    const clang::DeclContext* next = nullptr;
    for (const clang::NamedDecl* nd : dc->lookup(clang::DeclarationName(&ctx.Idents.get(part)))) {
      if (const auto* rd = llvm::dyn_cast<clang::CXXRecordDecl>(nd)) {
        if (last) return rd->getDefinition();
        next = rd;
      } else if (const auto* td = llvm::dyn_cast<clang::TypedefNameDecl>(nd)) {
        const clang::CXXRecordDecl* rd = definition_of(td->getUnderlyingType());
        if (last) return rd;
        next = rd;
      } else if (const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(nd)) {
        if (!last) next = ns;
      }
      if (next != nullptr) break;
    }
    if (next == nullptr) return nullptr;
    dc = next;
  }
  return nullptr;
}

std::string describe_class(clang::ASTContext& ctx, const clang::CXXRecordDecl& record) {
  clang::PrintingPolicy policy = ctx.getPrintingPolicy();
  policy.TerseOutput = true;  // declarations without bodies
  policy.PolishForDeclaration = true;

  const clang::ASTRecordLayout* layout =
      record.isDependentType() || record.isInvalidDecl() ? nullptr
                                                          : &ctx.getASTRecordLayout(&record);

  std::string out;
  llvm::raw_string_ostream os(out);
  os << record.getKindName() << ' ' << unversioned_name(record.getQualifiedNameAsString());
  bool first = true;
  for (const clang::CXXBaseSpecifier& b : record.bases()) {
    os << (first ? " : " : ", ");
    first = false;
    if (b.isVirtual()) os << "virtual ";
    if (b.getAccessSpecifierAsWritten() != clang::AS_none) {
      os << clang::getAccessSpelling(b.getAccessSpecifierAsWritten()) << ' ';
    }
    os << b.getType().getAsString(policy);
  }
  os << " {";
  if (layout != nullptr) {
    os << "  // size " << layout->getSize().getQuantity() << ", align "
       << layout->getAlignment().getQuantity();
  }
  os << '\n';

  // (indent, text, comment), printed with the comments aligned.
  struct Line {
    std::size_t indent;
    std::string text;
    std::string comment;
  };
  std::vector<Line> lines;
  if (layout != nullptr) {
    if (layout->hasOwnVFPtr()) lines.push_back({2, "", "+0  vptr"});
    for (const clang::CXXBaseSpecifier& b : record.bases()) {
      const clang::CXXRecordDecl* base = definition_of(b.getType());
      if (b.isVirtual() || base == nullptr) continue;
      lines.push_back({2, "",
                       "+" + std::to_string(layout->getBaseClassOffset(base).getQuantity()) +
                           "  base " + b.getType().getAsString(policy) + " (" +
                           bytes(ctx.getTypeSizeInChars(b.getType())) + ")"});
    }
  }
  for (const clang::Decl* d : record.decls()) {
    if (d->isImplicit()) continue;
    if (const auto* as = llvm::dyn_cast<clang::AccessSpecDecl>(d)) {
      lines.push_back({0, std::string(clang::getAccessSpelling(as->getAccess())) + ":", ""});
      continue;
    }
    std::string text;
    llvm::raw_string_ostream ts(text);
    d->print(ts, policy);
    std::string comment;
    if (const auto* f = llvm::dyn_cast<clang::FieldDecl>(d); f != nullptr && layout != nullptr) {
      const uint64_t bits = layout->getFieldOffset(f->getFieldIndex());
      if (f->isBitField()) {
        comment = "bit +" + std::to_string(bits);
      } else if (!f->getType()->isIncompleteType() && !f->getType()->isDependentType()) {
        comment = "+" + std::to_string(bits / 8) + ", " + bytes(ctx.getTypeSizeInChars(f->getType()));
      }
    }
    lines.push_back({2, one_line(std::move(text)) + ";", std::move(comment)});
  }

  // Align the comments of the members that have one.
  std::size_t column = 0;
  for (const Line& l : lines) {
    if (!l.text.empty() && !l.comment.empty()) column = std::max(column, l.indent + l.text.size());
  }
  column = std::min<std::size_t>(column, 48) + 2;
  for (const Line& l : lines) {
    std::string row = std::string(l.indent, ' ') + l.text;
    if (!l.comment.empty()) {
      if (!l.text.empty()) row.resize(std::max(row.size() + 1, column), ' ');
      row += "// " + l.comment;
    }
    os << row << '\n';
  }
  os << "};\n";
  return out;
}

std::string describe_members(clang::ASTContext& ctx, const clang::CXXRecordDecl& record,
                             MemberKind kind, MemberOptions options) {
  using namespace clang::ast_matchers;
  const DeclarationMatcher matcher = kind == MemberKind::Methods
                                         ? DeclarationMatcher(cxxMethodDecl(unless(isImplicit())))
                                         : DeclarationMatcher(decl(anyOf(fieldDecl(), varDecl())));
  auto policy = ctx.getPrintingPolicy();
  policy.SuppressScope = false;
  policy.SuppressTagKeyword = true;
  std::string out;
  llvm::raw_string_ostream os(out);
  std::set<const clang::CXXRecordDecl*> visited;
  std::function<void(const clang::CXXRecordDecl&)> append = [&](const auto& rd) {
    if (!visited.insert(rd.getCanonicalDecl()).second) {
      return;
    }
    rd.getNameForDiagnostic(os, policy, true);
    os << ":\n";
    bool found = false;
    for (const clang::Decl* decl : rd.decls()) {
      // Method and static data member templates wrap their declared member.
      if (const auto* templ = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
        decl = templ->getTemplatedDecl();
      }
      if (decl->isImplicit() || decl->isInvalidDecl() || match(matcher, *decl, ctx).empty()) {
        continue;
      }
      const auto* member = llvm::dyn_cast<clang::ValueDecl>(decl);
      if (!member || member->getDeclName().isEmpty()) {
        continue;
      }
      auto access = member->getAccess();
      if (access == clang::AS_none) {
        access = rd.isClass() ? clang::AS_private : clang::AS_public;
      }
      if (options.access != AccessFilter::All &&
          !((options.access == AccessFilter::Public && access == clang::AS_public) ||
            (options.access == AccessFilter::Private && access == clang::AS_private) ||
            (options.access == AccessFilter::Protected && access == clang::AS_protected))) {
        continue;
      }
      found = true;
      os << "  " << clang::getAccessSpelling(access) << ' ';
      if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(member);
          method && method->isStatic()) {
        os << "static ";
      }
      if (llvm::isa<clang::VarDecl>(member)) {
        os << "static ";
      }
      member->getNameForDiagnostic(os, policy, true);
      os << " : " << qualified_member_type(ctx, member->getType()).getAsString(policy) << '\n';
    }
    if (!found) {
      os << "  (no matching " << (kind == MemberKind::Methods ? "methods" : "fields") << ")\n";
    }
    if (options.include_parents) {
      for (const auto& base : rd.bases()) {
        if (const auto* definition = definition_of(base.getType())) {
          append(*definition);
        }
      }
    }
  };
  append(record);
  return out;
}

std::vector<std::string> member_names(clang::ASTContext& ctx,
                                      const std::vector<std::string>& names,
                                      const std::vector<bool>& arrows) {
  if (names.empty() || names.size() != arrows.size()) return {};
  clang::QualType type;
  for (const clang::NamedDecl* nd :
       ctx.getTranslationUnitDecl()->lookup(clang::DeclarationName(&ctx.Idents.get(names[0])))) {
    if (const auto* var = llvm::dyn_cast<clang::VarDecl>(nd)) type = var->getType();
  }
  if (type.isNull()) return {};

  const clang::CXXRecordDecl* rd = nullptr;
  for (std::size_t i = 0; i < names.size(); ++i) {
    clang::QualType object = type.getNonReferenceType();
    if (arrows[i]) {
      if (!object->isPointerType()) return {};
      object = object->getPointeeType();
    }
    rd = definition_of(object);
    if (rd == nullptr) return {};
    if (i + 1 < names.size()) {
      const clang::FieldDecl* f = find_field(*rd, names[i + 1]);
      if (f == nullptr) return {};
      type = f->getType();
    }
  }
  std::set<std::string> members;
  collect_members(*rd, members);
  return {members.begin(), members.end()};
}

}  // namespace example
