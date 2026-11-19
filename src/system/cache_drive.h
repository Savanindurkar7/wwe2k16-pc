#pragma once

#include <filesystem>
#include <string_view>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace rex::system::internal {

inline constexpr std::string_view kUtilityCacheDevicePath =
    "\\Device\\Harddisk0\\Cache0";
inline constexpr std::string_view kUtilityCacheSymbolicLink = "cache:";

// When enabled, mount <user_data_root>/cache_drive as Cache0 and expose it at
// cache:. A disabled call has no VFS or host-filesystem side effects.
bool SetupUtilityCacheDrive(rex::filesystem::VirtualFileSystem& file_system,
                            const std::filesystem::path& user_data_root, bool enabled);

}  // namespace rex::system::internal
