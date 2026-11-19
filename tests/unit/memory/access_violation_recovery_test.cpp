/**
 * @file        access_violation_recovery_test.cpp
 * @brief       Unit tests for the recovery branches of
 *              Memory::AccessViolationCallback (M6 cycle-3 fix, M6 cycle-9
 *              released-physical-page fix)
 *
 * The guest-virtual branch under test recovers stale host protection for
 * committed guest-virtual pages: when guest metadata reports the page
 * committed and the ORIGINAL allocation protection permits the access
 * direction, but host protection is still no-access (the stack-guard situation
 * created by XThread::AllocateStack: commit RW, then Protect(NoAccess) the
 * guard pages, which updates current_protect while allocation_protect stays
 * RW), the callback restores host read/write access and returns true so the
 * faulting guest instruction re-executes.
 *
 * The guest-physical branch under test recovers RELEASED physical pages:
 * MmFreePhysicalMemory releases the block (page table entry becomes exactly
 * free, state == 0) while host protection can remain no-access (the captured
 * `---s` VMA on the released 64 KB block). Real hardware keeps released
 * physical RAM readable/writable until reuse, so a fault on an exactly-free
 * physical page restores host read/write access and returns true. Reserve-only
 * and committed pages (state != 0) are declined.
 *
 * These tests exercise the real decision logic through the real
 * Memory::AccessViolationCallback method on real heap pages; no actual SIGSEGV
 * is required because the callback is invoked directly with the host address
 * of the page, exactly as the host MMIO/AV handler would. See the access shim
 * below.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// Test-only access shim: Memory::AccessViolationCallback is a private SDK API
// reachable in production only through the platform SIGSEGV handler
// (MMIOHandler -> ExceptionCallback). Unit tests invoke the real callback
// directly on a real guest-virtual heap page, so this translation unit opens
// the access specifier while parsing the header; no production code changes.
// The std headers above are pre-included so the keyword macro only applies to
// the SDK headers.
#define private public
#include <rex/system/xmemory.h>
#undef private

#include "test_memory.h"

namespace {

using rex::memory::BaseHeap;
using rex::memory::HeapAllocationInfo;
using rex::memory::Memory;
using rex::test::GetTestMemory;
using rex::test::MutableHeap;

// Allocates one 4 KB page on the guest-virtual v00000000 heap (4 KB pages) and
// returns its guest address and host pointer.
struct GuestVirtualPage {
  BaseHeap* heap = nullptr;
  uint32_t guest_address = 0;
  uint8_t* host_address = nullptr;

  ~GuestVirtualPage() {
    if (heap && guest_address) {
      heap->Release(guest_address, nullptr);
    }
  }
};

GuestVirtualPage AllocateGuestVirtualPage(uint32_t allocation_protect) {
  auto& memory = GetTestMemory();
  GuestVirtualPage page;
  page.heap = MutableHeap(memory.LookupHeap(0x10000000));  // v00000000, 4 KB pages
  REQUIRE(page.heap != nullptr);
  REQUIRE(page.heap->Alloc(
      4096, 4096,
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      allocation_protect, false, &page.guest_address));
  page.host_address = memory.TranslateVirtual(page.guest_address);
  REQUIRE(page.host_address != nullptr);
  return page;
}

// Allocates one 64 KB page on the guest-physical vA0000000 heap (64 KB pages)
// and returns its guest address and host pointer. The vA0000000 alias maps the
// same memfd pages as the canonical 0x00000000 physical heap.
struct GuestPhysicalPage {
  BaseHeap* heap = nullptr;
  uint32_t guest_address = 0;
  uint8_t* host_address = nullptr;
  bool released = false;

  void Release() {
    if (heap && guest_address && !released) {
      heap->Release(guest_address, nullptr);
      released = true;
    }
  }

  ~GuestPhysicalPage() { Release(); }
};

GuestPhysicalPage AllocateGuestPhysicalPage(
    uint32_t allocation_protect,
    uint32_t allocation_type = rex::memory::kMemoryAllocationReserve |
                              rex::memory::kMemoryAllocationCommit) {
  auto& memory = GetTestMemory();
  GuestPhysicalPage page;
  page.heap = MutableHeap(memory.LookupHeap(0xA0000000));  // vA0000000, 64 KB pages
  REQUIRE(page.heap != nullptr);
  REQUIRE(page.heap->Alloc(64 * 1024, 64 * 1024, allocation_type, allocation_protect, false,
                           &page.guest_address));
  page.host_address = memory.TranslateVirtual(page.guest_address);
  REQUIRE(page.host_address != nullptr);
  return page;
}

// Invokes the callback exactly as the host access-violation handler would.
bool InvokeAccessViolationCallback(Memory& memory, void* host_address, bool is_write) {
  std::unique_lock<std::recursive_mutex> lock;
  return memory.AccessViolationCallback(std::move(lock), host_address, is_write);
}

}  // namespace

TEST_CASE("AV recovery: committed RW guest-virtual page recovers a write fault",
          "[memory][accerr][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestVirtualPage(rex::memory::kMemoryProtectRead |
                                       rex::memory::kMemoryProtectWrite);

  // Stack-guard pattern: host/current protection dropped to no-access while the
  // original allocation protection stays read/write.
  uint32_t old_protect = 0;
  REQUIRE(page.heap->Protect(page.guest_address, 4096, rex::memory::kMemoryProtectNoAccess,
                             &old_protect));
  CHECK(old_protect == (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));

  HeapAllocationInfo info{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &info));
  CHECK((info.state & rex::memory::kMemoryAllocationCommit) != 0);
  CHECK(info.allocation_protect ==
        (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  CHECK(info.protect == rex::memory::kMemoryProtectNoAccess);

  CHECK(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/true));

  // Host page must be writable again: the guest store re-executes on this page.
  page.host_address[0] = 0xAB;
  CHECK(page.host_address[0] == 0xAB);

  // The recovery is host-only: guest current_protect stays no-access (the guest
  // guard page remains a guard; only stale host state is repaired).
  HeapAllocationInfo after{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &after));
  CHECK(after.protect == rex::memory::kMemoryProtectNoAccess);
  CHECK(after.allocation_protect ==
        (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
}

TEST_CASE("AV recovery: committed RW guest-virtual page recovers a read fault",
          "[memory][accerr][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestVirtualPage(rex::memory::kMemoryProtectRead |
                                       rex::memory::kMemoryProtectWrite);

  page.host_address[0] = 0x5A;
  REQUIRE(page.heap->Protect(page.guest_address, 4096, rex::memory::kMemoryProtectNoAccess,
                             nullptr));

  CHECK(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/false));
  CHECK(page.host_address[0] == 0x5A);  // readable again
}

TEST_CASE("AV recovery: write fault on a read-only allocation is not recovered",
          "[memory][accerr][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestVirtualPage(rex::memory::kMemoryProtectRead);

  REQUIRE(page.heap->Protect(page.guest_address, 4096, rex::memory::kMemoryProtectNoAccess,
                             nullptr));

  HeapAllocationInfo info{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &info));
  CHECK(info.allocation_protect == rex::memory::kMemoryProtectRead);

  CHECK_FALSE(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/true));
}

TEST_CASE("AV recovery: read fault on a read-only allocation is not recovered",
          "[memory][accerr][recovery]") {
  // The branch requires allocation_protect to include BOTH read and write for a
  // read fault (kReadProtectMask == kMemoryProtectRead | kMemoryProtectWrite);
  // this documents the implemented decision, not an idealised one.
  auto& memory = GetTestMemory();
  auto page = AllocateGuestVirtualPage(rex::memory::kMemoryProtectRead);

  REQUIRE(page.heap->Protect(page.guest_address, 4096, rex::memory::kMemoryProtectNoAccess,
                             nullptr));

  CHECK_FALSE(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/false));
}

TEST_CASE("AV recovery: write-combine allocations satisfy the write-protect mask",
          "[memory][accerr][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestVirtualPage(rex::memory::kMemoryProtectWriteCombine);

  HeapAllocationInfo info{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &info));
  CHECK(info.allocation_protect == rex::memory::kMemoryProtectWriteCombine);

  CHECK(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/true));
}

TEST_CASE("AV recovery: reserved-only (decommitted) pages are not recovered",
          "[memory][accerr][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestVirtualPage(rex::memory::kMemoryProtectRead |
                                       rex::memory::kMemoryProtectWrite);

  REQUIRE(page.heap->Decommit(page.guest_address, 4096));
  HeapAllocationInfo info{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &info));
  CHECK((info.state & rex::memory::kMemoryAllocationCommit) == 0);

  CHECK_FALSE(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/true));
  CHECK_FALSE(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/false));
}

TEST_CASE("AV recovery: host addresses outside guest virtual memory are ignored",
          "[memory][accerr][recovery]") {
  auto& memory = GetTestMemory();

  // Below virtual_membase_.
  CHECK_FALSE(InvokeAccessViolationCallback(memory, nullptr, /*is_write=*/true));
  // At/above physical_membase_ (the callback must not touch physical accesses).
  CHECK_FALSE(InvokeAccessViolationCallback(memory, memory.physical_membase(), /*is_write=*/true));
}

TEST_CASE("AV recovery: released guest-physical page recovers a read fault",
          "[memory][accerr][physical][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestPhysicalPage(rex::memory::kMemoryProtectRead |
                                        rex::memory::kMemoryProtectWrite);
  page.host_address[0] = 0x3C;

  // Captured production state (`---s` VMA on the released 64 KB block): host
  // protection is no-access while the allocation is still live...
  REQUIRE(page.heap->Protect(page.guest_address, 64 * 1024,
                             rex::memory::kMemoryProtectNoAccess, nullptr));

  // ...then MmFreePhysicalMemory releases the block. BaseHeap::Release is
  // host-protect-neutral, so PROT_NONE persists while the page table entry
  // becomes exactly free (state == 0), matching the captured maps line.
  page.Release();
  HeapAllocationInfo info{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &info));
  CHECK(info.state == 0);

  // The Bink decoder's 16-byte LOAD fault is recovered for reads.
  CHECK(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/false));
  CHECK(page.host_address[0] == 0x3C);  // readable again, contents preserved
}

TEST_CASE("AV recovery: released guest-physical page recovers a write fault",
          "[memory][accerr][physical][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestPhysicalPage(rex::memory::kMemoryProtectRead |
                                        rex::memory::kMemoryProtectWrite);

  REQUIRE(page.heap->Protect(page.guest_address, 64 * 1024,
                             rex::memory::kMemoryProtectNoAccess, nullptr));
  page.Release();
  HeapAllocationInfo info{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &info));
  CHECK(info.state == 0);

  CHECK(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/true));
  page.host_address[0] = 0xAB;
  CHECK(page.host_address[0] == 0xAB);
}

TEST_CASE("AV recovery: reserve-only guest-physical page is not recovered",
          "[memory][accerr][physical][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestPhysicalPage(rex::memory::kMemoryProtectNoAccess,
                                        rex::memory::kMemoryAllocationReserve);

  HeapAllocationInfo info{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &info));
  CHECK(info.state == rex::memory::kMemoryAllocationReserve);

  // Reserve-only (state == kMemoryAllocationReserve) is not exactly free and
  // has no commit metadata, so neither the released-page branch nor the
  // pre-existing stale-write branch may recover it.
  CHECK_FALSE(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/false));
  CHECK_FALSE(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/true));
}

TEST_CASE("AV recovery: committed no-access guest-physical page read fault is not recovered",
          "[memory][accerr][physical][recovery]") {
  auto& memory = GetTestMemory();
  auto page = AllocateGuestPhysicalPage(rex::memory::kMemoryProtectRead |
                                        rex::memory::kMemoryProtectWrite);

  REQUIRE(page.heap->Protect(page.guest_address, 64 * 1024,
                             rex::memory::kMemoryProtectNoAccess, nullptr));

  HeapAllocationInfo info{};
  REQUIRE(page.heap->QueryRegionInfo(page.guest_address, &info));
  CHECK((info.state & rex::memory::kMemoryAllocationCommit) != 0);
  CHECK(info.state != 0);

  // Pinned as a read: a write on a committed RW allocation legitimately hits
  // the pre-existing stale-write recovery path below the new branch.
  CHECK_FALSE(InvokeAccessViolationCallback(memory, page.host_address, /*is_write=*/false));
}
