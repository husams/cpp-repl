#pragma once

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace example {

struct LoadedSymbol {
  std::string library;
  std::string kind;
  std::string name;
  std::string mangled;
  std::uintptr_t address = 0;
};

std::string readable_symbol_name(const std::string& mangled);
bool is_system_library(const std::string& path);

// Enumerate defined exports of images currently loaded in the process. Filters
// use regex_search; library filtering happens before symbol enumeration.
void native_loaded_symbols(const std::regex& library, const std::regex& name,
                           std::vector<LoadedSymbol>& result,
                           bool include_system = true);
// Unresolved imports only; imports bound to existing storage are omitted.
void native_undefined_symbols(const std::regex& library, const std::regex& name,
                              std::vector<LoadedSymbol>& result,
                              bool include_system = false);
std::string format_loaded_symbols(std::vector<LoadedSymbol> symbols);

}  // namespace example
