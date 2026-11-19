/**
 * @file        rexcodegen/vtable_scanner.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/vtable_scanner.h>
#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>
#include <rex/types.h>

using rex::memory::load_and_swap;

namespace rex::codegen {

VTableScanner::VTableScanner(const BinaryView& binary) : binary_(binary) {}

std::vector<VTableInfo> VTableScanner::scan() {
  std::vector<VTableInfo> vtables;

  // Step 1: Find all Complete Object Locators
  auto cols = findCompleteObjectLocators();
  REXCODEGEN_DEBUG("VTableScanner: found {} Complete Object Locators", cols.size());

  // Step 2: For each COL, find EVERY referencing vtable and read its slots
  for (uint32_t colAddr : cols) {
    auto vtableAddrs = findVTablesForCOL(colAddr);
    if (vtableAddrs.empty()) {
      REXCODEGEN_TRACE("VTableScanner: COL at 0x{:08X} has no referencing vtable", colAddr);
      continue;
    }

    if (vtableAddrs.size() > 1) {
      REXCODEGEN_DEBUG("VTableScanner: COL at 0x{:08X} is referenced by {} vtables", colAddr,
                       vtableAddrs.size());
    }

    for (uint32_t vtableAddr : vtableAddrs) {
      VTableInfo info;
      info.vtableAddress = vtableAddr;
      info.colAddress = colAddr;
      info.className = extractClassName(colAddr);
      info.slots = readVTableSlots(vtableAddr);

      if (info.slots.empty()) {
        REXCODEGEN_TRACE("VTableScanner: vtable at 0x{:08X} has no valid slots", vtableAddr);
        continue;
      }

      REXCODEGEN_DEBUG("VTableScanner: vtable at 0x{:08X} ({}) has {} slots", info.vtableAddress,
                       info.className, info.slots.size());

      vtables.push_back(std::move(info));
    }
  }

  return vtables;
}

std::vector<uint32_t> VTableScanner::findCompleteObjectLocators() {
  std::vector<uint32_t> cols;

  // Scan .rdata section for COL patterns
  const auto* rdata = binary_.findSectionByName(".rdata");
  if (!rdata || !rdata->data) {
    REXCODEGEN_WARN("VTableScanner: .rdata section not found");
    return cols;
  }

  const uint8_t* data = rdata->data;
  uint32_t base = rdata->baseAddress;
  size_t size = rdata->size;

  // COL is 20 bytes, need room for it
  if (size < sizeof(RTTICompleteObjectLocator)) {
    return cols;
  }

  // Scan for COL pattern: signature=0, valid type descriptor pointer
  for (size_t offset = 0; offset + sizeof(RTTICompleteObjectLocator) <= size; offset += 4) {
    auto* col = reinterpret_cast<const RTTICompleteObjectLocator*>(data + offset);

    uint32_t signature = load_and_swap<uint32_t>(&col->signature);
    uint32_t typeDescPtr = load_and_swap<uint32_t>(&col->pTypeDescriptor);

    // COL signature must be 0 for 32-bit MSVC RTTI
    if (signature != 0) {
      continue;
    }

    // Type descriptor must point to valid memory
    const auto* typeDescSection = binary_.findSection(typeDescPtr);
    if (!typeDescSection || !typeDescSection->data) {
      continue;
    }

    // Check if type descriptor has ".?AV" mangling prefix
    std::string typeName = readString(typeDescPtr + 8, 64);
    if (typeName.find(".?AV") != 0 && typeName.find(".?AU") != 0) {
      continue;
    }

    uint32_t colAddr = base + static_cast<uint32_t>(offset);
    cols.push_back(colAddr);

    REXCODEGEN_TRACE("VTableScanner: found COL at 0x{:08X} -> {}", colAddr, typeName);
  }

  return cols;
}

std::vector<uint32_t> VTableScanner::findVTablesForCOL(uint32_t colAddr) {
  // The pointer to the COL is stored at vtable[-1], so every dword in .rdata
  // equal to colAddr marks a vtable starting 4 bytes later. Collect ALL of
  // them; see the header for why stopping at the first is wrong.
  std::vector<uint32_t> found;

  const auto* rdata = binary_.findSectionByName(".rdata");
  if (!rdata || !rdata->data) {
    return found;
  }

  const uint8_t* data = rdata->data;
  uint32_t base = rdata->baseAddress;
  size_t size = rdata->size;

  for (size_t offset = 0; offset + 4 <= size; offset += 4) {
    uint32_t value = load_and_swap<uint32_t>(data + offset);

    if (value == colAddr) {
      // Found a reference to the COL - that vtable starts at the next dword
      found.push_back(base + static_cast<uint32_t>(offset) + 4);
    }
  }

  return found;
}

std::vector<uint32_t> VTableScanner::readVTableSlots(uint32_t vtableStart) {
  std::vector<uint32_t> slots;

  uint32_t slotAddr = vtableStart;

  while (true) {
    auto funcAddr = readDword(slotAddr);
    if (!funcAddr) {
      break;  // Can't read memory
    }

    uint32_t addr = *funcAddr;

    // Termination: null pointer
    if (addr == 0) {
      break;
    }

    // Termination: not executable address
    if (!isExecutableAddress(addr)) {
      break;
    }

    // Termination: not 4-byte aligned (PPC requirement)
    if (addr & 0x3) {
      break;
    }

    slots.push_back(addr);
    slotAddr += 4;
  }

  return slots;
}

std::string VTableScanner::extractClassName(uint32_t colAddr) {
  auto typeDescPtr = readDword(colAddr + 12);  // pTypeDescriptor offset
  if (!typeDescPtr) {
    return "";
  }

  // Class name is at typeDescriptor + 8
  std::string mangled = readString(*typeDescPtr + 8, 256);

  // Simple demangling: ".?AVClassName@@" -> "ClassName"
  if (mangled.size() > 4 && (mangled.substr(0, 4) == ".?AV" || mangled.substr(0, 4) == ".?AU")) {
    size_t end = mangled.find("@@");
    if (end != std::string::npos) {
      return mangled.substr(4, end - 4);
    }
  }

  return mangled;
}

bool VTableScanner::isExecutableAddress(uint32_t addr) const {
  return binary_.isExecutable(addr);
}

std::optional<uint32_t> VTableScanner::readDword(uint32_t addr) const {
  const auto* section = binary_.findSection(addr);
  if (!section || !section->data) {
    return std::nullopt;
  }

  uint32_t offset = addr - section->baseAddress;
  if (offset + 4 > section->size) {
    return std::nullopt;
  }

  return load_and_swap<uint32_t>(section->data + offset);
}

std::string VTableScanner::readString(uint32_t addr, size_t maxLen) const {
  const auto* section = binary_.findSection(addr);
  if (!section || !section->data) {
    return "";
  }

  uint32_t offset = addr - section->baseAddress;
  size_t available = section->size - offset;
  size_t len = std::min(maxLen, available);

  const char* str = reinterpret_cast<const char*>(section->data + offset);
  size_t actualLen = strnlen(str, len);

  return std::string(str, actualLen);
}

}  // namespace rex::codegen
