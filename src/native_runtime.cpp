#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "native_runtime.hpp"

// LLVM ELF constants must precede the ELF macros in system headers.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Error.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif
#else
#include <fcntl.h>
#include <link.h>
#endif

namespace example {
namespace {

bool contains(std::uintptr_t start, std::size_t length, std::uintptr_t address, std::size_t size) {
  return address >= start && address - start <= length && size <= length - (address - start);
}

#ifndef __APPLE__
std::vector<NativeRange> memory_ranges() {
  std::vector<NativeRange> result;
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    std::uintptr_t begin = 0, end = 0;
    char permissions[5]{};
    if (std::sscanf(line.c_str(), "%lx-%lx %4s", &begin, &end, permissions) != 3) {
      continue;
    }
    const int protection = (permissions[0] == 'r' ? PROT_READ : 0) |
                           (permissions[1] == 'w' ? PROT_WRITE : 0) |
                           (permissions[2] == 'x' ? PROT_EXEC : 0);
    result.push_back({begin, end - begin, protection});
  }
  return result;
}
#endif

bool memory_region(std::uintptr_t address, NativeRange& range) {
#ifdef __APPLE__
  mach_vm_address_t start = address;
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t info{};
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  const auto status = mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
                                     reinterpret_cast<vm_region_info_t>(&info), &count, &object);
  if (object != MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), object);
  }
  if (status != KERN_SUCCESS || address < start || address - start >= size) {
    return false;
  }
  range = {static_cast<std::uintptr_t>(start), static_cast<std::size_t>(size), info.protection};
  return true;
#else
  for (const auto& item : memory_ranges()) {
    if (contains(item.address, item.size, address, 1)) {
      range = item;
      return true;
    }
  }
  return false;
#endif
}

#ifdef __APPLE__
struct MachImage {
  const mach_header_64* header;
  std::vector<const segment_command_64*> segments;
  const segment_command_64* linkedit = nullptr;
  const symtab_command* symbols = nullptr;
  const dysymtab_command* indirect = nullptr;
  std::uint32_t exports_offset = 0, exports_size = 0;
  std::uintptr_t bias;

  explicit MachImage(const NativeModule& module)
      : header(reinterpret_cast<const mach_header_64*>(module.base))
      , bias(module.bias) {
    if (!memory_readable(header, sizeof(*header)) || header->magic != MH_MAGIC_64) {
      return;
    }
    auto* cursor = reinterpret_cast<const std::uint8_t*>(header + 1);
    const auto* end = cursor + header->sizeofcmds;
    if (header->sizeofcmds > (1u << 24) || !memory_readable(cursor, header->sizeofcmds)) {
      return;
    }
    for (unsigned i = 0; i < header->ncmds && cursor + sizeof(load_command) <= end; ++i) {
      const auto* command = reinterpret_cast<const load_command*>(cursor);
      if (command->cmdsize < sizeof(*command) || command->cmdsize > std::size_t(end - cursor)) {
        break;
      }
      if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(segment_command_64)) {
        const auto* segment = reinterpret_cast<const segment_command_64*>(cursor);
        segments.push_back(segment);
        if (std::strncmp(segment->segname, SEG_LINKEDIT, 16) == 0) {
          linkedit = segment;
        }
      } else if (command->cmd == LC_SYMTAB && command->cmdsize >= sizeof(symtab_command)) {
        symbols = reinterpret_cast<const symtab_command*>(cursor);
      } else if (command->cmd == LC_DYSYMTAB && command->cmdsize >= sizeof(dysymtab_command)) {
        indirect = reinterpret_cast<const dysymtab_command*>(cursor);
      } else if (command->cmd == LC_DYLD_EXPORTS_TRIE &&
                 command->cmdsize >= sizeof(linkedit_data_command)) {
        const auto* trie = reinterpret_cast<const linkedit_data_command*>(cursor);
        exports_offset = trie->dataoff;
        exports_size = trie->datasize;
      } else if ((command->cmd == LC_DYLD_INFO_ONLY || command->cmd == LC_DYLD_INFO) &&
                 command->cmdsize >= sizeof(dyld_info_command) && !exports_size) {
        const auto* info = reinterpret_cast<const dyld_info_command*>(cursor);
        exports_offset = info->export_off;
        exports_size = info->export_size;
      }
      cursor += command->cmdsize;
    }
  }

  const std::uint8_t* data(std::uint32_t offset, std::size_t size) const {
    if (!linkedit || offset < linkedit->fileoff ||
        offset - linkedit->fileoff > linkedit->filesize ||
        size > linkedit->filesize - (offset - linkedit->fileoff)) {
      return nullptr;
    }
    const auto* address =
        reinterpret_cast<const std::uint8_t*>(bias + linkedit->vmaddr + offset - linkedit->fileoff);
    return memory_readable(address, size) ? address : nullptr;
  }
};

std::string darwin_name(llvm::StringRef name) {
  if (name.starts_with("_")) {
    name = name.drop_front();
  }
  return name.str();
}
#else

template <typename ELFT>
void elf_dependencies(const llvm::object::ELFObjectFile<ELFT>& object, NativeModule& module) {
  const auto& elf = object.getELFFile();
  auto sections = elf.sections();
  if (!sections) {
    llvm::consumeError(sections.takeError());
    return;
  }
  for (const auto& section : *sections) {
    if (section.sh_type != SHT_DYNAMIC) {
      continue;
    }
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
      if (entry.d_tag != DT_NEEDED || entry.d_un.d_val >= strings->size()) {
        continue;
      }
      const auto name = strings->substr(entry.d_un.d_val);
      module.dependencies.push_back(name.substr(0, name.find('\0')).str());
    }
  }
}
#endif

} // namespace

std::vector<NativeModule> native_modules() {
  std::vector<NativeModule> result;
#ifdef __APPLE__
  const auto count = _dyld_image_count();
  for (std::uint32_t i = 0; i < count; ++i) {
    NativeModule module;
    const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
    if (!header || header->magic != MH_MAGIC_64) {
      continue;
    }
    module.path = _dyld_get_image_name(i);
    module.name = std::filesystem::path(module.path).filename().string();
    module.base = reinterpret_cast<std::uintptr_t>(header);
    module.bias = _dyld_get_image_vmaddr_slide(i);
    module.main = i == 0;
    MachImage image(module);
    for (const auto* segment : image.segments) {
      if (!segment->vmsize || !segment->initprot) {
        continue;
      }
      const auto address = module.bias + segment->vmaddr;
      module.ranges.push_back(
          {address, static_cast<std::size_t>(segment->vmsize), segment->initprot});
      if (address >= module.base) {
        module.size = std::max(module.size, std::size_t(address - module.base + segment->vmsize));
      }
    }
    auto* cursor = reinterpret_cast<const std::uint8_t*>(header + 1);
    for (unsigned n = 0; n < header->ncmds; ++n) {
      const auto* command = reinterpret_cast<const load_command*>(cursor);
      if ((command->cmd == LC_LOAD_DYLIB || command->cmd == LC_LOAD_WEAK_DYLIB ||
           command->cmd == LC_REEXPORT_DYLIB || command->cmd == LC_LOAD_UPWARD_DYLIB ||
           command->cmd == LC_ID_DYLIB) &&
          command->cmdsize >= sizeof(dylib_command)) {
        const auto* dylib = reinterpret_cast<const dylib_command*>(cursor);
        if (command->cmd == LC_ID_DYLIB) {
          const auto version = dylib->dylib.current_version;
          module.version = std::to_string(version >> 16) + "." +
                           std::to_string((version >> 8) & 255) + "." +
                           std::to_string(version & 255);
        } else if (dylib->dylib.name.offset < command->cmdsize) {
          llvm::StringRef name(reinterpret_cast<const char*>(cursor) + dylib->dylib.name.offset,
                               command->cmdsize - dylib->dylib.name.offset);
          module.dependencies.push_back(name.substr(0, name.find('\0')).str());
        }
      }
      cursor += command->cmdsize;
    }
    result.push_back(std::move(module));
  }
#else
  dl_iterate_phdr(
      [](dl_phdr_info* info, std::size_t, void* data) {
        auto& modules = *static_cast<std::vector<NativeModule>*>(data);
        NativeModule module;
        module.main = !info->dlpi_name || !*info->dlpi_name;
        module.path = module.main ? "/proc/self/exe" : info->dlpi_name;
        std::error_code error;
        const auto canonical = std::filesystem::canonical(module.path, error);
        if (!error) {
          module.path = canonical.string();
        }
        module.name = std::filesystem::path(module.path).filename().string();
        module.bias = info->dlpi_addr;
        module.base = std::numeric_limits<std::uintptr_t>::max();
        std::uintptr_t end = 0;
        for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
          const auto& header = info->dlpi_phdr[i];
          if (header.p_type != PT_LOAD || !header.p_memsz) {
            continue;
          }
          const auto address = info->dlpi_addr + header.p_vaddr;
          module.base = std::min(module.base, std::uintptr_t(address));
          end = std::max(end, std::uintptr_t(address + header.p_memsz));
          const auto protection = (header.p_flags & PF_R ? PROT_READ : 0) |
                                  (header.p_flags & PF_W ? PROT_WRITE : 0) |
                                  (header.p_flags & PF_X ? PROT_EXEC : 0);
          module.ranges.push_back({address, static_cast<std::size_t>(header.p_memsz), protection});
        }
        if (end <= module.base) {
          return 0;
        }
        module.size = end - module.base;
        auto binary = llvm::object::ObjectFile::createObjectFile(module.path);
        if (binary) {
          if (auto* elf = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(binary->getBinary())) {
            elf_dependencies(*elf, module);
          }
        } else {
          llvm::consumeError(binary.takeError());
        }
        modules.push_back(std::move(module));
        return 0;
      },
      &result);
#endif
  return result;
}

std::vector<NativeExport> native_exports(const NativeModule& module) {
  std::vector<NativeExport> result;
#ifdef __APPLE__
  MachImage image(module);
  if (const auto* data = image.data(image.exports_offset, image.exports_size)) {
    auto error = llvm::Error::success();
    for (const auto& entry :
         llvm::object::MachOObjectFile::exports(error, {data, image.exports_size})) {
      auto name = darwin_name(entry.name());
      std::uintptr_t address;
      if (entry.flags() & EXPORT_SYMBOL_FLAGS_REEXPORT) {
        address = reinterpret_cast<std::uintptr_t>(dlsym(RTLD_DEFAULT, name.c_str()));
      } else if ((entry.flags() & EXPORT_SYMBOL_FLAGS_KIND_MASK) ==
                 EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE) {
        address = entry.address();
      } else {
        address = module.base + entry.address();
      }
      address = native_code_address(address);
      if (!address) {
        continue;
      }
      bool function =
          std::any_of(module.ranges.begin(), module.ranges.end(), [&](const auto& range) {
            return (range.protection & PROT_EXEC) &&
                   contains(range.address, range.size, address, 1);
          });
      if (entry.flags() & EXPORT_SYMBOL_FLAGS_REEXPORT) {
        NativeRange range{};
        function = memory_region(address, range) && (range.protection & PROT_EXEC);
      }
      result.push_back({std::move(name), address, function});
    }
    llvm::consumeError(std::move(error));
  }
#else
  auto binary = llvm::object::ObjectFile::createObjectFile(module.path);
  if (!binary) {
    llvm::consumeError(binary.takeError());
    return result;
  }
  const auto* elf = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(binary->getBinary());
  if (!elf) {
    return result;
  }
  for (const auto& symbol : elf->getDynamicSymbolIterators()) {
    auto flags = symbol.getFlags();
    auto name = symbol.getName();
    auto address = symbol.getAddress();
    auto type = symbol.getType();
    if (!flags || !name || !address || !type) {
      if (!flags) {
        llvm::consumeError(flags.takeError());
      }
      if (!name) {
        llvm::consumeError(name.takeError());
      }
      if (!address) {
        llvm::consumeError(address.takeError());
      }
      if (!type) {
        llvm::consumeError(type.takeError());
      }
      continue;
    }
    if ((*flags & llvm::object::SymbolRef::SF_Undefined) ||
        !(*flags & (llvm::object::SymbolRef::SF_Global | llvm::object::SymbolRef::SF_Weak)) ||
        name->empty() || !*address) {
      continue;
    }
    const auto visibility = llvm::object::ELFSymbolRef(symbol).getOther() & 3;
    if (visibility == STV_HIDDEN || visibility == STV_INTERNAL) {
      continue;
    }
    const auto bias = *flags & llvm::object::SymbolRef::SF_Absolute ? 0 : module.bias;
    result.push_back({name->str(), bias + *address, *type == llvm::object::SymbolRef::ST_Function});
  }
#endif
  return result;
}

std::vector<NativeImport> native_imports(const NativeModule& module,
                                         const std::function<bool(const std::string&)>& matches) {
  std::vector<NativeImport> result;
#ifdef __APPLE__
  MachImage image(module);
  if (!image.symbols) {
    return result;
  }
  const auto& table = *image.symbols;
  const auto* symbols = reinterpret_cast<const nlist_64*>(
      image.data(table.symoff, std::size_t(table.nsyms) * sizeof(nlist_64)));
  const auto* strings = reinterpret_cast<const char*>(image.data(table.stroff, table.strsize));
  if (!symbols || !strings) {
    return result;
  }
  std::map<std::uint32_t, std::vector<std::uintptr_t>> slots;
  if (image.indirect) {
    const auto* indexes = reinterpret_cast<const std::uint32_t*>(
        image.data(image.indirect->indirectsymoff,
                   std::size_t(image.indirect->nindirectsyms) * sizeof(std::uint32_t)));
    if (indexes) {
      for (const auto* segment : image.segments) {
        if (sizeof(*segment) + std::size_t(segment->nsects) * sizeof(section_64) >
            segment->cmdsize) {
          continue;
        }
        const auto* sections = reinterpret_cast<const section_64*>(segment + 1);
        for (unsigned i = 0; i < segment->nsects; ++i) {
          const auto& section = sections[i];
          const auto type = section.flags & SECTION_TYPE;
          if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) {
            continue;
          }
          for (std::size_t j = 0; j < section.size / sizeof(std::uintptr_t); ++j) {
            const auto index = section.reserved1 + j;
            if (index >= image.indirect->nindirectsyms) {
              break;
            }
            if (indexes[index] < table.nsyms) {
              slots[indexes[index]].push_back(module.bias + section.addr +
                                              j * sizeof(std::uintptr_t));
            }
          }
        }
      }
    }
  }
  for (std::uint32_t i = 0; i < table.nsyms; ++i) {
    const auto& symbol = symbols[i];
    if ((symbol.n_type & N_STAB) || (symbol.n_type & N_TYPE) != N_UNDF ||
        symbol.n_un.n_strx >= table.strsize) {
      continue;
    }
    const llvm::StringRef tail(strings + symbol.n_un.n_strx, table.strsize - symbol.n_un.n_strx);
    NativeImport item{darwin_name(tail.substr(0, tail.find('\0'))), "unknown"};
    item.weak = symbol.n_desc & N_WEAK_REF;
    if (item.name.empty() || !matches(item.name)) {
      continue;
    }
    if (slots[i].empty()) {
      item.address = native_code_address(
          reinterpret_cast<std::uintptr_t>(dlsym(RTLD_DEFAULT, item.name.c_str())));
      result.push_back(item);
    } else {
      for (const auto slot : slots[i]) {
        item.slot = slot;
        if (memory_readable(reinterpret_cast<void*>(slot), sizeof(std::uintptr_t))) {
          std::memcpy(&item.address, reinterpret_cast<void*>(slot), sizeof(item.address));
        }
        item.address = native_code_address(item.address);
        result.push_back(item);
      }
    }
  }
#else
  auto binary = llvm::object::ObjectFile::createObjectFile(module.path);
  if (!binary) {
    llvm::consumeError(binary.takeError());
    return result;
  }
  const auto* elf = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(binary->getBinary());
  if (!elf) {
    return result;
  }
  std::map<std::string, NativeImport> undefined;
  for (const auto& symbol : elf->getDynamicSymbolIterators()) {
    auto flags = symbol.getFlags();
    auto name = symbol.getName();
    auto type = symbol.getType();
    if (!flags || !name || !type) {
      if (!flags) {
        llvm::consumeError(flags.takeError());
      }
      if (!name) {
        llvm::consumeError(name.takeError());
      }
      if (!type) {
        llvm::consumeError(type.takeError());
      }
      continue;
    }
    if (!(*flags & llvm::object::SymbolRef::SF_Undefined) || name->empty()) {
      continue;
    }
    NativeImport item{name->str(), *type == llvm::object::SymbolRef::ST_Function ? "function"
                                   : *type == llvm::object::SymbolRef::ST_Data   ? "variable"
                                                                                 : "unknown"};
    if (!matches(item.name)) {
      continue;
    }
    item.weak = *flags & llvm::object::SymbolRef::SF_Weak;
    item.address = reinterpret_cast<std::uintptr_t>(dlsym(RTLD_DEFAULT, item.name.c_str()));
    undefined.emplace(item.name, std::move(item));
  }
  std::map<std::string, bool> emitted;
  if (undefined.empty()) {
    return result;
  }
  for (const auto& section : elf->sections()) {
    for (const auto& relocation : section.relocations()) {
      if (relocation.getType() != R_X86_64_GLOB_DAT && relocation.getType() != R_X86_64_JUMP_SLOT) {
        continue;
      }
      const auto symbol = relocation.getSymbol();
      if (symbol == elf->symbol_end()) {
        continue;
      }
      auto name = symbol->getName();
      if (!name) {
        llvm::consumeError(name.takeError());
        continue;
      }
      const auto found = undefined.find(name->str());
      if (found == undefined.end()) {
        continue;
      }
      auto item = found->second;
      item.slot = module.bias + relocation.getOffset();
      if (memory_readable(reinterpret_cast<void*>(item.slot), sizeof(std::uintptr_t))) {
        std::memcpy(&item.address, reinterpret_cast<void*>(item.slot), sizeof(item.address));
      }
      result.push_back(item);
      emitted[item.name] = true;
    }
  }
  for (const auto& [name, item] : undefined) {
    if (!emitted[name]) {
      result.push_back(item);
    }
  }
#endif
  return result;
}

bool native_function_has_aliases(std::uintptr_t address) {
  for (const auto& module : native_modules()) {
    if (!std::any_of(module.ranges.begin(), module.ranges.end(), [&](const auto& range) {
          return (range.protection & PROT_EXEC) && contains(range.address, range.size, address, 1);
        })) {
      continue;
    }
    unsigned count = 0;
    for (const auto& symbol : native_exports(module)) {
      if (symbol.function && symbol.address == address && ++count > 1) {
        return true;
      }
    }
    return false;
  }
  return false;
}

std::uintptr_t native_code_address(std::uintptr_t address) {
#if defined(__APPLE__) && __has_feature(ptrauth_calls)
  return reinterpret_cast<std::uintptr_t>(
      ptrauth_strip(reinterpret_cast<void*>(address), ptrauth_key_function_pointer));
#else
  return address;
#endif
}

bool memory_readable(const void* pointer, std::size_t size) {
  auto address = reinterpret_cast<std::uintptr_t>(pointer);
  if (!pointer || size > std::numeric_limits<std::uintptr_t>::max() - address) {
    return false;
  }
  while (size) {
    NativeRange range{};
    if (!memory_region(address, range) || !(range.protection & PROT_READ)) {
      return false;
    }
    const auto chunk = std::min(size, range.size - (address - range.address));
    address += chunk;
    size -= chunk;
  }
  return true;
}

std::size_t code_page_size() {
  static const auto size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  return size;
}

void* allocate_code_near(std::uintptr_t address, std::size_t distance) {
  const auto page = code_page_size();
  const auto center = address & ~(std::uintptr_t(page) - 1);
  // Hints never replace existing mappings. Reject mappings outside branch reach.
  for (std::size_t delta = page; delta < distance; delta += 16 * page) {
    for (const bool above : {true, false}) {
      if ((above && delta > std::numeric_limits<std::uintptr_t>::max() - center) ||
          (!above && delta > center)) {
        continue;
      }
      const auto hint = above ? center + delta : center - delta;
      void* memory = mmap(reinterpret_cast<void*>(hint), page, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (memory == MAP_FAILED) {
        continue;
      }
      const auto actual = reinterpret_cast<std::uintptr_t>(memory);
      if ((actual > address ? actual - address : address - actual) <= distance) {
        return memory;
      }
      munmap(memory, page);
    }
  }
  return nullptr;
}

bool make_code_executable(void* address) {
  if (mprotect(address, code_page_size(), PROT_READ | PROT_EXEC) != 0) {
    return false;
  }
  __builtin___clear_cache(static_cast<char*>(address),
                          static_cast<char*>(address) + code_page_size());
  return true;
}

void free_code(void* address) {
  munmap(address, code_page_size());
}

bool patch_code(std::uintptr_t address, const void* bytes, std::size_t size) {
  if (!size || !memory_readable(reinterpret_cast<void*>(address), size)) {
    return false;
  }
#ifndef __APPLE__
  // Keep executable mappings RX: restoring X after dirtying a file-backed
  // writable page is forbidden by execmod policies on some Linux systems.
  const int memory = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
  if (memory < 0) {
    return false;
  }
  std::vector<std::uint8_t> original(size);
  std::memcpy(original.data(), reinterpret_cast<void*>(address), size);
  auto write = [&](const void* data, std::size_t length) {
    std::size_t written = 0;
    while (written < length) {
      const auto count = pwrite(memory, static_cast<const std::uint8_t*>(data) + written,
                                length - written, static_cast<off_t>(address + written));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        break;
      }
      written += static_cast<std::size_t>(count);
    }
    return written == length;
  };
  const bool changed = write(bytes, size);
  if (!changed) {
    write(original.data(), original.size());
  }
  close(memory);
  __builtin___clear_cache(reinterpret_cast<char*>(address),
                          reinterpret_cast<char*>(address + size));
  return changed;
#else
  const auto page = code_page_size();
  const auto start = address & ~(std::uintptr_t(page) - 1);
  const auto end = (address + size + page - 1) & ~(std::uintptr_t(page) - 1);

  struct Page {
    std::uintptr_t address;
    int protection;
  };

  std::vector<Page> pages;
  for (auto cursor = start; cursor < end; cursor += page) {
    NativeRange range{};
    if (!memory_region(cursor, range)) {
      return false;
    }
    pages.push_back({cursor, range.protection});
  }
  auto protect = [&](const Page& item, bool writable) {
    const auto protection =
        writable ? VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY : item.protection;
    return mach_vm_protect(mach_task_self(), item.address, page, FALSE, protection) == KERN_SUCCESS;
  };
  std::size_t changed = 0;
  while (changed < pages.size() && protect(pages[changed], true)) {
    ++changed;
  }
  if (changed != pages.size()) {
    while (changed) {
      protect(pages[--changed], false);
    }
    return false;
  }
  std::memcpy(reinterpret_cast<void*>(address), bytes, size);
  __builtin___clear_cache(reinterpret_cast<char*>(address),
                          reinterpret_cast<char*>(address + size));
  bool restored = true;
  for (const auto& item : pages) {
    restored &= protect(item, false);
  }
  return restored;
#endif
}

} // namespace example
