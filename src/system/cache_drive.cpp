#include "cache_drive.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

namespace rex::system::internal {
namespace {

constexpr std::uintmax_t kBytesPerAllocationUnit = 512;
constexpr std::uintmax_t kMinimumUtilityCacheBytes = 0x09600000;

std::uint32_t BytesToAllocationUnits(std::uintmax_t bytes) {
  const auto units = bytes / kBytesPerAllocationUnit;
  return static_cast<std::uint32_t>(
      std::min(units, static_cast<std::uintmax_t>(std::numeric_limits<std::uint32_t>::max())));
}

class HostBackedCacheDevice final : public rex::filesystem::HostPathDevice {
 public:
  HostBackedCacheDevice(const std::filesystem::path& host_path)
      : HostPathDevice(kUtilityCacheDevicePath, host_path, false) {}

  std::uint32_t total_allocation_units() const override {
    return BytesToAllocationUnits(QuerySpace().capacity);
  }

  std::uint32_t available_allocation_units() const override {
    const auto space = QuerySpace();
    return BytesToAllocationUnits(std::min(space.available, space.capacity));
  }

 private:
  std::filesystem::space_info QuerySpace() const {
    std::error_code error;
    auto space = std::filesystem::space(host_path(), error);
    if (error) {
      return {};
    }
    return space;
  }
};

}  // namespace

bool SetupUtilityCacheDrive(rex::filesystem::VirtualFileSystem& file_system,
                            const std::filesystem::path& user_data_root, bool enabled) {
  if (!enabled) {
    return true;
  }
  if (user_data_root.empty()) {
    REXSYS_ERROR("Cannot mount utility cache: user_data_root is empty");
    return false;
  }

  const auto host_path = std::filesystem::absolute(user_data_root) / "cache_drive";
  auto device = std::make_unique<HostBackedCacheDevice>(host_path);
  if (!device->Initialize()) {
    REXSYS_ERROR("Failed to initialize utility cache host path: {}", rex::path_to_utf8(host_path));
    return false;
  }

  std::error_code space_error;
  const auto host_space = std::filesystem::space(host_path, space_error);
  if (space_error) {
    REXSYS_WARN("Could not query free space for utility cache '{}': {}",
                rex::path_to_utf8(host_path), space_error.message());
  } else {
    REXSYS_INFO("Utility cache host path '{}' has {} bytes available", rex::path_to_utf8(host_path),
                host_space.available);
    if (host_space.available < kMinimumUtilityCacheBytes) {
      REXSYS_WARN("Utility cache has less than 150 MiB available; title cache setup may fail");
    }
  }

  if (!file_system.RegisterDevice(std::move(device))) {
    REXSYS_ERROR("Failed to register utility cache device at {}",
                 kUtilityCacheDevicePath);
    return false;
  }
  file_system.RegisterSymbolicLink(kUtilityCacheSymbolicLink, kUtilityCacheDevicePath);
  REXSYS_INFO("Mounted {} at {}", rex::path_to_utf8(host_path), kUtilityCacheSymbolicLink);
  return true;
}

}  // namespace rex::system::internal
