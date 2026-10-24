#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpp_symbols.hpp"

namespace example {

struct LoadedLibrary {
  std::string name;
  std::string path;
  std::string version;
  std::uintptr_t base = 0;
  std::size_t size = 0;
  std::vector<std::string> dependencies;
};

std::vector<LoadedLibrary> loaded_libraries();
std::string library_list(const std::string& pattern, std::string& error);
std::string library_info(const std::string& query, std::string& error);
std::vector<SymbolDescription> native_symbol_details(const std::string& query);

} // namespace example
