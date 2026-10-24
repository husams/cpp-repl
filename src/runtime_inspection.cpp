#include "runtime_inspection.hpp"

// LLVM ELF constants must precede platform headers that define ELF macros.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Error.h"

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#include "loaded_symbols.hpp"
#include "native_call_patch.hpp"
#ifdef CPP_REPL_RUNTIME_SYMBOLS
#include "gum_runtime.hpp"
#endif

namespace example {
namespace {

using Fields = std::vector<std::pair<std::string, std::string>>;

std::string hex(std::uintptr_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

std::string join(const std::set<std::string>& values) {
  std::string result;
  for (const auto& value : values) {
    result += (result.empty() ? "" : ", ") + value;
  }
  return result.empty() ? "none recorded" : result;
}

std::string escaped(const std::string& value) {
  std::string result;
  for (const char c : value) {
    if (std::string_view(R"(\.^$|()[]{}*+?)").find(c) != std::string_view::npos) {
      result += '\\';
    }
    result += c;
  }
  return result;
}

std::string normalized(std::string name) {
  if (name.rfind("__Z", 0) == 0) {
    name.erase(0, 1);
  }
  return name;
}

[[maybe_unused]] bool matches(const std::string& query, const std::string& mangled) {
  const auto name = readable_symbol_name(mangled);
  return normalized(query) == normalized(mangled) || name == query ||
         name.rfind(query + "(", 0) == 0;
}

template <typename ELFT>
void elf_metadata(const llvm::object::ELFObjectFile<ELFT>& object, Fields& fields) {
  const auto& elf = object.getELFFile();
  auto sections = elf.sections();
  if (!sections) {
    fields.emplace_back("Metadata", llvm::toString(sections.takeError()));
    return;
  }
  std::set<std::string> glibc, glibcxx, cxxabi, other, provided;
  for (const auto& section : *sections) {
    if (section.sh_type == llvm::ELF::SHT_GNU_verneed) {
      auto needs = elf.getVersionDependencies(section);
      if (!needs) {
        fields.emplace_back("Metadata", llvm::toString(needs.takeError()));
        continue;
      }
      for (const auto& need : *needs) {
        for (const auto& aux : need.AuxV) {
          auto& group = aux.Name.rfind("GLIBCXX_", 0) == 0  ? glibcxx
                        : aux.Name.rfind("GLIBC_", 0) == 0  ? glibc
                        : aux.Name.rfind("CXXABI_", 0) == 0 ? cxxabi
                                                            : other;
          group.insert(aux.Name);
        }
      }
    } else if (section.sh_type == llvm::ELF::SHT_GNU_verdef) {
      auto definitions = elf.getVersionDefinitions(section);
      if (!definitions) {
        llvm::consumeError(definitions.takeError());
        continue;
      }
      for (const auto& definition : *definitions) {
        provided.insert(definition.Name);
      }
    } else if (section.sh_type == llvm::ELF::SHT_DYNAMIC) {
      auto strings_section = elf.getSection(section.sh_link);
      if (!strings_section) {
        llvm::consumeError(strings_section.takeError());
        continue;
      }
      auto strings = elf.getStringTable(**strings_section);
      if (!strings) {
        llvm::consumeError(strings.takeError());
        continue;
      }
      auto entries = elf.dynamicEntries();
      if (!entries) {
        llvm::consumeError(entries.takeError());
        continue;
      }
      for (const auto& entry : *entries) {
        if (entry.d_tag == llvm::ELF::DT_SONAME && entry.d_un.d_val < strings->size()) {
          const auto tail = strings->substr(entry.d_un.d_val);
          fields.emplace_back("SONAME", tail.substr(0, tail.find('\0')).str());
        }
      }
    }
  }
  fields.emplace_back("GLIBC req", join(glibc));
  fields.emplace_back("GLIBCXX req", join(glibcxx));
  fields.emplace_back("CXXABI req", join(cxxabi));
  fields.emplace_back("Other ABI", join(other));
  fields.emplace_back("ABI exports", join(provided));
}

[[maybe_unused]] std::string packed_version(std::uint32_t value) {
  return std::to_string(value >> 16) + "." + std::to_string((value >> 8) & 255) + "." +
         std::to_string(value & 255);
}

void binary_metadata(const LoadedLibrary& library, Fields& fields) {
  auto binary = llvm::object::ObjectFile::createObjectFile(library.path);
  if (binary) {
    const auto* object = binary->getBinary();
    fields.emplace_back("Format", object->getFileFormatName().str());
    fields.emplace_back("Architecture", object->makeTriple().getArchName().str());
    if (const auto* elf = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(object)) {
      elf_metadata(*elf, fields);
    } else if (const auto* elf = llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(object)) {
      elf_metadata(*elf, fields);
    } else if (const auto* elf = llvm::dyn_cast<llvm::object::ELF64BEObjectFile>(object)) {
      elf_metadata(*elf, fields);
    } else if (const auto* elf = llvm::dyn_cast<llvm::object::ELF32BEObjectFile>(object)) {
      elf_metadata(*elf, fields);
    }
  } else {
    // Shared-cache images may have no separate file on macOS.
    fields.emplace_back("File data", "unavailable: " + llvm::toString(binary.takeError()));
  }
#if defined(__APPLE__) && defined(CPP_REPL_RUNTIME_SYMBOLS)
  // Inspect loaded Mach-O metadata as well, including dyld shared-cache images.
  const auto* header = reinterpret_cast<const llvm::MachO::mach_header_64*>(library.base);
  if (!gum_memory_is_readable(header, sizeof(*header)) ||
      header->magic != llvm::MachO::MH_MAGIC_64) {
    return;
  }
  auto* data = reinterpret_cast<const unsigned char*>(header + 1);
  std::size_t remaining = header->sizeofcmds;
  if (remaining > (1u << 24) || !gum_memory_is_readable(data, remaining)) {
    return;
  }
  for (unsigned i = 0; i < header->ncmds && remaining >= sizeof(llvm::MachO::load_command); ++i) {
    const auto* command = reinterpret_cast<const llvm::MachO::load_command*>(data);
    if (command->cmdsize < sizeof(*command) || command->cmdsize > remaining) {
      break;
    }
    if (command->cmd == llvm::MachO::LC_ID_DYLIB &&
        command->cmdsize >= sizeof(llvm::MachO::dylib_command)) {
      const auto* dylib = reinterpret_cast<const llvm::MachO::dylib_command*>(data);
      fields.emplace_back("Current", packed_version(dylib->dylib.current_version));
      fields.emplace_back("Compatible", packed_version(dylib->dylib.compatibility_version));
      if (dylib->dylib.name < command->cmdsize) {
        llvm::StringRef name(reinterpret_cast<const char*>(data) + dylib->dylib.name,
                             command->cmdsize - dylib->dylib.name);
        fields.emplace_back("Install name", name.substr(0, name.find('\0')).str());
      }
    } else if (command->cmd == llvm::MachO::LC_UUID &&
               command->cmdsize >= sizeof(llvm::MachO::uuid_command)) {
      const auto* uuid = reinterpret_cast<const llvm::MachO::uuid_command*>(data);
      std::ostringstream value;
      for (unsigned j = 0; j < 16; ++j) {
        if (j == 4 || j == 6 || j == 8 || j == 10) {
          value << '-';
        }
        value << std::hex << std::setfill('0') << std::setw(2) << unsigned(uuid->uuid[j]);
      }
      fields.emplace_back("UUID", value.str());
    }
    data += command->cmdsize;
    remaining -= command->cmdsize;
  }
#endif
}

} // namespace

std::vector<LoadedLibrary> loaded_libraries() {
  std::vector<LoadedLibrary> result;
#ifdef CPP_REPL_RUNTIME_SYMBOLS
  initialize_gum();
  gum_process_enumerate_modules(
      [](GumModule* module, gpointer data) -> gboolean {
        if (module == gum_process_get_main_module()) {
          return TRUE;
        }
        auto& result = *static_cast<std::vector<LoadedLibrary>*>(data);
        const auto* range = gum_module_get_range(module);
        const auto* version = gum_module_get_version(module);
        LoadedLibrary library{gum_module_get_name(module),
                              gum_module_get_path(module),
                              version ? version : "not recorded",
                              range->base_address,
                              range->size,
                              {}};
        gum_module_enumerate_dependencies(
            module,
            [](const GumDependencyDetails* dependency, gpointer data) -> gboolean {
              static_cast<std::vector<std::string>*>(data)->push_back(dependency->name);
              return TRUE;
            },
            &library.dependencies);
        result.push_back(std::move(library));
        return TRUE;
      },
      &result);
#endif
  return result;
}

std::string library_list(const std::string& pattern, std::string& error) {
  error.clear();
#ifndef CPP_REPL_RUNTIME_SYMBOLS
  error = "library inspection is unavailable in this build";
  return {};
#endif
  std::regex filter;
  try {
    filter.assign(pattern);
  } catch (const std::regex_error& failure) {
    error = "invalid library regex: " + std::string(failure.what());
    return {};
  }
  std::set<std::string> names;
  for (const auto& library : loaded_libraries()) {
    if (std::regex_search(library.name, filter)) {
      names.insert(library.name);
    }
  }
  std::string result;
  for (const auto& name : names) {
    result += name + '\n';
  }
  return result;
}

std::string library_info(const std::string& query, std::string& error) {
  error.clear();
#ifndef CPP_REPL_RUNTIME_SYMBOLS
  error = "library inspection is unavailable in this build";
  return {};
#endif
  const auto libraries = loaded_libraries();
  std::vector<LoadedLibrary> selected;
  for (const auto& library : libraries) {
    if (query == library.name || query == library.path ||
        query == std::filesystem::path(library.name).stem()) {
      selected.push_back(library);
    }
  }
  if (selected.empty()) {
    std::regex filter;
    try {
      filter.assign(query);
    } catch (const std::regex_error& failure) {
      error = "invalid library regex: " + std::string(failure.what());
      return {};
    }
    for (const auto& library : libraries) {
      if (std::regex_search(library.name, filter) || std::regex_search(library.path, filter)) {
        selected.push_back(library);
      }
    }
  }
  if (selected.empty()) {
    error = "no loaded library matches '" + query + "'";
    return {};
  }
  std::vector<SymbolDescription> blocks;
  for (const auto& library : selected) {
    SymbolDescription block;
    block.name = library.name;
    block.fields = {{"Path", library.path},
                    {"Version", library.version},
                    {"Base", hex(library.base)},
                    {"Size", std::to_string(library.size) + " bytes"}};
#ifdef __GLIBC__
    block.fields.emplace_back("glibc", std::string(gnu_get_libc_version()) + " (running process)");
#else
    block.fields.emplace_back("glibc", "not applicable on this platform");
#endif
    binary_metadata(library, block.fields);
    if (library.dependencies.empty()) {
      block.fields.emplace_back("Dependency", "none recorded");
    }
    for (const auto& dependency : library.dependencies) {
      const auto found =
          std::find_if(libraries.begin(), libraries.end(), [&](const auto& candidate) {
            return candidate.path == dependency ||
                   candidate.name == std::filesystem::path(dependency).filename();
          });
      block.fields.emplace_back(
          "Dependency", dependency + (found == libraries.end() ? "" : " -> " + found->path));
    }
    blocks.push_back(std::move(block));
  }
  auto text = format_symbols(blocks, query);
  std::size_t pos = 0;
  while ((pos = text.find("Symbol: ", pos)) != std::string::npos) {
    text.replace(pos, 8, "Library: ");
    pos += 9;
  }
  return text;
}

std::vector<SymbolDescription> native_symbol_details(const std::string& input) {
  std::string query = input;
  if (query.rfind("::", 0) == 0) {
    query.erase(0, 2);
  }
  if (query.rfind("_Z", 0) == 0 || query.rfind("__Z", 0) == 0) {
    query = readable_symbol_name(query);
  }
  std::vector<LoadedSymbol> exports;
  native_loaded_symbols(std::regex(""), std::regex("^" + escaped(query) + "(\\(|$)"), exports);
  std::vector<SymbolDescription> result;
  for (const auto& symbol : exports) {
    SymbolDescription block;
    block.name = symbol.name;
    block.mangled = normalized(symbol.mangled);
    block.fields = {{"Kind", symbol.kind},
                    {"Mangled", block.mangled},
                    {"Defined", "yes (native export)"},
                    {"Undefined", "no (in exporting library)"},
                    {"Library", std::filesystem::path(symbol.library).filename().string()},
                    {"Path", symbol.library},
                    {"Address", hex(symbol.address)}};
    result.push_back(std::move(block));
  }
#ifdef CPP_REPL_RUNTIME_SYMBOLS
  struct Imports {
    const std::string& query;
    std::vector<SymbolDescription>& result;
    std::string path;
  } imports{query, result, {}};

  gum_process_enumerate_modules(
      [](GumModule* module, gpointer data) -> gboolean {
        auto& imports = *static_cast<Imports*>(data);
        imports.path = gum_module_get_path(module);
        gum_module_enumerate_imports(
            module,
            [](const GumImportDetails* symbol, gpointer data) -> gboolean {
              auto& imports = *static_cast<Imports*>(data);
              if (!symbol->name || !matches(imports.query, symbol->name)) {
                return TRUE;
              }
              const auto mangled = normalized(symbol->name);
              auto block = std::find_if(imports.result.begin(), imports.result.end(),
                                        [&](const auto& item) { return item.mangled == mangled; });
              if (block == imports.result.end()) {
                SymbolDescription undefined;
                undefined.name = readable_symbol_name(mangled);
                undefined.mangled = mangled;
                undefined.fields = {
                    {"Kind", symbol->type == GUM_IMPORT_FUNCTION   ? "function"
                             : symbol->type == GUM_IMPORT_VARIABLE ? "variable"
                                                                   : "unknown"},
                    {"Mangled", mangled},
                    {"Defined", "no (undefined import)"},
                    {"Undefined", "yes (native import)"},
                    {"Address", symbol->address ? hex(symbol->address) : "unresolved"}};
                imports.result.push_back(std::move(undefined));
                block = std::prev(imports.result.end());
              }
              block->fields.emplace_back(
                  "Import", imports.path + (symbol->address ? " (undefined, resolved)"
                                                            : " (undefined, unresolved)"));
              if (symbol->slot) {
                block->fields.emplace_back("Ref site",
                                           imports.path + " import slot " + hex(symbol->slot));
              }
              return TRUE;
            },
            &imports);
        return TRUE;
      },
      &imports);
#endif
  // Loaders may omit unresolved weak imports from their runtime enumeration.
  // Consult the image's undefined-symbol table when no runtime record exists.
  if (result.empty()) {
    for (const auto& library : loaded_libraries()) {
      auto binary = llvm::object::ObjectFile::createObjectFile(library.path);
      if (!binary) {
        llvm::consumeError(binary.takeError());
        continue;
      }
      const auto* object = binary->getBinary();
      const auto* elf = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(object);
      auto inspect = [&](const llvm::object::SymbolRef& symbol) {
        auto flags = symbol.getFlags();
        if (!flags) {
          llvm::consumeError(flags.takeError());
          return;
        }
        if (!(*flags & llvm::object::SymbolRef::SF_Undefined)) {
          return;
        }
        auto raw_name = symbol.getName();
        if (!raw_name) {
          llvm::consumeError(raw_name.takeError());
          return;
        }
        std::string mangled = raw_name->str();
        if (object->isMachO() && !mangled.empty() && mangled[0] == '_') {
          mangled.erase(0, 1);
        }
        if (!matches(query, mangled)) {
          return;
        }
        SymbolDescription block;
        block.name = readable_symbol_name(mangled);
        block.mangled = mangled;
        auto type = symbol.getType();
        std::string kind = "unknown";
        if (!type) {
          llvm::consumeError(type.takeError());
        } else if (*type == llvm::object::SymbolRef::ST_Function) {
          kind = "function";
        } else if (*type == llvm::object::SymbolRef::ST_Data) {
          kind = "variable";
        }
        block.fields = {{"Kind", kind},
                        {"Mangled", mangled},
                        {"Defined", "no (undefined import)"},
                        {"Undefined", "yes (native import)"},
                        {"Binding", *flags & llvm::object::SymbolRef::SF_Weak ? "weak" : "global"},
                        {"Library", library.name},
                        {"Path", library.path},
                        {"Import", library.path + " (undefined, unresolved)"},
                        {"Address", "unresolved"}};
#ifdef CPP_REPL_RUNTIME_SYMBOLS
        if (elf && elf->getEMachine() == llvm::ELF::EM_X86_64) {
          for (const auto& section : object->sections())
            for (const auto& relocation : section.relocations()) {
              if (relocation.getType() != llvm::ELF::R_X86_64_GLOB_DAT &&
                  relocation.getType() != llvm::ELF::R_X86_64_JUMP_SLOT)
                continue;
              const auto target = relocation.getSymbol();
              if (target == object->symbol_end())
                continue;
              auto target_name = target->getName();
              if (!target_name) {
                llvm::consumeError(target_name.takeError());
                continue;
              }
              if (*target_name != *raw_name)
                continue;
              const auto site = relocation.getOffset() +
                                (elf->getEType() == llvm::ELF::ET_DYN ? library.base : 0);
              block.fields.emplace_back("Ref site", library.path + " import slot " + hex(site));
              if (gum_memory_is_readable(reinterpret_cast<void*>(site), sizeof(std::uintptr_t))) {
                std::uintptr_t destination = 0;
                std::memcpy(&destination, reinterpret_cast<void*>(site), sizeof(destination));
                if (destination) {
                  for (auto& field : block.fields) {
                    if (field.first == "Address")
                      field.second = hex(destination);
                    if (field.first == "Import")
                      field.second = library.path + " (undefined, resolved)";
                  }
                }
              }
            }
        }
#endif
        result.push_back(std::move(block));
      };
      if (elf) {
        for (const auto& symbol : elf->getDynamicSymbolIterators()) {
          inspect(symbol);
        }
      } else {
        for (const auto& symbol : object->symbols()) {
          inspect(symbol);
        }
      }
    }
  }
  for (std::size_t i = 0; i < exports.size(); ++i) {
    const auto& target = exports[i];
    if (target.kind != "function") {
      continue;
    }
    std::vector<LoadedSymbol> callers;
    native_loaded_symbols(std::regex("^" + escaped(target.library) + "$"), std::regex(""), callers);
    if (callers.size() > 2048) {
      result[i].fields.emplace_back("Native scan",
                                    "not scanned (large image); import sites are shown");
      continue;
    }
    for (const auto& caller : callers) {
      if (caller.kind != "function" || !caller.address) {
        continue;
      }
      for (const auto site : NativeCallPatch::reference_sites(caller.address, target.address)) {
        result[i].fields.emplace_back("Ref site", hex(site) + " in " + caller.name + " [" +
                                                      target.library + "]");
      }
    }
  }
  for (auto& block : result) {
    const auto count = std::count_if(block.fields.begin(), block.fields.end(),
                                     [](const auto& field) { return field.first == "Ref site"; });
    block.fields.emplace_back("Native refs", std::to_string(count) + " known sites");
    block.fields.emplace_back("Ref scope", "import slots and supported calls in defining image; "
                                           "indirect and other native calls may be absent");
  }
  return result;
}

} // namespace example
