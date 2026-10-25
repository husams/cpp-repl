#pragma once

#include <string>
#include <utility>
#include <vector>

namespace example {

struct LibraryBinaryInfo {
  std::vector<std::string> dependencies;
  std::vector<std::pair<std::string, std::string>> search_paths;
};

// Read loader metadata without changing the file or loading its code.
LibraryBinaryInfo library_binary_info(const std::string& path);
std::string executable_path();

} // namespace example
