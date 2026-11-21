#pragma once

#include <filesystem>

namespace rex {
class Runtime;
}

namespace wwe2k16 {

// Lets the game run straight from a disc image (.iso) instead of an extracted folder.
// When `game_root` is an .iso file, it is remembered and replaced by an empty placeholder folder in the user data, so
// the runtime sets up its usual mounts; MountDiscImage then puts the image in place of that folder.
bool UseDiscImage(std::filesystem::path& game_root, const std::filesystem::path& user_data_root);

// Mounts the remembered image as the game drive (game:\, d:\). Call after the runtime is set up and before
// default.xex is loaded. Does nothing when the game runs from a folder.
void MountDiscImage(rex::Runtime* runtime);

// The Xbox utility cache partition (cache:\EditEx.cac, cache:\CAR.cac: Create mode and arena data), backed by
// <user data>/cache_drive. ReXGlue 0.7 (this project's SDK patch) mounts it itself with --mount_cache; the WWE 2K14
// SDK has no such setting, so the app mounts it there. Does nothing when the drive is already mounted.
void MountUtilityCache(rex::Runtime* runtime);

}  // namespace wwe2k16
