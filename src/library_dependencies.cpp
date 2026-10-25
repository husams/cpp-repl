#include "library_dependencies.hpp"

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace example {
namespace {

template <typename ELFT>
void elf_info(const llvm::object::ELFObjectFile<ELFT>& object, LibraryBinaryInfo& result) {
  const auto& file = object.getELFFile();
  auto sections = file.sections();
  if (!sections) {
    llvm::consumeError(sections.takeError());
    return;
  }
  for (const auto& section : *sections) {
    if (section.sh_type != llvm::ELF::SHT_DYNAMIC) {
      continue;
    }
    auto string_section = file.getSection(section.sh_link);
    if (!string_section) {
      llvm::consumeError(string_section.takeError());
      continue;
    }
    auto strings = file.getStringTable(**string_section);
    if (!strings) {
      llvm::consumeError(strings.takeError());
      continue;
    }
    auto entries = file.dynamicEntries();
    if (!entries) {
      llvm::consumeError(entries.takeError());
      continue;
    }
    for (const auto& entry : *entries) {
      if (entry.d_tag != llvm::ELF::DT_NEEDED && entry.d_tag != llvm::ELF::DT_RPATH &&
          entry.d_tag != llvm::ELF::DT_RUNPATH) {
        continue;
      }
      if (entry.d_un.d_val >= strings->size()) {
        continue;
      }
      auto value = strings->substr(entry.d_un.d_val);
      value = value.substr(0, value.find('\0'));
      if (entry.d_tag == llvm::ELF::DT_NEEDED) {
        result.dependencies.push_back(value.str());
      } else {
        result.search_paths.emplace_back(entry.d_tag == llvm::ELF::DT_RPATH ? "RPATH" : "RUNPATH",
                                         value.str());
      }
    }
  }
}

void inspect(const llvm::object::ObjectFile& object, LibraryBinaryInfo& result) {
  if (const auto* macho = llvm::dyn_cast<llvm::object::MachOObjectFile>(&object)) {
    for (const auto& load : macho->load_commands()) {
      if (load.C.cmd == llvm::MachO::LC_LOAD_DYLIB ||
          load.C.cmd == llvm::MachO::LC_REEXPORT_DYLIB ||
          load.C.cmd == llvm::MachO::LC_LOAD_UPWARD_DYLIB) {
        const auto command = macho->getDylibIDLoadCommand(load);
        if (command.dylib.name >= load.C.cmdsize) {
          continue;
        }
        llvm::StringRef name(load.Ptr + command.dylib.name, load.C.cmdsize - command.dylib.name);
        result.dependencies.push_back(name.substr(0, name.find('\0')).str());
      } else if (load.C.cmd == llvm::MachO::LC_RPATH) {
        const auto command = macho->getRpathCommand(load);
        if (command.path >= load.C.cmdsize) {
          continue;
        }
        llvm::StringRef path(load.Ptr + command.path, load.C.cmdsize - command.path);
        result.search_paths.emplace_back("RPATH", path.substr(0, path.find('\0')).str());
      }
    }
  } else if (const auto* elf = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(&object)) {
    elf_info(*elf, result);
  } else if (const auto* elf = llvm::dyn_cast<llvm::object::ELF64BEObjectFile>(&object)) {
    elf_info(*elf, result);
  } else if (const auto* elf = llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(&object)) {
    elf_info(*elf, result);
  } else if (const auto* elf = llvm::dyn_cast<llvm::object::ELF32BEObjectFile>(&object)) {
    elf_info(*elf, result);
  }
}

} // namespace

LibraryBinaryInfo library_binary_info(const std::string& path) {
  LibraryBinaryInfo result;
  auto binary = llvm::object::createBinary(path);
  if (!binary) {
    llvm::consumeError(binary.takeError());
    return result;
  }
  if (const auto* object = llvm::dyn_cast<llvm::object::ObjectFile>(binary->getBinary())) {
    inspect(*object, result);
  } else if (const auto* fat =
                 llvm::dyn_cast<llvm::object::MachOUniversalBinary>(binary->getBinary())) {
    const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    auto object = fat->getMachOObjectForArch(triple.getArchName());
    if (object) {
      inspect(**object, result);
    } else {
      llvm::consumeError(object.takeError());
    }
  }
  return result;
}

std::string executable_path() {
  return llvm::sys::fs::getMainExecutable("cpp-repl", reinterpret_cast<void*>(&executable_path));
}

} // namespace example
