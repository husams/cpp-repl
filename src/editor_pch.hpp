#ifndef CPP_REPL_EDITOR_PCH_HPP
#define CPP_REPL_EDITOR_PCH_HPP

#include <map>
#include <set>
#include <string>
#include <vector>

namespace clang {
class CompilerInstance;
class Decl;
class IdentifierInfo;
class MacroDirective;
class NamespaceDecl;
class SourceManager;
class TranslationUnitDecl;
}

namespace example {

class EditorPchCache {
public:
  using MacroSnapshot =
      std::map<const clang::IdentifierInfo*, const clang::MacroDirective*>;
  using DeclSnapshot = std::set<const clang::Decl*>;

  void observe_input(const std::string& code, const std::vector<std::string>& args, bool success,
                     clang::CompilerInstance* session = nullptr);
  bool prepare(clang::CompilerInstance& compiler, const std::vector<std::string>& args);
  void invalidate(const std::string& reason);

  const std::set<std::string>& headers() const noexcept;
  const MacroSnapshot& macro_snapshot() const noexcept;
  const DeclSnapshot& decl_snapshot() const noexcept;
  bool active() const noexcept;
  const std::string& cache_path() const noexcept;
  const std::string& status() const noexcept;

private:
  std::vector<std::string> prefix_;
  std::vector<std::string> captured_args_;
  std::string captured_cwd_;
  std::string cache_path_;
  std::string status_;
  std::set<std::string> headers_;
  MacroSnapshot macro_snapshot_;
  DeclSnapshot decl_snapshot_;
  std::set<const clang::TranslationUnitDecl*> captured_translation_units_;
  std::set<const clang::NamespaceDecl*> captured_namespace_declarations_;
  std::map<std::pair<const clang::SourceManager*, unsigned>, bool> prefix_decl_files_;
  std::map<std::string, std::string> header_hashes_;
  std::map<std::string, std::string> observed_hashes_;
  std::map<std::string, std::string> dependency_stamps_;
  std::string source_;
  std::string virtual_source_path_;
  std::string pch_hash_;
  std::string pch_stamp_;
  bool observing_ = true;
  bool eligible_ = true;
  bool active_ = false;
  bool capture_started_ = false;
  bool warm_cache_validated_ = false;
};

} // namespace example

#endif
