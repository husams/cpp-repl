#include "cpp_symbols.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "cpp_lexer.hpp"
#include "definition_shadowing.hpp"
#include "llvm/Config/llvm-config.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/raw_ostream.h"

namespace example {
namespace {

std::string one_line(const std::string& text) {
  std::string result;
  bool blank = false;
  for (unsigned char c : text) {
    if (std::isspace(c)) {
      blank = !result.empty();
      continue;
    }
    if (blank) result += ' ';
    result += static_cast<char>(c);
    blank = false;
  }
  return result;
}

void collect(const clang::DeclContext& context, std::vector<const clang::NamedDecl*>& result) {
  for (const auto* decl : context.decls()) {
    if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl)) {
      if (!named->isImplicit() && !named->getDeclName().isEmpty() &&
          is_current_definition(*named)) result.push_back(named);
    }
    if (const auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl))
      collect(*space, result);
    else if (const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
      if (!record->isImplicit() && record->isThisDeclarationADefinition() &&
          !record->isDependentContext())
        collect(*record, result);
    } else if (const auto* linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(decl))
      collect(*linkage, result);
  }
}

std::vector<const clang::NamedDecl*> declarations(clang::ASTContext& context) {
  std::vector<const clang::NamedDecl*> result;
  for (const auto* tu : context.getTranslationUnitDecl()->redecls()) collect(*tu, result);
  return result;
}

const clang::NamedDecl* definition(const clang::NamedDecl* decl) {
  if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    if (const auto* body = function->getDefinition()) return body;
  } else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
    if (const auto* body = variable->getDefinition()) return body;
  } else if (const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
    if (const auto* body = tag->getDefinition()) return body;
  }
  return decl;
}

std::string mangled_name(clang::ASTContext& context, const clang::NamedDecl* decl) {
  clang::GlobalDecl global;
  if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    if (function->isDependentContext() || function->isTemplated()) return "";
    if (const auto* ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(function))
      global = clang::GlobalDecl(ctor, clang::Ctor_Complete);
    else if (const auto* dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(function))
      global = clang::GlobalDecl(dtor, clang::Dtor_Complete);
    else
      global = clang::GlobalDecl(function);
  } else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
    if (variable->getDeclContext()->isDependentContext()) return "";
    global = clang::GlobalDecl(variable);
  } else
    return "";
  std::unique_ptr<clang::MangleContext> mangler(context.createMangleContext());
  if (!mangler->shouldMangleDeclName(decl)) return decl->getNameAsString();
  std::string result;
  llvm::raw_string_ostream stream(result);
  mangler->mangleName(global, stream);
  return result;
}

std::string linkage_name(const clang::NamedDecl& decl) {
  switch (decl.getFormalLinkage()) {
    case clang::Linkage::External:
      return "external";
    case clang::Linkage::Internal:
      return "internal";
    case clang::Linkage::UniqueExternal:
      return "unique external";
    case clang::Linkage::VisibleNone:
      return "visible, no linkage";
    case clang::Linkage::Module:
      return "module";
    case clang::Linkage::None:
      return "none";
    case clang::Linkage::Invalid:
      return "unknown";
  }
  return "unknown";
}

std::vector<std::string> signature_tokens(const std::string& text) {
  std::vector<std::string> result;
  for (const auto& token : lex_cpp(text).tokens) {
    auto word = text.substr(token.begin, token.end - token.begin);
    if (std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isspace(c); })) {
      continue;
    }
    result.push_back(std::move(word));
  }
  return result;
}

std::string parameter_spelling(clang::ASTContext& context, clang::QualType type, bool postfix) {
  auto policy = context.getPrintingPolicy();
  policy.SuppressTagKeyword = true;
  policy.FullyQualifiedName = true;
  if (!postfix) {
    return clang::TypeName::getFullyQualifiedType(type, context).getAsString(policy);
  }
  if (!type->isReferenceType()) {
    return "";
  }
  const auto referent = type->getPointeeType();
  const auto qualifiers = referent.getLocalQualifiers();
  if (qualifiers.empty()) {
    return "";
  }
  const auto* spelling_type = referent.getTypePtr();
#if LLVM_VERSION_MAJOR < 22
  while (const auto* elaborated = llvm::dyn_cast<clang::ElaboratedType>(spelling_type)) {
    spelling_type = elaborated->getNamedType().getTypePtr();
  }
#endif
  // An alias is a valid decl-specifier even when its underlying type is an
  // array. Direct array/function declarators cannot take cv words here.
  if (!llvm::isa<clang::TypedefType>(spelling_type) &&
      (referent->isArrayType() || referent->isFunctionType())) {
    return "";
  }
  // Move only the reference's referent-local qualifiers. Qualifiers behind a
  // pointer stay there; const Pointer& must not become const Pointee*&.
  std::string result = qualifiers.getAsString();
  result += type->isLValueReferenceType() ? " &" : " &&";
  clang::TypeName::getFullyQualifiedType(referent.getLocalUnqualifiedType(), context)
      .getAsStringInternal(result, policy);
  return result;
}

bool matches_ast_signature(clang::ASTContext& context, const clang::FunctionDecl& function,
                           const std::vector<std::string>& query) {
  auto consume = [&](const std::string& text, std::size_t& position) {
    const auto tokens = signature_tokens(unversioned_name(text));
    if (tokens.size() > query.size() - position ||
        !std::equal(tokens.begin(), tokens.end(), query.begin() + position)) {
      return false;
    }
    position += tokens.size();
    return true;
  };
  std::size_t position = 0;
  if (!consume(function.getQualifiedNameAsString() + "(", position)) {
    return false;
  }
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const std::string delimiter =
        i + 1 < function.getNumParams() || function.isVariadic() ? "," : ")";
    bool matched = false;
    for (bool canonical : {false, true}) {
      auto type = function.getParamDecl(i)->getType();
      if (canonical) {
        type = context.getCanonicalParamType(type);
      }
      for (bool postfix : {false, true}) {
        const auto spelling = parameter_spelling(context, type, postfix);
        if (spelling.empty()) {
          continue;
        }
        auto end = position;
        if (consume(spelling, end) && end < query.size() && query[end] == delimiter) {
          position = end + (delimiter == "," ? 1 : 0);
          matched = true;
          break;
        }
      }
      if (matched) {
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  if (function.isVariadic() && !consume("...", position)) {
    return false;
  }
  std::string suffix = ")";
  if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function)) {
    if (method->isConst()) {
      suffix += " const";
    }
    if (method->isVolatile()) {
      suffix += " volatile";
    }
    if (method->getRefQualifier() == clang::RQ_LValue) {
      suffix += " &";
    }
    if (method->getRefQualifier() == clang::RQ_RValue) {
      suffix += " &&";
    }
  }
  if (!consume(suffix, position)) {
    return false;
  }
  if (position == query.size()) {
    return true;
  }
  const auto* prototype = function.getType()->getAs<clang::FunctionProtoType>();
  return prototype && prototype->isNothrow() && consume("noexcept", position) &&
         position == query.size();
}

bool matches_signature(clang::ASTContext& context, const clang::FunctionDecl& function,
                       const std::vector<std::string>& query) {
  // Compare AST type spellings, not a parsed expression: inspection must not
  // execute a call, add helper declarations, or perturb incremental lookup.
  if (matches_ast_signature(context, function, query)) {
    return true;
  }
  // Retain native demangled forms, including cv spellings inside pointer types.
  const auto mangled = mangled_name(context, &function);
  return !mangled.empty() && signature_tokens(unversioned_name(llvm::demangle(mangled))) == query;
}

SymbolDescription describe(clang::ASTContext& context, const clang::NamedDecl* decl) {
  decl = definition(decl);
  SymbolDescription result;
  result.name = unversioned_name(decl->getQualifiedNameAsString());
  auto add = [&](const std::string& label, const std::string& value) {
    result.fields.emplace_back(label, one_line(unversioned_name(value)));
  };
  clang::PrintingPolicy policy = context.getPrintingPolicy();
  policy.TerseOutput = true;
  policy.PolishForDeclaration = true;
  policy.SuppressInitializers = true;
  policy.FullyQualifiedName = true;
  std::string kind = decl->getDeclKindName();
  std::string declaration;
  clang::QualType type;
  if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    kind = llvm::isa<clang::CXXMethodDecl>(function) ? "method" : "function";
    type = function->getType();
    result.type_summary = "(";
    for (unsigned i = 0; i < function->getNumParams(); ++i) {
      if (i) result.type_summary += ", ";
      result.type_summary += function->getParamDecl(i)->getType().getAsString(policy);
    }
    if (function->isVariadic()) {
      if (function->getNumParams()) result.type_summary += ", ";
      result.type_summary += "...";
    }
    result.type_summary += ")";
    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function)) {
      if (method->isConst()) result.type_summary += " const";
      if (method->isVolatile()) result.type_summary += " volatile";
      if (method->getRefQualifier() == clang::RQ_LValue) result.type_summary += " &";
      if (method->getRefQualifier() == clang::RQ_RValue) result.type_summary += " &&";
    }
    if (const auto* prototype = type->getAs<clang::FunctionProtoType>();
        prototype && prototype->isNothrow())
      result.type_summary += " noexcept";
    result.type_summary += " -> " + function->getReturnType().getAsString(policy);
    llvm::raw_string_ostream stream(declaration);
    function->print(stream, policy);
  } else if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(decl)) {
    kind = llvm::isa<clang::FieldDecl>(value)          ? "field"
           : llvm::isa<clang::EnumConstantDecl>(value) ? "enumerator"
                                                       : "variable";
    type = value->getType();
    declaration = result.name;
    type.getAsStringInternal(declaration, policy);
  } else if (const auto* record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
    kind = record->getKindName().str();
    declaration = kind + " " + result.name;
#if LLVM_VERSION_MAJOR >= 22
    type = context.getCanonicalTagType(record);
#else
    type = context.getTypeDeclType(record);
#endif
  } else if (const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
    kind = "type alias";
    type = alias->getUnderlyingType();
  } else if (const auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(decl)) {
    kind = "enum";
#if LLVM_VERSION_MAJOR >= 22
    type = context.getCanonicalTagType(enumeration);
#else
    type = context.getTypeDeclType(enumeration);
#endif
  } else if (llvm::isa<clang::NamespaceDecl>(decl)) {
    kind = "namespace";
    declaration = "namespace " + result.name;
  } else if (llvm::isa<clang::FunctionTemplateDecl>(decl))
    kind = "function template";
  else if (llvm::isa<clang::ClassTemplateDecl>(decl))
    kind = "class template";
  if (declaration.empty()) {
    llvm::raw_string_ostream stream(declaration);
    decl->print(stream, policy);
  }
  add("Kind", kind);
  add("Declaration", declaration);
  if (!type.isNull()) {
    add("Type", type.getAsString(policy));
    if (result.type_summary.empty()) result.type_summary = "(" + type.getAsString(policy) + ")";
    result.type_summary = one_line(unversioned_name(result.type_summary));
  }
  result.mangled = mangled_name(context, decl);
  if (!result.mangled.empty()) add("Mangled", result.mangled);
  add("Linkage", linkage_name(*decl));
  if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl))
    add("Definition", function->getDefinition() ? "yes" : "no");
  else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl))
    add("Definition", variable->getDefinition() ? "yes" : "no");
  else if (const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl))
    add("Definition", tag->getDefinition() ? "yes" : "no");
  const auto& sources = context.getSourceManager();
  auto location = sources.getPresumedLoc(sources.getExpansionLoc(decl->getLocation()));
  if (location.isValid())
    add("Location", std::string(location.getFilename()) + ":" + std::to_string(location.getLine()) +
                        ":" + std::to_string(location.getColumn()));
  else
    add("Location", "unavailable");
  if (!type.isNull() && !type->isDependentType() && !type->isIncompleteType() &&
      type->isObjectType() && !type->isVariableArrayType()) {
    add("Size", std::to_string(context.getTypeSizeInChars(type).getQuantity()) + " bytes");
    add("Alignment", std::to_string(context.getTypeAlignInChars(type).getQuantity()) + " bytes");
  }
  if (const auto* field = llvm::dyn_cast<clang::FieldDecl>(decl)) {
    const auto* record = field->getParent();
    if (record->isCompleteDefinition() && !record->isDependentContext() && !record->isInvalidDecl())
      add("Offset", std::to_string(
                        context.getASTRecordLayout(record).getFieldOffset(field->getFieldIndex())) +
                        " bits");
    result.address_note = "requires an object";
  }
  if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
    if (variable->getTLSKind() != clang::VarDecl::TLS_None) {
      result.mangled.clear();
      result.address_note = "thread-local; no process-wide address";
    }
  }
  return result;
}

}  // namespace

std::optional<VariableDefinition> variable_definition(
    clang::ASTContext& context, std::string_view query, std::string& error) {
  error.clear();
  if (query.substr(0, 2) == "::") query.remove_prefix(2);
  if (query.substr(0, 3) == "__Z") query.remove_prefix(1);
  for (const auto* decl : declarations(context)) {
    if (decl->isInvalidDecl()) continue;
    if (unversioned_name(decl->getQualifiedNameAsString()) != query &&
        mangled_name(context, decl) != query)
      continue;
    const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl);
    if (!variable || !variable->hasGlobalStorage() ||
        variable->isLocalVarDecl()) {
      error = "symbol is not a global variable or static data member";
      return std::nullopt;
    }
    if (variable->getDeclContext()->isDependentContext() ||
        variable->getType()->isDependentType()) {
      error = "variable requires a concrete template specialization";
      return std::nullopt;
    }
    VariableDefinition result;
    result.mangled = mangled_name(context, variable);
    const auto* definition = variable->getDefinition();
    result.defined = definition && !definition->isInvalidDecl();
    result.declaration = unversioned_name(variable->getQualifiedNameAsString());
    auto policy = context.getPrintingPolicy();
    policy.FullyQualifiedName = true;
    variable->getType().getAsStringInternal(result.declaration, policy);
    result.thread_local_storage = variable->getTLSKind() != clang::VarDecl::TLS_None;
    if (result.thread_local_storage)
      result.declaration = "thread_local " + result.declaration;
    return result;
  }
  error = "no variable declaration named '" + std::string(query) + "'";
  return std::nullopt;
}

void discard_failed_variable_definition(clang::ASTContext& context,
                                        std::string_view query) {
  if (query.substr(0, 2) == "::") query.remove_prefix(2);
  if (query.substr(0, 3) == "__Z") query.remove_prefix(1);
  for (const auto* decl : declarations(context)) {
    const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl);
    if (!variable || (unversioned_name(variable->getQualifiedNameAsString()) != query &&
                      mangled_name(context, variable) != query))
      continue;
    if (auto* definition =
            const_cast<clang::VarDecl*>(variable->getDefinition())) {
      // Failed Parse inputs are not registered PTUs, so Undo would remove an
      // earlier successful input. Clang cleans lookup but retains
      // redeclarations; demote this failed definition to an invalid declaration
      // instead.
      definition->setInit(nullptr);
      if (definition->isStaticDataMember()) {
        definition->setStorageClass(clang::SC_Static);
        definition->setLexicalDeclContext(definition->getDeclContext());
      } else {
        definition->setStorageClass(clang::SC_Extern);
      }
      definition->setInvalidDecl();
    }
  }
}

void discard_failed_callable_definition(clang::FunctionDecl& function) {
  // Instantiated functions are held in their template's specialization table,
  // not necessarily in the ordinary declaration list. Clean the selected
  // entity's redeclarations directly, leaving other instances intact.
  for (auto* decl : function.redecls()) {
    // Parse failure retains the redeclaration chain. Keep a valid prototype:
    // leaving an invalid redeclaration poisons later calls after a retry.
    decl->setInvalidDecl(false);
    if (auto* body = decl->getDefinition()) {
      body->setBody(nullptr);
      body->setInvalidDecl(false);
    }
  }
}

std::vector<SymbolDescription> describe_symbols(clang::ASTContext& context,
                                                std::string_view query) {
  if (query.substr(0, 2) == "::") query.remove_prefix(2);
  const auto all = declarations(context);
  std::vector<SymbolDescription> result;
  std::set<const clang::Decl*> seen;
  const bool mangled_query =
      query.substr(0, 2) == "_Z" || query.substr(0, 3) == "__Z";
  const bool signature_query = query.find('(') != std::string_view::npos;
  const auto signature =
      signature_query ? signature_tokens(std::string(query)) : std::vector<std::string>{};
  for (const auto* decl : all) {
    const auto name = unversioned_name(decl->getQualifiedNameAsString());
    bool match = name == query;
    if (!match && signature_query && query.substr(0, name.size()) == name) {
      if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        match = matches_signature(context, *function, signature);
      }
    }
    if (!match && mangled_query) {
      const auto symbol = mangled_name(context, decl);
      match = !symbol.empty() && (symbol == query
#ifdef __APPLE__
                                  || "_" + symbol == query
#endif
                                 );
    }
    if (match && seen.insert(decl->getCanonicalDecl()).second)
      result.push_back(describe(context, decl));
  }
  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.fields < b.fields; });
  return result;
}

std::vector<std::string> user_symbol_names(clang::ASTContext& context) {
  std::set<std::string> names;
  const auto& sources = context.getSourceManager();
  for (const auto* decl : declarations(context)) {
    if (decl->getLocation().isInvalid() || sources.isInSystemHeader(decl->getLocation())) continue;
    const auto name = unversioned_name(decl->getQualifiedNameAsString());
    if (!name.empty() && name.rfind("__", 0) != 0) names.insert(name);
  }
  return {names.begin(), names.end()};
}

std::vector<RuntimeDeclaration> runtime_symbol_declarations(clang::ASTContext& context) {
  std::vector<RuntimeDeclaration> result;
  std::set<const clang::Decl*> seen;
  const auto& sources = context.getSourceManager();
  for (const auto* decl : declarations(context)) {
    if (decl->getLocation().isInvalid() || sources.isInSystemHeader(decl->getLocation()) ||
        decl->getNameAsString().rfind("__", 0) == 0 ||
        !seen.insert(decl->getCanonicalDecl()).second) continue;
    std::string kind;
    bool defined = false;
    if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (function->isDependentContext() || function->isTemplated()) continue;
      kind = llvm::isa<clang::CXXConstructorDecl>(function) ? "constructor" :
             llvm::isa<clang::CXXDestructorDecl>(function) ? "destructor" :
             llvm::isa<clang::CXXMethodDecl>(function) ? "method" : "function";
      defined = function->getDefinition() != nullptr;
    } else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
      if (!variable->hasGlobalStorage() || variable->getType()->isDependentType()) continue;
      kind = variable->getTLSKind() != clang::VarDecl::TLS_None ? "thread-local" : "variable";
      defined = variable->getDefinition() != nullptr;
      // Obtaining a TLS address can execute a thread-local initializer.
      if (kind == "thread-local") defined = false;
    } else continue;
    auto mangled = mangled_name(context, decl);
    if (!mangled.empty()) result.push_back({std::move(mangled), std::move(kind), defined});
  }
  return result;
}

std::string format_symbols(const std::vector<SymbolDescription>& symbols,
                           const std::string& query) {
  std::ostringstream out;
  if (symbols.size() > 1) out << symbols.size() << " matches for '" << query << "'\n\n";
  bool first = true;
  for (const auto& symbol : symbols) {
    if (!first) out << '\n';
    first = false;
    out << "Symbol: " << one_line(symbol.name) << '\n';
    std::size_t width = 11;
    for (const auto& field : symbol.fields) width = std::max(width, field.first.size());
    for (const auto& field : symbol.fields)
      out << "  " << std::left << std::setw(static_cast<int>(width)) << field.first << " : " << one_line(field.second)
          << '\n';
  }
  return out.str();
}

std::vector<std::string> symbol_reference_sites(clang::ASTContext& context, const std::string& mangled) {
  class Visitor : public clang::RecursiveASTVisitor<Visitor> {
   public:
    clang::ASTContext& context;
    const std::string& target;
    std::string caller;
    std::set<std::string> result;
    Visitor(clang::ASTContext& context, const std::string& target) : context(context), target(target) {}
    bool TraverseDecl(clang::Decl* decl) {
      if (!decl || context.getSourceManager().isInSystemHeader(decl->getLocation())) return true;
      const auto previous = caller;
      if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) caller = unversioned_name(function->getQualifiedNameAsString());
      const bool ok = clang::RecursiveASTVisitor<Visitor>::TraverseDecl(decl);
      caller = previous;
      return ok;
    }
    void add(const clang::NamedDecl* decl, clang::SourceLocation location) {
      if (mangled_name(context, decl) != target) return;
      const auto& sources = context.getSourceManager();
      if (sources.isInSystemHeader(location)) return;
      const auto where = sources.getPresumedLoc(sources.getExpansionLoc(location));
      if (!where.isValid()) return;
      result.insert(std::string(where.getFilename()) + ":" + std::to_string(where.getLine()) + ":" +
          std::to_string(where.getColumn()) + (caller.empty() ? "" : " in " + caller));
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr* expression) { add(expression->getDecl(), expression->getExprLoc()); return true; }
    bool VisitMemberExpr(clang::MemberExpr* expression) { add(expression->getMemberDecl(), expression->getMemberLoc()); return true; }
  } visitor(context, mangled);
  for (auto* tu : context.getTranslationUnitDecl()->redecls()) visitor.TraverseDecl(tu);
  return {visitor.result.begin(), visitor.result.end()};
}

}  // namespace example
