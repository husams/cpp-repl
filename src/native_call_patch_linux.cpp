#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// clang-format off
#include "native_call_patch.hpp"
// Parse LLVM's ELF enums before link.h introduces conflicting system macros.
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include "frida-gum.h"
#include "gum_runtime.hpp"
// clang-format on

namespace example {
namespace {

std::mutex patch_mutex;

struct Range {
  std::uintptr_t begin;
  std::uintptr_t end;
};

bool contains(const std::vector<Range>& ranges, std::uintptr_t address, std::size_t size) {
  return std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
    return address >= range.begin && address <= range.end && size <= range.end - address;
  });
}

struct Decoder {
  csh handle = 0;
  cs_insn* instruction = nullptr;

  Decoder() {
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) == CS_ERR_OK) {
      cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
      instruction = cs_malloc(handle);
    }
  }

  ~Decoder() {
    if (instruction) {
      cs_free(instruction, 1);
    }
    if (handle) {
      cs_close(&handle);
    }
  }
};

bool got_target(const cs_insn& instruction, const std::vector<Range>& got, std::uintptr_t callee) {
  const auto& x86 = instruction.detail->x86;
  if (x86.op_count != 1 || x86.operands[0].type != X86_OP_MEM) {
    return false;
  }
  const auto& memory = x86.operands[0].mem;
  if (memory.base != X86_REG_RIP || memory.index != X86_REG_INVALID ||
      memory.segment != X86_REG_INVALID) {
    return false;
  }
  const auto pointer = instruction.address + instruction.size + memory.disp;
  if (!contains(got, pointer, sizeof(std::uintptr_t)) ||
      !gum_memory_is_readable(reinterpret_cast<void*>(pointer), sizeof(std::uintptr_t))) {
    return false;
  }
  std::uintptr_t destination;
  std::memcpy(&destination, reinterpret_cast<void*>(pointer), sizeof(destination));
  return destination == callee;
}

bool branch_target(std::uintptr_t address, std::uintptr_t callee, const std::vector<Range>& plt,
                   const std::vector<Range>& got) {
  if (address == callee) {
    return true;
  }
  if (!contains(plt, address, 1)) {
    return false;
  }
  Decoder decoder;
  if (!decoder.instruction) {
    return false;
  }
  auto* code = reinterpret_cast<const std::uint8_t*>(address);
  std::size_t size = 0;
  for (const auto& range : plt) {
    if (address >= range.begin && address < range.end) {
      size = std::min<std::size_t>(32, range.end - address);
    }
  }
  std::uint64_t pc = address;
  // CET PLT entries may start with ENDBR64 or padding before their GOT jump.
  for (unsigned n = 0;
       n < 3 && cs_disasm_iter(decoder.handle, &code, &size, &pc, decoder.instruction); ++n) {
    const auto& instruction = *decoder.instruction;
    if (instruction.id == X86_INS_JMP) {
      return got_target(instruction, got, callee);
    }
    if (instruction.id != X86_INS_ENDBR64 && instruction.id != X86_INS_NOP) {
      return false;
    }
  }
  return false;
}

bool function_bounds(std::uintptr_t caller, std::uintptr_t& end, std::vector<Range>& plt,
                     std::vector<Range>& got, std::string& error) {
  Dl_info info{};
  void* extra = nullptr;
  if (!dladdr1(reinterpret_cast<void*>(caller), &info, &extra, RTLD_DL_SYMENT) || !info.dli_fbase ||
      reinterpret_cast<std::uintptr_t>(info.dli_saddr) != caller || !extra) {
    error = "native caller requires an exact ELF dynamic function symbol";
    return false;
  }
  const auto* symbol = static_cast<const Elf64_Sym*>(extra);
  if (ELF64_ST_TYPE(symbol->st_info) != STT_FUNC || !symbol->st_size ||
      symbol->st_size > (1u << 26)) {
    error = "native caller requires a bounded ELF function symbol";
    return false;
  }
  end = caller + symbol->st_size;
  if (end <= caller || !gum_memory_is_readable(reinterpret_cast<void*>(caller), symbol->st_size)) {
    error = "native caller's ELF function range is unreadable";
    return false;
  }
  auto binary = llvm::object::ObjectFile::createObjectFile(info.dli_fname);
  if (!binary) {
    error = "cannot read native caller's ELF metadata: " + llvm::toString(binary.takeError());
    return false;
  }
  const auto* elf = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(binary->getBinary());
  if (!elf || elf->getEMachine() != EM_X86_64) {
    error = "native caller must be in an x86_64 ELF image";
    return false;
  }
  const auto bias =
      elf->getEType() == ET_DYN ? reinterpret_cast<std::uintptr_t>(info.dli_fbase) : 0;
  for (const auto& section : elf->sections()) {
    auto name = section.getName();
    if (!name) {
      error = llvm::toString(name.takeError());
      return false;
    }
    const Range range{bias + section.getAddress(), bias + section.getAddress() + section.getSize()};
    if (*name == ".plt" || *name == ".plt.sec" || *name == ".plt.got" || *name == ".iplt") {
      plt.push_back(range);
    }
    if (*name == ".got" || *name == ".got.plt") {
      got.push_back(range);
    }
  }
  return true;
}

bool write_bytes(std::uintptr_t address, const std::vector<std::uint8_t>& bytes) {
  return gum_memory_patch_code(
      reinterpret_cast<void*>(address), bytes.size(),
      [](gpointer memory, gpointer data) {
        const auto& code = *static_cast<const std::vector<std::uint8_t>*>(data);
        std::memcpy(memory, code.data(), code.size());
      },
      const_cast<std::vector<std::uint8_t>*>(&bytes));
}

bool matches(std::uintptr_t address, const std::vector<std::uint8_t>& bytes) {
  return std::memcmp(reinterpret_cast<void*>(address), bytes.data(), bytes.size()) == 0;
}

} // namespace

struct NativeCallPatch::Impl {
  struct Site {
    std::uintptr_t address;
    std::vector<std::uint8_t> original;
    std::vector<std::uint8_t> replacement;
    bool call;
    std::size_t relative_offset;
  };

  std::vector<Site> sites;
  std::uintptr_t caller = 0;
  std::size_t length = 0;
  void* island = nullptr;
  std::uintptr_t destination = 0;
  bool installed = false;

  ~Impl() {
    if (island) {
      gum_memory_free(island, gum_query_page_size());
    }
  }
};

NativeCallPatch::NativeCallPatch(ConstructionKey, std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

bool NativeCallPatch::is_native_address(std::uintptr_t address) {
  Dl_info info{};
  return dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fbase;
}

std::shared_ptr<NativeCallPatch>
NativeCallPatch::prepare(std::uintptr_t caller, std::uintptr_t callee, std::string& error) {
  initialize_gum();
  std::lock_guard<std::mutex> lock(patch_mutex);
  auto impl = std::make_unique<Impl>();
  std::uintptr_t end = 0;
  std::vector<Range> plt, got;
  if (!function_bounds(caller, end, plt, got, error)) {
    return nullptr;
  }
  if (caller == callee) {
    error = "native caller and target must be distinct functions";
    return nullptr;
  }
  if (auto* module = gum_process_find_module_by_address(caller)) {
    struct Aliases {
      std::uintptr_t address;
      unsigned count = 0;
    } aliases{caller};

    gum_module_enumerate_exports(
        module,
        [](const GumExportDetails* details, gpointer data) -> gboolean {
          auto& found = *static_cast<Aliases*>(data);
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
  Decoder decoder;
  if (!decoder.instruction) {
    error = "cannot initialize the x86_64 instruction decoder";
    return nullptr;
  }
  auto* code = reinterpret_cast<const std::uint8_t*>(caller);
  std::size_t remaining = end - caller;
  std::uint64_t pc = caller;
  while (remaining) {
    if (!cs_disasm_iter(decoder.handle, &code, &remaining, &pc, decoder.instruction)) {
      error = "cannot decode the complete native caller; no calls were changed";
      return nullptr;
    }
    const auto& instruction = *decoder.instruction;
    const bool call = instruction.id == X86_INS_CALL;
    const bool jump = instruction.id == X86_INS_JMP;
    const bool other_jump = cs_insn_group(decoder.handle, &instruction, CS_GRP_JUMP);
    if (!call && !jump && !other_jump) {
      continue;
    }
    const auto& x86 = instruction.detail->x86;
    if (x86.op_count != 1) {
      continue;
    }
    const bool direct =
        x86.operands[0].type == X86_OP_IMM && branch_target(x86.operands[0].imm, callee, plt, got);
    const bool through_got = got_target(instruction, got, callee);
    if (!direct && !through_got) {
      continue;
    }
    if ((!call && !jump) || instruction.size < 5 ||
        (direct &&
         (x86.encoding.imm_size != 4 || x86.encoding.imm_offset + 4 != instruction.size)) ||
        (through_got && (instruction.size != 6 || instruction.bytes[0] != 0xff))) {
      error = "native caller uses an unsupported short or conditional branch to the target";
      return nullptr;
    }
    std::vector<std::uint8_t> original(instruction.bytes, instruction.bytes + instruction.size);
    impl->sites.push_back({instruction.address,
                           std::move(original),
                           {},
                           call,
                           direct ? x86.encoding.imm_offset : std::size_t(0)});
  }
  if (impl->sites.empty()) {
    error = "native caller has no supported native call to the target; inlined and dynamic "
            "indirect calls are unsupported";
    return nullptr;
  }
  impl->caller = caller;
  impl->length = end - caller;
  return std::make_shared<NativeCallPatch>(ConstructionKey{}, std::move(impl));
}

bool NativeCallPatch::install(std::uintptr_t destination, std::string& error) {
  std::lock_guard<std::mutex> lock(patch_mutex);
  if (impl_->installed) {
    for (const auto& site : impl_->sites) {
      if (!matches(site.address, site.replacement)) {
        error = "native patch is incomplete or changed; restore it before retrying";
        return false;
      }
    }
    return true;
  }
  if (!impl_->island) {
    const GumAddressSpec spec{reinterpret_cast<void*>(impl_->caller),
                              std::size_t(std::numeric_limits<std::int32_t>::max()) -
                                  impl_->length - 16};
    impl_->island =
        gum_memory_allocate_near(&spec, gum_query_page_size(), gum_query_page_size(), GUM_PAGE_RW);
    if (!impl_->island) {
      error = "cannot allocate a branch island within x86_64 rel32 reach";
      return false;
    }
    // JMP [RIP+0]; address. No argument register or variadic AL is clobbered.
    const std::array<std::uint8_t, 6> jump{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(impl_->island, jump.data(), jump.size());
    std::memcpy(static_cast<std::uint8_t*>(impl_->island) + jump.size(), &destination,
                sizeof(destination));
    if (!gum_try_mprotect(impl_->island, gum_query_page_size(), GUM_PAGE_RX)) {
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
          static_cast<std::int64_t>(site.address +
                                    (site.relative_offset ? site.original.size() : 5));
      if (displacement < std::numeric_limits<std::int32_t>::min() ||
          displacement > std::numeric_limits<std::int32_t>::max()) {
        error = "native branch island is out of reach";
        gum_memory_free(impl_->island, gum_query_page_size());
        impl_->island = nullptr;
        return false;
      }
      const auto relative = static_cast<std::int32_t>(displacement);
      if (site.relative_offset) {
        site.replacement = site.original;
        std::memcpy(site.replacement.data() + site.relative_offset, &relative, sizeof(relative));
      } else {
        site.replacement.assign(site.original.size(), 0x90);
        site.replacement[0] = site.call ? 0xe8 : 0xe9;
        std::memcpy(site.replacement.data() + 1, &relative, sizeof(relative));
      }
    }
  }
  if (destination != impl_->destination) {
    error = "native branch island destination cannot change";
    return false;
  }
  for (const auto& site : impl_->sites) {
    if (!matches(site.address, site.original)) {
      error = "native call site was changed by another patch";
      return false;
    }
  }
  for (const auto& site : impl_->sites) {
    if (write_bytes(site.address, site.replacement)) {
      continue;
    }
    bool reverted = true;
    for (const auto& previous : impl_->sites) {
      if (matches(previous.address, previous.replacement)) {
        reverted &= write_bytes(previous.address, previous.original);
      }
      if (matches(previous.address, previous.replacement)) {
        impl_->installed = true;
      }
    }
    error = reverted ? "native code patch was rejected; original instructions restored"
                     : "native code patch failed and rollback was incomplete";
    return false;
  }
  impl_->installed = true;
  return true;
}

bool NativeCallPatch::restore(std::string& error) {
  std::lock_guard<std::mutex> lock(patch_mutex);
  if (!impl_->installed) {
    return true;
  }
  std::vector<std::size_t> pending;
  for (std::size_t i = 0; i < impl_->sites.size(); ++i) {
    const auto& site = impl_->sites[i];
    if (matches(site.address, site.replacement)) {
      pending.push_back(i);
    } else if (!matches(site.address, site.original)) {
      error = "native call site no longer belongs to this hook";
      return false;
    }
  }
  for (auto index : pending) {
    const auto& site = impl_->sites[index];
    if (write_bytes(site.address, site.original)) {
      continue;
    }
    bool reverted = true;
    impl_->installed = false;
    for (auto previous_index : pending) {
      const auto& previous = impl_->sites[previous_index];
      if (matches(previous.address, previous.original)) {
        reverted &= write_bytes(previous.address, previous.replacement);
      }
      if (matches(previous.address, previous.replacement)) {
        impl_->installed = true;
      }
    }
    error = reverted ? "cannot restore native call sites; existing hook retained"
                     : "native restore failed and rollback was incomplete";
    return false;
  }
  impl_->installed = false;
  return true;
}

bool NativeCallPatch::active() const {
  return impl_->installed;
}

std::vector<std::uintptr_t> NativeCallPatch::reference_sites(std::uintptr_t caller, std::uintptr_t callee) {
  std::string error;
  auto patch = prepare(caller, callee, error);
  std::vector<std::uintptr_t> result;
  if (patch) for (const auto& site : patch->impl_->sites) result.push_back(site.address);
  return result;
}

NativeCallPatch::~NativeCallPatch() {
  std::string error;
  if (!restore(error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    impl_.release();
  }
}

} // namespace example
