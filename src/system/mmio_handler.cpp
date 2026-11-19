/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#if defined(__linux__)
#include <execinfo.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <utility>

#include <rex/assert.h>
#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>
#include <rex/system/mmio_handler.h>
#include <rex/types.h>

using namespace rex::arch;

namespace rex::runtime {

MMIOHandler* MMIOHandler::global_handler_ = nullptr;

namespace {

// Bounded diagnostics for undecodable MMIO instructions.
//
// A guest thread whose fault is not handled re-executes the faulting
// instruction indefinitely, so logging once per fault produced ~1M identical
// lines for a single RIP (cycle 10 Phase A, thid 9 / Bink). Log at most one
// line per distinct faulting RIP, and report the running failure total in that
// line as the summary counter.
struct DecodeFailureLog {
  std::mutex mutex;
  std::unordered_set<uint64_t> logged_rips;
  uint64_t total = 0;
};

DecodeFailureLog& GetDecodeFailureLog() {
  static DecodeFailureLog log;
  return log;
}

// Logs at most one bounded diagnostic per distinct faulting RIP for an MMIO
// instruction the handler declines, with the running failure total. A declined
// instruction is re-executed by the guest thread, so this must not log once
// per fault. `what` names the failure (the message shape is shared by the
// decode-failure and the unsupported-store paths).
void LogMmioDecline(const uint8_t* rip, uint32_t guest_ea, const char* what) {
  auto& failure_log = GetDecodeFailureLog();
  std::lock_guard<std::mutex> failure_lock(failure_log.mutex);
  ++failure_log.total;
  if (failure_log.logged_rips.insert(uint64_t(rip)).second) {
    REXLOG_ERROR(
        "{} at {:p} (guest EA {:08X}, failure #{}; further failures at this RIP are suppressed)",
        what, static_cast<const void*>(rip), guest_ea, failure_log.total);
  }
}

}  // namespace

std::unique_ptr<MMIOHandler> MMIOHandler::Install(uint8_t* virtual_membase,
                                                  uint8_t* physical_membase, uint8_t* membase_end,
                                                  HostToGuestVirtual host_to_guest_virtual,
                                                  const void* host_to_guest_virtual_context,
                                                  AccessViolationCallback access_violation_callback,
                                                  void* access_violation_callback_context) {
  // There can be only one handler at a time.
  assert_null(global_handler_);
  if (global_handler_) {
    return nullptr;
  }

  auto handler = std::unique_ptr<MMIOHandler>(new MMIOHandler(
      virtual_membase, physical_membase, membase_end, host_to_guest_virtual,
      host_to_guest_virtual_context, access_violation_callback, access_violation_callback_context));

  // Install exception handler for memory coherence (SharedMemory write tracking).
  // Note: MMIO operations are handled at the recompiler level via PPC_MM_LOAD/STORE
  // macros that call CheckLoad/CheckStore directly.
  arch::ExceptionHandler::Install(ExceptionCallbackThunk, handler.get());

  global_handler_ = handler.get();
  return handler;
}

MMIOHandler::MMIOHandler(uint8_t* virtual_membase, uint8_t* physical_membase, uint8_t* membase_end,
                         HostToGuestVirtual host_to_guest_virtual,
                         const void* host_to_guest_virtual_context,
                         AccessViolationCallback access_violation_callback,
                         void* access_violation_callback_context)
    : virtual_membase_(virtual_membase),
      physical_membase_(physical_membase),
      memory_end_(membase_end),
      host_to_guest_virtual_(host_to_guest_virtual),
      host_to_guest_virtual_context_(host_to_guest_virtual_context),
      access_violation_callback_(access_violation_callback),
      access_violation_callback_context_(access_violation_callback_context) {}

MMIOHandler::~MMIOHandler() {
  arch::ExceptionHandler::Uninstall(ExceptionCallbackThunk, this);

  if (global_handler_ == this) {
    global_handler_ = nullptr;
  } else {
    REXLOG_ERROR("~MMIOHandler: global_handler_ does not match this instance");
  }
}

bool MMIOHandler::RegisterRange(uint32_t virtual_address, uint32_t mask, uint32_t size,
                                void* context, MMIOReadCallback read_callback,
                                MMIOWriteCallback write_callback) {
  mapped_ranges_.push_back({
      virtual_address,
      mask,
      size,
      context,
      read_callback,
      write_callback,
  });
  return true;
}

MMIORange* MMIOHandler::LookupRange(uint32_t virtual_address) {
  for (auto& range : mapped_ranges_) {
    if ((virtual_address & range.mask) == range.address) {
      return &range;
    }
  }
  return nullptr;
}

bool MMIOHandler::CheckLoad(uint32_t virtual_address, uint32_t* out_value) {
  for (const auto& range : mapped_ranges_) {
    if ((virtual_address & range.mask) == range.address) {
      *out_value =
          static_cast<uint32_t>(range.read(nullptr, range.callback_context, virtual_address));
      return true;
    }
  }
  return false;
}

bool MMIOHandler::CheckStore(uint32_t virtual_address, uint32_t value) {
  for (const auto& range : mapped_ranges_) {
    if ((virtual_address & range.mask) == range.address) {
      range.write(nullptr, range.callback_context, virtual_address, value);
      return true;
    }
  }
  return false;
}

#if REX_ARCH_AMD64

namespace {

// Scans the optional legacy operand-size prefix (0x66) and the optional REX
// prefix (they precede the opcode in this order in x86-64) and returns the
// offset of the first opcode byte. `operand_size_66_out` reports whether the
// 0x66 prefix was present.
uint8_t ScanX64Prefixes(const uint8_t* p, bool* operand_size_66_out) {
  uint8_t i = 0;
  *operand_size_66_out = false;
  if (p[i] == 0x66) {
    *operand_size_66_out = true;
    ++i;
  }
  if ((p[i] & 0xF0) == 0x40) {
    ++i;
  }
  return i;
}

// True for the 66 [REX] 0F 6F /r form - MOVDQA xmm, xmm/m128, the aligned
// 16-byte load. TryDecodeLoadStore accepts exactly this form; the execution
// path in ExceptionCallback uses this predicate to select the four-read
// 128-bit load handling. Only with the 0x66 operand-size prefix is 0F 6F a
// 128-bit XMM load; the F2/F3-prefixed (MOVDQU) forms are intentionally
// declined.
bool IsMovdqaLoad(const uint8_t* p) {
  bool operand_size_66 = false;
  uint8_t i = ScanX64Prefixes(p, &operand_size_66);
  return operand_size_66 && p[i] == 0x0F && p[i + 1] == 0x6F;
}

// True for the 66 [REX] 0F 7F /r form - MOVDQA xmm/m128, xmm, the aligned
// 16-byte store. TryDecodeLoadStore accepts exactly this form; the execution
// path in ExceptionCallback uses this predicate to select the four-write
// 128-bit store handling. Only with the 0x66 operand-size prefix is 0F 7F a
// 128-bit XMM store; the F2/F3-prefixed (MOVDQU) forms are intentionally
// declined.
bool IsMovdqaStore(const uint8_t* p) {
  bool operand_size_66 = false;
  uint8_t i = ScanX64Prefixes(p, &operand_size_66);
  return operand_size_66 && p[i] == 0x0F && p[i + 1] == 0x7F;
}

}  // namespace

#endif  // REX_ARCH_AMD64

bool MMIOHandler::TryDecodeLoadStore(const uint8_t* p, DecodedLoadStore& decoded_out) {
  std::memset(&decoded_out, 0, sizeof(decoded_out));
#if REX_ARCH_AMD64
  bool operand_size_66 = false;
  uint8_t i = ScanX64Prefixes(p, &operand_size_66);
  uint8_t rex = 0;
  if (i > 0 && (p[i - 1] & 0xF0) == 0x40) {
    rex = p[i - 1];
  }
  if (operand_size_66) {
    if (p[i] == 0x0F && p[i + 1] == 0x6F) {
      // 66 [REX] 0F 6F /r: MOVDQA xmm, xmm/m128 (aligned 16-byte load).
      // Decoded with the generic ModRM/SIB machinery below; `value_reg` is the
      // XMM destination index and the execution path performs four ordered
      // 32-bit MMIO reads for it (see ExceptionCallback).
      decoded_out.is_load = true;
      decoded_out.byte_swap = false;
      i += 2;
    } else if (p[i] == 0x0F && p[i + 1] == 0x7F) {
      // 66 [REX] 0F 7F /r: MOVDQA xmm/m128, xmm (aligned 16-byte store).
      // Decoded with the generic ModRM/SIB machinery below; `value_reg` is the
      // source XMM index and the execution path performs four ordered 32-bit
      // MMIO writes for it (see ExceptionCallback). The length comes from the
      // consumed bytes, so encodings that carry a displacement count it (for
      // example mod 10 / SIB base 100 is 10 bytes) while the captured
      // `66 41 0F 7F 04 04` (mod 00, SIB base 100 -> r12) has no displacement
      // and is 6 bytes.
      decoded_out.is_load = false;
      decoded_out.byte_swap = false;
      i += 2;
    } else {
      // No other 0x66-prefixed form is supported.
      return false;
    }
  } else if (p[i] == 0x0F && p[i + 1] == 0x38 && p[i + 2] == 0xF1) {
    // MOVBE m32, r32 (store)
    // https://web.archive.org/web/20170629091435/https://www.tptp.cc/mirrors/siyobik.info/instruction/MOVBE.html
    // 44 0f 38 f1 a4 02 00     movbe  DWORD PTR [rdx+rax*1+0x0],r12d
    // 42 0f 38 f1 8c 22 00     movbe  DWORD PTR [rdx+r12*1+0x0],ecx
    // 0f 38 f1 8c 02 00 00     movbe  DWORD PTR [rdx + rax * 1 + 0x0], ecx
    decoded_out.is_load = false;
    decoded_out.byte_swap = true;
    i += 3;
  } else if (p[i] == 0x0F && p[i + 1] == 0x38 && p[i + 2] == 0xF0) {
    // MOVBE r32, m32 (load)
    // https://web.archive.org/web/20170629091435/https://www.tptp.cc/mirrors/siyobik.info/instruction/MOVBE.html
    // 44 0f 38 f0 a4 02 00     movbe  r12d,DWORD PTR [rdx+rax*1+0x0]
    // 42 0f 38 f0 8c 22 00     movbe  ecx,DWORD PTR [rdx+r12*1+0x0]
    // 46 0f 38 f0 a4 22 00     movbe  r12d,DWORD PTR [rdx+r12*1+0x0]
    // 0f 38 f0 8c 02 00 00     movbe  ecx,DWORD PTR [rdx+rax*1+0x0]
    // 0F 38 F0 1C 02           movbe  ebx,dword ptr [rdx+rax]
    decoded_out.is_load = true;
    decoded_out.byte_swap = true;
    i += 3;
  } else if (p[i] == 0x89) {
    // MOV m32, r32 (store)
    // https://web.archive.org/web/20170629072136/https://www.tptp.cc/mirrors/siyobik.info/instruction/MOV.html
    // 44 89 24 02              mov  DWORD PTR[rdx + rax * 1], r12d
    // 42 89 0c 22              mov  DWORD PTR[rdx + r12 * 1], ecx
    // 89 0c 02                 mov  DWORD PTR[rdx + rax * 1], ecx
    decoded_out.is_load = false;
    decoded_out.byte_swap = false;
    ++i;
  } else if (p[i] == 0x8B) {
    // MOV r32, m32 (load)
    // https://web.archive.org/web/20170629072136/https://www.tptp.cc/mirrors/siyobik.info/instruction/MOV.html
    // 44 8b 24 02              mov  r12d, DWORD PTR[rdx + rax * 1]
    // 42 8b 0c 22              mov  ecx, DWORD PTR[rdx + r12 * 1]
    // 46 8b 24 22              mov  r12d, DWORD PTR[rdx + r12 * 1]
    // 8b 0c 02                 mov  ecx, DWORD PTR[rdx + rax * 1]
    decoded_out.is_load = true;
    decoded_out.byte_swap = false;
    ++i;
  } else if (p[i] == 0xC7) {
    // MOV m32, simm32
    // https://web.archive.org/web/20161017042413/https://www.asmpedia.org/index.php?title=MOV
    // C7 04 02 02 00 00 00     mov  dword ptr [rdx+rax],2
    decoded_out.is_load = false;
    decoded_out.byte_swap = false;
    decoded_out.is_constant = true;
    ++i;
  } else {
    return false;
  }

  uint8_t rex_b = rex & 0b0001;
  uint8_t rex_x = rex & 0b0010;
  uint8_t rex_r = rex & 0b0100;
  uint8_t rex_w = rex & 0b1000;

  // http://www.sandpile.org/x86/opc_rm.htm
  // http://www.sandpile.org/x86/opc_sib.htm
  uint8_t modrm = p[i++];
  uint8_t mod = (modrm & 0b11000000) >> 6;
  uint8_t reg = (modrm & 0b00111000) >> 3;
  uint8_t rm = (modrm & 0b00000111);
  decoded_out.value_reg = reg + (rex_r ? 8 : 0);
  decoded_out.mem_has_base = false;
  decoded_out.mem_base_reg = 0;
  decoded_out.mem_has_index = false;
  decoded_out.mem_index_reg = 0;
  decoded_out.mem_scale = 1;
  decoded_out.mem_displacement = 0;
  // The decoded EA fields (base/index/scale/displacement) are informational on
  // x86-64: ExceptionCallback uses the real fault address and consumes only
  // `length` (for the resume PC), the direction, the value register and the
  // byte-swap flag. They are still decoded exactly because the AArch64 path
  // and future forms rely on the contract, and `length` must count every
  // consumed byte for the resume PC to be correct.
  bool has_sib = false;
  switch (rm) {
    case 0b100:  // SIB
      has_sib = true;
      break;
    case 0b101:
      if (mod == 0b00) {
        // RIP-relative not supported.
        return false;
      }
      decoded_out.mem_has_base = true;
      decoded_out.mem_base_reg = rm + (rex_b ? 8 : 0);
      break;
    default:
      decoded_out.mem_has_base = true;
      decoded_out.mem_base_reg = rm + (rex_b ? 8 : 0);
      break;
  }
  // SIB base 101 means "no base" only when mod == 00 (the absolute
  // [index * scale + disp32] form, which always carries a 32-bit
  // displacement). With mod != 00 it is a real base register: rbp, or r13
  // with REX.B=1.
  bool sib_no_base_disp32 = false;
  if (has_sib) {
    uint8_t sib = p[i++];
    decoded_out.mem_scale = 1 << ((sib & 0b11000000) >> 6);
    uint8_t sib_index = (sib & 0b00111000) >> 3;
    uint8_t sib_base = (sib & 0b00000111);
    switch (sib_index) {
      case 0b100:
        // No index.
        break;
      default:
        decoded_out.mem_has_index = true;
        decoded_out.mem_index_reg = sib_index + (rex_x ? 8 : 0);
        decoded_out.mem_index_size = sizeof(uint64_t);
        break;
    }
    if (sib_base == 0b101 && mod == 0b00) {
      // [index * scale + disp32]: no base register, disp32 follows.
      sib_no_base_disp32 = true;
    } else {
      decoded_out.mem_has_base = true;
      decoded_out.mem_base_reg = sib_base + (rex_b ? 8 : 0);
    }
  }
  switch (mod) {
    case 0b00: {
      if (sib_no_base_disp32) {
        // The no-base SIB form always carries a 32-bit displacement.
        decoded_out.mem_displacement += memory::load<int32_t>(p + i);
        i += 4;
      } else {
        decoded_out.mem_displacement += 0;
      }
    } break;
    case 0b01: {
      decoded_out.mem_displacement += int8_t(p[i++]);
    } break;
    case 0b10: {
      decoded_out.mem_displacement += memory::load<int32_t>(p + i);
      i += 4;
    } break;
  }
  if (decoded_out.is_constant) {
    decoded_out.constant = memory::load<int32_t>(p + i);
    i += 4;
  }
  decoded_out.length = i;
  return true;

#elif REX_ARCH_ARM64
  decoded_out.length = sizeof(uint32_t);
  uint32_t instruction = *reinterpret_cast<const uint32_t*>(p);

  // Literal loading (PC-relative) is not handled.

  if ((instruction & kArm64LoadStoreAnyFMask) != kArm64LoadStoreAnyFixed) {
    // Not a load or a store instruction.
    return false;
  }

  if ((instruction & kArm64LoadStorePairAnyFMask) == kArm64LoadStorePairAnyFixed) {
    // Handling MMIO only for single 32-bit values, not for pairs.
    return false;
  }

  uint8_t value_reg_base;
  switch (Arm64LoadStoreOp(instruction & kArm64LoadStoreMask)) {
    case Arm64LoadStoreOp::kSTR_w:
      decoded_out.is_load = false;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kLDR_w:
      decoded_out.is_load = true;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kSTR_s:
      decoded_out.is_load = false;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kLDR_s:
      decoded_out.is_load = true;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    default:
      return false;
  }

  // `Rt` field (load / store register).
  decoded_out.value_reg = value_reg_base + (instruction & 31);
  if (decoded_out.is_load && decoded_out.value_reg == DecodedLoadStore::kArm64ValueRegZero) {
    // Zero constant rather than a register read.
    decoded_out.is_constant = true;
    decoded_out.constant = 0;
  }

  decoded_out.mem_has_base = true;
  // The base is Xn (for 0...30) or SP (for 31).
  // `Rn` field (first source register).
  decoded_out.mem_base_reg = (instruction >> 5) & 31;

  bool is_unsigned_offset =
      (instruction & kArm64LoadStoreUnsignedOffsetFMask) == kArm64LoadStoreUnsignedOffsetFixed;
  if (is_unsigned_offset) {
    // LDR|STR Wt|St, [Xn|SP{, #pimm}]
    uint32_t unsigned_offset = (instruction >> 10) & 4095;
    decoded_out.mem_displacement = ptrdiff_t(sizeof(uint32_t) * unsigned_offset);
  } else {
    Arm64LoadStoreOffsetFixed offset =
        Arm64LoadStoreOffsetFixed(instruction & kArm64LoadStoreOffsetFMask);
    int32_t signed_offset = int32_t(instruction << (32 - (9 + 12))) >> (32 - 9);
    switch (offset) {
      case Arm64LoadStoreOffsetFixed::kUnscaledOffset: {
        decoded_out.mem_displacement = signed_offset;
      } break;
      case Arm64LoadStoreOffsetFixed::kPostIndex: {
        decoded_out.mem_base_writeback = true;
        decoded_out.mem_base_writeback_offset = signed_offset;
      } break;
      case Arm64LoadStoreOffsetFixed::kPreIndex: {
        decoded_out.mem_base_writeback = true;
        decoded_out.mem_base_writeback_offset = signed_offset;
        decoded_out.mem_displacement = signed_offset;
      } break;
      case Arm64LoadStoreOffsetFixed::kRegisterOffset: {
        decoded_out.mem_index_reg = (instruction >> 16) & 31;
        if (decoded_out.mem_index_reg != DecodedLoadStore::kArm64RegZero) {
          decoded_out.mem_has_index = true;
          uint32_t extend_mode = (instruction >> 13) & 0b111;
          if (!(extend_mode & 0b010)) {
            return false;
          }
          decoded_out.mem_index_size = (extend_mode & 0b001) ? sizeof(uint64_t) : sizeof(uint32_t);
          decoded_out.mem_index_sign_extend = (extend_mode & 0b100) != 0;
          decoded_out.mem_scale = (instruction & (UINT32_C(1) << 12)) ? sizeof(uint32_t) : 1;
        }
      } break;
      default:
        return false;
    }
  }

  return true;

#else
#error TryDecodeLoadStore not implemented for the target CPU architecture.
  return false;
#endif  // REX_ARCH
}


// Diagnostic read watch (see ReadWatchArm in mmio_handler.h). Guest views of a physical range are
// made inaccessible; the first guest access logs the host PC and call chain, then restores access.
#if defined(__linux__)
namespace {
struct ReadWatch {
  uint8_t* views[3];
  size_t length;
  uint32_t physical_address;
  bool active;
};
std::mutex read_watch_mutex;
std::vector<ReadWatch> read_watches;
}  // namespace

void ReadWatchArm(uint8_t* virtual_membase, uint32_t physical_address, uint32_t length) {
  static bool warmed = [] {
#if !defined(__ANDROID__)
    void* frames[4];
    backtrace(frames, 4);
#endif
    return true;
  }();
  (void)warmed;
  size_t page = size_t(sysconf(_SC_PAGESIZE));
  if ((physical_address & (page - 1)) || physical_address < 0x1000) return;
  size_t len = (size_t(length) + page - 1) & ~(page - 1);
  ReadWatch w{{virtual_membase + 0xA0000000ull + physical_address,
               virtual_membase + 0xC0000000ull + physical_address,
               virtual_membase + 0xE0000000ull + physical_address - 0x1000},
              len, physical_address, true};
  std::lock_guard<std::mutex> lock(read_watch_mutex);
  for (uint8_t* v : w.views) mprotect(v, len, PROT_NONE);
  read_watches.push_back(w);
  REXLOG_INFO("[readwatch] armed phys={:08X} len={}", physical_address, length);
}

static bool ReadWatchOnFault(arch::Exception* ex) {
  if (read_watches.empty()) return false;
  uint8_t* a = reinterpret_cast<uint8_t*>(ex->fault_address());
  std::lock_guard<std::mutex> lock(read_watch_mutex);
  for (ReadWatch& w : read_watches) {
    if (!w.active) continue;
    for (int v = 0; v < 3; ++v) {
      if (a < w.views[v] || a >= w.views[v] + w.length) continue;
      for (uint8_t* view : w.views) mprotect(view, w.length, PROT_READ | PROT_WRITE);
      w.active = false;
      void* frames[16]{};
      int n = 0;
#if !defined(__ANDROID__)
      n = backtrace(frames, 16);
#endif
      char buf[512];
      int k = std::snprintf(buf, sizeof(buf), "[readwatch] hit phys=%08X offset=%zu view=%d %s pc=%p frames:",
                            w.physical_address, size_t(a - w.views[v]), v,
                            ex->access_violation_operation() ==
                                    arch::Exception::AccessViolationOperation::kWrite
                                ? "write"
                                : "read",
                            reinterpret_cast<void*>(ex->pc()));
      for (int i = 0; i < n && k < int(sizeof(buf)) - 20; ++i)
        k += std::snprintf(buf + k, sizeof(buf) - k, " %p", frames[i]);
      REXLOG_INFO("{}", buf);
      return true;
    }
  }
  return false;
}
#else
void ReadWatchArm(uint8_t*, uint32_t, uint32_t) {
  static std::once_flag logged;
  std::call_once(logged, [] {
    REXLOG_WARN("[readwatch] diagnostic is unsupported on this platform");
  });
}

static bool ReadWatchOnFault(arch::Exception*) { return false; }
#endif

bool MMIOHandler::ExceptionCallbackThunk(arch::Exception* ex, void* data) {
  return reinterpret_cast<MMIOHandler*>(data)->ExceptionCallback(ex);
}

bool MMIOHandler::ExceptionCallback(arch::Exception* ex) {
  if (ex->code() != arch::Exception::Code::kAccessViolation) {
    return false;
  }
  arch::Exception::AccessViolationOperation operation = ex->access_violation_operation();
  if (operation != arch::Exception::AccessViolationOperation::kRead &&
      operation != arch::Exception::AccessViolationOperation::kWrite) {
    // Data Execution Prevention or something else uninteresting.
    return false;
  }
  bool is_write = operation == arch::Exception::AccessViolationOperation::kWrite;
  if (ex->fault_address() < uint64_t(virtual_membase_) ||
      ex->fault_address() > uint64_t(memory_end_)) {
    // Quick kill anything outside our mapping.
    return false;
  }
  void* fault_host_address = reinterpret_cast<void*>(ex->fault_address());
  if (ReadWatchOnFault(ex)) {
    return true;
  }

  // Access violations are pretty rare, so we can do a linear search here.
  // Only check if in the virtual range, as we only support virtual ranges.
  const MMIORange* range = nullptr;
  uint32_t fault_guest_virtual_address = 0;
  if (ex->fault_address() < uint64_t(physical_membase_)) {
    fault_guest_virtual_address =
        host_to_guest_virtual_(host_to_guest_virtual_context_, fault_host_address);
    for (const auto& test_range : mapped_ranges_) {
      if ((fault_guest_virtual_address & test_range.mask) == test_range.address) {
        // Address is within the range of this mapping.
        range = &test_range;
        break;
      }
    }
  }
  if (!range) {
    auto lock = global_critical_region_.Acquire();
    if (access_violation_callback_) {
      // Try the write-watch callback before reading /proc/self/maps.
      // PhysicalHeap::TriggerCallbacks returns false without changing state
      // when no page in the range is watched, and watch flags and host
      // protection change together under this lock. Keep the recheck below for
      // a concurrently cleared watch and real access violations; a false
      // recovery attempt has no side effects to repeat.
      if (access_violation_callback_(std::move(lock), access_violation_callback_context_,
                                     fault_host_address, is_write)) {
        return true;
      }
      lock = global_critical_region_.Acquire();
    }
    // Recheck if the pages are still protected (race condition - another thread
    // clears the watch we just hit).
    // Do this under the lock so we don't introduce another race condition.
    memory::PageAccess cur_access;
    size_t page_length = memory::page_size();
    memory::QueryProtect(fault_host_address, page_length, cur_access);
    if (cur_access != memory::PageAccess::kNoAccess &&
        (!is_write || cur_access != memory::PageAccess::kReadOnly)) {
      // Another thread has cleared this watch. Abort.
      return true;
    }
    // The address is not found within any range, so either a write watch or an
    // actual access violation.
    if (access_violation_callback_) {
      return access_violation_callback_(std::move(lock), access_violation_callback_context_,
                                        fault_host_address, is_write);
    }
    return false;
  }

  auto rip = ex->pc();
  auto p = reinterpret_cast<const uint8_t*>(rip);
  DecodedLoadStore decoded_load_store;
  if (!TryDecodeLoadStore(p, decoded_load_store)) {
    // Bounded logging: an undecodable instruction is re-executed forever by
    // the guest thread, so this path must not log once per fault (cycle 10
    // Phase A: ~1M lines). One line per distinct RIP, with the running total.
    LogMmioDecline(p, fault_guest_virtual_address,
                   "Unable to decode MMIO load or store instruction");
    assert_always("Unknown MMIO instruction type");
    return false;
  }

  arch::HostThreadContext& thread_context = *ex->thread_context();

#if REX_ARCH_ARM64
  // Preserve the base address with the pre- or the post-index offset to write
  // it after writing the result (since the base address register and the
  // register to load to may be the same, in which case it should receive the
  // original base address with the offset).
  uintptr_t mem_base_writeback_address = 0;
  if (decoded_load_store.mem_has_base && decoded_load_store.mem_base_writeback) {
    if (decoded_load_store.mem_base_reg == DecodedLoadStore::kArm64MemBaseRegSp) {
      mem_base_writeback_address = thread_context.sp;
    } else {
      assert_true(decoded_load_store.mem_base_reg <= 30);
      mem_base_writeback_address = thread_context.x[decoded_load_store.mem_base_reg];
    }
    mem_base_writeback_address += decoded_load_store.mem_base_writeback_offset;
  }
#endif  // REX_ARCH_ARM64

  uint8_t value_reg = decoded_load_store.value_reg;
  if (decoded_load_store.is_load) {
#if REX_ARCH_AMD64
    if (IsMovdqaLoad(p)) {
      // 66 [REX] 0F 6F /r (MOVDQA xmm, m128): a 16-byte aligned load that
      // cannot be served by a single 32-bit MMIO register read. Perform four
      // ordered 32-bit reads at EA+0/4/8/12 and assemble them into the
      // destination XMM register.
      //
      // Byte order: the MMIO range stores guest (big-endian) 32-bit register
      // values, so the raw host-memory bytes the faulting movdqa would have
      // loaded are the big-endian encoding of each callback value. Each lane
      // is therefore byte-swapped exactly like the existing 32-bit MOV load
      // path (byte_swap == false). Lane i, which is u32[i] (the low to high
      // 32-bit words of the XMM register), receives the swapped value of the
      // read at EA+4*i, so u8[0] holds the byte at EA+0 - the memory-order
      // bytes the generated lvlx128 pshufb consumes after resumption.
      vec128_t vector_value;
      for (uint32_t lane = 0; lane < 4; ++lane) {
        uint32_t lane_value = range->read(nullptr, range->callback_context,
                                          fault_guest_virtual_address + lane * sizeof(uint32_t));
        if (!decoded_load_store.byte_swap) {
          lane_value = rex::byte_swap(lane_value);
        }
        vector_value.u32[lane] = lane_value;
      }
      ex->ModifyXmmRegister(value_reg) = vector_value;
    } else
#endif  // REX_ARCH_AMD64
    {
      // Load of a memory value - read from range, swap, and store in the
      // register.
      uint32_t value = range->read(nullptr, range->callback_context, fault_guest_virtual_address);
      if (!decoded_load_store.byte_swap) {
        // We swap only if it's not a movbe, as otherwise we are swapping twice.
        value = rex::byte_swap(value);
      }
#if REX_ARCH_AMD64
      ex->ModifyIntRegister(value_reg) = value;
#elif REX_ARCH_ARM64
      if (value_reg >= DecodedLoadStore::kArm64ValueRegX0 &&
          value_reg <= (DecodedLoadStore::kArm64ValueRegX0 + 30)) {
        ex->ModifyXRegister(value_reg - DecodedLoadStore::kArm64ValueRegX0) = value;
      } else if (value_reg >= DecodedLoadStore::kArm64ValueRegV0 &&
                 value_reg <= (DecodedLoadStore::kArm64ValueRegV0 + 31)) {
        ex->ModifyVRegister(value_reg - DecodedLoadStore::kArm64ValueRegV0).u32[0] = value;
      } else {
        assert_true(value_reg == DecodedLoadStore::kArm64ValueRegZero);
        // Register write is ignored for X31.
      }
#else
#error Register value writing not implemented for the target CPU architecture.
#endif  // REX_ARCH
    }
  } else {
#if REX_ARCH_AMD64
    if (IsMovdqaStore(p)) {
      // 66 [REX] 0F 7F /r (MOVDQA m128, xmm): a 16-byte aligned store that
      // cannot be served by a single 32-bit MMIO register write. Perform four
      // ordered 32-bit writes at EA+0/4/8/12, reading the source lanes from
      // the faulting thread's XMM register.
      //
      // Byte order: the MMIO range stores guest (big-endian) 32-bit register
      // values, so the raw host-memory bytes the faulting movdqa would have
      // stored are the big-endian encoding of each callback value. Each lane
      // is therefore byte-swapped exactly like the existing 32-bit MOV store
      // path (byte_swap == false) - the exact inverse of the 16-byte load
      // path: u8[0] of lane i is the byte at EA+4*i.
      if (!range->write) {
        // The matched range cannot accept a store: do not silently discard it
        // and do not invent write semantics - decline it with the same bounded
        // one-shot-per-RIP logging used for undecodable instructions.
        LogMmioDecline(p, fault_guest_virtual_address,
                       "MMIO range has no write callback for MOVDQA store instruction");
        return false;
      }
      const vec128_t& source = ex->ModifyXmmRegister(value_reg);
      for (uint32_t lane = 0; lane < 4; ++lane) {
        uint32_t lane_value = source.u32[lane];
        if (!decoded_load_store.byte_swap) {
          lane_value = rex::byte_swap(lane_value);
        }
        range->write(nullptr, range->callback_context,
                     fault_guest_virtual_address + lane * sizeof(uint32_t), lane_value);
      }
    } else
#endif  // REX_ARCH_AMD64
    {
      // Store of a register value - read register, swap, write to range.
      uint32_t value;
      if (decoded_load_store.is_constant) {
        value = uint32_t(decoded_load_store.constant);
      } else {
#if REX_ARCH_AMD64
        value = uint32_t(thread_context.int_registers[value_reg]);
#elif REX_ARCH_ARM64
        if (value_reg >= DecodedLoadStore::kArm64ValueRegX0 &&
            value_reg <= (DecodedLoadStore::kArm64ValueRegX0 + 30)) {
          value = uint32_t(thread_context.x[value_reg - DecodedLoadStore::kArm64ValueRegX0]);
        } else if (value_reg >= DecodedLoadStore::kArm64ValueRegV0 &&
                   value_reg <= (DecodedLoadStore::kArm64ValueRegV0 + 31)) {
          value = thread_context.v[value_reg - DecodedLoadStore::kArm64ValueRegV0].u32[0];
        } else {
          assert_true(value_reg == DecodedLoadStore::kArm64ValueRegZero);
          value = 0;
        }
#else
#error Register value reading not implemented for the target CPU architecture.
#endif  // REX_ARCH
        if (!decoded_load_store.byte_swap) {
          // We swap only if it's not a movbe, as otherwise we are swapping twice.
          value = rex::byte_swap(value);
        }
      }
      range->write(nullptr, range->callback_context, fault_guest_virtual_address, value);
    }
  }

#if REX_ARCH_ARM64
  // Write the base address with the pre- or the post-index offset, overwriting
  // the register to load to if it's the same.
  if (decoded_load_store.mem_has_base && decoded_load_store.mem_base_writeback) {
    if (decoded_load_store.mem_base_reg == DecodedLoadStore::kArm64MemBaseRegSp) {
      thread_context.sp = mem_base_writeback_address;
    } else {
      assert_true(decoded_load_store.mem_base_reg <= 30);
      ex->ModifyXRegister(decoded_load_store.mem_base_reg) = mem_base_writeback_address;
    }
  }
#endif  // REX_ARCH_ARM64

  // Advance RIP to the next instruction so that we resume properly.
  ex->set_resume_pc(rip + decoded_load_store.length);

  return true;
}

}  // namespace rex::runtime
