#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/filesystem/devices/null_device.h>
#include <rex/filesystem/vfs.h>

#include "system/cache_drive.h"

REXCVAR_DECLARE(bool, mount_cache);

namespace {

std::uint32_t BytesToAllocationUnits(std::uintmax_t bytes) {
  const auto units = bytes / 512;
  return static_cast<std::uint32_t>(
      std::min(units, static_cast<std::uintmax_t>(std::numeric_limits<std::uint32_t>::max())));
}

struct TemporaryDirectory {
  std::filesystem::path path;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

struct ResetMountCacheFlag {
  ~ResetMountCacheFlag() { rex::cvar::ResetToDefault("mount_cache"); }
};

}  // namespace

TEST_CASE("utility cache mount is opt-in and resolves through Cache0", "[system][filesystem]") {
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  TemporaryDirectory temp_root{std::filesystem::temp_directory_path() /
                               ("rexglue-cache-drive-" + std::to_string(unique_suffix))};
  REQUIRE(std::filesystem::create_directories(temp_root.path));

  [[maybe_unused]] ResetMountCacheFlag reset_mount_cache;
  const auto* mount_cache_info = rex::cvar::GetFlagInfo("mount_cache");
  REQUIRE(mount_cache_info != nullptr);
  CHECK(mount_cache_info->default_value == "false");
  CHECK(mount_cache_info->lifecycle == rex::cvar::Lifecycle::kInitOnly);

  rex::filesystem::VirtualFileSystem file_system;
  const auto host_cache_path = temp_root.path / "cache_drive";
  REXCVAR_SET(mount_cache, false);
  REQUIRE(rex::system::internal::SetupUtilityCacheDrive(
      file_system, temp_root.path, REXCVAR_GET(mount_cache)));
  CHECK_FALSE(std::filesystem::exists(host_cache_path));

  std::string symbolic_target;
  CHECK_FALSE(file_system.FindSymbolicLink("cache:", symbolic_target));
  CHECK(file_system.ResolvePath("cache:\\sample.bin") == nullptr);

  REXCVAR_SET(mount_cache, true);
  REQUIRE(rex::system::internal::SetupUtilityCacheDrive(
      file_system, temp_root.path, REXCVAR_GET(mount_cache)));
  CHECK(std::filesystem::exists(host_cache_path));
  REQUIRE(file_system.FindSymbolicLink("cache:", symbolic_target));
  CHECK(symbolic_target == rex::system::internal::kUtilityCacheDevicePath);

  {
    std::ofstream sample_file(host_cache_path / "sample.bin", std::ios::binary);
    REQUIRE(sample_file.good());
    sample_file.put('x');
    REQUIRE(sample_file.good());
  }

  // The runtime registers this broader NullDevice after Cache0. Verify that
  // first-match device routing preserves the host-backed child.
  const std::initializer_list<std::string> null_paths = {
      "\\Partition0", "\\Cache0", "\\Cache1"};
  auto null_device = std::make_unique<rex::filesystem::NullDevice>("\\Device\\Harddisk0",
                                                                   null_paths);
  REQUIRE(null_device->Initialize());
  REQUIRE(file_system.RegisterDevice(std::move(null_device)));

  auto* cache_path_entry = file_system.ResolvePath("cache:\\sample.bin");
  REQUIRE(cache_path_entry != nullptr);
  CHECK(cache_path_entry->device()->mount_path() ==
        std::string(rex::system::internal::kUtilityCacheDevicePath));

  auto* raw_device_path_entry =
      file_system.ResolvePath("\\Device\\Harddisk0\\Cache0\\sample.bin");
  REQUIRE(raw_device_path_entry != nullptr);
  CHECK(raw_device_path_entry->device()->mount_path() ==
        std::string(rex::system::internal::kUtilityCacheDevicePath));

  const auto* cache_device = cache_path_entry->device();
  std::error_code space_error;
  const auto host_space = std::filesystem::space(host_cache_path, space_error);
  REQUIRE_FALSE(space_error);
  CHECK(cache_device->total_allocation_units() == BytesToAllocationUnits(host_space.capacity));
  CHECK(cache_device->available_allocation_units() ==
        BytesToAllocationUnits(std::min(host_space.available, host_space.capacity)));
  CHECK(cache_device->sectors_per_allocation_unit() == 1);
  CHECK(cache_device->bytes_per_sector() == 0x200);
}
