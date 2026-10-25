#include "source_reload.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "definition_shadowing.hpp"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/Support/MemoryBuffer.h"

namespace example {
struct SourceReload::Impl {
  using Files = std::map<std::string, std::string>;
  clang::CompilerInstance &compiler;
  std::map<std::string, Files> sources;
  std::map<std::string, Files> failed_sources;
  std::string root;
  Files pending;
  std::vector<std::string> refreshing;
  // Macro tokens and old AST nodes retain pointers into earlier source text.
  // Keep every generation alive, and lend non-owning views to SourceManager.
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> buffers;
  std::set<std::string> prepared_files;
  struct Backup {
    clang::FileEntryRef file;
    llvm::MemoryBufferRef contents;
    clang::HeaderFileInfo header;
    bool included;
  };
  std::vector<Backup> backups;
  std::map<clang::IdentifierInfo *, clang::MacroInfo *> macros;
  bool modified = false;
  bool reloading = false;

  explicit Impl(clang::CompilerInstance &compiler) : compiler(compiler) {}

  void install(clang::FileEntryRef file,
               std::unique_ptr<llvm::MemoryBuffer> buffer) {
    auto &manager = compiler.getSourceManager();
    manager.overrideFileContents(file, buffer->getMemBufferRef());
    const auto id = manager.translateFile(file);
    if (id.isValid())
      manager.getSLocEntry(id).getFile().getContentCache().SourceLineCache = {};
    buffers.push_back(std::move(buffer));
    prepared_files.insert(file.getName().str());
  }

  void record(clang::FileID file_id) {
    if (root.empty() || file_id.isInvalid())
      return;
    auto &manager = compiler.getSourceManager();
    auto location = manager.getLocForStartOfFile(file_id);
    if (manager.isInSystemHeader(location))
      return;
    auto file = manager.getFileEntryRefForID(file_id);
    if (!file || file->getName().starts_with("input_line_") ||
        file->getName().starts_with("<"))
      return;
    pending[file->getName().str()] = manager.getBufferData(file_id).str();
  }

  void undefine(clang::IdentifierInfo *identifier) {
    auto &pp = compiler.getPreprocessor();
    // Macro directives have the preprocessor allocator's lifetime.
    auto *memory =
        pp.getPreprocessorAllocator().Allocate<clang::UndefMacroDirective>();
    auto *directive = new (memory) clang::UndefMacroDirective({});
    pp.appendMacroDirective(identifier, directive);
  }

  std::map<clang::IdentifierInfo *, clang::MacroInfo *>
  active_macros(bool files_only = false) {
    std::map<clang::IdentifierInfo *, clang::MacroInfo *> found;
    auto &pp = compiler.getPreprocessor();
    auto &manager = compiler.getSourceManager();
    for (const auto &entry : pp.macros(false)) {
      auto *identifier = pp.getIdentifierInfo(entry.first->getName());
      auto *macro = pp.getMacroInfo(identifier);
      if (!macro)
        continue;
      auto filename = manager.getFilename(macro->getDefinitionLoc()).str();
      if (!files_only || std::find(refreshing.begin(), refreshing.end(),
                                   filename) != refreshing.end())
        found.emplace(identifier, macro);
    }
    return found;
  }

  class Callbacks : public clang::PPCallbacks {
    Impl &state;

  public:
    explicit Callbacks(Impl &state) : state(state) {}
    void InclusionDirective(clang::SourceLocation, const clang::Token &,
                            llvm::StringRef, bool, clang::CharSourceRange,
                            clang::OptionalFileEntryRef file, llvm::StringRef,
                            llvm::StringRef, const clang::Module *, bool,
                            clang::SrcMgr::CharacteristicKind kind) override {
      if (!file || clang::SrcMgr::isSystem(kind))
        return;
      auto &manager = state.compiler.getSourceManager();
      const bool previously_loaded = manager.translateFile(*file).isValid();
      if (!state.prepared_files.count(file->getName().str())) {
        auto buffer = llvm::MemoryBuffer::getFile(file->getName());
        if (buffer)
          state.install(*file, std::move(*buffer));
      }
      if (state.root.empty())
        return;
      if (std::none_of(
              state.backups.begin(), state.backups.end(),
              [&](const auto &backup) { return backup.file == *file; })) {
        auto &pp = state.compiler.getPreprocessor();
        auto header = pp.getHeaderSearchInfo().getFileInfo(*file);
        // InclusionDirective follows markIncluded, but precedes the file's
        // lexer.
        if (!previously_loaded)
          header.IsLocallyIncluded = false;
        state.backups.push_back(
            {*file, manager.getMemoryBufferForFileOrFake(*file), header,
             previously_loaded && pp.alreadyIncluded(*file)});
      }
      if (state.reloading &&
          std::find(state.refreshing.begin(), state.refreshing.end(),
                    file->getName()) == state.refreshing.end()) {
        state.refreshing.push_back(file->getName().str());
        version_source_files(state.compiler, state.refreshing);
      }
    }
    void FileChanged(clang::SourceLocation location, FileChangeReason reason,
                     clang::SrcMgr::CharacteristicKind,
                     clang::FileID) override {
      if (reason == EnterFile)
        state.record(state.compiler.getSourceManager().getFileID(location));
    }
    void FileSkipped(const clang::FileEntryRef &file, const clang::Token &,
                     clang::SrcMgr::CharacteristicKind) override {
      state.record(state.compiler.getSourceManager().translateFile(file));
    }
  };
};

SourceReload::SourceReload(clang::CompilerInstance &compiler)
    : impl_(std::make_unique<Impl>(compiler)) {
  compiler.getPreprocessor().addPPCallbacks(
      std::make_unique<Impl::Callbacks>(*impl_));
}
SourceReload::~SourceReload() = default;

bool SourceReload::begin(const std::string &root, bool reload,
                         std::string &error) {
  auto &state = *impl_;
  state.root = root;
  state.pending.clear();
  if (root.empty())
    return true;
  state.macros = state.active_macros();
  state.modified = true;
  const auto &previous_sources = reload ? state.sources : state.failed_sources;
  auto previous = previous_sources.find(root);
  if (previous == previous_sources.end() && !reload)
    return true;
  if (previous == previous_sources.end()) {
    error = "source has not been added: " + root;
    return false;
  }
  state.reloading = true;
  auto &compiler = state.compiler;
  auto &manager = compiler.getSourceManager();
  auto &pp = compiler.getPreprocessor();
  std::vector<
      std::pair<clang::FileEntryRef, std::unique_ptr<llvm::MemoryBuffer>>>
      fresh;
  // Validate all dependencies before changing either lookup or preprocessing.
  for (const auto &[path, contents] : previous->second) {
    auto file = compiler.getFileManager().getFileRef(path);
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!file || !buffer) {
      if (!file)
        llvm::consumeError(file.takeError());
      error = "cannot reread source dependency: " + path;
      return false;
    }
    state.refreshing.push_back(path);
    state.backups.push_back({*file, manager.getMemoryBufferForFileOrFake(*file),
                             pp.getHeaderSearchInfo().getFileInfo(*file),
                             pp.alreadyIncluded(*file)});
    fresh.emplace_back(*file, std::move(*buffer));
  }
  for (auto entry : state.active_macros(true))
    state.undefine(entry.first);
  for (auto &entry : fresh) {
    auto &header = pp.getHeaderSearchInfo().getFileInfo(entry.first);
    header.isPragmaOnce = false;
    header.isImport = false;
    pp.getHeaderSearchInfo().SetFileControllingMacro(entry.first, nullptr);
    pp.getIncludedFiles().erase(entry.first);
    state.install(entry.first, std::move(entry.second));
  }
  hide_source_definitions(compiler, state.refreshing);
  version_source_files(compiler, state.refreshing);
  return true;
}

void SourceReload::finish(bool committed) {
  auto &state = *impl_;
  auto &compiler = state.compiler;
  auto &pp = compiler.getPreprocessor();
  if (!committed && state.modified) {
    for (auto entry : state.active_macros()) {
      auto previous = state.macros.find(entry.first);
      if (previous == state.macros.end() || previous->second != entry.second)
        state.undefine(entry.first);
    }
    for (auto entry : state.macros)
      if (pp.getMacroInfo(entry.first) != entry.second)
        pp.appendDefMacroDirective(entry.first, entry.second);
    for (auto &backup : state.backups) {
      auto &manager = compiler.getSourceManager();
      manager.overrideFileContents(backup.file, backup.contents);
      const auto id = manager.translateFile(backup.file);
      if (id.isValid())
        manager.getSLocEntry(id).getFile().getContentCache().SourceLineCache =
            {};
      pp.getHeaderSearchInfo().getFileInfo(backup.file) = backup.header;
      if (backup.included)
        pp.getIncludedFiles().insert(backup.file);
      else
        pp.getIncludedFiles().erase(backup.file);
    }
  }
  if (!state.root.empty()) {
    if (committed) {
      state.sources[state.root] = state.pending;
      state.failed_sources.erase(state.root);
    } else if (!state.pending.empty())
      state.failed_sources[state.root] = state.pending;
  }
  version_source_files(compiler, {});
  state.root.clear();
  state.pending.clear();
  state.refreshing.clear();
  state.macros.clear();
  state.backups.clear();
  state.modified = false;
  state.reloading = false;
}

std::vector<std::string> SourceReload::changed_sources() const {
  std::vector<std::string> result;
  for (const auto &[root, files] : impl_->sources) {
    for (const auto &[path, contents] : files) {
      auto current = llvm::MemoryBuffer::getFile(path);
      if (!current || (*current)->getBuffer() != contents) {
        result.push_back(root);
        break;
      }
    }
  }
  return result;
}
} // namespace example
