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
  std::map<std::string, Files> failed_skipped_sources;
  std::string root;
  Files pending;
  Files skipped;
  // FileID generations keep the include graph corresponding to each parse.
  std::map<clang::FileID, std::set<std::string>> includes;
  std::map<std::string, clang::FileID> loaded_files;
  std::map<std::string, std::optional<clang::FileID>> loaded_backups;
  std::set<std::string> unreadable;
  std::set<std::string> hidden_files;
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
  std::map<clang::IdentifierInfo *, const clang::MacroDirective *> refresh_undefs;
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

  void record(clang::FileID file_id, bool skipped_file = false) {
    if (file_id.isInvalid())
      return;
    auto &manager = compiler.getSourceManager();
    auto location = manager.getLocForStartOfFile(file_id);
    if (manager.isInSystemHeader(location))
      return;
    auto file = manager.getFileEntryRefForID(file_id);
    if (!file || file->getName().starts_with("input_line_") ||
        file->getName().starts_with("<"))
      return;
    const auto path = file->getName().str();
    if (!skipped_file) {
      if (!root.empty() && !loaded_backups.count(path)) {
        auto previous = loaded_files.find(path);
        loaded_backups[path] = previous == loaded_files.end()
            ? std::optional<clang::FileID>() : previous->second;
      }
      loaded_files[path] = file_id;
      if (reloading && hidden_files.insert(path).second)
        hide_source_definitions(compiler, {path});
    }
    if (root.empty())
      return;
    auto &files = skipped_file ? skipped : pending;
    files[path] = manager.getBufferData(file_id).str();
  }

  void record_skipped(const std::string &path, std::set<std::string> &visited) {
    if (root.empty())
      return;
    if (!visited.insert(path).second)
      return;
    auto loaded = loaded_files.find(path);
    if (loaded == loaded_files.end())
      return;
    record(loaded->second, true);
    for (const auto &child : includes[loaded->second])
      record_skipped(child, visited);
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
    void InclusionDirective(clang::SourceLocation location, const clang::Token &,
                            llvm::StringRef, bool, clang::CharSourceRange,
                            clang::OptionalFileEntryRef file, llvm::StringRef,
                            llvm::StringRef, const clang::Module *, bool,
                            clang::SrcMgr::CharacteristicKind kind) override {
      if (!file || clang::SrcMgr::isSystem(kind))
        return;
      auto &manager = state.compiler.getSourceManager();
      state.includes[manager.getFileID(location)].insert(file->getName().str());
      if (state.unreadable.count(file->getName().str())) {
        auto &diagnostics = state.compiler.getDiagnostics();
        diagnostics.Report(location, diagnostics.getCustomDiagID(
            clang::DiagnosticsEngine::Error, "cannot reread source dependency: %0"))
            << file->getName();
        return;
      }
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
      std::set<std::string> visited;
      state.record_skipped(file.getName().str(), visited);
    }
  };
};

SourceReload::SourceReload(clang::CompilerInstance &compiler)
    : impl_(std::make_unique<Impl>(compiler)) {
  // Startup -include inputs may have been parsed before these callbacks exist.
  auto &manager = compiler.getSourceManager();
  for (auto file = manager.fileinfo_begin(); file != manager.fileinfo_end(); ++file)
    impl_->record(manager.translateFile(file->first));
  for (const auto &[path, file] : impl_->loaded_files) {
    auto location = manager.getIncludeLoc(file);
    if (location.isValid())
      impl_->includes[manager.getFileID(location)].insert(path);
  }
  compiler.getPreprocessor().addPPCallbacks(
      std::make_unique<Impl::Callbacks>(*impl_));
}
SourceReload::~SourceReload() = default;

bool SourceReload::begin(const std::string &root, bool reload,
                         std::string &error) {
  auto &state = *impl_;
  state.root = root;
  state.pending.clear();
  state.skipped.clear();
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
  const auto prepare = [&](const std::string &path,
                           const std::string *unchanged = nullptr) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (buffer && unchanged && (*buffer)->getBuffer() == *unchanged)
      return true;
    auto file = compiler.getFileManager().getFileRef(path);
    if (!file || !buffer) {
      if (!file)
        llvm::consumeError(file.takeError());
      if (path == root) {
        error = "cannot reread source dependency: " + path;
        return false;
      }
      state.unreadable.insert(path);
      return true; // Diagnose only if the new source still includes this file.
    }
    state.refreshing.push_back(path);
    state.backups.push_back({*file, manager.getMemoryBufferForFileOrFake(*file),
                             pp.getHeaderSearchInfo().getFileInfo(*file),
                             pp.alreadyIncluded(*file)});
    fresh.emplace_back(*file, std::move(*buffer));
    return true;
  };
  for (const auto &[path, contents] : previous->second)
    if (!prepare(path))
      return false;
  if (!reload) {
    if (auto skipped = state.failed_skipped_sources.find(root);
        skipped != state.failed_skipped_sources.end()) {
      std::set<std::string> changed;
      for (const auto &[path, contents] : skipped->second) {
        auto buffer = llvm::MemoryBuffer::getFile(path);
        if (!buffer) {
          state.unreadable.insert(path);
          changed.insert(path); // Reopen ancestors to diagnose a still-needed leaf.
        } else if ((*buffer)->getBuffer() != contents)
          changed.insert(path);
      }
      // Reopen unchanged ancestors too, so a changed leaf is actually reached.
      std::map<std::string, std::vector<std::string>> parents;
      for (const auto &[path, contents] : skipped->second) {
        auto loaded = state.loaded_files.find(path);
        if (loaded != state.loaded_files.end())
          for (const auto &child : state.includes[loaded->second])
            parents[child].push_back(path);
      }
      std::vector<std::string> work(changed.begin(), changed.end());
      for (std::size_t i = 0; i != work.size(); ++i)
        for (const auto &parent : parents[work[i]])
          if (changed.insert(parent).second)
            work.push_back(parent);
      for (const auto &path : changed)
        if (!previous->second.count(path) && !prepare(path))
          return false;
    }
  }
  for (auto entry : state.active_macros(true)) {
    state.undefine(entry.first);
    state.refresh_undefs[entry.first] = pp.getLocalMacroDirective(entry.first);
  }
  for (auto &entry : fresh) {
    auto &header = pp.getHeaderSearchInfo().getFileInfo(entry.first);
    header.isPragmaOnce = false;
    header.isImport = false;
    pp.getHeaderSearchInfo().SetFileControllingMacro(entry.first, nullptr);
    pp.getIncludedFiles().erase(entry.first);
    state.install(entry.first, std::move(entry.second));
  }
  // Hide definitions only when FileChanged confirms the file was entered.
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
  }
  std::set<std::string> unused;
  for (auto &backup : state.backups) {
    const auto path = backup.file.getName().str();
    if (!committed || !state.pending.count(path)) {
      unused.insert(path);
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
  if (!committed) {
    for (const auto &[path, previous] : state.loaded_backups) {
      if (previous)
        state.loaded_files[path] = *previous;
      else
        state.loaded_files.erase(path);
    }
  } else {
    // Undo only our own macro suspensions for files never re-entered. Preserve
    // deliberate macro edits made by the successfully committed source.
    for (const auto &[identifier, directive] : state.refresh_undefs)
      if (pp.getLocalMacroDirective(identifier) == directive) {
        auto *macro = state.macros.at(identifier);
        if (unused.count(compiler.getSourceManager().getFilename(macro->getDefinitionLoc()).str()))
          pp.appendDefMacroDirective(identifier, macro);
      }
  }
  if (!state.root.empty()) {
    if (committed) {
      // Successful sources still track shared headers for change detection.
      // On failed-source retry, unchanged skipped headers keep their earlier
      // committed declarations; edited skipped headers must be refreshed.
      state.pending.insert(state.skipped.begin(), state.skipped.end());
      state.sources[state.root] = state.pending;
      state.failed_sources.erase(state.root);
      state.failed_skipped_sources.erase(state.root);
    } else if (!state.pending.empty()) {
      state.failed_sources[state.root] = state.pending;
      state.failed_skipped_sources[state.root] = state.skipped;
    }
  }
  version_source_files(compiler, {});
  state.root.clear();
  state.pending.clear();
  state.skipped.clear();
  state.refreshing.clear();
  state.macros.clear();
  state.backups.clear();
  state.loaded_backups.clear();
  state.unreadable.clear();
  state.hidden_files.clear();
  state.refresh_undefs.clear();
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
