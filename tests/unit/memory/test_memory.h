/**
 * @file        test_memory.h
 * @brief       Shared process-wide rex::memory::Memory instance for unit tests
 *
 * A rex::memory::Memory instance is expensive to create (it reserves and maps
 * the whole 4 GB guest address space) and is a process-wide singleton: the
 * constructor asserts that no other Memory instance is active. Inline
 * function-local statics keep a single instance shared by every test
 * translation unit that includes this header.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace rex::test {

// Shared memory instance - expensive to create, reuse across tests.
inline rex::memory::Memory& GetTestMemory() {
  static rex::memory::Memory memory;
  static bool initialized = false;
  if (!initialized) {
    rex::InitLogging();
    bool result = memory.Initialize();
    REQUIRE(result);
    initialized = true;
  }
  return memory;
}

// Helper to cast away const for heap operations.
inline rex::memory::BaseHeap* MutableHeap(const rex::memory::BaseHeap* heap) {
  return const_cast<rex::memory::BaseHeap*>(heap);
}

}  // namespace rex::test
