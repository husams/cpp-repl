#include "native_call_patch.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#ifdef CPP_REPL_NATIVE_CALL_PATCHES
#include <dlfcn.h>
#include <mach-o/loader.h>

#include "frida-gum.h"
#include "gum_runtime.hpp"
#endif

namespace example {

#ifdef CPP_REPL_NATIVE_CALL_PATCHES
namespace {

std::mutex patch_mutex;

struct StubRange {
  std::uintptr_t start;
  std::uintptr_t end;
  std::size_t stride;
};

bool function_bounds(std::uintptr_t caller, std::uintptr_t& end,
                     std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& data,
                     std::vector<StubRange>& stubs, std::string& error) {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(caller), &info) || !info.dli_fbase) {
    error = "native caller does not belong to a loaded Mach-O image";
    return false;
  }
  const auto* header = static_cast<const mach_header_64*>(info.dli_fbase);
  if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64) {
    error = "native caller must be in an ARM64 Mach-O image";
    return false;
  }
  const auto* cursor = reinterpret_cast<const std::uint8_t*>(header + 1);
  const auto* commands_end = cursor + header->sizeofcmds;
  const segment_command_64* text = nullptr;
  const segment_command_64* linkedit = nullptr;
  const linkedit_data_command* starts = nullptr;
  const linkedit_data_command* data_in_code = nullptr;
  std::uintptr_t text_end = 0;
  for (unsigned i = 0; i < header->ncmds; ++i) {
    if (cursor + sizeof(load_command) > commands_end) {
      break;
    }
    const auto* command = reinterpret_cast<const load_command*>(cursor);
    if (command->cmdsize < sizeof(load_command) || cursor + command->cmdsize > commands_end) {
      break;
    }
    if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(segment_command_64)) {
      const auto* segment = reinterpret_cast<const segment_command_64*>(command);
      if (std::strncmp(segment->segname, SEG_TEXT, 16) == 0) {
        text = segment;
      }
      if (std::strncmp(segment->segname, SEG_LINKEDIT, 16) == 0) {
        linkedit = segment;
      }
    } else if (command->cmd == LC_FUNCTION_STARTS &&
               command->cmdsize >= sizeof(linkedit_data_command)) {
      starts = reinterpret_cast<const linkedit_data_command*>(command);
    } else if (command->cmd == LC_DATA_IN_CODE &&
               command->cmdsize >= sizeof(linkedit_data_command)) {
      data_in_code = reinterpret_cast<const linkedit_data_command*>(command);
    }
    cursor += command->cmdsize;
  }
  if (!text || !linkedit || !starts || !starts->datasize) {
    error = "native caller requires Mach-O function-start metadata";
    return false;
  }
  const auto slide = reinterpret_cast<std::uintptr_t>(header) - text->vmaddr;
  const auto* sections = reinterpret_cast<const section_64*>(text + 1);
  if (sizeof(segment_command_64) + text->nsects * sizeof(section_64) <= text->cmdsize) {
    for (unsigned i = 0; i < text->nsects; ++i) {
      if (std::strncmp(sections[i].sectname, SECT_TEXT, 16) == 0 &&
          caller >= slide + sections[i].addr &&
          caller < slide + sections[i].addr + sections[i].size) {
        text_end = slide + sections[i].addr + sections[i].size;
      }
      if ((sections[i].flags & SECTION_TYPE) == S_SYMBOL_STUBS && sections[i].reserved2 >= 12) {
        stubs.push_back({slide + sections[i].addr, slide + sections[i].addr + sections[i].size,
                         sections[i].reserved2});
      }
    }
  }
  if (!text_end || starts->dataoff < linkedit->fileoff ||
      starts->dataoff + std::uint64_t(starts->datasize) > linkedit->fileoff + linkedit->filesize) {
    error = "native caller has no valid text/function-start range";
    return false;
  }
  if (data_in_code) {
    if (data_in_code->dataoff < linkedit->fileoff ||
        data_in_code->dataoff + std::uint64_t(data_in_code->datasize) >
            linkedit->fileoff + linkedit->filesize ||
        data_in_code->datasize % sizeof(data_in_code_entry)) {
      error = "invalid Mach-O data-in-code table";
      return false;
    }
    const auto* entries = reinterpret_cast<const data_in_code_entry*>(
        slide + linkedit->vmaddr + data_in_code->dataoff - linkedit->fileoff);
    for (std::size_t i = 0; i < data_in_code->datasize / sizeof(data_in_code_entry); ++i) {
      const auto start = reinterpret_cast<std::uintptr_t>(header) + entries[i].offset;
      data.emplace_back(start, start + entries[i].length);
    }
  }
  cursor = reinterpret_cast<const std::uint8_t*>(slide + linkedit->vmaddr + starts->dataoff -
                                                 linkedit->fileoff);
  const auto* table_end = cursor + starts->datasize;
  std::uintptr_t address = slide + text->vmaddr;
  bool found = false;
  while (cursor < table_end) {
    std::uint64_t delta = 0;
    unsigned shift = 0;
    std::uint8_t byte;
    do {
      if (cursor == table_end || shift >= 63) {
        error = "invalid Mach-O function-start table";
        return false;
      }
      byte = *cursor++;
      delta |= std::uint64_t(byte & 0x7f) << shift;
      shift += 7;
    } while (byte & 0x80);
    if (!delta) {
      break;
    }
    address += delta;
    if (address > caller) {
      if (found) {
        end = address < text_end ? address : text_end;
        return true;
      }
      break;
    }
    found = address == caller;
  }
  if (found) {
    end = text_end;
    return true;
  }
  error = "native caller address is not an exact function start";
  return false;
}

bool write_instruction(std::uintptr_t address, std::uint32_t instruction) {
  return gum_memory_patch_code(
      reinterpret_cast<void*>(address), sizeof(instruction),
      [](gpointer memory, gpointer data) { std::memcpy(memory, data, sizeof(std::uint32_t)); },
      &instruction);
}

bool targets_callee(std::uintptr_t address, std::uintptr_t callee,
                    const std::vector<StubRange>& stubs) {
  if (address == callee) {
    return true;
  }
  for (const auto& stub : stubs) {
    if (address < stub.start || address + 12 > stub.end || (address - stub.start) % stub.stride) {
      continue;
    }
    std::uint32_t code[3];
    std::memcpy(code, reinterpret_cast<void*>(address), sizeof(code));
    // Standard ARM64 linker stub: ADRP X16; LDR X16,[X16,#offset]; BR X16.
    if ((code[0] & 0x9f00001f) != 0x90000010 || (code[1] & 0xffc003ff) != 0xf9400210 ||
        code[2] != 0xd61f0200) {
      continue;
    }
    std::int64_t pages = ((code[0] >> 5) & 0x7ffff) * 4 + ((code[0] >> 29) & 3);
    if (pages & (1 << 20)) {
      pages -= (1 << 21);
    }
    const auto pointer =
        (address & ~std::uintptr_t(0xfff)) + pages * 4096 + ((code[1] >> 10) & 0xfff) * 8;
    if (!gum_memory_is_readable(reinterpret_cast<void*>(pointer), sizeof(std::uintptr_t))) {
      continue;
    }
    std::uintptr_t target;
    std::memcpy(&target, reinterpret_cast<void*>(pointer), sizeof(target));
    if (gum_strip_code_address(target) == callee) {
      return true;
    }
  }
  return false;
}

} // namespace
#endif

struct NativeCallPatch::Impl {
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
  struct Site {
    std::uintptr_t address;
    std::uint32_t original;
    std::uint32_t replacement = 0;
  };

  std::vector<Site> sites;
  std::uintptr_t caller = 0;
  std::size_t length = 0;
  void* island = nullptr;
  std::uintptr_t destination = 0;

  ~Impl() {
    if (island) {
      gum_memory_free(island, gum_query_page_size());
    }
  }
#endif
  bool installed = false;
};

NativeCallPatch::NativeCallPatch(ConstructionKey, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

bool NativeCallPatch::is_native_address(std::uintptr_t address) {
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
  Dl_info info{};
  return dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fbase;
#else
  (void)address;
  return false;
#endif
}

std::shared_ptr<NativeCallPatch>
NativeCallPatch::prepare(std::uintptr_t caller, std::uintptr_t callee, std::string& error) {
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
  initialize_gum();
  std::lock_guard<std::mutex> lock(patch_mutex);
  std::uintptr_t end = 0;
  std::vector<std::pair<std::uintptr_t, std::uintptr_t>> data;
  std::vector<StubRange> stubs;
  if (!function_bounds(caller, end, data, stubs, error)) {
    return nullptr;
  }
  if (auto* module = gum_process_find_module_by_address(caller)) {
    struct Aliases {
      std::uintptr_t address;
      unsigned count = 0;
    } aliases{caller};

    gum_module_enumerate_exports(
        module,
        [](const GumExportDetails* details, gpointer user) {
          auto& found = *static_cast<Aliases*>(user);
          if (details->type == GUM_EXPORT_FUNCTION && details->address == found.address) {
            ++found.count;
          }
          return TRUE;
        },
        &aliases);
    g_object_unref(module);
    if (aliases.count > 1) {
      error = "native caller shares its code address with another exported symbol";
      return nullptr;
    }
  }
  if (caller == callee || (caller & 3) || end <= caller || end - caller > (1u << 26)) {
    error = "unsupported native caller range";
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  impl->caller = caller;
  impl->length = end - caller;
  for (auto pc = caller; pc + 4 <= end; pc += 4) {
    bool is_data = false;
    for (const auto& range : data) {
      if (pc < range.second && pc + 4 > range.first) {
        is_data = true;
      }
    }
    if (is_data) {
      continue;
    }
    std::uint32_t instruction;
    std::memcpy(&instruction, reinterpret_cast<void*>(pc), sizeof(instruction));
    // ARM64 B and BL share a signed 26-bit displacement in instruction words.
    if ((instruction & 0x7c000000) != 0x14000000) {
      continue;
    }
    std::int64_t displacement = instruction & 0x03ffffff;
    if (displacement & (1 << 25)) {
      displacement -= (1 << 26);
    }
    if (targets_callee(pc + displacement * 4, callee, stubs)) {
      impl->sites.push_back({pc, instruction, 0});
    }
  }
  if (impl->sites.empty()) {
    error = "native caller has no direct B/BL branch to the target; "
            "inlined, indirect and unrecognized-stub calls are unsupported";
    return nullptr;
  }
  return std::make_shared<NativeCallPatch>(ConstructionKey{}, std::move(impl));
#else
  (void)caller;
  (void)callee;
  error = "native caller hooks require a supported runtime patch backend";
  return nullptr;
#endif
}

bool NativeCallPatch::install(std::uintptr_t destination, std::string& error) {
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
  std::lock_guard<std::mutex> lock(patch_mutex);
  if (impl_->installed) {
    for (const auto& site : impl_->sites) {
      if (*reinterpret_cast<const std::uint32_t*>(site.address) != site.replacement) {
        error = "native patch is incomplete or changed; restore it before retrying";
        return false;
      }
    }
    return true;
  }
  if (!impl_->island) {
    const GumAddressSpec spec{reinterpret_cast<void*>(impl_->caller),
                              GUM_ARM64_B_MAX_DISTANCE - impl_->length};
    impl_->island =
        gum_memory_allocate_near(&spec, gum_query_page_size(), gum_query_page_size(), GUM_PAGE_RW);
    if (!impl_->island) {
      error = "cannot allocate an executable branch island near the native caller";
      return false;
    }
    GumArm64Writer writer;
    gum_arm64_writer_init(&writer, impl_->island);
    const bool written =
        gum_arm64_writer_put_ldr_reg_address(&writer, ARM64_REG_X16, destination) &&
        gum_arm64_writer_put_br_reg_no_auth(&writer, ARM64_REG_X16) &&
        gum_arm64_writer_flush(&writer);
    gum_arm64_writer_clear(&writer);
    if (!written || !gum_try_mprotect(impl_->island, gum_query_page_size(), GUM_PAGE_RX)) {
      error = "cannot make the native branch island executable";
      gum_memory_free(impl_->island, gum_query_page_size());
      impl_->island = nullptr;
      return false;
    }
    gum_clear_cache(impl_->island, gum_query_page_size());
    impl_->destination = destination;
    for (auto& site : impl_->sites) {
      const auto displacement =
          static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(impl_->island)) -
          static_cast<std::int64_t>(site.address);
      if ((displacement & 3) || displacement < -(1LL << 27) ||
          displacement > GUM_ARM64_B_MAX_DISTANCE) {
        error = "native branch island is out of reach";
        gum_memory_free(impl_->island, gum_query_page_size());
        impl_->island = nullptr;
        return false;
      }
      site.replacement = (site.original & 0xfc000000) |
                         (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffff);
    }
  }
  if (destination != impl_->destination) {
    error = "native branch island destination cannot change";
    return false;
  }
  for (const auto& site : impl_->sites) {
    if (*reinterpret_cast<const std::uint32_t*>(site.address) != site.original) {
      error = "native call site was changed by another patch";
      return false;
    }
  }
  std::size_t done = 0;
  for (; done < impl_->sites.size(); ++done) {
    const auto& site = impl_->sites[done];
    if (!write_instruction(site.address, site.replacement)) {
      break;
    }
  }
  if (done != impl_->sites.size()) {
    bool reverted = true;
    for (const auto& site : impl_->sites) {
      if (*reinterpret_cast<const std::uint32_t*>(site.address) == site.replacement) {
        reverted &= write_instruction(site.address, site.original);
      }
      if (*reinterpret_cast<const std::uint32_t*>(site.address) == site.replacement) {
        impl_->installed = true;
      }
    }
    error = reverted ? "native code patch was rejected; original instructions restored"
                     : "native code patch failed and rollback was incomplete";
    return false;
  }
  impl_->installed = true;
  return true;
#else
  (void)destination;
  error = "native caller hooks are unavailable on this platform";
  return false;
#endif
}

bool NativeCallPatch::restore(std::string& error) {
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
  std::lock_guard<std::mutex> lock(patch_mutex);
  if (!impl_->installed) {
    return true;
  }
  std::vector<std::size_t> pending;
  for (std::size_t i = 0; i < impl_->sites.size(); ++i) {
    const auto& site = impl_->sites[i];
    const auto instruction = *reinterpret_cast<const std::uint32_t*>(site.address);
    if (instruction != site.original && instruction != site.replacement) {
      error = "native call site no longer belongs to this hook";
      return false;
    }
    if (instruction == site.replacement) {
      pending.push_back(i);
    }
  }
  std::size_t done = 0;
  for (; done < pending.size(); ++done) {
    const auto& site = impl_->sites[pending[done]];
    if (!write_instruction(site.address, site.original)) {
      break;
    }
  }
  if (done != pending.size()) {
    bool reverted = true;
    impl_->installed = false;
    for (auto index : pending) {
      const auto& site = impl_->sites[index];
      if (*reinterpret_cast<const std::uint32_t*>(site.address) == site.original) {
        reverted &= write_instruction(site.address, site.replacement);
      }
      if (*reinterpret_cast<const std::uint32_t*>(site.address) == site.replacement) {
        impl_->installed = true;
      }
    }
    error = reverted ? "cannot restore native call sites; existing hook retained"
                     : "native restore failed and rollback was incomplete";
    return false;
  }
  impl_->installed = false;
#else
  (void)error;
#endif
  return true;
}

bool NativeCallPatch::active() const {
  return impl_->installed;
}

std::vector<std::uintptr_t> NativeCallPatch::reference_sites(std::uintptr_t caller, std::uintptr_t callee) {
  std::string error;
  auto patch = prepare(caller, callee, error);
  std::vector<std::uintptr_t> result;
#ifdef CPP_REPL_NATIVE_CALL_PATCHES
  if (patch) for (const auto& site : patch->impl_->sites) result.push_back(site.address);
#endif
  return result;
}

NativeCallPatch::~NativeCallPatch() {
  std::string error;
  if (!restore(error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    // Do not unmap an island while a branch still points at it.
    impl_.release();
  }
}

} // namespace example
