#include "library_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

#include <dlfcn.h>

#include "library_dependencies.hpp"

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

void close_handles(std::vector<void*>& handles, std::size_t keep = 0) {
  while (handles.size() > keep) {
    dlclose(handles.back());
    handles.pop_back();
  }
}

void* load_with_dependencies(const std::string& path, const std::vector<std::string>& search,
                             std::set<std::string>& visiting, std::vector<void*>& handles,
                             std::string& error) {
  if (void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
    handles.push_back(handle);
    return handle;
  }
  const auto* message = dlerror();
  error = message ? message : "dlopen failed";
  const auto original_error = error;
  std::error_code ec;
  if (!is_direct(path) || !fs::is_regular_file(path, ec)) {
    return nullptr;
  }
  const auto canonical = fs::canonical(path, ec);
  if (ec) {
    return nullptr;
  }
  if (!visiting.insert(canonical.string()).second) {
    error = "cyclic dependency while resolving " + canonical.string();
    return nullptr;
  }
  const auto checkpoint = handles.size();
  const auto parent = canonical.parent_path();
  std::vector<fs::path> directories{parent};
  for (const auto& directory : search) {
    directories.emplace_back(directory);
  }
  bool ready = true;
  for (const auto& needed : library_binary_info(canonical.string()).dependencies) {
    std::string dependency_error;
    if (load_with_dependencies(needed, search, visiting, handles, dependency_error)) {
      continue;
    }
    std::string relative = needed;
    for (const auto* token : {"@rpath/", "@loader_path/", "$ORIGIN/", "${ORIGIN}/"}) {
      if (relative.rfind(token, 0) == 0) {
        relative.erase(0, std::string(token).size());
        break;
      }
    }
    std::set<std::string> attempted;
    bool found = false;
    for (const auto& directory : directories) {
      for (const auto& suffix : {fs::path(relative), fs::path(needed).filename()}) {
        if (suffix.is_absolute()) {
          continue;
        }
        const auto candidate = (directory / suffix).lexically_normal().string();
        if (!attempted.insert(candidate).second || !fs::is_regular_file(candidate, ec)) {
          continue;
        }
        if (load_with_dependencies(candidate, search, visiting, handles, dependency_error)) {
          found = true;
          break;
        }
      }
      if (found) {
        break;
      }
    }
    if (!found) {
      error = original_error + "\nwhile resolving " + needed + ": " + dependency_error;
      ready = false;
      break;
    }
  }
  visiting.erase(canonical.string());
  if (ready) {
    if (void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
      handles.push_back(handle);
      return handle;
    }
    const auto* message = dlerror();
    error = message ? message : original_error;
  }
  close_handles(handles, checkpoint);
  return nullptr;
}

} // namespace

std::string expand_home(std::string_view path) {
  if (path == "~" || path.rfind("~/", 0) == 0) {
    if (const char* home = std::getenv("HOME")) {
      return std::string(home) + std::string(path.substr(1));
    }
  }
  return std::string(path);
}

LibraryManager::~LibraryManager() {
  for (auto lib = loaded_.rbegin(); lib != loaded_.rend(); ++lib) {
    dlclose(lib->handle);
  }
  close_handles(dependency_handles_);
}

LibraryManager::Result LibraryManager::add_path(std::string_view dir) {
  std::error_code ec;
  const fs::path canonical = fs::canonical(expand_home(dir), ec);
  if (ec) {
    return {false, "no such directory: " + std::string(dir)};
  }
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

std::vector<std::string> LibraryManager::candidates(std::string_view name_or_path) const {
  if (is_direct(name_or_path)) {
    return {expand_home(name_or_path)};
  }

  const std::vector<std::string> forms = name_forms(std::string(name_or_path));
  std::vector<std::string> out;
  for (const std::string& dir : paths_) {
    for (const std::string& form : forms) {
      out.push_back(dir + "/" + form);
    }
  }
  out.insert(out.end(), forms.begin(), forms.end());
  return out;
}

std::string LibraryManager::describe_search_paths() const {
  std::vector<std::pair<std::string, std::string>> fields;
  if (paths_.empty()) {
    fields.emplace_back("Added", "none (%add-path adds directories)");
  }
  for (const auto& path : paths_) {
    fields.emplace_back("Added", path);
  }
  fields.emplace_back("Dependency fallback", "requesting library's directory, then Added paths");
  const auto executable = executable_path();
  const auto exe_directory = fs::path(executable).parent_path().string();
  auto rpaths = [&](const std::string& binary, const std::string& label) {
    for (const auto& [kind, raw] : library_binary_info(binary).search_paths) {
      std::size_t begin = 0;
      do {
#ifdef __APPLE__
        const auto end = std::string::npos;
#else
        const auto end = raw.find(':', begin);
#endif
        auto value = raw.substr(begin, end == std::string::npos ? end : end - begin);
        for (const auto& [token, directory] : std::vector<std::pair<std::string, std::string>>{
                 {"@loader_path", fs::path(binary).parent_path().string()},
                 {"@executable_path", exe_directory},
                 {"${ORIGIN}", fs::path(binary).parent_path().string()},
                 {"$ORIGIN", fs::path(binary).parent_path().string()}}) {
          if (value.rfind(token, 0) == 0) {
            value.replace(0, token.size(), directory);
            break;
          }
        }
        fields.emplace_back(label + " " + kind, value.empty() ? "(current directory)" : value);
        if (end == std::string::npos)
          break;
        begin = end + 1;
      } while (begin <= raw.size());
    }
  };
  rpaths(executable, "Executable");
  for (const auto& library : loaded_) {
    rpaths(library.path, fs::path(library.path).filename().string());
  }
#ifdef __APPLE__
  const char* const environment[] = {"DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH",
                                     "DYLD_FRAMEWORK_PATH"};
  fields.emplace_back("Native loader", "dyld paths and system defaults (loader-managed)");
#else
  const char* const environment[] = {"LD_LIBRARY_PATH"};
  fields.emplace_back("Native loader",
                      "ELF RPATH/RUNPATH, ld.so cache and system defaults (loader-managed)");
#endif
  for (const auto* name : environment) {
    const auto* value = std::getenv(name);
    fields.emplace_back(name, value ? (*value ? value : "(empty)") : "(unset)");
  }
  std::size_t width = 0;
  for (const auto& field : fields) {
    width = std::max(width, field.first.size());
  }
  std::ostringstream out;
  out << "Library search paths:\n";
  for (const auto& [label, value] : fields) {
    out << "  " << std::left << std::setw(static_cast<int>(width)) << label << " : " << value
        << '\n';
  }
  return out.str();
}

LibraryManager::Result LibraryManager::load(std::string_view name_or_path) {
  const std::vector<std::string> tries = candidates(name_or_path);
  // Index of the first system-lookup attempt (plain name), whose error is the
  // most meaningful one when nothing exists on disk.
  const std::size_t plain_index =
      is_direct(name_or_path) ? 0 : tries.size() - name_forms(std::string(name_or_path)).size();

  std::string on_disk_error;
  std::string plain_error;
  for (std::size_t i = 0; i < tries.size(); ++i) {
    std::vector<void*> handles;
    std::set<std::string> visiting;
    std::string error;
    void* handle = load_with_dependencies(tries[i], paths_, visiting, handles, error);
    if (handle != nullptr) {
      for (const Loaded& lib : loaded_) {
        if (lib.handle == handle) {
          close_handles(handles); // drop the extra references
          return {true, "already loaded: " + lib.path};
        }
      }
      if (hook_) {
        std::string error = hook_(tries[i]);
        if (!error.empty()) {
          close_handles(handles);
          return {false, std::move(error)};
        }
      }
      loaded_.push_back({tries[i], handle});
      handles.pop_back(); // the root handle is owned by loaded_
      dependency_handles_.insert(dependency_handles_.end(), handles.begin(), handles.end());
      return {true, "loaded: " + tries[i]};
    }
    std::error_code ec;
    if (on_disk_error.empty() && fs::exists(tries[i], ec)) {
      on_disk_error = error;
    }
    if (i == plain_index) {
      plain_error = std::move(error);
    }
  }
  return {false, on_disk_error.empty() ? plain_error : on_disk_error};
}

} // namespace example
