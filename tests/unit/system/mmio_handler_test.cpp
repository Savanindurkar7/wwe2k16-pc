/**
 * @file        mmio_handler_test.cpp
 * @brief       Unit tests for MMIOHandler's x86-64 load/store decoding and the
 *              16-byte MOVDQA MMIO load execution path (cycle 10 Phase B)
 *
 * Cycle 10 Phase A attributed the cycle-9 SEGV_ACCERR storm to thid 9 (Bink)
 * executing the inlined aligned base load of guest `lvlx128 v51,r8,r5`:
 * `movdqa (%r8,%r10,1),%xmm4` (bytes 66 43 0F 6F 24 10) from guest
 * 0x7FC82580, inside the registered GPU MMIO range. The decoder only accepted
 * 32-bit MOV forms, so every fault logged "Unable to decode MMIO load or store
 * instruction" and the instruction was re-executed forever.
 *
 * These tests cover the new 66 [REX] 0F 6F /r (MOVDQA xmm, m128) load
 * decoding, its four ordered 32-bit MMIO reads assembled into the destination
 * XMM register (matching the existing per-lane byte-swap convention), the
 * declined forms, and the pre-existing range-mismatch / miss behavior. The
 * exception callback is invoked directly with a synthetic Exception and a fake
 * faulting instruction, exactly like the platform SIGSEGV handler would, so no
 * actual signal is required.
 *
 * Cycle 11 corrects two pre-existing SIB parser defects and adds coverage for
 * them: SIB base 101 was declined unconditionally (it is only "no base" when
 * mod == 00; otherwise it is rbp, or r13 with REX.B), and the scale field was
 * shifted by 8 instead of 6 (every SIB scale decoded as 1). The captured
 * cycle-11 site is 66 41 0F 6F 4C 0D 00 (mod 01 / SIB base 101 + REX.B -> r13,
 * index rcx, scale 1, disp8 0), a 7-byte encoding.
 *
 * Cycle 12 accepts and executes the aligned 16-byte MOVDQA store form
 * 66 [REX] 0F 7F /r, the direction complement of the cycle-10 load. The
 * captured cycle-12 faulting instruction is 66 41 0F 7F 04 04
 * (movdqa %xmm0,(%r12,%rax,1)): mod 00 / reg 000 (xmm0) / rm 100 (SIB),
 * SIB 0x04 = scale 1 / index rax / base 100 -> r12 with REX.B=1. SIB base 100
 * is a real base at mod 00, so this encoding carries NO displacement and
 * objdump decodes exactly 6 bytes as one instruction (the 16-byte store is
 * executed as four ordered 32-bit MMIO writes at EA+0/4/8/12, each lane
 * byte-swapped exactly like the 32-bit MOV store path). A range without a
 * write callback declines the store with the bounded one-shot logging.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/exception_handler.h>
#include <rex/platform.h>
#include <rex/types.h>
#include <rex/vec128.h>

// Test-only access shim: DecodedLoadStore and TryDecodeLoadStore are private,
// and the constructor / ExceptionCallback are protected, in production. The
// unit tests parse real instruction bytes through the real decoder and invoke
// the real callback with a synthetic Exception, so this translation unit opens
// the access specifier while parsing the header; no production code changes.
// The standard headers above are pre-included so the keyword macro only
// applies to the SDK header.
#define private public
#include <rex/system/mmio_handler.h>
#undef private

#if REX_PLATFORM_LINUX
#include <sys/mman.h>
#endif

namespace {

using rex::arch::Exception;
using rex::arch::HostThreadContext;
using rex::runtime::MMIOHandler;

// GPU MMIO range as registered by GraphicsSystem::Initialize (cycle 10
// Phase A: base 0x7FC80000, mask 0xFFFF0000, 64 KB).
constexpr uint32_t kGuestMmioBase = 0x7FC80000;
constexpr uint32_t kGuestMmioMask = 0xFFFF0000;
constexpr size_t kWindowSize = 0x40000;

// Fake host window backing virtual_membase_; the GPU MMIO range is mapped at
// window offset 0.
struct TestWindow {
  std::vector<uint8_t> storage = std::vector<uint8_t>(kWindowSize);

  uint8_t* base() { return storage.data(); }
  uint8_t* physical_base() { return storage.data() + storage.size(); }
  uint8_t* end() { return storage.data() + storage.size(); }

  uint32_t GuestAddress(const void* host_address) const {
    return kGuestMmioBase + uint32_t(static_cast<const uint8_t*>(host_address) - storage.data());
  }
};

uint32_t HostToGuestThunk(const void* context, const void* host_address) {
  return static_cast<const TestWindow*>(context)->GuestAddress(host_address);
}

// Records the ordered 32-bit MMIO callback reads and returns the configured
// sentinel value for each lane.
struct RangeReads {
  std::vector<uint32_t> addresses;
  std::vector<uint32_t> values = {0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00};
};

uint32_t ReadThunk(void* /*ppc_context*/, void* callback_context, uint32_t addr) {
  auto* reads = static_cast<RangeReads*>(callback_context);
  reads->addresses.push_back(addr);
  const size_t index = reads->addresses.size() - 1;
  return index < reads->values.size() ? reads->values[index] : 0xDEADBEEFu;
}

struct AvCallbackState {
  uint32_t calls = 0;
  void* host_address = nullptr;
  bool is_write = false;
  bool result = false;
};

bool AvCallbackThunk(std::unique_lock<std::recursive_mutex> /*global_lock*/, void* context,
                     void* host_address, bool is_write) {
  auto* state = static_cast<AvCallbackState*>(context);
  ++state->calls;
  state->host_address = host_address;
  state->is_write = is_write;
  return state->result;
}

// Exposes the protected constructor and exception callback to the tests. The
// production handler is a process-wide singleton, so keep global_handler_
// consistent to avoid the destructor's mismatch warning.
class TestMMIOHandler : public MMIOHandler {
 public:
  TestMMIOHandler(uint8_t* virtual_membase, uint8_t* physical_membase, uint8_t* membase_end,
                  HostToGuestVirtual host_to_guest_virtual, const void* host_to_guest_context,
                  AccessViolationCallback access_violation_callback,
                  void* access_violation_context)
      : MMIOHandler(virtual_membase, physical_membase, membase_end, host_to_guest_virtual,
                    host_to_guest_context, access_violation_callback, access_violation_context) {
    global_handler_ = this;
  }

  bool Handle(Exception* ex) { return ExceptionCallback(ex); }
};

// The captured cycle-10 faulting instruction:
//   66 43 0F 6F 24 10   movdqa (%r8,%r10,1),%xmm4
// (0x43 = REX.X|REX.B, ModRM 0x24 = mod 00 / reg 100 (xmm4) / rm 100 (SIB),
//  SIB 0x10 = scale 1 / index r10 / base r8).
constexpr uint8_t kMovdqaInstruction[] = {0x66, 0x43, 0x0F, 0x6F, 0x24, 0x10};

// The captured cycle-11 faulting instruction:
//   66 41 0F 6F 4C 0D 00   movdqa 0x0(%r13,%rcx,1),%xmm1
// (0x41 = REX.B, ModRM 0x4C = mod 01 / reg 001 (xmm1) / rm 100 (SIB),
//  SIB 0x0D = scale 1 / index rcx / base 101 -> r13 with REX.B=1, disp8 0x00).
// The mod 01 disp8 is part of the encoding, so the instruction is 7 bytes.
constexpr uint8_t kMovdqaR13SibInstruction[] = {0x66, 0x41, 0x0F, 0x6F, 0x4C, 0x0D, 0x00};

// The captured cycle-12 faulting instruction:
//   66 41 0F 7F 04 04   movdqa %xmm0,(%r12,%rax,1)
// (0x41 = REX.B, ModRM 0x04 = mod 00 / reg 000 (xmm0) / rm 100 (SIB),
//  SIB 0x04 = scale 1 / index rax / base 100 -> r12 with REX.B=1).
// SIB base 100 (rsp/r12) is a real base at mod 00, so there is no
// displacement: objdump -D -b binary -m i386:x86-64 -M intel on the captured
// logs/m9-accerr-stop1.insn.bin decodes exactly these 6 bytes as one
// instruction and the next instruction starts at offset 6.
constexpr uint8_t kMovdqaStoreInstruction[] = {0x66, 0x41, 0x0F, 0x7F, 0x04, 0x04};

// The same store with an explicit disp32: ModRM 0x84 = mod 10 / reg 000 /
// rm 100 (SIB), SIB 0x04 = scale 1 / index rax / base r12, disp32 0. objdump
// decodes 66 41 0F 7F 84 04 00 00 00 00 as movdqa [r12+rax*1+0x0],xmm0 in
// 10 bytes - the displacement is consumed as part of the instruction.
constexpr uint8_t kMovdqaStoreDisp32Instruction[] = {0x66, 0x41, 0x0F, 0x7F, 0x84,
                                                     0x04, 0x00, 0x00, 0x00, 0x00};

// Records the ordered 32-bit MMIO callback writes.
struct RangeWrites {
  std::vector<uint32_t> addresses;
  std::vector<uint32_t> values;
};

void WriteThunk(void* /*ppc_context*/, void* callback_context, uint32_t addr, uint32_t value) {
  auto* writes = static_cast<RangeWrites*>(callback_context);
  writes->addresses.push_back(addr);
  writes->values.push_back(value);
}

}  // namespace

#if REX_ARCH_AMD64

TEST_CASE("MMIO decode: 66 REX 0F 6F MOVDQA load decodes dest XMM and length",
          "[mmio][mmio_decode]") {
  TestWindow window;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);

  MMIOHandler::DecodedLoadStore decoded{};
  REQUIRE(handler.TryDecodeLoadStore(kMovdqaInstruction, decoded));

  // The 16-byte load form: load direction, no byte swap (same convention as the
  // 32-bit MOV load form), XMM4 destination, 6-byte encoding.
  CHECK(decoded.is_load);
  CHECK_FALSE(decoded.byte_swap);
  CHECK_FALSE(decoded.is_constant);
  CHECK(decoded.value_reg == 4);
  CHECK(decoded.length == 6);

  // Generic ModRM/SIB parsing is reused unchanged: base r8, index r10, scale 1.
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 8);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 10);
  CHECK(decoded.mem_scale == 1);
  CHECK(decoded.mem_displacement == 0);
}

TEST_CASE("MMIO decode: 66 REX 0F 6F without SIB decodes with length",
          "[mmio][mmio_decode]") {
  TestWindow window;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);

  // 66 0F 6F 08 = movdqa (%rax),%xmm1 (no REX, no SIB, mod 00).
  const uint8_t instruction[] = {0x66, 0x0F, 0x6F, 0x08};
  MMIOHandler::DecodedLoadStore decoded{};
  REQUIRE(handler.TryDecodeLoadStore(instruction, decoded));
  CHECK(decoded.is_load);
  CHECK_FALSE(decoded.byte_swap);
  CHECK(decoded.value_reg == 1);
  CHECK(decoded.length == 4);
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 0);
  CHECK_FALSE(decoded.mem_has_index);
}

TEST_CASE("MMIO decode: existing 32-bit MOV/MOVBE forms are unchanged",
          "[mmio][mmio_decode]") {
  TestWindow window;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  MMIOHandler::DecodedLoadStore decoded{};

  // 44 8B 24 02 = mov r12d, [rdx + rax*1]
  const uint8_t mov_load[] = {0x44, 0x8B, 0x24, 0x02};
  REQUIRE(handler.TryDecodeLoadStore(mov_load, decoded));
  CHECK(decoded.is_load);
  CHECK_FALSE(decoded.byte_swap);
  CHECK(decoded.value_reg == 12);
  CHECK(decoded.length == 4);
  CHECK(decoded.mem_base_reg == 2);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 0);

  // 0F 38 F0 8C 02 00 00 00 = movbe ecx, [rdx + rax*1]
  const uint8_t movbe_load[] = {0x0F, 0x38, 0xF0, 0x8C, 0x02, 0x00, 0x00, 0x00};
  REQUIRE(handler.TryDecodeLoadStore(movbe_load, decoded));
  CHECK(decoded.is_load);
  CHECK(decoded.byte_swap);
  CHECK(decoded.value_reg == 1);
  CHECK(decoded.length == 9);

  // 89 0C 02 = mov [rdx + rax*1], ecx
  const uint8_t mov_store[] = {0x89, 0x0C, 0x02};
  REQUIRE(handler.TryDecodeLoadStore(mov_store, decoded));
  CHECK_FALSE(decoded.is_load);
  CHECK_FALSE(decoded.byte_swap);
  CHECK(decoded.value_reg == 1);
  CHECK(decoded.length == 3);
}

TEST_CASE("MMIO decode: unsupported prefixes and plain 0F 6F/0F 7F are declined",
          "[mmio][mmio_decode]") {
  TestWindow window;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  MMIOHandler::DecodedLoadStore decoded{};

  // F3 0F 6F = MOVDQU load and F3 0F 7F = MOVDQU store (not accepted; only the
  // aligned 0x66 forms are). F2 0F 7F is likewise declined.
  const uint8_t movdqu_f3_load[] = {0xF3, 0x0F, 0x6F, 0x24, 0x10};
  CHECK_FALSE(handler.TryDecodeLoadStore(movdqu_f3_load, decoded));
  const uint8_t movdqu_f3_store[] = {0xF3, 0x0F, 0x7F, 0x24, 0x10};
  CHECK_FALSE(handler.TryDecodeLoadStore(movdqu_f3_store, decoded));
  const uint8_t f2_store[] = {0xF2, 0x0F, 0x7F, 0x24, 0x10};
  CHECK_FALSE(handler.TryDecodeLoadStore(f2_store, decoded));

  // 0F 6F/0F 7F without 0x66 are not 128-bit XMM forms.
  const uint8_t no_prefix_load[] = {0x0F, 0x6F, 0x24, 0x10};
  CHECK_FALSE(handler.TryDecodeLoadStore(no_prefix_load, decoded));
  const uint8_t no_prefix_store[] = {0x0F, 0x7F, 0x24, 0x10};
  CHECK_FALSE(handler.TryDecodeLoadStore(no_prefix_store, decoded));

  // 66-prefixed 32-bit MOV: the 0x66 changes the operand size, so it must not
  // fall through to the 32-bit MOV decoder.
  const uint8_t prefixed_mov[] = {0x66, 0x8B, 0x24, 0x10};
  CHECK_FALSE(handler.TryDecodeLoadStore(prefixed_mov, decoded));
}

TEST_CASE("MMIO decode: SIB base 101 with mod != 00 is a real rbp/r13 base",
          "[mmio][mmio_decode]") {
  TestWindow window;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  MMIOHandler::DecodedLoadStore decoded{};

  // Captured cycle-11 site: mod 01, SIB base 101 + REX.B -> r13, index rcx,
  // scale 1, disp8 0. The pre-fix parser declined this form unconditionally,
  // which is only correct for mod 00. 7 bytes: mod 01 always carries disp8.
  REQUIRE(handler.TryDecodeLoadStore(kMovdqaR13SibInstruction, decoded));
  CHECK(decoded.is_load);
  CHECK_FALSE(decoded.byte_swap);
  CHECK(decoded.value_reg == 1);
  CHECK(decoded.length == 7);
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 13);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 1);
  CHECK(decoded.mem_scale == 1);
  CHECK(decoded.mem_displacement == 0);

  // The same SIB base without REX.B is rbp (5); mod 10 carries a 4-byte
  // displacement, so the encoding is 9 bytes.
  const uint8_t rbp_mod10[] = {0x66, 0x0F, 0x6F, 0x8C, 0x0D, 0x00, 0x00, 0x00, 0x00};
  REQUIRE(handler.TryDecodeLoadStore(rbp_mod10, decoded));
  CHECK(decoded.length == 9);
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 5);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 1);
  CHECK(decoded.mem_scale == 1);
  CHECK(decoded.mem_displacement == 0);
}

TEST_CASE("MMIO decode: SIB scale ss=01/10/11 decodes to 2/4/8",
          "[mmio][mmio_decode]") {
  TestWindow window;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  MMIOHandler::DecodedLoadStore decoded{};

  // 66 41 0F 6F 4C <SIB> 00: mod 01, index rcx, base 101 + REX.B -> r13.
  // The scale field is SIB bits 7-6, so it must shift by 6 (not 8).
  const uint8_t scale2[] = {0x66, 0x41, 0x0F, 0x6F, 0x4C, 0x4D, 0x00};
  REQUIRE(handler.TryDecodeLoadStore(scale2, decoded));
  CHECK(decoded.mem_scale == 2);
  CHECK(decoded.length == 7);
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 13);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 1);

  const uint8_t scale4[] = {0x66, 0x41, 0x0F, 0x6F, 0x4C, 0x8D, 0x00};
  REQUIRE(handler.TryDecodeLoadStore(scale4, decoded));
  CHECK(decoded.mem_scale == 4);
  CHECK(decoded.length == 7);
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 13);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 1);

  const uint8_t scale8[] = {0x66, 0x41, 0x0F, 0x6F, 0x4C, 0xCD, 0x00};
  REQUIRE(handler.TryDecodeLoadStore(scale8, decoded));
  CHECK(decoded.mem_scale == 8);
  CHECK(decoded.length == 7);
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 13);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 1);
}

TEST_CASE("MMIO decode: SIB base 101 with mod 00 is the no-base absolute form",
          "[mmio][mmio_decode]") {
  TestWindow window;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  MMIOHandler::DecodedLoadStore decoded{};

  // 66 0F 6F 0C 0D 78 56 34 12 = movdqa 0x12345678(,%rcx,1),%xmm1:
  // mod 00, SIB base 101 -> no base, index rcx, scale 1, disp32 0x12345678.
  // The no-base form always carries the 4-byte displacement (9 bytes total).
  const uint8_t absolute[] = {0x66, 0x0F, 0x6F, 0x0C, 0x0D, 0x78, 0x56, 0x34, 0x12};
  REQUIRE(handler.TryDecodeLoadStore(absolute, decoded));
  CHECK(decoded.is_load);
  CHECK(decoded.value_reg == 1);
  CHECK(decoded.length == 9);
  CHECK_FALSE(decoded.mem_has_base);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 1);
  CHECK(decoded.mem_scale == 1);
  CHECK(decoded.mem_displacement == 0x12345678);

  // SIB index 100 (no index) with base 101 and mod 00 is the plain absolute
  // [disp32] form: no base, no index, still 9 bytes.
  const uint8_t absolute_no_index[] = {0x66, 0x0F, 0x6F, 0x0C, 0x25, 0x00, 0x00, 0x00, 0x00};
  REQUIRE(handler.TryDecodeLoadStore(absolute_no_index, decoded));
  CHECK(decoded.length == 9);
  CHECK_FALSE(decoded.mem_has_base);
  CHECK_FALSE(decoded.mem_has_index);
  CHECK(decoded.mem_displacement == 0);
}

TEST_CASE("MMIO decode: 66 REX 0F 7F MOVDQA store decodes source XMM and length",
          "[mmio][mmio_decode]") {
  TestWindow window;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  MMIOHandler::DecodedLoadStore decoded{};

  // Captured cycle-12 site: mod 00 / SIB base 100 + REX.B -> r12, index rax,
  // scale 1. The 16-byte store direction, no byte swap, source xmm0, and the
  // objdump-verified 6-byte encoding (base 100 at mod 00 has no displacement).
  REQUIRE(handler.TryDecodeLoadStore(kMovdqaStoreInstruction, decoded));
  CHECK_FALSE(decoded.is_load);
  CHECK_FALSE(decoded.byte_swap);
  CHECK_FALSE(decoded.is_constant);
  CHECK(decoded.value_reg == 0);
  CHECK(decoded.length == 6);
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 12);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 0);
  CHECK(decoded.mem_scale == 1);
  CHECK(decoded.mem_displacement == 0);

  // The same store with an explicit disp32 (mod 10) consumes all 4
  // displacement bytes: objdump-verified 10-byte encoding.
  REQUIRE(handler.TryDecodeLoadStore(kMovdqaStoreDisp32Instruction, decoded));
  CHECK_FALSE(decoded.is_load);
  CHECK_FALSE(decoded.byte_swap);
  CHECK(decoded.value_reg == 0);
  CHECK(decoded.length == 10);
  CHECK(decoded.mem_has_base);
  CHECK(decoded.mem_base_reg == 12);
  CHECK(decoded.mem_has_index);
  CHECK(decoded.mem_index_reg == 0);
  CHECK(decoded.mem_scale == 1);
  CHECK(decoded.mem_displacement == 0);
}

TEST_CASE("MMIO execute: 16-byte MOVDQA load reads four lanes and writes XMM",
          "[mmio][mmio_execute]") {
  TestWindow window;
  RangeReads reads;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  REQUIRE(handler.RegisterRange(kGuestMmioBase, kGuestMmioMask, 0x10000, &reads, &ReadThunk,
                                nullptr));

  HostThreadContext context{};
  context.rip = reinterpret_cast<uint64_t>(kMovdqaInstruction);
  Exception ex;
  const uint64_t fault = reinterpret_cast<uint64_t>(window.base() + 0x1000);
  ex.InitializeAccessViolation(&context, fault, Exception::AccessViolationOperation::kRead);

  CHECK(handler.Handle(&ex));

  // Four ordered 32-bit callback reads at EA+0/4/8/12.
  const uint32_t guest_ea = kGuestMmioBase + 0x1000;
  REQUIRE(reads.addresses.size() == 4);
  CHECK(reads.addresses[0] == guest_ea + 0);
  CHECK(reads.addresses[1] == guest_ea + 4);
  CHECK(reads.addresses[2] == guest_ea + 8);
  CHECK(reads.addresses[3] == guest_ea + 12);

  // Lane order: u32[0] is the first 4 bytes at EA (low dword of the XMM
  // register). The MMIO callback returns guest (big-endian) 32-bit values, so
  // each lane is byte-swapped exactly like the existing 32-bit MOV load path;
  // u8[0] then holds the byte at EA+0, the memory-order bytes the faulting
  // movdqa would have loaded.
  const rex::vec128_t& dest = context.xmm_registers[4];
  CHECK(dest.u32[0] == rex::byte_swap(reads.values[0]));
  CHECK(dest.u32[1] == rex::byte_swap(reads.values[1]));
  CHECK(dest.u32[2] == rex::byte_swap(reads.values[2]));
  CHECK(dest.u32[3] == rex::byte_swap(reads.values[3]));

  // XMM4 is marked modified and RIP advances past the 6-byte instruction.
  CHECK((ex.modified_xmm_registers() & (UINT16_C(1) << 4)) != 0);
  CHECK(context.rip == reinterpret_cast<uint64_t>(kMovdqaInstruction) + 6);
}

TEST_CASE("MMIO execute: 16-byte MOVDQA store writes four lanes from XMM",
          "[mmio][mmio_execute]") {
  TestWindow window;
  RangeWrites writes;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  REQUIRE(handler.RegisterRange(kGuestMmioBase, kGuestMmioMask, 0x10000, &writes, nullptr,
                                &WriteThunk));

  HostThreadContext context{};
  context.rip = reinterpret_cast<uint64_t>(kMovdqaStoreInstruction);
  // A source XMM whose four lanes are pairwise distinguishable under the
  // per-lane byte swap (every byte differs from its mirrored position).
  context.xmm_registers[0].u32[0] = 0x01020304;
  context.xmm_registers[0].u32[1] = 0x11223344;
  context.xmm_registers[0].u32[2] = 0xA1B2C3D4;
  context.xmm_registers[0].u32[3] = 0xDEADBEEF;
  Exception ex;
  const uint64_t fault = reinterpret_cast<uint64_t>(window.base() + 0x1000);
  ex.InitializeAccessViolation(&context, fault, Exception::AccessViolationOperation::kWrite);

  CHECK(handler.Handle(&ex));

  // Four ordered 32-bit callback writes at EA+0/4/8/12, one per XMM lane.
  const uint32_t guest_ea = kGuestMmioBase + 0x1000;
  REQUIRE(writes.addresses.size() == 4);
  CHECK(writes.addresses[0] == guest_ea + 0);
  CHECK(writes.addresses[1] == guest_ea + 4);
  CHECK(writes.addresses[2] == guest_ea + 8);
  CHECK(writes.addresses[3] == guest_ea + 12);

  // The MMIO callback takes guest (big-endian) 32-bit values, so each lane is
  // byte-swapped exactly like the existing 32-bit MOV store path - the exact
  // inverse of the 16-byte load path. Lane i, u8[0] of which is the byte at
  // EA+4*i, is written as byte_swap(u32[i]).
  CHECK(writes.values[0] == rex::byte_swap(0x01020304u));
  CHECK(writes.values[1] == rex::byte_swap(0x11223344u));
  CHECK(writes.values[2] == rex::byte_swap(0xA1B2C3D4u));
  CHECK(writes.values[3] == rex::byte_swap(0xDEADBEEFu));

  // RIP advances past the objdump-verified 6-byte instruction.
  CHECK(context.rip == reinterpret_cast<uint64_t>(kMovdqaStoreInstruction) + 6);
}

TEST_CASE("MMIO execute: 16-byte MOVDQA store without a write callback is declined",
          "[mmio][mmio_execute]") {
  TestWindow window;
  RangeWrites writes;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  // A matching range that cannot accept a store: no write callback.
  REQUIRE(handler.RegisterRange(kGuestMmioBase, kGuestMmioMask, 0x10000, &writes, nullptr,
                                nullptr));

  HostThreadContext context{};
  context.rip = reinterpret_cast<uint64_t>(kMovdqaStoreInstruction);
  context.xmm_registers[0].u32[0] = 0x01020304;
  context.xmm_registers[0].u32[1] = 0x11223344;
  context.xmm_registers[0].u32[2] = 0xA1B2C3D4;
  context.xmm_registers[0].u32[3] = 0xDEADBEEF;
  Exception ex;
  const uint64_t fault = reinterpret_cast<uint64_t>(window.base() + 0x1000);
  ex.InitializeAccessViolation(&context, fault, Exception::AccessViolationOperation::kWrite);

  // Declined (bounded failure-log path): no writes attempted, the source XMM
  // is not touched, and RIP is not advanced.
  CHECK_FALSE(handler.Handle(&ex));
  CHECK(writes.addresses.empty());
  CHECK(writes.values.empty());
  CHECK((ex.modified_xmm_registers() & UINT16_C(1)) == 0);
  CHECK(context.rip == reinterpret_cast<uint64_t>(kMovdqaStoreInstruction));
}

TEST_CASE("MMIO execute: a fault outside every registered range is not consumed",
          "[mmio][mmio_execute]") {
  TestWindow window;
  RangeReads reads;
  TestMMIOHandler handler(window.base(), window.physical_base(), window.end(), &HostToGuestThunk,
                          &window, nullptr, nullptr);
  REQUIRE(handler.RegisterRange(kGuestMmioBase, kGuestMmioMask, 0x10000, &reads, &ReadThunk,
                                nullptr));

  HostThreadContext context{};
  context.rip = reinterpret_cast<uint64_t>(kMovdqaInstruction);
  Exception ex;
  // One 64 KB range past the GPU range: no registered range matches.
  const uint64_t fault = reinterpret_cast<uint64_t>(window.base() + 0x20000);
  ex.InitializeAccessViolation(&context, fault, Exception::AccessViolationOperation::kRead);

  // The window page is readable/writable, so the pre-existing "another thread
  // cleared the watch" branch consumes the fault without an MMIO access.
  // (QueryProtect reports no-access unconditionally on non-Linux hosts, where
  // the callback-less path returns false instead.)
#if REX_PLATFORM_LINUX
  CHECK(handler.Handle(&ex));
#else
  CHECK_FALSE(handler.Handle(&ex));
#endif

  CHECK(reads.addresses.empty());
  CHECK((ex.modified_xmm_registers() & (UINT16_C(1) << 4)) == 0);
  CHECK((ex.modified_int_registers() & (UINT16_C(1) << 4)) == 0);
  CHECK(context.rip == reinterpret_cast<uint64_t>(kMovdqaInstruction));
}

#endif  // REX_ARCH_AMD64

#if REX_PLATFORM_LINUX

TEST_CASE("MMIO execute: a miss falls through to the access-violation callback",
          "[mmio][mmio_execute]") {
  // PROT_NONE makes QueryProtect report no-access for a fault outside every
  // registered MMIO range, which is the pre-existing fall-through path.
  void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(page != MAP_FAILED);

  TestWindow window;
  AvCallbackState av_state;
  av_state.result = true;
  TestMMIOHandler handler(static_cast<uint8_t*>(page), static_cast<uint8_t*>(page) + 4096,
                          static_cast<uint8_t*>(page) + 4096, &HostToGuestThunk, &window,
                          &AvCallbackThunk, &av_state);

  HostThreadContext context{};
  Exception ex;
  const uint64_t fault = reinterpret_cast<uint64_t>(page) + 0x800;
  ex.InitializeAccessViolation(&context, fault, Exception::AccessViolationOperation::kRead);

  CHECK(handler.Handle(&ex));
  CHECK(av_state.calls == 1);
  CHECK(av_state.host_address == reinterpret_cast<void*>(fault));
  CHECK_FALSE(av_state.is_write);
  CHECK(context.rip == 0);  // no instruction was executed

  CHECK(munmap(page, 4096) == 0);
}

#else

TEST_CASE("MMIO execute: a miss falls through to the access-violation callback",
          "[mmio][mmio_execute]") {
  SKIP("PROT_NONE host pages are only set up on Linux in this test");
}

#endif  // REX_PLATFORM_LINUX
