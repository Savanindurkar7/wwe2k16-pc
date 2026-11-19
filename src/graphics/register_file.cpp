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

#include <array>
#include <cstring>

#include <rex/graphics/register_file.h>
#include <rex/math.h>

namespace rex::graphics {

namespace {

const std::array<bool, RegisterFile::kRegisterCount> kKnownRegisterIndices = [] {
  std::array<bool, RegisterFile::kRegisterCount> known_registers = {};
#define XE_GPU_REGISTER(index, type, name) known_registers[index] = true;
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
  return known_registers;
}();

}  // namespace

RegisterFile::RegisterFile() {
  std::memset(values, 0, sizeof(values));
}

bool RegisterFile::IsRegisterKnown(uint32_t index) {
  return index < kKnownRegisterIndices.size() && kKnownRegisterIndices[index];
}

const RegisterInfo* RegisterFile::GetRegisterInfo(uint32_t index) {
  switch (index) {
#define XE_GPU_REGISTER(index, type, name) \
  case index: {                            \
    static const RegisterInfo reg_info = { \
        RegisterInfo::Type::type,          \
        #name,                             \
    };                                     \
    return &reg_info;                      \
  }
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    default:
      return nullptr;
  }
}

}  // namespace rex::graphics
