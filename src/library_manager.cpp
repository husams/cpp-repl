#include "library_manager.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace example {

namespace fs = std::filesystem;

namespace {

// <name>, then lib<name>.<ext> for the platform's extensions (no duplicates).
std::vector<std::string> name_forms(const std::string& name) {
  const std::string base = name.rfind("lib", 0) == 0 ? name : "lib" + name;
#ifdef __APPLE__
  const char* const exts[] = {".dylib", ".so"};
#else
  const char* const exts[] = {".so", ".dylib"};
#endif
  std::vector<std::string> forms{name};
  for (const char* ext : exts) {
    std::string form = base + ext;
    if (std::find(forms.begin(), forms.end(), form) == forms.end()) {
      forms.push_back(std::move(form));
    }
  }
  return forms;
}

bool is_direct(std::string_view arg) {
  return arg.find('/') != std::string_view::npos;
}

}  // namespace

std::string expand_home(std::string_view path) {
  if (path == "~" || path.rfind("~/", 0) == 0) {
    if (const char* home = std::getenv("HOME")) {
      return std::string(home) + std::string(path.substr(1));
    }
  }
  return std::string(path);
}

LibraryManager::~LibraryManager() {
  for (const Loaded& lib : loaded_) dlclose(lib.handle);
}

LibraryManager::Result LibraryManager::add_path(std::string_view dir) {
  std::error_code ec;
  const fs::path canonical = fs::canonical(expand_home(dir), ec);
  if (ec) return {false, "no such directory: " + std::string(dir)};
  if (!fs::is_directory(canonical, ec)) {
    return {false, "not a directory: " + std::string(dir)};
  }
  const std::string text = canonical.string();
  if (std::find(paths_.begin(), paths_.end(), text) != paths_.end()) {
    return {true, "already in search path: " + text};
  }
  paths_.push_back(text);
  return {true, "added: " + text};
}

std::vector<std::string> LibraryManager::candidates(
    std::string_view name_or_path) const {
  if (is_direct(name_or_path)) return {expand_home(name_or_path)};

  const std::vector<std::string> forms = name_forms(std::string(name_or_path));
  std::vector<std::string> out;
  for (const std::string& dir : paths_) {
    for (const std::string& form : forms) out.push_back(dir + "/" + form);
  }
  out.insert(out.end(), forms.begin(), forms.end());
  return out;
}

LibraryManager::Result LibraryManager::load(std::string_view name_or_path) {
  const std::vector<std::string> tries = candidates(name_or_path);
  // Index of the first system-lookup attempt (plain name), whose error is the
  // most meaningful one when nothing exists on disk.
  const std::size_t plain_index =
      is_direct(name_or_path)
          ? 0
          : tries.size() - name_forms(std::string(name_or_path)).size();

  std::string on_disk_error;
  std::string plain_error;
  for (std::size_t i = 0; i < tries.size(); ++i) {
    void* handle = dlopen(tries[i].c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle != nullptr) {
      for (const Loaded& lib : loaded_) {
        if (lib.handle == handle) {
          dlclose(handle);  // drop the extra reference
          return {true, "already loaded: " + lib.path};
        }
      }
      if (hook_) {
        std::string error = hook_(tries[i]);
        if (!error.empty()) {
          dlclose(handle);
          return {false, std::move(error)};
        }
      }
      loaded_.push_back({tries[i], handle});
      return {true, "loaded: " + tries[i]};
    }
    const char* raw = dlerror();
    std::string error = raw != nullptr ? raw : "dlopen failed";
    std::error_code ec;
    if (on_disk_error.empty() && fs::exists(tries[i], ec)) on_disk_error = error;
    if (i == plain_index) plain_error = std::move(error);
  }
  return {false, on_disk_error.empty() ? plain_error : on_disk_error};
}

}  // namespace example
