#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace example {

// Replaces a leading "~" or "~/" with $HOME (if set).
std::string expand_home(std::string_view path);

// Owns the library search-path list and the shared libraries loaded with
// dlopen for the lifetime of the object.
class LibraryManager {
 public:
  struct Result {
    bool ok;
    std::string message;
  };
  struct Loaded {
    std::string path;  // the candidate that dlopen accepted
    void* handle;
  };

  LibraryManager() = default;
  ~LibraryManager();
  LibraryManager(const LibraryManager&) = delete;
  LibraryManager& operator=(const LibraryManager&) = delete;

  // Called with the path of each newly loaded library; returns an error
  // message to reject it (the library is closed again), or "" to accept it.
  // The REPL uses it to register the library with the C++ interpreter.
  using LoadHook = std::function<std::string(const std::string& path)>;
  void set_load_hook(LoadHook hook) { hook_ = std::move(hook); }

  // Adds an existing directory to the search path.
  Result add_path(std::string_view dir);

  // Loads a library by path (contains '/') or by name; see candidates().
  Result load(std::string_view name_or_path);

  // Names/paths load() tries, in order. An argument containing '/' is opened
  // directly; otherwise each search dir is tried, then the plain name (system
  // lookup), each as <name>, lib<name>.dylib and lib<name>.so.
  std::vector<std::string> candidates(std::string_view name_or_path) const;

  const std::vector<std::string>& search_paths() const { return paths_; }
  const std::vector<Loaded>& loaded() const { return loaded_; }

 private:
  std::vector<std::string> paths_;
  std::vector<Loaded> loaded_;
  LoadHook hook_;
};

}  // namespace example
