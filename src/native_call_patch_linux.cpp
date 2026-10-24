#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// clang-format off
#include "native_call_patch.hpp"
// Parse LLVM's ELF enums before link.h introduces conflicting system macros.
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm-c/Disassembler.h"
#include "llvm-c/Target.h"

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

#include "native_runtime.hpp"
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

struct Instruction {
  std::uintptr_t address = 0, target = 0;
  std::size_t size = 0, relative_offset = 0;
  std::array<std::uint8_t, 15> bytes{};
  bool call = false, jump = false, conditional = false, rip_relative = false;
  bool padding = false;
};

struct Decoder {
  LLVMDisasmContextRef handle = nullptr;
  Instruction instruction;

  Decoder() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
      LLVMInitializeNativeTarget();
      LLVMInitializeNativeDisassembler();
      LLVMInitializeNativeAsmPrinter();
    });
    handle = LLVMCreateDisasm("x86_64-pc-linux-gnu", nullptr, 0, nullptr, nullptr);
  }

  ~Decoder() {
    if (handle) {
      LLVMDisasmDispose(handle);
    }
  }

  bool next(const std::uint8_t*& code, std::size_t& remaining, std::uint64_t& pc) {
    char text[256];
    if (!handle) {
      return false;
    }
    const auto length = LLVMDisasmInstruction(handle, const_cast<std::uint8_t*>(code), remaining,
                                              pc, text, sizeof(text));
    if (!length || length > instruction.bytes.size()) {
      return false;
    }
    instruction = {};
    auto& item = instruction;
    item.address = pc;
    item.size = length;
    std::memcpy(item.bytes.data(), code, length);
    std::size_t opcode = 0;
    // Decode boundaries with LLVM; recognize only the branch forms we can patch.
    while (opcode + 1 < length &&
           (code[opcode] == 0xf2 || code[opcode] == 0xf3 || code[opcode] == 0x66 ||
            code[opcode] == 0x2e || code[opcode] == 0x3e)) {
      ++opcode;
    }
    const auto byte = code[opcode];
    std::size_t displacement_size = 0;
    if ((byte == 0xe8 || byte == 0xe9) && length == opcode + 5) {
      item.call = byte == 0xe8;
      item.jump = byte == 0xe9;
      item.relative_offset = opcode + 1;
      displacement_size = 4;
    } else if (byte == 0xeb || (byte >= 0x70 && byte <= 0x7f) || (byte >= 0xe0 && byte <= 0xe3)) {
      item.jump = byte == 0xeb;
      item.conditional = !item.jump;
      item.relative_offset = opcode + 1;
      displacement_size = 1;
    } else if (byte == 0x0f && opcode + 1 < length && code[opcode + 1] >= 0x80 &&
               code[opcode + 1] <= 0x8f) {
      item.conditional = true;
      item.relative_offset = opcode + 2;
      displacement_size = 4;
    } else if (byte == 0xff && length == opcode + 6 &&
               (code[opcode + 1] == 0x15 || code[opcode + 1] == 0x25)) {
      item.call = code[opcode + 1] == 0x15;
      item.jump = !item.call;
      item.rip_relative = true;
      displacement_size = 4;
    }
    if (displacement_size && displacement_size <= length) {
      std::int64_t displacement;
      if (displacement_size == 4) {
        std::int32_t relative;
        std::memcpy(&relative, code + length - 4, 4);
        displacement = relative;
      } else {
        displacement = static_cast<std::int8_t>(code[length - 1]);
      }
      item.target = pc + length + displacement;
    }
    item.padding = byte == 0x90 ||
                   (byte == 0x0f && opcode + 1 < length && code[opcode + 1] == 0x1f) ||
                   (length == 4 && std::memcmp(code, "\xf3\x0f\x1e\xfa", 4) == 0);
    code += length;
    remaining -= length;
    pc += length;
    return true;
  }
};

bool got_target(const Instruction& instruction, const std::vector<Range>& got,
                std::uintptr_t callee) {
  if (!instruction.rip_relative) {
    return false;
  }
  const auto pointer = instruction.target;
  if (!contains(got, pointer, sizeof(std::uintptr_t)) ||
      !memory_readable(reinterpret_cast<void*>(pointer), sizeof(std::uintptr_t))) {
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
  if (!decoder.handle) {
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
  for (unsigned n = 0; n < 3 && decoder.next(code, size, pc); ++n) {
    const auto& instruction = decoder.instruction;
    if (instruction.jump) {
      return got_target(instruction, got, callee);
    }
    if (!instruction.padding) {
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
  if (end <= caller || !memory_readable(reinterpret_cast<void*>(caller), symbol->st_size)) {
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
  return patch_code(address, bytes.data(), bytes.size());
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
      free_code(island);
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
  if (native_function_has_aliases(caller)) {
    error = "native caller shares its code address with another exported symbol";
    return nullptr;
  }
  Decoder decoder;
  if (!decoder.handle) {
    error = "cannot initialize the x86_64 instruction decoder";
    return nullptr;
  }
  auto* code = reinterpret_cast<const std::uint8_t*>(caller);
  std::size_t remaining = end - caller;
  std::uint64_t pc = caller;
  while (remaining) {
    if (!decoder.next(code, remaining, pc)) {
      error = "cannot decode the complete native caller; no calls were changed";
      return nullptr;
    }
    const auto& instruction = decoder.instruction;
    const bool call = instruction.call;
    const bool jump = instruction.jump;
    const bool other_jump = instruction.conditional;
    if (!call && !jump && !other_jump) {
      continue;
    }
    const bool direct =
        instruction.relative_offset && branch_target(instruction.target, callee, plt, got);
    const bool through_got = got_target(instruction, got, callee);
    if (!direct && !through_got) {
      continue;
    }
    if ((!call && !jump) || instruction.size < 5 ||
        (direct && instruction.relative_offset + 4 != instruction.size) ||
        (through_got && (instruction.size != 6 || instruction.bytes[0] != 0xff))) {
      error = "native caller uses an unsupported short or conditional branch to the target";
      return nullptr;
    }
    std::vector<std::uint8_t> original(instruction.bytes.begin(),
                                       instruction.bytes.begin() + instruction.size);
    impl->sites.push_back({instruction.address,
                           std::move(original),
                           {},
                           call,
                           direct ? instruction.relative_offset : std::size_t(0)});
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
    impl_->island = allocate_code_near(
        impl_->caller, std::size_t(std::numeric_limits<std::int32_t>::max()) - impl_->length - 16);
    if (!impl_->island) {
      error = "cannot allocate a branch island within x86_64 rel32 reach";
      return false;
    }
    // JMP [RIP+0]; address. No argument register or variadic AL is clobbered.
    const std::array<std::uint8_t, 6> jump{0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(impl_->island, jump.data(), jump.size());
    std::memcpy(static_cast<std::uint8_t*>(impl_->island) + jump.size(), &destination,
                sizeof(destination));
    if (!make_code_executable(impl_->island)) {
      error = "cannot make the native branch island executable";
      free_code(impl_->island);
      impl_->island = nullptr;
      return false;
    }
    impl_->destination = destination;
    for (auto& site : impl_->sites) {
      const auto displacement =
          static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(impl_->island)) -
          static_cast<std::int64_t>(site.address +
                                    (site.relative_offset ? site.original.size() : 5));
      if (displacement < std::numeric_limits<std::int32_t>::min() ||
          displacement > std::numeric_limits<std::int32_t>::max()) {
        error = "native branch island is out of reach";
        free_code(impl_->island);
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

std::vector<std::uintptr_t> NativeCallPatch::reference_sites(std::uintptr_t caller,
                                                             std::uintptr_t callee) {
  std::string error;
  auto patch = prepare(caller, callee, error);
  std::vector<std::uintptr_t> result;
  if (patch) {
    for (const auto& site : patch->impl_->sites) {
      result.push_back(site.address);
    }
  }
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
