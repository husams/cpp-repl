#include "definition_generation.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Interpreter/Interpreter.h"
#include "clang/Interpreter/PartialTranslationUnit.h"
#include "cpp_symbols.hpp"
#include "definition_shadowing.hpp"
#include "function_hooks.hpp"
#include "interpreter_symbols.hpp"
#include "llvm/Config/llvm-config.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

namespace example {
PreparedDefinitions::PreparedDefinitions() = default;
PreparedDefinitions::~PreparedDefinitions() = default;

namespace {
bool write_all(int fd, const std::string& text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto count = ::write(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::string location(clang::ASTContext& context, clang::SourceLocation loc) {
  auto& sm = context.getSourceManager();
  const auto at = sm.getPresumedLoc(sm.getExpansionLoc(loc));
  if (!at.isValid()) {
    return "unavailable";
  }
  return std::string(at.getFilename()) + ":" + std::to_string(at.getLine()) + ":" +
         std::to_string(at.getColumn());
}

std::string symbol_name(clang::ASTContext& context, const clang::NamedDecl* decl) {
  std::unique_ptr<clang::MangleContext> mangler(context.createMangleContext());
  if (!mangler->shouldMangleDeclName(decl)) {
    return decl->getNameAsString();
  }
  std::string result;
  llvm::raw_string_ostream out(result);
  if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    if (llvm::isa<clang::CXXConstructorDecl>(function)) {
      mangler->mangleName(clang::GlobalDecl(function, clang::Ctor_Complete), out);
    } else if (llvm::isa<clang::CXXDestructorDecl>(function)) {
      mangler->mangleName(clang::GlobalDecl(function, clang::Dtor_Complete), out);
    } else {
      mangler->mangleName(clang::GlobalDecl(function), out);
    }
  } else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
    mangler->mangleName(clang::GlobalDecl(variable), out);
  }
  return result;
}

std::string signature(clang::ASTContext& context, const clang::FunctionDecl& function) {
  auto policy = context.getPrintingPolicy();
  policy.FullyQualifiedName = true;
  policy.SuppressTagKeyword = true;
  std::string name;
  llvm::raw_string_ostream out(name);
  function.getNameForDiagnostic(out, policy, true);
  out.flush();
  name = unversioned_name(name) + "(";
  std::set<std::string> used;
  for (const auto* parameter : function.parameters()) {
    if (!parameter->getName().empty()) {
      used.insert(parameter->getNameAsString());
    }
  }
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    if (i) {
      name += ", ";
    }
    const auto* parameter = function.getParamDecl(i);
    auto text = parameter->getNameAsString();
    if (text.empty()) {
      text = "arg" + std::to_string(i);
      while (!used.insert(text).second) {
        text += "_";
      }
    }
    clang::TypeName::getFullyQualifiedType(parameter->getType(), context, true)
        .getAsStringInternal(text, policy);
    name += text;
  }
  name += ")";
  if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
      method && !method->isStatic()) {
    if (method->isConst()) {
      name += " const";
    }
    if (method->isVolatile()) {
      name += " volatile";
    }
    if (method->getRefQualifier() == clang::RQ_LValue) {
      name += " &";
    }
    if (method->getRefQualifier() == clang::RQ_RValue) {
      name += " &&";
    }
  }
  if (const auto* proto = function.getType()->getAs<clang::FunctionProtoType>();
      proto && proto->isNothrow()) {
    name += " noexcept";
  }
  return name;
}

struct Reference {
  const clang::NamedDecl* decl;
  std::set<std::string> sites;
};

struct GeneratedRegion {
  std::size_t begin;
  std::size_t end;
  std::string text;
  std::string type;
};

using GeneratedRegions = std::map<std::string, GeneratedRegion>;

std::optional<GeneratedRegions> generated_regions(const std::string& text, std::string& error) {
  GeneratedRegions regions;
  std::string id;
  std::size_t start = 0;
  constexpr std::string_view begin = "// cpp-repl:begin ";
  constexpr std::string_view end = "// cpp-repl:end ";
  for (std::size_t pos = 0; pos < text.size();) {
    auto next = text.find('\n', pos);
    if (next == std::string::npos) {
      next = text.size();
    }
    const auto line = std::string_view(text).substr(pos, next - pos);
    if (line.substr(0, begin.size()) == begin) {
      if (!id.empty()) {
        error = "nested generated definition markers";
        return std::nullopt;
      }
      id = std::string(line.substr(begin.size()));
      start = pos;
      if (id.empty() || regions.count(id)) {
        error = "empty or duplicate generated definition marker";
        return std::nullopt;
      }
    } else if (line.substr(0, end.size()) == end) {
      if (id.empty() || line.substr(end.size()) != id) {
        error = "mismatched generated definition markers";
        return std::nullopt;
      }
      auto block = text.substr(start, next - start);
      const auto type_begin = block.find("\n// Type: ");
      const auto type_end = block.find('\n', type_begin + 1);
      const auto type =
          type_begin == std::string::npos ? "" : block.substr(type_begin, type_end - type_begin);
      regions.emplace(id, GeneratedRegion{start, next, std::move(block), type});
      id.clear();
    }
    pos = next + 1;
  }
  if (!id.empty()) {
    error = "unclosed generated definition marker";
    return std::nullopt;
  }
  return regions;
}

std::string prepare_line(const std::string& text) {
  const auto begin = text.find("\n%prepare-defines ");
  if (begin == std::string::npos) {
    return "";
  }
  return text.substr(begin, text.find('\n', begin + 1) - begin);
}

class References : public clang::RecursiveASTVisitor<References> {
  clang::ASTContext& context_;

public:
  std::map<std::string, Reference> found;
  std::set<const clang::CXXRecordDecl*> dynamic_records;

  explicit References(clang::ASTContext& context)
      : context_(context) {}

  bool shouldVisitTemplateInstantiations() const { return true; }

  bool TraverseDecl(clang::Decl* decl) {
    if (decl && context_.getSourceManager().isInSystemHeader(decl->getLocation())) {
      return true;
    }
    return clang::RecursiveASTVisitor<References>::TraverseDecl(decl);
  }

  void add(const clang::NamedDecl* decl, clang::SourceLocation site) {
    if (!decl || decl->isInvalidDecl() || decl->getDeclContext()->isDependentContext() ||
        context_.getSourceManager().isInSystemHeader(decl->getLocation())) {
      return;
    }
    if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (function->getDefinition() || function->getType()->isDependentType() ||
          function->getDescribedFunctionTemplate()) {
        return;
      }
    } else if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
      if (!variable->hasGlobalStorage() || variable->isLocalVarDecl() ||
          variable->getDefinition() || variable->getType()->isDependentType()) {
        return;
      }
    } else {
      return;
    }
    const auto mangled = symbol_name(context_, decl);
    if (mangled.empty()) {
      return;
    }
    auto entry = found.emplace(mangled, Reference{decl, {}}).first;
    entry->second.sites.insert(location(context_, site));
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
    add(expr->getDecl(), expr->getExprLoc());
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr* expr) {
    add(expr->getMemberDecl(), expr->getExprLoc());
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr* expr) {
    add(expr->getConstructor(), expr->getExprLoc());
    return true;
  }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl* record) {
    if (record->isCompleteDefinition() && !record->isDependentContext() &&
        record->isDynamicClass()) {
      dynamic_records.insert(record);
    }
    return true;
  }
};
} // namespace

std::optional<std::string>
isolated_definition_scan(const std::function<std::optional<std::string>(std::string&)>& scan,
                         std::string& error) {
  int fds[2];
  if (pipe(fds) != 0) {
    error = std::strerror(errno);
    return std::nullopt;
  }
  const auto child = fork();
  if (child == 0) {
    close(fds[0]);
    std::string failure;
    auto result = scan(failure);
    const bool sent =
        write_all(fds[1], std::string(result ? "1" : "0") + (result ? *result : failure));
    close(fds[1]);
    _exit(sent ? 0 : 1); // No copied JIT destructors or user atexit handlers.
  }
  close(fds[1]);
  if (child < 0) {
    error = std::strerror(errno);
    close(fds[0]);
    return std::nullopt;
  }
  std::string response;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool failed = false;
  while (true) {
    if (std::chrono::steady_clock::now() >= deadline) {
      error = "definition analysis timed out";
      failed = true;
      break;
    }
    pollfd fd{fds[0], POLLIN, 0};
    const auto ready = poll(&fd, 1, 100);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0) {
      error = std::strerror(errno);
      failed = true;
      break;
    }
    if (!ready) {
      continue;
    }
    char buffer[8192];
    const auto count = read(fds[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      error = std::strerror(errno);
      failed = true;
      break;
    }
    if (!count) {
      break;
    }
    response.append(buffer, static_cast<std::size_t>(count));
    if (response.size() > 16 * 1024 * 1024) {
      error = "definition analysis exceeds the 16 MiB report limit";
      failed = true;
      break;
    }
  }
  close(fds[0]);
  if (failed) {
    kill(child, SIGKILL);
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (failed) {
    return std::nullopt;
  }
  if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) || response.empty()) {
    error = "definition analysis process failed";
    return std::nullopt;
  }
  if (response.front() != '1') {
    error = response.substr(1);
    return std::nullopt;
  }
  return response.substr(1);
}

std::string generate_definition_entries(clang::Interpreter& interpreter, FunctionHooks& hooks,
                                        llvm::orc::LLJIT& jit,
                                        const PreparedDefinitions& prepared) {
  auto& context = interpreter.getASTContext();
  References references(context);
  for (std::size_t i = 0; i < prepared.source_inputs; ++i) {
    references.TraverseDecl(prepared.inputs[i]->TUPart);
  }
  std::set<std::string> emitted, defined;
  for (auto* input : prepared.inputs) {
    if (!input->TheModule) {
      continue;
    }
    for (const auto& global : input->TheModule->global_values()) {
      if (!global.isDeclarationForLinker()) {
        defined.insert(global.getName().str());
      } else if (!global.use_empty() && !global.hasExternalWeakLinkage()) {
        const auto* function = llvm::dyn_cast<llvm::Function>(&global);
        if (!function || !function->isIntrinsic()) {
          emitted.insert(global.getName().str());
        }
      }
    }
  }
  const auto resolved = [&](const std::string& name) {
    if (defined.count(name)) {
      return true;
    }
    auto address = lookup_symbol(jit, name);
    if (address) {
      return true;
    }
    llvm::consumeError(address.takeError());
    return false;
  };
  std::map<std::string, std::string> support_owners;
  std::unique_ptr<clang::MangleContext> mangler(context.createMangleContext());
  std::set<const clang::CXXRecordDecl*> visited_slots;
  const std::function<void(const clang::CXXRecordDecl*)> add_slots = [&](const auto* record) {
    if (!record || !visited_slots.insert(record).second) {
      return;
    }
    for (const auto* method : record->methods()) {
      if (method->isVirtual() && !method->isPureVirtual()) {
        references.add(method, record->getLocation());
      }
    }
    for (const auto& base : record->bases()) {
      add_slots(base.getType()->getAsCXXRecordDecl());
    }
  };
  for (const auto* record : references.dynamic_records) {
    std::string vtable, rtti;
    llvm::raw_string_ostream vtable_out(vtable), rtti_out(rtti);
    mangler->mangleCXXVTable(record, vtable_out);
#if LLVM_VERSION_MAJOR >= 22
    mangler->mangleCXXRTTI(context.getCanonicalTagType(record), rtti_out);
#else
    mangler->mangleCXXRTTI(context.getTypeDeclType(record), rtti_out);
#endif
    vtable_out.flush();
    rtti_out.flush();
    if (!emitted.count(vtable) && !emitted.count(rtti)) {
      continue;
    }
    if (const auto* key = context.getCurrentKeyFunction(record)) {
      references.add(key, record->getLocation());
      const auto owner = signature(context, *key);
      support_owners[vtable] = owner;
      support_owners[rtti] = owner;
    }
    // A key method emits the vtable's other non-pure slots as dependencies.
    // Stage their definitions as part of the same owning class group.
    add_slots(record);
  }
  std::ostringstream out;
  out << "// cpp-repl generated definitions v1\n// Input: " << prepared.path
      << "\n// Complete and uncomment the %define entries before replaying.\n"
         "// Referenced candidates may not require linkage until used.\n"
      << "%prepare-defines " << std::quoted(prepared.path);
  if (prepared.context != prepared.path) {
    out << " --tu " << std::quoted(prepared.context);
  }
  out << '\n';
  for (const auto& [mangled, ref] : references.found) {
    if (resolved(mangled)) {
      emitted.erase(mangled);
      continue;
    }
    std::string target, reason;
    const bool callable = llvm::isa<clang::FunctionDecl>(ref.decl);
    if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(ref.decl)) {
      target = signature(context, *function);
      if (!hooks.definition(target, reason) && reason.empty()) {
        reason = "unsupported callable";
      }
    } else {
      target = unversioned_name(ref.decl->getQualifiedNameAsString());
      const auto variable = variable_definition(context, target, reason);
      if (variable && variable->thread_local_storage) {
        reason = "thread-local publication is unsupported";
      }
      if (ref.decl->getFormalLinkage() != clang::Linkage::External) {
        reason = "definition requires external linkage";
      }
    }
    out << "\n// cpp-repl:begin " << mangled << '\n'
        << "// "
        << (emitted.count(mangled) ? "Unresolved emitted dependency: " : "Referenced candidate: ")
        << target << "\n// Declaration: " << location(context, ref.decl->getLocation()) << '\n';
    if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(ref.decl)) {
      out << "// Type: " << unversioned_name(value->getType().getCanonicalType().getAsString())
          << '\n';
    }
    for (const auto& site : ref.sites) {
      out << "// Reference: " << site << '\n';
    }
    if (!reason.empty()) {
      out << "// Unsupported: " << reason << '\n';
    } else {
      out << "// %define " << target << " cpp`/* " << (callable ? "function body" : "initializer")
          << " */`\n";
    }
    out << "// cpp-repl:end " << mangled << '\n';
    emitted.erase(mangled);
  }
  for (const auto& name : emitted) {
    if (!resolved(name)) {
      const auto owner = support_owners.find(name);
      if (owner != support_owners.end()) {
        out << "\n// Support dependency: " << llvm::demangle(name)
            << " (owning method: " << owner->second << ")\n";
      } else {
        out << "\n// Unresolved support symbol (no supported declaration): " << llvm::demangle(name)
            << '\n';
      }
    }
  }
  out << "\n%commit-defines\n";
  return out.str();
}

bool write_definition_script(const std::string& path, const std::string& script, bool update,
                             std::string& error) {
  std::string result = script;
  if (update) {
    std::ifstream input(path);
    if (!input) {
      error = "cannot read definitions file: " + path;
      return false;
    }
    const std::string old{std::istreambuf_iterator<char>(input), {}};
    if (old.rfind("// cpp-repl generated definitions v1\n", 0) != 0) {
      error = "update requires a generated definitions file";
      return false;
    }
    auto previous = generated_regions(old, error);
    auto fresh = generated_regions(script, error);
    if (!previous || !fresh) {
      return false;
    }
    if (prepare_line(old).empty() || prepare_line(old) != prepare_line(script)) {
      error = "definitions file belongs to a different input or compilation context";
      return false;
    }
    result = old;
    std::vector<GeneratedRegion> stale;
    for (const auto& [id, region] : *previous) {
      const auto current = fresh->find(id);
      if (current != fresh->end() && current->second.type == region.type) {
        fresh->erase(current);
      } else {
        stale.push_back(region);
      }
    }
    std::sort(stale.begin(), stale.end(),
              [](const auto& a, const auto& b) { return a.begin > b.begin; });
    for (const auto& region : stale) {
      std::istringstream lines(region.text);
      std::string commented = "// Stale definition (review signature or provider change):\n", line;
      while (std::getline(lines, line)) {
        commented += "// " + line + '\n';
      }
      result.replace(region.begin, region.end - region.begin, commented);
    }
    const auto commit = result.rfind("\n%commit-defines\n");
    if (commit == std::string::npos) {
      error = "generated file is missing its %commit-defines line";
      return false;
    }
    std::string additions;
    for (const auto& [id, region] : *fresh) {
      additions += "\n" + region.text + "\n";
    }
    result.insert(commit, additions);
  }
  struct stat before{};
  if (update && (lstat(path.c_str(), &before) != 0 || !S_ISREG(before.st_mode))) {
    error = "update requires a regular file, not a symlink";
    return false;
  }
  std::string temporary = path + ".tmp-XXXXXX";
  const int fd = mkstemp(temporary.data());
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  const bool written = (!update || fchmod(fd, before.st_mode & 0777) == 0) && write_all(fd, result);
  const int closed = close(fd);
  if (!written || closed != 0) {
    error = "could not write definitions file";
    unlink(temporary.c_str());
    return false;
  }
  // link() creates the destination exclusively, including against symlinks.
  const int installed =
      update ? rename(temporary.c_str(), path.c_str()) : link(temporary.c_str(), path.c_str());
  const int saved_errno = errno;
  unlink(temporary.c_str());
  if (installed != 0) {
    error = std::strerror(saved_errno);
    return false;
  }
  return true;
}
} // namespace example
