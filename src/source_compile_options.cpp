#include "source_compile_options.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/MemoryBuffer.h"

namespace example {
namespace {

std::string absolute_path(const std::string& name,
                          const std::string& directory) {
  const auto path = std::filesystem::path(name);
  return path.is_relative() ? (std::filesystem::path(directory) / path)
                                  .lexically_normal()
                                  .string()
                            : name;
}

// Only keyword properties change; existing IdentifierInfo objects carry the
// session's macro chains and declaration bindings and must keep their identity.
void set_keywords(clang::IdentifierTable& table,
                  const clang::LangOptions& language) {
  clang::IdentifierTable desired(language);
  std::vector<std::string> names;
  for (const auto& entry : table) names.push_back(entry.getKey().str());
  for (const auto& entry : desired) names.push_back(entry.getKey().str());
  for (const auto& name : names) {
    const auto& from = desired.get(name);
    auto& to = table.get(name, from.getTokenID());
    to.setIsExtensionToken(from.isExtensionToken());
    to.setIsFutureCompatKeyword(from.isFutureCompatKeyword());
    to.setIsCPlusPlusOperatorKeyword(from.isCPlusPlusOperatorKeyword());
  }
}

using Definitions = std::map<std::string, std::string>;
Definitions definitions(const std::string& predefines) {
  Definitions result;
  std::istringstream lines(predefines);
  std::string line;
  while (std::getline(lines, line)) {
    std::size_t start;
    if (line.rfind("#define ", 0) == 0)
      start = 8;
    else if (line.rfind("#undef ", 0) == 0)
      start = 7;
    else
      continue;
    const auto end = line.find_first_of("( \t\r", start);
    const auto name = line.substr(start, end - start);
    result[name] = line;
  }
  return result;
}

std::string macro_definition(clang::Preprocessor& pp,
                             const clang::IdentifierInfo& name,
                             const clang::MacroInfo& macro) {
  std::string result = "#define " + name.getName().str();
  if (macro.isFunctionLike()) {
    result += '(';
    unsigned i = 0;
    for (auto* parameter : macro.params()) {
      if (i++) result += ',';
      if (macro.isC99Varargs() && i == macro.getNumParams())
        result += "...";
      else {
        result += parameter->getName().str();
        if (macro.isGNUVarargs() && i == macro.getNumParams()) result += "...";
      }
    }
    result += ')';
  }
  for (const auto& token : macro.tokens())
    result += ' ' + pp.getSpelling(token);
  return result;
}

}  // namespace

struct SourceCompileScope::Impl {
  clang::CompilerInstance& session;
  clang::LangOptions language;
  clang::HeaderSearchOptions headers;
  std::string error, prelude, includes, epilogue;
  bool applied = false;

  explicit Impl(clang::CompilerInstance& ci)
      : session(ci),
        language(ci.getLangOpts()),
        headers(ci.getHeaderSearchOpts()) {}

  void apply_headers() {
    clang::ApplyHeaderSearchOptions(
        session.getPreprocessor().getHeaderSearchInfo(),
        session.getHeaderSearchOpts(), session.getLangOpts(),
        session.getTarget().getTriple());
  }

  ~Impl() {
    if (!applied) return;
    session.getLangOpts() = language;
    session.getHeaderSearchOpts() = headers;
    set_keywords(session.getPreprocessor().getIdentifierTable(), language);
    apply_headers();
  }
};

SourceCompileScope::SourceCompileScope(clang::CompilerInstance& session,
                                       clang::CompilerInstance& configured,
                                       const std::string& directory)
    : impl_(std::make_unique<Impl>(session)) {
  auto& state = *impl_;
  // The persistent JIT and AST share one target ABI. A compile database may
  // contain cross-compiled files, but those cannot run in this process.
  const auto& target = configured.getTarget().getTriple();
  const auto& host = session.getTarget().getTriple();
  if (target.getArch() != host.getArch() ||
      target.getSubArch() != host.getSubArch() ||
      target.getOS() != host.getOS() ||
      target.getEnvironment() != host.getEnvironment() ||
      target.getObjectFormat() != host.getObjectFormat() ||
      configured.getTarget().getABI() != session.getTarget().getABI() ||
      llvm::StringRef(configured.getTarget().getDataLayoutString()) !=
          session.getTarget().getDataLayoutString()) {
    state.error =
        "compile command targets a different ABI than the running REPL (" +
        configured.getTarget().getTriple().str() + " vs " +
        session.getTarget().getTriple().str() + ")";
    return;
  }
  auto& pp_options = configured.getPreprocessorOpts();
  // Later -D/-U flags override earlier layers. Keep only the final directive
  // for each macro when validating the disposable frontend.
  std::set<std::string> seen_macros;
  decltype(pp_options.Macros) macros;
  for (auto it = pp_options.Macros.rbegin(); it != pp_options.Macros.rend();
       ++it) {
    const auto name = it->first.substr(0, it->first.find_first_of("(="));
    if (seen_macros.insert(name).second) macros.push_back(*it);
  }
  std::reverse(macros.begin(), macros.end());
  pp_options.Macros = std::move(macros);
  if (!pp_options.ImplicitPCHInclude.empty() ||
      !pp_options.ChainedIncludes.empty()) {
    state.error = "per-file PCH requires a separate compiler session";
    return;
  }
  if (!configured.getHeaderSearchOpts().VFSOverlayFiles.empty()) {
    state.error = "per-file VFS overlays require a separate compiler session";
    return;
  }
  auto& search = configured.getHeaderSearchOpts();
  for (auto& entry : search.UserEntries)
    entry.Path = absolute_path(entry.Path, directory);
  if (!search.Sysroot.empty())
    search.Sysroot = absolute_path(search.Sysroot, directory);
  if (!search.ResourceDir.empty())
    search.ResourceDir = absolute_path(search.ResourceDir, directory);
  configured.createFileManager();
#if LLVM_VERSION_MAJOR >= 22
  configured.createSourceManager();
#else
  configured.createSourceManager(configured.getFileManager());
#endif
  configured.createPreprocessor(clang::TU_Complete);
  auto resolve_includes = [&](std::vector<std::string>& includes) {
    for (auto& include : includes) {
      const auto absolute = absolute_path(include, directory);
      std::error_code error;
      if (std::filesystem::is_regular_file(absolute, error))
        include = absolute;
      else {
        auto found =
            configured.getPreprocessor().getHeaderSearchInfo().LookupFile(
                include, clang::SourceLocation(), false, nullptr, nullptr, {},
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!found) {
          state.error = "cannot find compile-command include: " + include;
          return false;
        }
        include = found->getName().str();
      }
      if (include.find_first_of("\"\n\r") != std::string::npos) {
        state.error =
            "compile-command include contains a quote or newline: " + include;
        return false;
      }
    }
    return true;
  };
  if (!resolve_includes(pp_options.Includes) ||
      !resolve_includes(pp_options.MacroIncludes))
    return;
  // Forced includes belong to the live AST; -imacros only contributes macro
  // definitions, so preprocess it with Clang in the disposable frontend.
  for (const auto& include : pp_options.Includes)
    state.includes += "#include \"" + include + "\"\n";
  pp_options.Includes.clear();
  clang::InitializePreprocessor(configured.getPreprocessor(), pp_options,
                                configured.getPCHContainerReader(),
                                configured.getFrontendOpts(),
                                configured.getCodeGenOpts());
  const auto& predefines = configured.getPreprocessor().getPredefines();
  const auto baseline = definitions(session.getPreprocessor().getPredefines());
  auto desired = definitions(predefines);
  {
    auto& manager = configured.getSourceManager();
    manager.setMainFileID(manager.createFileID(
        llvm::MemoryBuffer::getMemBuffer("", "<compile-options>")));
    auto& pp = configured.getPreprocessor();
    const bool incremental = configured.getLangOpts().IncrementalExtensions;
    configured.getLangOpts().IncrementalExtensions = false;
    pp.enableIncrementalProcessing(false);
    configured.getDiagnosticClient().BeginSourceFile(configured.getLangOpts(),
                                                     &pp);
    pp.EnterMainSourceFile();
    clang::Token token;
    do {
      pp.Lex(token);
    } while (!token.is(clang::tok::eof));
    configured.getDiagnosticClient().EndSourceFile();
    configured.getLangOpts().IncrementalExtensions = incremental;
    if (configured.getDiagnostics().hasErrorOccurred()) {
      state.error = "invalid compiler definitions or -imacros input";
      return;
    }
  }
  if (!pp_options.MacroIncludes.empty()) {
    auto& pp = configured.getPreprocessor();
    for (auto& [name, directive] : desired) {
      if (!pp.getMacroInfo(&pp.getIdentifierTable().get(name)))
        directive = "#undef " + name;
    }
    for (const auto& entry : pp.macros()) {
      auto* macro = pp.getMacroInfo(entry.first);
      if (macro && !macro->isBuiltinMacro())
        desired[entry.first->getName().str()] =
            macro_definition(pp, *entry.first, *macro);
    }
  }
  std::set<std::string> changed;
  for (const auto& [name, directive] : baseline) {
    auto found = desired.find(name);
    if (found == desired.end() || found->second != directive)
      changed.insert(name);
  }
  for (const auto& [name, directive] : desired) {
    auto found = baseline.find(name);
    if (found == baseline.end() || found->second != directive)
      changed.insert(name);
  }
  // Explicit -D/-U must override a macro even if user input has changed it
  // since startup, or it has no corresponding builtin definition.
  for (const auto& macro : pp_options.Macros)
    changed.insert(macro.first.substr(0, macro.first.find_first_of("(=")));
  if (!changed.empty())
    state.prelude =
        "#pragma clang diagnostic push\n#pragma clang diagnostic ignored "
        "\"-Wbuiltin-macro-redefined\"\n";
  for (const auto& name : changed) {
    state.prelude +=
        "#pragma push_macro(\"" + name + "\")\n#undef " + name + '\n';
    if (auto found = desired.find(name); found != desired.end())
      state.prelude += found->second + '\n';
    state.epilogue = "#pragma pop_macro(\"" + name + "\")\n" + state.epilogue;
  }
  if (!changed.empty()) state.prelude += "#pragma clang diagnostic pop\n";
  session.getLangOpts() = configured.getLangOpts();
  session.getHeaderSearchOpts() = search;
  set_keywords(session.getPreprocessor().getIdentifierTable(),
               session.getLangOpts());
  state.applied = true;
  state.apply_headers();
}

SourceCompileScope::~SourceCompileScope() = default;
const std::string& SourceCompileScope::error() const { return impl_->error; }
const std::string& SourceCompileScope::prelude() const {
  return impl_->prelude;
}
const std::string& SourceCompileScope::includes() const {
  return impl_->includes;
}
const std::string& SourceCompileScope::epilogue() const {
  return impl_->epilogue;
}

}  // namespace example
