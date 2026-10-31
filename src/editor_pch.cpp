#include "editor_pch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>
#include <utility>

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/Token.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace example {

namespace {

std::string sha256(const std::string& text) {
  llvm::SHA256 digest;
  digest.update(text);
  const auto bytes = digest.final();
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 0x0f]);
  }
  return result;
}

std::string read_file(const fs::path& path, bool* readable = nullptr) {
  std::ifstream input(path, std::ios::binary);
  if (readable) {
    *readable = static_cast<bool>(input);
  }
  if (!input) {
    return {};
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string trim(std::string value) {
  const auto first =
      std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
                      return std::isspace(c);
                    }).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

bool safe_preprocessor_prefix(const std::string& code) {
  std::istringstream lines(code);
  std::string line;
  std::string logical_line;
  bool block_comment = false;
  auto safe_line = [&block_comment](std::string content) {
    content = trim(std::move(content));
    if (block_comment) {
      const auto end = content.find("*/");
      if (end == std::string::npos) {
        return true;
      }
      content = trim(content.substr(end + 2));
      block_comment = false;
    }
    for (;;) {
      if (content.empty() || content.rfind("//", 0) == 0) {
        return true;
      }
      if (content.rfind("/*", 0) == 0) {
        const auto end = content.find("*/", 2);
        if (end == std::string::npos) {
          block_comment = true;
          return true;
        }
        content = trim(content.substr(end + 2));
        continue;
      }
      break;
    }
    if (content.empty()) {
      return true;
    }
    if (content[0] != '#') {
      return false;
    }
    content = trim(content.substr(1));
    const auto word_end = content.find_first_of(" \t\r\n");
    const std::string directive = content.substr(0, word_end);
    return directive == "include" || directive == "include_next" || directive == "define" ||
           directive == "undef" || directive == "pragma" || directive == "if" ||
           directive == "ifdef" || directive == "ifndef" || directive == "elif" ||
           directive == "else" || directive == "endif";
  };
  while (std::getline(lines, line)) {
    logical_line += line;
    const auto last = logical_line.find_last_not_of(" \t\r");
    if (last != std::string::npos && logical_line[last] == '\\') {
      logical_line.erase(last);
      logical_line.push_back(' ');
      continue;
    }
    if (!safe_line(std::move(logical_line))) {
      return false;
    }
    logical_line.clear();
  }
  return logical_line.empty() || safe_line(std::move(logical_line));
}

std::string private_cache_dir() {
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  if (xdg && *xdg) {
    return (fs::path(xdg) / "cpp-repl" / "editor-pch").string();
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return (fs::path(home) / ".cache" / "cpp-repl" / "editor-pch").string();
  }
  return {};
}

std::string unique_suffix() {
  static std::atomic<unsigned long long> sequence{0};
  return std::to_string(::getpid()) + "." +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string file_stamp(const std::string& path) {
  struct stat info{};
  if (::stat(path.c_str(), &info) != 0) {
    return {};
  }
#if defined(__APPLE__)
  return std::to_string(info.st_size) + ":" + std::to_string(info.st_mtimespec.tv_sec) + ":" +
         std::to_string(info.st_mtimespec.tv_nsec) + ":" +
         std::to_string(info.st_ctimespec.tv_sec) + ":" + std::to_string(info.st_ctimespec.tv_nsec);
#else
  return std::to_string(info.st_size) + ":" + std::to_string(info.st_mtim.tv_sec) + ":" +
         std::to_string(info.st_mtim.tv_nsec) + ":" + std::to_string(info.st_ctim.tv_sec) + ":" +
         std::to_string(info.st_ctim.tv_nsec);
#endif
}

std::map<std::string, std::string> loaded_file_hashes(clang::SourceManager& source_manager) {
  std::map<std::string, std::string> result;
  for (auto it = source_manager.fileinfo_begin(); it != source_manager.fileinfo_end(); ++it) {
    const auto file_id = source_manager.translateFile(it->first);
    if (file_id.isInvalid()) {
      continue;
    }
    const auto buffer = source_manager.getBufferDataIfLoaded(file_id);
    if (!buffer) {
      continue;
    }
    std::error_code path_error;
    const auto path = fs::weakly_canonical(it->first.getName().str(), path_error);
    if (path_error) {
      continue;
    }
    std::error_code file_error;
    if (!fs::is_regular_file(path, file_error) || file_error) {
      continue;
    }
    result.emplace(path.string(), sha256(buffer->str()));
  }
  return result;
}

class EditorPchAction final : public clang::GeneratePCHAction {
protected:
  bool BeginSourceFileAction(clang::CompilerInstance& compiler) override {
    if (!clang::GeneratePCHAction::BeginSourceFileAction(compiler)) {
      return false;
    }
    compiler.getPreprocessor().setTokenWatcher([](const clang::Token& token) {
      if (token.is(clang::tok::annot_repl_input_end)) {
        const_cast<clang::Token&>(token).setKind(clang::tok::eof);
      }
    });
    compiler.getPreprocessor().enableIncrementalProcessing(false);
    return true;
  }
};

} // namespace

void EditorPchCache::observe_input(const std::string& code, const std::vector<std::string>& args,
                                   bool success, clang::CompilerInstance* session) {
  if (!observing_) {
    return;
  }
  if (!success) {
    observing_ = false;
    headers_.clear();
    status_ = "prefix capture stopped after a failed input";
    return;
  }
  std::error_code error;
  const auto cwd = fs::current_path(error).string();
  if (error || (!captured_cwd_.empty() && captured_cwd_ != cwd) ||
      (capture_started_ && captured_args_ != args)) {
    observing_ = false;
    eligible_ = false;
    headers_.clear();
    status_ = "prefix capture stopped because compiler arguments or working directory changed";
    return;
  }
  if (!safe_preprocessor_prefix(code)) {
    observing_ = false;
    status_ = "prefix capture stopped at the first non-preprocessor input";
    return;
  }
  if (!capture_started_) {
    capture_started_ = true;
  }
  captured_cwd_ = cwd;
  captured_args_ = args;
  prefix_.push_back(code);
  if (session && session->hasPreprocessor()) {
    auto& preprocessor = session->getPreprocessor();
    macro_snapshot_.clear();
    for (const auto& [identifier, definition] : preprocessor.macros()) {
      (void)definition;
      const auto current = preprocessor.getMacroDefinition(identifier);
      // The local directive pointer does not describe a macro that is supplied
      // or made ambiguous by a module. Leave those names out of the snapshot so
      // replay conservatively handles module-state changes later.
      if (!current.getModuleMacros().empty() || current.isAmbiguous()) {
        continue;
      }
      macro_snapshot_.emplace(identifier,
                              preprocessor.getLocalMacroDirectiveHistory(identifier));
    }
  }
  if (session && session->hasSourceManager()) {
    auto& source_manager = session->getSourceManager();
    for (auto it = source_manager.fileinfo_begin(); it != source_manager.fileinfo_end(); ++it) {
      const auto file_id = source_manager.translateFile(it->first);
      if (file_id.isInvalid()) {
        continue;
      }
      const auto buffer = source_manager.getBufferDataIfLoaded(file_id);
      if (!buffer) {
        continue;
      }
      std::error_code path_error;
      const auto canonical = fs::weakly_canonical(it->first.getName().str(), path_error);
      if (path_error) {
        continue;
      }
      const auto path = canonical.string();
      std::error_code file_error;
      if (!fs::is_regular_file(canonical, file_error) || file_error) {
        continue;
      }
      if (observed_hashes_.count(path)) {
        continue;
      }
      observed_hashes_.emplace(path, sha256(buffer->str()));
      const auto stamp = file_stamp(path);
      if (!stamp.empty()) {
        dependency_stamps_.try_emplace(path, stamp);
      }
    }
  }
  if (session && session->hasASTContext() && session->hasSourceManager()) {
    auto& source_manager = session->getSourceManager();
    const auto from_captured_prefix_file = [&](const clang::Decl* decl) {
      if (!decl || decl->isImplicit()) {
        return false;
      }
      if (const auto* specialization =
              llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl);
          specialization && specialization->getSpecializationKind() ==
                                clang::TSK_ImplicitInstantiation) {
        return false;
      }
      if (const auto* specialization =
              llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl);
          specialization && specialization->getSpecializationKind() ==
                                clang::TSK_ImplicitInstantiation) {
        return false;
      }
      if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
          function && function->getTemplateSpecializationKind() ==
                          clang::TSK_ImplicitInstantiation) {
        return false;
      }
      const auto location = source_manager.getSpellingLoc(decl->getLocation());
      if (!location.isValid() || !location.isFileID()) {
        return false;
      }
      const auto file_id = source_manager.getFileID(location);
      const auto key = std::make_pair(&source_manager, file_id.getHashValue());
      auto found = prefix_decl_files_.find(key);
      if (found == prefix_decl_files_.end()) {
        std::error_code path_error;
        const auto canonical =
            fs::weakly_canonical(source_manager.getFilename(location).str(), path_error);
        const bool captured = !path_error && observed_hashes_.count(canonical.string());
        found = prefix_decl_files_.emplace(key, captured).first;
      }
      return found->second;
    };
    std::function<void(clang::DeclContext*)> capture_context = [&](clang::DeclContext* context) {
      for (auto* decl : context->decls()) {
        if (from_captured_prefix_file(decl)) {
          decl_snapshot_.insert(decl);
        }
        if (auto* space = llvm::dyn_cast<clang::NamespaceDecl>(decl);
            space && captured_namespace_declarations_.insert(space).second) {
          capture_context(space);
        }
      }
    };
    for (auto* unit = session->getASTContext().getTranslationUnitDecl(); unit;
         unit = unit->getPreviousDecl()) {
      if (captured_translation_units_.insert(unit).second) {
        capture_context(unit);
      }
    }
  }
}

void EditorPchCache::invalidate(const std::string& reason) {
  active_ = false;
  eligible_ = false;
  warm_cache_validated_ = false;
  headers_.clear();
  status_ = reason.empty() ? "cached PCH disabled by the caller" : reason;
}

bool EditorPchCache::prepare(clang::CompilerInstance& compiler,
                             const std::vector<std::string>& args) {
  observing_ = false;
  active_ = false;
  if (!eligible_ || prefix_.empty()) {
    headers_.clear();
    if (status_.empty()) {
      status_ = "no safe preprocessor-only prefix was captured";
    }
    eligible_ = false;
    return false;
  }
  std::error_code error;
  const auto cwd = fs::current_path(error).string();
  if (error || cwd != captured_cwd_ || args != captured_args_) {
    eligible_ = false;
    headers_.clear();
    status_ = "cached PCH disabled: compiler arguments or working directory changed";
    return false;
  }

  std::string source;
  for (const auto& input : prefix_) {
    source.append(input);
    source.push_back('\n');
  }
  clang::CompilerInvocation signature(compiler.getInvocation());
  signature.getFrontendOpts().Inputs.clear();
  signature.getFrontendOpts().OutputFile.clear();
  signature.getPreprocessorOpts().ImplicitPCHInclude.clear();
  signature.getPreprocessorOpts().clearRemappedFiles();
  const auto cc1_args = signature.getCC1CommandLine();
  const std::string key_material =
      source + "\n--cc1--\n" +
      [&] {
        std::string value;
        for (const auto& arg : cc1_args) {
          value += arg;
          value.push_back('\0');
        }
        return value;
      }() +
      "\n--schema--\n2\n--cwd--\n" + cwd + "\n--llvm--\n" + LLVM_VERSION_STRING + "\n--target--\n" +
      compiler.getInvocation().getTargetOpts().Triple + "\n--sysroot--\n" +
      compiler.getInvocation().getHeaderSearchOpts().Sysroot + "\n--resource-dir--\n" +
      compiler.getInvocation().getHeaderSearchOpts().ResourceDir + "\n--captured-files--\n" + [&] {
        std::string value;
        for (const auto& [path, digest] : observed_hashes_) {
          value += path;
          value.push_back('\0');
          value += digest;
          value.push_back('\0');
        }
        return value;
      }();
  const auto key = sha256(key_material);
  std::error_code fs_error;
  const auto cache_root = private_cache_dir();
  if (cache_root.empty()) {
    invalidate("cached PCH disabled: no private cache directory is available");
    return false;
  }
  const auto cache_dir = fs::path(cache_root);
  const auto pch_path = cache_dir / (key + ".pch");
  const auto manifest_path = cache_dir / (key + ".deps");
  virtual_source_path_ = (fs::path(cwd) / (".cpp-repl-editor-prefix-" + key + ".hpp")).string();
  source_ = source;

  auto activate = [&]() {
    auto& preprocessor = compiler.getInvocation().getPreprocessorOpts();
    preprocessor.ImplicitPCHInclude = pch_path.string();
    preprocessor.addRemappedFile(
        virtual_source_path_,
        llvm::MemoryBuffer::getMemBufferCopy(source_, virtual_source_path_).release());
    cache_path_ = pch_path.string();
    active_ = true;
    status_ = "cached startup PCH is active";
    return true;
  };

  const bool warm_profile = warm_cache_validated_ && cache_path_ == pch_path.string();
  if (warm_profile) {
    if (observed_hashes_.size() != header_hashes_.size()) {
      invalidate("cached PCH disabled: cached dependencies differ from parsed session headers");
      return false;
    }
    for (const auto& [path, expected] : observed_hashes_) {
      const auto cached = header_hashes_.find(path);
      if (cached == header_hashes_.end() || cached->second != expected) {
        invalidate("cached PCH disabled: cached dependency differs from parsed session header: " +
                   path);
        return false;
      }
    }

    const auto current_pch_stamp = file_stamp(pch_path.string());
    if (current_pch_stamp.empty()) {
      invalidate("cached PCH disabled: cached PCH file disappeared");
      return false;
    }
    if (current_pch_stamp != pch_stamp_) {
      bool readable = false;
      const auto bytes = read_file(pch_path, &readable);
      if (!readable || sha256(bytes) != pch_hash_) {
        invalidate("cached PCH disabled: cached PCH file changed");
        return false;
      }
      pch_stamp_ = current_pch_stamp;
    }

    // The observed and cached dependency maps are equal, so one stat pass checks
    // every header used by the session and by this PCH.
    for (const auto& [path, expected] : observed_hashes_) {
      const auto stamp = file_stamp(path);
      if (stamp.empty()) {
        invalidate("cached PCH disabled: session header disappeared: " + path);
        return false;
      }
      if (stamp != dependency_stamps_[path]) {
        bool readable = false;
        const auto contents = read_file(path, &readable);
        if (!readable || sha256(contents) != expected) {
          invalidate("cached PCH disabled: session header changed: " + path);
          return false;
        }
        dependency_stamps_[path] = stamp;
      }
    }
    pch_stamp_ = current_pch_stamp;
    return activate();
  }

  bool cache_valid = fs::is_regular_file(pch_path, fs_error) && !fs_error;
  fs_error.clear();
  std::map<std::string, std::string> cached_dependencies;
  if (cache_valid) {
    std::ifstream manifest(manifest_path);
    std::string line;
    std::string expected_pch_hash;
    if (!manifest || !std::getline(manifest, line) || line.rfind("pch\t", 0) != 0) {
      cache_valid = false;
    } else {
      expected_pch_hash = line.substr(4);
      bool readable = false;
      const auto contents = read_file(pch_path, &readable);
      if (!readable || sha256(contents) != expected_pch_hash) {
        cache_valid = false;
      } else {
        pch_hash_ = expected_pch_hash;
        pch_stamp_ = file_stamp(pch_path.string());
      }
    }
    while (cache_valid && std::getline(manifest, line)) {
      const auto tab = line.find('\t');
      if (tab == std::string::npos) {
        cache_valid = false;
        break;
      }
      const auto path = line.substr(0, tab);
      const auto expected = line.substr(tab + 1);
      bool readable = false;
      const auto contents = read_file(path, &readable);
      if (!readable || sha256(contents) != expected) {
        cache_valid = false;
      } else {
        cached_dependencies[path] = expected;
        dependency_stamps_[path] = file_stamp(path);
      }
    }
    if (cache_valid) {
      warm_cache_validated_ = true;
      if (cached_dependencies.size() != observed_hashes_.size()) {
        std::string mismatch_path;
        for (const auto& [path, digest] : cached_dependencies) {
          if (!observed_hashes_.count(path)) {
            mismatch_path = path;
            break;
          }
        }
        if (mismatch_path.empty() && !observed_hashes_.empty()) {
          mismatch_path = observed_hashes_.begin()->first;
        }
        invalidate("cached PCH disabled: dependency set differs from parsed session headers: " +
                   mismatch_path);
        return false;
      }
    }
  }

  if (!cache_valid) {
    fs::create_directories(cache_dir, fs_error);
    if (fs_error) {
      invalidate("cached PCH disabled: cannot create the cache directory");
      return false;
    }
    fs::permissions(cache_dir, fs::perms::owner_all, fs::perm_options::replace, fs_error);
    if (fs_error) {
      invalidate("cached PCH disabled: cannot restrict cache directory permissions");
      return false;
    }
    warm_cache_validated_ = false;
    header_hashes_.clear();
    dependency_stamps_.clear();
    const auto suffix = unique_suffix();
    const auto temp_pch = cache_dir / (key + "." + suffix + ".tmp.pch");
    const auto temp_manifest = cache_dir / (key + "." + suffix + ".tmp.manifest");
    auto invocation = std::make_shared<clang::CompilerInvocation>(compiler.getInvocation());
    auto& frontend = invocation->getFrontendOpts();
    frontend.Inputs.clear();
    frontend.Inputs.emplace_back(virtual_source_path_, clang::InputKind(clang::Language::CXX));
    frontend.ProgramAction = clang::frontend::GeneratePCH;
    frontend.OutputFile = temp_pch.string();
    auto& preprocessor = invocation->getPreprocessorOpts();
    preprocessor.ImplicitPCHInclude.clear();
    preprocessor.clearRemappedFiles();
    preprocessor.addRemappedFile(
        virtual_source_path_,
        llvm::MemoryBuffer::getMemBufferCopy(source, virtual_source_path_).release());
    clang::CompilerInstance builder(invocation);
    clang::IgnoringDiagConsumer diagnostics;
#if LLVM_VERSION_MAJOR >= 22
    builder.createDiagnostics(&diagnostics, false);
#else
    auto vfs = llvm::vfs::getRealFileSystem();
    builder.createDiagnostics(*vfs, &diagnostics, false);
#endif
    EditorPchAction action;
    const bool built = builder.ExecuteAction(action);
    if (!built || !fs::is_regular_file(temp_pch, fs_error) || fs_error) {
      fs::remove(temp_pch, fs_error);
      headers_.clear();
      invalidate("cached PCH disabled: Clang could not build the prefix PCH");
      return false;
    }
    auto dependencies = loaded_file_hashes(builder.getSourceManager());
    dependencies.erase(virtual_source_path_);
    headers_.clear();
    for (const auto& [path, digest] : dependencies) {
      headers_.insert(path);
    }
    if (dependencies.size() != observed_hashes_.size()) {
      std::string mismatch_path;
      for (const auto& [path, digest] : dependencies) {
        if (!observed_hashes_.count(path)) {
          mismatch_path = path;
          break;
        }
      }
      if (mismatch_path.empty() && !observed_hashes_.empty()) {
        mismatch_path = observed_hashes_.begin()->first;
      }
      fs::remove(temp_pch, fs_error);
      headers_.clear();
      invalidate("cached PCH disabled: dependency set differs from parsed session headers: " +
                 mismatch_path);
      return false;
    }
    for (const auto& [path, digest] : observed_hashes_) {
      const auto found = dependencies.find(path);
      if (found == dependencies.end() || found->second != digest) {
        fs::remove(temp_pch, fs_error);
        headers_.clear();
        invalidate(
            "cached PCH disabled: prefix PCH dependencies differ from parsed session headers: " +
            path);
        return false;
      }
    }
    header_hashes_.clear();
    std::ofstream manifest(temp_manifest, std::ios::binary | std::ios::trunc);
    const auto pch_hash = sha256(read_file(temp_pch));
    manifest << "pch\t" << pch_hash << '\n';
    for (const auto& [header, parsed_hash] : dependencies) {
      bool readable = false;
      const auto contents = read_file(header, &readable);
      if (!readable) {
        manifest.close();
        fs::remove(temp_pch, fs_error);
        fs::remove(temp_manifest, fs_error);
        headers_.clear();
        invalidate("cached PCH disabled: a dependency could not be read for validation");
        return false;
      }
      const auto digest = sha256(contents);
      if (digest != parsed_hash) {
        manifest.close();
        fs::remove(temp_pch, fs_error);
        fs::remove(temp_manifest, fs_error);
        headers_.clear();
        invalidate("cached PCH disabled: a dependency changed while it was being profiled: " +
                   header);
        return false;
      }
      manifest << header << '\t' << digest << '\n';
      header_hashes_[header] = digest;
      dependency_stamps_[header] = file_stamp(header);
    }
    manifest.close();
    if (!manifest) {
      fs::remove(temp_pch, fs_error);
      fs::remove(temp_manifest, fs_error);
      headers_.clear();
      invalidate("cached PCH disabled: could not write dependency validation data");
      return false;
    }
    std::error_code rename_error;
    fs::rename(temp_manifest, manifest_path, rename_error);
    if (!rename_error) {
      fs::rename(temp_pch, pch_path, rename_error);
    }
    if (rename_error) {
      fs::remove(temp_pch, fs_error);
      fs::remove(temp_manifest, fs_error);
      headers_.clear();
      invalidate("cached PCH disabled: could not atomically publish the PCH and manifest");
      return false;
    }
    bool pch_readable = false;
    pch_hash_ = sha256(read_file(pch_path, &pch_readable));
    if (!pch_readable) {
      invalidate("cached PCH disabled: published PCH cannot be read");
      return false;
    }
    pch_stamp_ = file_stamp(pch_path.string());
    warm_cache_validated_ = true;
  } else {
    headers_.clear();
    for (const auto& [path, digest] : cached_dependencies) {
      headers_.insert(path);
      header_hashes_[path] = digest;
      dependency_stamps_[path] = file_stamp(path);
    }
    for (const auto& [path, digest] : observed_hashes_) {
      const auto found = cached_dependencies.find(path);
      if (found == cached_dependencies.end() || found->second != digest) {
        headers_.clear();
        invalidate("cached PCH disabled: cached dependencies differ from parsed session header: " +
                   path);
        return false;
      }
    }
  }

  auto& preprocessor = compiler.getInvocation().getPreprocessorOpts();
  preprocessor.ImplicitPCHInclude = pch_path.string();
  preprocessor.addRemappedFile(
      virtual_source_path_,
      llvm::MemoryBuffer::getMemBufferCopy(source_, virtual_source_path_).release());
  cache_path_ = pch_path.string();
  active_ = true;
  status_ = "cached startup PCH is active";
  return true;
}

const std::set<std::string>& EditorPchCache::headers() const noexcept {
  return headers_;
}

const EditorPchCache::MacroSnapshot& EditorPchCache::macro_snapshot() const noexcept {
  return macro_snapshot_;
}

const EditorPchCache::DeclSnapshot& EditorPchCache::decl_snapshot() const noexcept {
  return decl_snapshot_;
}

bool EditorPchCache::active() const noexcept {
  return active_;
}

const std::string& EditorPchCache::cache_path() const noexcept {
  return cache_path_;
}

const std::string& EditorPchCache::status() const noexcept {
  return status_;
}

} // namespace example
