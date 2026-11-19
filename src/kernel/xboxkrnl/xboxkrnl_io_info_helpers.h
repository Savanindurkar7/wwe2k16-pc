#pragma once

#include <rex/system/info/volume.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl::internal {

inline constexpr rex::system::X_FILE_DEVICE_TYPE kFileDeviceDisk =
    static_cast<rex::system::X_FILE_DEVICE_TYPE>(0x00000007);

inline void PopulateHostDiskFsDeviceInformation(
    rex::system::X_FILE_FS_DEVICE_INFORMATION& info) {
  // XMountUtilityDrive probes \\Device\\Harddisk0 NullDevice while checking
  // Partition0/Cache0/Cache1. Its caller only distinguishes EHSTOR (0x46) from
  // the generic path, so report disk type (0x7) for this synthetic device.
  info.device_type = kFileDeviceDisk;
  info.characteristics = 0;
}

}  // namespace rex::kernel::xboxkrnl::internal
