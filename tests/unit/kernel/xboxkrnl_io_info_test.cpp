#include <catch2/catch_test_macros.hpp>

#include <kernel/xboxkrnl/xboxkrnl_io_info_helpers.h>

TEST_CASE("host disk volume information reports disk type with no characteristics",
          "[kernel][io_info]") {
  rex::system::X_FILE_FS_DEVICE_INFORMATION info{};

  rex::kernel::xboxkrnl::internal::PopulateHostDiskFsDeviceInformation(info);

  CHECK(static_cast<uint32_t>(info.device_type) == 0x7);
  CHECK(static_cast<uint32_t>(info.characteristics) == 0);
}
