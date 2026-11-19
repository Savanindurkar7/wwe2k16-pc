#pragma once

// Local achievements: unlocks written by the title (XGIUserWriteAchievements) are kept in
// <user data>/achievements/<title id>.txt ("<id> <FILETIME>" per line) and reported back as unlocked by
// XamUserCreateAchievementEnumerator. <title id>.json next to it lists every achievement of the title (name,
// description, gamerscore, unlock time, icon) for launchers and other tools; the icons, read from the title's own
// achievement data in the XEX, are saved once as <title id>/<image id>.png.

#include <cstdint>
#include <functional>
#include <string>

namespace rex::system::xam {

struct AchievementInfo {
  uint32_t id = 0;
  std::string name;
  std::string description;
  uint32_t gamerscore = 0;
  uint32_t image_id = 0;
  std::string icon_path;  // PNG saved by WriteAchievementSummary, empty if not saved yet
};

// Records an unlock. Returns true when it was not unlocked before (the unlocked handler then runs).
bool UnlockAchievement(uint32_t id);

// FILETIME of the unlock (100 ns since 1601), or 0 when locked.
uint64_t AchievementUnlockTime(uint32_t id);

// Called on the unlocking (guest) thread for every new unlock.
void SetAchievementUnlockedHandler(std::function<void(const AchievementInfo&)> handler);

// Rewrites <title id>.json (call once the title's XEX is loaded; unlocks rewrite it too).
void WriteAchievementSummary();

}  // namespace rex::system::xam
