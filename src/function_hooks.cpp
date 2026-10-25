#include "function_hooks.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/Basic/SourceManager.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/PartialTranslationUnit.h"
#include "cpp_lexer.hpp"
#include "definition_shadowing.hpp"
#include "interpreter_undo.hpp"
#include "native_call_patch.hpp"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace example {
namespace {

std::unique_ptr<clang::CodeGenerator> make_codegen(clang::CompilerInstance& compiler,
    const std::string& name, llvm::LLVMContext& context) {
#if LLVM_VERSION_MAJOR >= 22
  return clang::CreateLLVMCodeGen(compiler, name, context);
#else
  return std::unique_ptr<clang::CodeGenerator>(clang::CreateLLVMCodeGen(
      compiler.getDiagnostics(), name, &compiler.getVirtualFileSystem(),
      compiler.getHeaderSearchOpts(), compiler.getPreprocessorOpts(), compiler.getCodeGenOpts(), context));
#endif
}

std::unique_ptr<llvm::Module> release_module(clang::CodeGenerator& generator) {
#if LLVM_VERSION_MAJOR >= 22
  return generator.ReleaseModule();
#else
  return std::unique_ptr<llvm::Module>(generator.ReleaseModule());
#endif
}

void functions(clang::DeclContext& context, std::vector<clang::FunctionDecl*>& out,
               const clang::SourceManager& sources) {
  for (clang::Decl* decl : context.decls()) {
    if (sources.isInSystemHeader(decl->getLocation())) continue;
    if (auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (!function->isImplicit() && !llvm::isa<clang::CXXConstructorDecl>(function) &&
          !llvm::isa<clang::CXXDestructorDecl>(function) &&
          function->getNameAsString().rfind("__cpp_repl_", 0) != 0 &&
          is_current_definition(*function))
        out.push_back(function);
    } else if (auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
      functions(*space, out, sources);
    } else if (auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
      if (record->isCompleteDefinition()) functions(*record, out, sources);
    } else if (auto* linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
      functions(*linkage, out, sources);
    }
  }
}

std::string symbol_name(clang::ASTContext& context, const clang::FunctionDecl& decl) {
  std::unique_ptr<clang::MangleContext> mangler(context.createMangleContext());
  if (!mangler->shouldMangleDeclName(&decl)) return decl.getNameAsString();
  std::string name;
  llvm::raw_string_ostream out(name);
  mangler->mangleName(clang::GlobalDecl(&decl), out);
  return name;
}

void dispatch_through_slot(llvm::Function& function, llvm::Constant* original,
                           const std::string& slot_name, bool define_slot) {
  auto& module = *function.getParent();
  function.deleteBody();
  function.setMemoryEffects(llvm::MemoryEffects::unknown());
  for (auto attr :
       {llvm::Attribute::NoUnwind, llvm::Attribute::NoFree, llvm::Attribute::NoSync,
        llvm::Attribute::NoRecurse, llvm::Attribute::NoCallback, llvm::Attribute::WillReturn,
        llvm::Attribute::MustProgress, llvm::Attribute::Speculatable, llvm::Attribute::NoReturn,
        llvm::Attribute::AlwaysInline})
    function.removeFnAttr(attr);
  function.addFnAttr(llvm::Attribute::NoInline);
  auto* pointer = llvm::PointerType::getUnqual(module.getContext());
  auto* slot = module.getNamedGlobal(slot_name);
  if (!slot)
    slot = new llvm::GlobalVariable(module, pointer, false, llvm::GlobalValue::ExternalLinkage,
        define_slot ? llvm::ConstantPointerNull::get(pointer) : nullptr, slot_name);
  const auto alignment = module.getDataLayout().getPointerABIAlignment(0);
  slot->setAlignment(alignment);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(module.getContext(), "entry", &function));
  auto* replacement = builder.CreateLoad(pointer, slot);
  replacement->setAlignment(alignment);
  replacement->setAtomic(llvm::AtomicOrdering::Acquire);
  auto* destination = builder.CreateSelect(builder.CreateIsNull(replacement), original, replacement);
  std::vector<llvm::Value*> args;
  for (auto& arg : function.args()) args.push_back(&arg);
  auto* call = builder.CreateCall(function.getFunctionType(), destination, args);
  call->setCallingConv(function.getCallingConv());
  // Preserve the declaration's ABI attributes on the forwarding call.
  call->setAttributes(function.getAttributes().removeFnAttributes(module.getContext()));
  // A byval argument lives in the incoming stack frame. A musttail transfer
  // can copy it across the return address on x86_64 (LLVM 21), corrupting the
  // caller's stack. Let normal call lowering allocate a fresh outgoing copy.
  bool has_byval = false;
  for (const auto& arg : function.args()) {
    has_byval |= arg.hasByValAttr();
  }
  if (!has_byval) {
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
  }
  if (function.getReturnType()->isVoidTy()) builder.CreateRetVoid();
  else builder.CreateRet(call);
}

std::string replace_this(const std::string& text) {
  std::string result;
  std::size_t end = 0;
  for (const Token& token : lex_cpp(text).tokens) {
    if (text.substr(token.begin, token.end - token.begin) != "this" ||
        token.kind != TokenKind::Keyword)
      continue;
    result += text.substr(end, token.begin - end) + "self";
    end = token.end;
  }
  return result + text.substr(end);
}

struct Signature {
  std::string name;
  std::string declaration;
  bool is_const = false;
  bool is_volatile = false;
  bool has_return_type = true;
  clang::RefQualifierKind ref = clang::RQ_None;
};

// Clang validates the types and parameter declarations. Here we only separate
// the qualified name and member qualifiers from an ordinary function signature.
bool split_signature(const std::string& text, const std::string& helper, Signature& signature,
                     std::string& error) {
  const auto tokens = lex_cpp(text).tokens;
  std::size_t open = 0;
  while (open < tokens.size() &&
         text.substr(tokens[open].begin, tokens[open].end - tokens[open].begin) != "(")
    ++open;
  if (open < 1 || open == tokens.size() || tokens[open - 1].kind != TokenKind::Identifier) {
    error = "use a signature, e.g. ns::function(int x) or int Class::method() const";
    return false;
  }
  std::size_t first = open - 1;
  while (first >= 2 &&
         text.substr(tokens[first - 1].begin, tokens[first - 1].end - tokens[first - 1].begin) ==
             "::" &&
         tokens[first - 2].end == tokens[first - 1].begin &&
         tokens[first - 2].kind == TokenKind::Identifier)
    first -= 2;
  if (first > 0 &&
      text.substr(tokens[first - 1].begin, tokens[first - 1].end - tokens[first - 1].begin) == "::")
    --first;
  for (std::size_t i = first; i < open; ++i)
    signature.name += text.substr(tokens[i].begin, tokens[i].end - tokens[i].begin);
  if (signature.name.rfind("::", 0) == 0) signature.name.erase(0, 2);
  std::size_t close = open + 1;
  for (; close < tokens.size(); ++close) {
    if (text.substr(tokens[close].begin, tokens[close].end - tokens[close].begin) == ")" &&
        tokens[close].depth == tokens[open].depth)
      break;
  }
  if (close == tokens.size()) {
    error = "unclosed signature parameter list";
    return false;
  }
  std::string suffix;
  for (std::size_t i = close + 1; i < tokens.size(); ++i) {
    const std::string word = text.substr(tokens[i].begin, tokens[i].end - tokens[i].begin);
    if (word == "const")
      signature.is_const = true;
    else if (word == "volatile")
      signature.is_volatile = true;
    else if (word == "&")
      signature.ref = clang::RQ_LValue;
    else if (word == "&&")
      signature.ref = clang::RQ_RValue;
    else if (word == "noexcept")
      suffix = " noexcept";
    else {
      error = "unsupported signature suffix: " + word;
      return false;
    }
  }
  signature.has_return_type = first != 0;
  signature.declaration =
      (signature.has_return_type ? text.substr(0, tokens[first].begin) : "void ") + helper +
      text.substr(tokens[open].begin, tokens[close].end - tokens[open].begin) + suffix + ";";
  return true;
}

}  // namespace

struct FunctionHooks::Impl {
  explicit Impl(clang::Interpreter& interpreter) : interp(interpreter) {}
  struct Body {
    std::string text;
    std::vector<std::string> names;
    std::string signature;
    std::string caller;
  };
  struct Target {
    std::string slot;
    std::string original;
    std::array<Body, 3> bodies;
    std::string entry;
    std::shared_ptr<NativeCallPatch> native = nullptr;
  };
  clang::Interpreter& interp;
  std::map<std::string, Target> targets;
  std::map<std::pair<std::string, std::string>, Target> calls;
  unsigned next_id = 0;

  clang::FunctionDecl* resolve(const std::string& text, std::vector<std::string>* names,
                               std::string& error, bool defining = false) {
    auto& context = interp.getASTContext();
    const std::string helper = "__cpp_repl_signature_" + std::to_string(++next_id);
    Signature signature;
    if (!split_signature(text, helper, signature, error)) return nullptr;
    auto parsed = interp.Parse(signature.declaration);
    if (!parsed) {
      error = llvm::toString(parsed.takeError());
      return nullptr;
    }
    clang::FunctionDecl* probe = nullptr;
    for (auto* decl : parsed->TUPart->decls())
      if (auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl))
        if (function->getNameAsString() == helper) probe = function;
    std::vector<clang::FunctionDecl*> declarations;
    for (auto* tu : context.getTranslationUnitDecl()->redecls())
      functions(*tu, declarations, context.getSourceManager());
    clang::FunctionDecl* selected = nullptr;
    std::set<const clang::FunctionDecl*> seen;
    if (probe)
      for (auto* function : declarations) {
        if (unversioned_name(function->getQualifiedNameAsString()) != signature.name ||
            function->getNumParams() != probe->getNumParams() ||
            function->isVariadic() != probe->isVariadic() ||
            (signature.has_return_type &&
             !context.hasSameType(function->getReturnType(), probe->getReturnType())))
          continue;
        bool match = true;
        for (unsigned i = 0; i < probe->getNumParams(); ++i)
          match &= context.hasSameType(
              context.getCanonicalParamType(function->getParamDecl(i)->getType()),
              context.getCanonicalParamType(probe->getParamDecl(i)->getType()));
        if (auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function))
          match &= method->isConst() == signature.is_const &&
                   method->isVolatile() == signature.is_volatile &&
                   method->getRefQualifier() == signature.ref;
        else
          match &= !signature.is_const && !signature.is_volatile && signature.ref == clang::RQ_None;
        if (!match || !seen.insert(function->getCanonicalDecl()).second) continue;
        if (selected) {
          error = "ambiguous function signature";
          break;
        }
        selected = function;
      }
    if (probe && names)
      for (unsigned i = 0; i < probe->getNumParams(); ++i) {
        auto name = probe->getParamDecl(i)->getNameAsString();
        if (name.empty()) name = "arg" + std::to_string(i);
        if (!defining &&
            (name == "self" || name == "result" || name.rfind("__cpp_repl_", 0) == 0)) {
          error = "parameter names self, result and __cpp_repl_* are reserved in hooks";
        }
        names->push_back(std::move(name));
      }
    if (auto undo = undo_input(interp, parsed->TUPart)) error = llvm::toString(std::move(undo));
    if (error.empty() && !selected) error = "no function matches '" + text + "'";
    return error.empty() ? selected : nullptr;
  }

  bool compile(const std::string& source, bool instrument_input, FunctionHooks& hooks,
               std::string& error) {
    auto compiled = interp.Parse(source);
    if (!compiled) {
      error = llvm::toString(compiled.takeError());
      return false;
    }
    return execute(*compiled, instrument_input, hooks, error);
  }

  bool execute(clang::PartialTranslationUnit& compiled, bool instrument_input,
               FunctionHooks& hooks, std::string& error) {
    const auto added = instrument_input ? hooks.instrument(compiled) : HookInstrumentation{};
    auto fail = [&] {
      hooks.discard(added);
      if (llvm::Error undo = undo_input(interp, compiled.TUPart)) llvm::consumeError(std::move(undo));
      return false;
    };
    // Check before Execute: failed ORC initializers otherwise poison later inputs.
    if (compiled.TheModule) {
      auto check = [&](const llvm::GlobalValue& value) {
        if (!value.isDeclaration() || value.hasExternalWeakLinkage() || !value.hasName())
          return true;
        if (const auto* function = llvm::dyn_cast<llvm::Function>(&value))
          if (function->isIntrinsic()) return true;
        auto address = interp.getSymbolAddress(value.getName());
        if (address) return true;
        llvm::consumeError(address.takeError());
        error = "undefined symbol in hook: " + value.getName().str();
        return false;
      };
      for (const auto& function : *compiled.TheModule)
        if (!check(function)) return fail();
      for (const auto& global : compiled.TheModule->globals())
        if (!check(global)) return fail();
      if (llvm::Error execution = interp.Execute(compiled)) {
        error = llvm::toString(std::move(execution));
        return fail();
      }
    }
    return true;
  }

  bool materialize(clang::FunctionDecl& function, FunctionHooks& hooks, std::string& error) {
    auto& context = interp.getASTContext();
    if (function.isVariadic() || function.isTemplated() ||
        function.isConstexpr()) {
      error =
          "target and caller must be non-template, non-constexpr, non-variadic functions";
      return false;
    }
    if (targets.count(symbol_name(context, function))) return true;
    bool native_definition = false;
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
    if (function.getDefinition()) {
      auto address = interp.getSymbolAddress(symbol_name(context, function));
      if (address) native_definition = NativeCallPatch::is_native_address(address->getValue());
      else llvm::consumeError(address.takeError());
    }
#endif
    if (!function.getDefinition()) {
      // Loaded-library symbols have no AST body to emit. Resolve their native
      // address directly through ORC, without a C++ member-access expression.
      auto address = interp.getSymbolAddress(symbol_name(context, function));
      if (!address) {
        error = "cannot resolve external function: " + llvm::toString(address.takeError());
        return false;
      }
    }
    auto input = interp.Parse(";");
    if (!input) {
      error = llvm::toString(input.takeError());
      return false;
    }
    // Emit a deferred inline definition through CodeGen's AST API. No source
    // expression is synthesized and C++ access checking remains enabled.
    auto generator = make_codegen(*interp.getCompilerInstance(),
        "__cpp_repl_materialize_" + std::to_string(++next_id),
        input->TheModule->getContext());
    generator->Initialize(context);
    auto* definition = function.getDefinition();
    if (definition && !native_definition)
      generator->HandleTopLevelDecl(clang::DeclGroupRef(definition));
    generator->GetAddrOfGlobal(clang::GlobalDecl(definition ? definition : &function), false);
    generator->HandleTranslationUnit(context);
    input->TheModule = release_module(*generator);
    if (!input->TheModule) {
      error = "failed to emit function for hook routing";
      if (auto undo = undo_input(interp, input->TUPart)) llvm::consumeError(std::move(undo));
      return false;
    }
    return execute(*input, true, hooks, error);
  }

  bool native_edge(clang::FunctionDecl& caller, clang::FunctionDecl& callee,
                   FunctionHooks& hooks, Target& target, std::string& error) {
    auto& context = interp.getASTContext();
    const auto callee_symbol = symbol_name(context, callee);
    auto caller_address = interp.getSymbolAddress(symbol_name(context, caller));
    if (!caller_address) {
      error = llvm::toString(caller_address.takeError());
      return false;
    }
    auto callee_address = interp.getSymbolAddress(callee_symbol);
    if (!callee_address) {
      error = llvm::toString(callee_address.takeError());
      return false;
    }
    auto patch = NativeCallPatch::prepare(caller_address->getValue(), callee_address->getValue(), error);
    if (!patch) return false;
    auto input = interp.Parse(";");
    if (!input) {
      error = llvm::toString(input.takeError());
      return false;
    }
    const auto id = std::to_string(++next_id);
    auto generator = make_codegen(*interp.getCompilerInstance(),
        "__cpp_repl_native_module_" + id, input->TheModule->getContext());
    generator->Initialize(context);
    generator->GetAddrOfGlobal(clang::GlobalDecl(&callee), false);
    generator->HandleTranslationUnit(context);
    input->TheModule = release_module(*generator);
    auto* original = input->TheModule ? input->TheModule->getFunction(callee_symbol) : nullptr;
    if (!original) {
      error = "cannot emit the native target's ABI declaration";
      if (auto undo = undo_input(interp, input->TUPart)) llvm::consumeError(std::move(undo));
      return false;
    }
    target = Target{"__cpp_repl_native_slot_" + id, callee_symbol, {},
        "__cpp_repl_native_entry_" + id, std::move(patch)};
    auto* entry = llvm::Function::Create(original->getFunctionType(),
        llvm::GlobalValue::ExternalLinkage, target.entry, input->TheModule.get());
    entry->setCallingConv(original->getCallingConv());
    entry->setAttributes(original->getAttributes());
    dispatch_through_slot(*entry, original, target.slot, true);
    return execute(*input, false, hooks, error);
  }

  bool clear(std::vector<std::pair<Target*, std::uintptr_t*>>& selected, std::string& error) {
    std::vector<std::pair<NativeCallPatch*, std::uintptr_t>> native;
    for (auto& entry : selected) {
      if (!entry.first->native || !entry.first->native->active()) continue;
      auto address = interp.getSymbolAddress(entry.first->entry);
      if (!address) {
        error = llvm::toString(address.takeError());
        return false;
      }
      native.emplace_back(entry.first->native.get(), address->getValue());
    }
    std::size_t done = 0;
    for (; done < native.size(); ++done)
      if (!native[done].first->restore(error)) break;
    if (done != native.size()) {
      while (done) {
        const auto& entry = native[--done];
        std::string rollback;
        if (!entry.first->install(entry.second, rollback)) error += "; rollback: " + rollback;
      }
      return false;
    }
    for (auto& entry : selected) {
      __atomic_store_n(entry.second, 0, __ATOMIC_RELEASE);
      entry.first->bodies = {};
    }
    return true;
  }
};

FunctionHooks::FunctionHooks(clang::Interpreter& interpreter)
    : impl_(std::make_unique<Impl>(interpreter)) {}
FunctionHooks::~FunctionHooks() = default;

std::optional<CallableDefinition> FunctionHooks::definition(const std::string& signature,
                                                            std::string& error) {
  std::vector<std::string> names;
  auto* function = impl_->resolve(signature, &names, error, true);
  if (!function) {
    return std::nullopt;
  }
  if (function->getDefinition()) {
    error = "function already has a C++ definition: " + signature;
    return std::nullopt;
  }
  const auto* type = function->getType()->getAs<clang::FunctionProtoType>();
  const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
  if (!type || function->isTemplated() || function->isDependentContext() ||
      function->isConstexpr() || function->isVariadic() || function->isDeleted() ||
      function->getFormalLinkage() != clang::Linkage::External ||
      type->getCallConv() != clang::CC_C || (method && method->isVirtual())) {
    error = "callable definitions require external, non-template, non-constexpr, "
            "non-variadic, non-virtual functions with the default calling convention";
    return std::nullopt;
  }
  auto& context = impl_->interp.getASTContext();
  auto policy = context.getPrintingPolicy();
  policy.FullyQualifiedName = true;
  auto qualified = [&](clang::QualType type) {
    return clang::TypeName::getFullyQualifiedType(type, context, true);
  };
  std::string declaration = unversioned_name(function->getQualifiedNameAsString()) + "(";
  for (unsigned i = 0; i < function->getNumParams(); ++i) {
    if (i) {
      declaration += ", ";
    }
    auto parameter = names[i];
    qualified(function->getParamDecl(i)->getType()).getAsStringInternal(parameter, policy);
    declaration += parameter;
  }
  declaration += ")";
  if (method && !method->isStatic()) {
    if (method->isConst()) {
      declaration += " const";
    }
    if (method->isVolatile()) {
      declaration += " volatile";
    }
    if (method->getRefQualifier() == clang::RQ_LValue) {
      declaration += " &";
    }
    if (method->getRefQualifier() == clang::RQ_RValue) {
      declaration += " &&";
    }
  }
  if (type->isNothrow()) {
    declaration += " noexcept";
  }
  qualified(function->getReturnType()).getAsStringInternal(declaration, policy);
  return CallableDefinition{std::move(declaration), symbol_name(context, *function)};
}

std::vector<ActiveHook> FunctionHooks::active() const {
  std::vector<ActiveHook> result;
  auto collect = [&](const auto& entries) {
    for (const auto& entry : entries) {
      for (unsigned i = 0; i < entry.second.bodies.size(); ++i) {
        const auto& body = entry.second.bodies[i];
        if (!body.text.empty())
          result.push_back({static_cast<HookKind>(i), body.signature, body.text, body.caller});
      }
    }
  };
  collect(impl_->targets);
  collect(impl_->calls);
  std::sort(result.begin(), result.end(), [](const ActiveHook& a, const ActiveHook& b) {
    if (a.signature != b.signature) return a.signature < b.signature;
    if (a.caller != b.caller) return a.caller < b.caller;
    return a.kind < b.kind;
  });
  return result;
}

HookSymbolDetails FunctionHooks::symbol_details(const std::string& symbol) const {
  HookSymbolDetails result;
  auto append = [&](const Impl::Target& target) {
    for (unsigned i = 0; i < target.bodies.size(); ++i) {
      const auto& body = target.bodies[i];
      if (!body.text.empty()) result.hooks.push_back({static_cast<HookKind>(i), body.signature, body.text, body.caller});
    }
  };
  const auto found = impl_->targets.find(symbol);
  if (found != impl_->targets.end()) {
    result.original_symbol = found->second.original;
    result.routing_symbol = found->second.entry;
    append(found->second);
  }
  for (const auto& entry : impl_->calls) if (entry.first.second == symbol) append(entry.second);
  return result;
}

HookInstrumentation FunctionHooks::instrument(clang::PartialTranslationUnit& input) {
  HookInstrumentation added;
  if (!input.TheModule) return added;
  auto& module = *input.TheModule;
  auto& context = impl_->interp.getASTContext();
  std::vector<clang::FunctionDecl*> declarations;
  for (auto* tu : context.getTranslationUnitDecl()->redecls())
    functions(*tu, declarations, context.getSourceManager());
  std::set<std::string> user_symbols;
  for (auto* decl : declarations)
    if (!decl->isVariadic() && !decl->isTemplated() && !decl->isConstexpr())
      user_symbols.insert(symbol_name(context, *decl));
  // Keep native library symbols intact. Only references generated by this
  // interpreter are redirected to a stable JIT entry with a mutable slot.
  std::map<llvm::Function*, std::string> external_symbols;
  std::set<std::string> processed_externals;
  for (auto* decl : declarations) {
    if (decl->isVariadic() || decl->isTemplated() || decl->isConstexpr())
      continue;
    const std::string name = symbol_name(context, *decl);
    auto* function = module.getFunction(name);
    if (!function || function->isIntrinsic() ||
        !processed_externals.insert(name).second) continue;
    auto found = impl_->targets.find(name);
    if (decl->getDefinition()) {
#ifndef CPP_REPL_NATIVE_CALL_PATCHES
      continue;
#else
      if (found != impl_->targets.end() && found->second.entry.empty()) continue;
      auto address = impl_->interp.getSymbolAddress(name);
      if (!address) {
        llvm::consumeError(address.takeError());
        continue;
      }
      if (!NativeCallPatch::is_native_address(address->getValue())) continue;
      // An inline header body can also exist in a loaded library. Keep using
      // that native definition rather than creating a second JIT definition.
      function->deleteBody();
      function->setLinkage(llvm::GlobalValue::ExternalLinkage);
#endif
    } else if (!function->isDeclaration()) {
      continue;
    }
    const bool is_new = found == impl_->targets.end();
    if (is_new) {
      const auto id = std::to_string(++impl_->next_id);
      found = impl_->targets.emplace(name, Impl::Target{"__cpp_repl_slot_" + id, name, {},
          "__cpp_repl_external_" + id}).first;
      added.functions.push_back(name);
    }
    // A session definition may follow an earlier declaration in the AST.
    if (found->second.entry.empty()) continue;
    auto* entry = llvm::cast<llvm::Function>(module.getOrInsertFunction(
        found->second.entry, function->getFunctionType()).getCallee());
    entry->setCallingConv(function->getCallingConv());
    entry->setAttributes(function->getAttributes());
    function->replaceAllUsesWith(entry);
    external_symbols.emplace(entry, name);
    if (is_new) dispatch_through_slot(*entry, function, found->second.slot, true);
  }
  std::set<std::string> processed;
  for (const auto* decl : declarations) {
    if (decl->isVariadic() || decl->isTemplated() || decl->isConstexpr()) continue;
    const std::string name = symbol_name(context, *decl);
    llvm::Function* function = module.getFunction(name);
    if (!function || function->isDeclaration()) continue;
    // A declaration can occur both in the class and out of line in one input.
    if (!processed.insert(name).second) continue;
    // Give each direct caller/callee pair its own mutable slot before cloning
    // the body. Existing callers can then be filtered without recompilation.
    for (auto& block : *function)
      for (auto& instruction : block) {
        auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (!call) continue;
        auto* callee =
            llvm::dyn_cast<llvm::Function>(call->getCalledOperand()->stripPointerCasts());
        if (!callee) continue;
        const auto external = external_symbols.find(callee);
        const std::string callee_name = external == external_symbols.end()
            ? callee->getName().str() : external->second;
        if (!user_symbols.count(callee_name)) continue;
        const auto key = std::make_pair(name, callee_name);
        auto edge = impl_->calls.find(key);
        const bool new_edge = edge == impl_->calls.end();
        if (new_edge) {
          const auto id = std::to_string(++impl_->next_id);
          edge =
              impl_->calls.emplace(key, Impl::Target{"__cpp_repl_call_slot_" + id, "", {}, ""}).first;
          added.calls.push_back(key);
        }
        auto* pointer = llvm::PointerType::getUnqual(module.getContext());
        auto* slot = module.getNamedGlobal(edge->second.slot);
        if (!slot)
          slot = new llvm::GlobalVariable(
              module, pointer, false, llvm::GlobalValue::ExternalLinkage,
              new_edge ? llvm::ConstantPointerNull::get(pointer) : nullptr, edge->second.slot);
        auto alignment = module.getDataLayout().getPointerABIAlignment(0);
        slot->setAlignment(alignment);
        llvm::IRBuilder<> at(call);
        auto* replacement = at.CreateLoad(pointer, slot);
        replacement->setAlignment(alignment);
        replacement->setAtomic(llvm::AtomicOrdering::Acquire);
        call->setCalledOperand(at.CreateSelect(at.CreateIsNull(replacement), callee, replacement));
        call->setAttributes(call->getAttributes().removeFnAttributes(module.getContext()));
      }
    const auto linkage = function->getLinkage();
    const auto existing = impl_->targets.find(name);
    const bool is_new = existing == impl_->targets.end();
    const std::string id = std::to_string(++impl_->next_id);
    Impl::Target target =
        is_new ? Impl::Target{"__cpp_repl_slot_" + id, "__cpp_repl_original_" + id, {}, ""}
               : existing->second;
    llvm::Function* original;
    if (is_new) {
      llvm::ValueToValueMapTy values;
      original = llvm::CloneFunction(function, values);
      original->setName(target.original);
      original->setLinkage(llvm::GlobalValue::ExternalLinkage);
      original->setComdat(nullptr);
      // Route self calls through the public entry so recursion observes hooks.
      original->replaceAllUsesWith(function);
    } else {
      // Static/inline functions may be emitted again by a later input. Share
      // their original body and slot instead of creating an unhooked copy.
      original = llvm::cast<llvm::Function>(
          module.getOrInsertFunction(target.original, function->getFunctionType()).getCallee());
      original->setCallingConv(function->getCallingConv());
    }
    dispatch_through_slot(*function, original, target.slot, is_new);
    function->setLinkage(linkage);
    if (is_new) {
      added.functions.push_back(name);
      impl_->targets.emplace(name, std::move(target));
    }
  }
  return added;
}

void FunctionHooks::discard(const HookInstrumentation& added) {
  for (const auto& name : added.functions) impl_->targets.erase(name);
  for (const auto& key : added.calls) impl_->calls.erase(key);
}

bool FunctionHooks::set(HookKind kind, const std::string& signature_text, const std::string& body,
                        std::string& error, const std::string& caller) {
  return update(kind, signature_text, body, error, caller);
}

bool FunctionHooks::restore(const std::string& signature, std::string& error,
                            const std::string& caller) {
  if (signature.empty()) {
    // Resolve every active slot first so a lookup failure leaves all hooks intact.
    std::vector<std::pair<Impl::Target*, std::uintptr_t*>> active;
    auto collect = [&](auto& entries) {
      for (auto& entry : entries) {
        auto& target = entry.second;
        if (std::all_of(target.bodies.begin(), target.bodies.end(),
                        [](const Impl::Body& body) { return body.text.empty(); }) &&
            (!target.native || !target.native->active()))
          continue;
        auto address = impl_->interp.getSymbolAddress(target.slot);
        if (!address) {
          error = llvm::toString(address.takeError());
          return false;
        }
        active.emplace_back(&target, address->template toPtr<std::uintptr_t*>());
      }
      return true;
    };
    if (!collect(impl_->targets) || !collect(impl_->calls)) return false;
    return impl_->clear(active, error);
  }
  return update(std::nullopt, signature, "", error, caller);
}

bool FunctionHooks::update(std::optional<HookKind> kind, const std::string& signature_text,
                           const std::string& body, std::string& error,
                           const std::string& caller_text) {
  auto& interp = impl_->interp;
  auto& context = interp.getASTContext();
  const std::string id = std::to_string(++impl_->next_id);
  std::vector<std::string> names;
  auto* selected = impl_->resolve(signature_text, kind ? &names : nullptr, error);
  if (!selected) return false;
  clang::FunctionDecl* caller = nullptr;
  if (!caller_text.empty()) {
    caller = impl_->resolve(caller_text, nullptr, error);
    if (!caller) return false;
  }
  const std::string symbol = symbol_name(context, *selected);
  auto found = impl_->targets.find(symbol);
  if (kind && found == impl_->targets.end()) {
    if (!impl_->materialize(*selected, *this, error)) return false;
    found = impl_->targets.find(symbol);
  }
  if (!kind && !caller) {
    std::vector<std::pair<Impl::Target*, std::uintptr_t*>> clear;
    auto add = [&](Impl::Target& target) {
      auto address = interp.getSymbolAddress(target.slot);
      if (!address) {
        error = llvm::toString(address.takeError());
        return false;
      }
      clear.emplace_back(&target, address->toPtr<std::uintptr_t*>());
      return true;
    };
    if (found != impl_->targets.end() && !add(found->second)) return false;
    for (auto& entry : impl_->calls)
      if (entry.first.second == symbol && !add(entry.second)) return false;
    return impl_->clear(clear, error);
  }
  if (found == impl_->targets.end()) {
    error =
        "target needs a session-defined body or a resolvable exported library symbol; "
        "templates, constexpr functions and variadic functions are unsupported";
    return false;
  }
  Impl::Target* destination = &found->second;
  if (caller) {
    const auto key = std::make_pair(symbol_name(context, *caller), symbol);
    auto edge = impl_->calls.find(key);
    if (edge == impl_->calls.end() && kind) {
      bool native_caller = !caller->getDefinition();
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
      const auto routed = impl_->targets.find(key.first);
      if (!native_caller && (routed == impl_->targets.end() || !routed->second.entry.empty())) {
        auto address = interp.getSymbolAddress(key.first);
        if (address) native_caller = NativeCallPatch::is_native_address(address->getValue());
        else llvm::consumeError(address.takeError());
      }
#endif
      if (native_caller) {
        if (found->second.entry.empty()) {
          error = "a native caller requires a precompiled library target";
          return false;
        }
        Impl::Target native;
        if (!impl_->native_edge(*caller, *selected, *this, native, error)) return false;
        edge = impl_->calls.emplace(key, std::move(native)).first;
      } else {
        if (!impl_->materialize(*caller, *this, error)) return false;
        edge = impl_->calls.find(key);
      }
    }
    if (edge == impl_->calls.end()) {
      if (!kind) return true;
      error =
          "caller has no supported direct call to the target; indirect and virtual calls cannot be "
          "caller-filtered";
      return false;
    }
    destination = &edge->second;
  }
  auto target = *destination;
  target.original = found->second.original;
  if (kind)
    target.bodies[static_cast<unsigned>(*kind)] = {body, std::move(names), signature_text,
                                                   caller_text};
  else
    target.bodies = {};
  auto slot_address = interp.getSymbolAddress(target.slot);
  if (!slot_address) {
    error = llvm::toString(slot_address.takeError());
    return false;
  }
  if (target.bodies[0].text.empty() && target.bodies[1].text.empty() &&
      target.bodies[2].text.empty()) {
    if (target.native && !target.native->restore(error)) return false;
    __atomic_store_n(slot_address->toPtr<std::uintptr_t*>(), 0, __ATOMIC_RELEASE);
    *destination = std::move(target);
    return true;
  }
  auto original = interp.getSymbolAddress(target.original);
  if (!original) {
    error = llvm::toString(original.takeError());
    return false;
  }
  const auto policy = context.getPrintingPolicy();
  auto type_name = [&](clang::QualType type) {
    return clang::TypeName::getFullyQualifiedName(type, context, policy);
  };
  const bool is_void = selected->getReturnType()->isVoidType();
  std::string source =
      "#pragma clang diagnostic push\n"
      "#pragma clang diagnostic error \"-Wreturn-type\"\n"
      "#pragma clang diagnostic ignored \"-Wreturn-type-c-linkage\"\n"
      "using __cpp_repl_ret_" +
      id + " = " + type_name(selected->getReturnType()) + ";\n";
  std::string parameters, types, arguments;
  bool instance = false;
  if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(selected)) {
    instance = !method->isStatic();
    if (instance) {
      const std::string type = type_name(method->getThisType());
      parameters = type + " __cpp_repl_self";
      types = type;
      arguments = "__cpp_repl_self";
    }
  }
  for (unsigned i = 0; i < selected->getNumParams(); ++i) {
    const clang::QualType type = selected->getParamDecl(i)->getType();
    if (!type->isReferenceType() && !type.isTriviallyCopyableType(context)) {
      error = "non-trivial by-value parameters are unsupported; use references";
      return false;
    }
    const std::string alias = "__cpp_repl_argtype_" + id + "_" + std::to_string(i);
    source += "using " + alias + " = " + type_name(type) + ";\n";
    const std::string arg = "__cpp_repl_arg_" + std::to_string(i);
    if (!parameters.empty()) {
      parameters += ", ";
      types += ", ";
      arguments += ", ";
    }
    parameters += alias + " " + arg;
    types += alias;
    arguments += "static_cast<" + alias + "&&>(" + arg + ")";
  }
  const std::string result_type = "__cpp_repl_ret_" + id;
  const std::string wrapper = "__cpp_repl_hook_" + id;
  const bool is_noexcept = selected->getType()->castAs<clang::FunctionProtoType>()->isNothrow();
  source += "extern \"C\" " + result_type + " " + wrapper + "(" + parameters + ")" +
            (is_noexcept ? " noexcept" : "") + " {\n";
  auto block = [&](const Impl::Body& code, bool returns) {
    std::string text = "[&]()" + (returns ? " -> " + result_type : "") + " {\n";
    if (instance) text += "auto* self = __cpp_repl_self; (void)self;\n";
    for (unsigned i = 0; i < code.names.size(); ++i)
      text += "auto& " + code.names[i] + " = __cpp_repl_arg_" + std::to_string(i) + "; (void)" +
              code.names[i] + ";\n";
    return text + replace_this(code.text) + "\n}()";
  };
  if (!target.bodies[1].text.empty()) source += block(target.bodies[1], false) + ";\n";
  if (!is_void) source += result_type + " result = ";
  if (!target.bodies[0].text.empty())
    source += block(target.bodies[0], true);
  else
    source += "reinterpret_cast<" + result_type + " (*)(" + types + ")>(" +
              std::to_string(original->getValue()) + "ULL)(" + arguments + ")";
  source += ";\n";
  if (!target.bodies[2].text.empty()) source += block(target.bodies[2], false) + ";\n";
  if (!is_void) source += "return static_cast<" + result_type + "&&>(result);\n";
  source += "}\n#pragma clang diagnostic pop\n";
  if (!impl_->compile(source, false, *this, error)) return false;
  auto replacement = interp.getSymbolAddress(wrapper);
  if (!replacement) {
    error = llvm::toString(replacement.takeError());
    return false;
  }
  if (target.native && !target.native->active()) {
    auto entry = interp.getSymbolAddress(target.entry);
    if (!entry) {
      error = llvm::toString(entry.takeError());
      return false;
    }
    if (!target.native->install(entry->getValue(), error)) return false;
  }
  __atomic_store_n(slot_address->toPtr<std::uintptr_t*>(), replacement->getValue(),
                   __ATOMIC_RELEASE);
  *destination = std::move(target);
  return true;
}

}  // namespace example
