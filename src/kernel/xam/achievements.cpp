#include <rex/system/xam/achievements.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>

REXCVAR_DECLARE(uint32_t, user_language);
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/util/xdbf_utils.h>

namespace rex::system::xam {
namespace {

std::mutex g_mutex;
bool g_loaded = false;
uint32_t g_title_id = 0;
std::map<uint32_t, uint64_t> g_unlocked;  // id -> FILETIME
std::function<void(const AchievementInfo&)> g_handler;

// From the runtime, not REX_KERNEL_STATE(): that reads the calling thread's guest context, which host threads
// (the UI thread running OnPostLaunchModule) do not have.
system::KernelState* Kernel() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->kernel_state() : nullptr;
}

std::filesystem::path StoreFolder() {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || runtime->user_data_root().empty())
    return {};
  return runtime->user_data_root() / "achievements";
}

uint64_t NowFileTime() {
  const auto unix_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count() /
                          100;
  return uint64_t(unix_100ns) + 116444736000000000ull;  // 1601 -> 1970
}

// Caller holds g_mutex.
void LoadLocked() {
  if (g_loaded)
    return;
  auto* kernel = Kernel();
  if (!kernel || !kernel->GetExecutableModule())
    return;  // the title is not known before the module launch; loaded on the next call
  g_loaded = true;
  g_title_id = kernel->title_id();
  const auto path = StoreFolder() / fmt::format("{:08X}.txt", g_title_id);
  std::ifstream in(path);
  uint32_t id;
  uint64_t time;
  while (in >> id >> time)
    g_unlocked[id] = time;
  if (!g_unlocked.empty())
    REXLOG_INFO("[achievements] {} unlocked so far ({})", g_unlocked.size(), rex::path_to_utf8(path));
}

// Caller holds g_mutex.
void SaveLocked() {
  const auto folder = StoreFolder();
  if (folder.empty())
    return;
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / fmt::format("{:08X}.txt", g_title_id);
  const auto temp = folder / fmt::format("{:08X}.txt.tmp", g_title_id);
  {
    std::ofstream out(temp, std::ios::trunc);
    for (const auto& [id, time] : g_unlocked)
      out << id << ' ' << time << '\n';
    if (!out)
      return;
  }
  std::filesystem::rename(temp, path, ec);  // atomic replace: a crash never leaves half a file
}

std::vector<AchievementInfo> ReadTitleAchievements() {
  std::vector<AchievementInfo> list;
  auto* kernel = Kernel();
  if (!kernel || !kernel->GetExecutableModule())
    return list;
  const util::XdbfGameData db = kernel->title_xdbf();
  if (!db.is_valid())
    return list;
  // The console language chosen for the game (user_language), or the title's default if it has no text for it.
  const XLanguage language = db.GetExistingLanguage(XLanguage(REXCVAR_GET(user_language)));
  for (const auto& entry : db.GetAchievements()) {
    list.push_back({uint32_t(entry.id), db.GetStringTableEntry(language, entry.label_id),
                    db.GetStringTableEntry(language, entry.description_id), uint32_t(entry.gamerscore),
                    uint32_t(entry.image_id)});
  }
  return list;
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20)
          out += fmt::format("\\u{:04x}", c);
        else
          out += char(c);
    }
  }
  return out + "\"";
}

// Saves each achievement icon (PNG in the title's XDBF image section) once; returns the ids that exist on disk.
std::map<uint32_t, bool> ExtractIcons(const std::filesystem::path& icon_folder,
                                      const std::vector<AchievementInfo>& list) {
  std::map<uint32_t, bool> present;
  auto* kernel = Kernel();
  if (!kernel || !kernel->GetExecutableModule())
    return present;
  const util::XdbfGameData db = kernel->title_xdbf();
  std::error_code ec;
  std::filesystem::create_directories(icon_folder, ec);
  for (const auto& a : list) {
    if (present.count(a.image_id))
      continue;
    const auto path = icon_folder / fmt::format("{}.png", a.image_id);
    if (std::filesystem::exists(path, ec)) {
      present[a.image_id] = true;
      continue;
    }
    const util::XdbfBlock image = db.GetEntry(util::XdbfSection::kImage, a.image_id);
    if (!image || image.size < 8)
      continue;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.buffer), std::streamsize(image.size));
    present[a.image_id] = bool(out);
  }
  return present;
}

// Caller holds g_mutex.
void WriteSummaryLocked() {
  const auto folder = StoreFolder();
  if (folder.empty() || !g_loaded)
    return;
  const auto list = ReadTitleAchievements();
  if (list.empty())
    return;
  const std::string icon_dir = fmt::format("{:08X}", g_title_id);
  const auto icons = ExtractIcons(folder / icon_dir, list);
  // Name and description in every language the title has, so the launcher can show its own language.
  static const std::pair<const char*, XLanguage> kLanguages[] = {
      {"en", XLanguage::kEnglish}, {"fr", XLanguage::kFrench}, {"de", XLanguage::kGerman},
      {"it", XLanguage::kItalian}, {"es", XLanguage::kSpanish}};
  const util::XdbfGameData db = Kernel()->title_xdbf();
  std::map<uint32_t, std::string> texts;
  for (const auto& entry : db.GetAchievements()) {
    std::string text;
    for (const auto& [code, lang] : kLanguages) {
      if (db.GetExistingLanguage(lang) != lang)
        continue;
      text += fmt::format("{}\"{}\":{{\"name\":{},\"description\":{}}}", text.empty() ? "" : ",", code,
                          JsonString(db.GetStringTableEntry(lang, entry.label_id)),
                          JsonString(db.GetStringTableEntry(lang, entry.description_id)));
    }
    texts[uint32_t(entry.id)] = text;
  }
  std::ostringstream json;
  json << "{\"titleId\":\"" << fmt::format("{:08X}", g_title_id) << "\",\"achievements\":[";
  for (size_t i = 0; i < list.size(); ++i) {
    const auto& a = list[i];
    auto it = g_unlocked.find(a.id);
    // Unlock time as Unix milliseconds, convenient for JavaScript/.NET.
    const long long unix_ms = it == g_unlocked.end()
                                  ? 0
                                  : (long long)((it->second - 116444736000000000ull) / 10000);
    json << (i ? "," : "") << "{\"id\":" << a.id << ",\"name\":" << JsonString(a.name)
         << ",\"description\":" << JsonString(a.description) << ",\"text\":{" << texts[a.id] << "}"
         << ",\"gamerscore\":" << a.gamerscore
         << ",\"unlocked\":" << (it != g_unlocked.end() ? "true" : "false") << ",\"unlockedAt\":" << unix_ms
         << ",\"image\":"
         << (icons.count(a.image_id) ? JsonString(fmt::format("{}/{}.png", icon_dir, a.image_id)) : "null")
         << "}";
  }
  json << "]}";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / fmt::format("{:08X}.json", g_title_id);
  const auto temp = folder / fmt::format("{:08X}.json.tmp", g_title_id);
  {
    std::ofstream out(temp, std::ios::trunc | std::ios::binary);
    out << json.str();
    if (!out)
      return;
  }
  std::filesystem::rename(temp, path, ec);
}

}  // namespace

bool UnlockAchievement(uint32_t id) {
  std::function<void(const AchievementInfo&)> handler;
  AchievementInfo info{id};
  {
    std::lock_guard lock(g_mutex);
    LoadLocked();
    if (!g_loaded || g_unlocked.count(id))
      return false;
    g_unlocked[id] = NowFileTime();
    SaveLocked();
    for (auto& a : ReadTitleAchievements()) {
      if (a.id == id) {
        info = std::move(a);
        break;
      }
    }
    const auto icon = StoreFolder() / fmt::format("{:08X}", g_title_id) / fmt::format("{}.png", info.image_id);
    std::error_code icon_ec;
    if (std::filesystem::exists(icon, icon_ec))
      info.icon_path = rex::path_to_utf8(icon);
    WriteSummaryLocked();
    handler = g_handler;
  }
  REXLOG_INFO("[achievements] unlocked {} \"{}\" ({}G)", id, info.name, info.gamerscore);
  if (handler)
    handler(info);
  return true;
}

uint64_t AchievementUnlockTime(uint32_t id) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  auto it = g_unlocked.find(id);
  return it == g_unlocked.end() ? 0 : it->second;
}

void SetAchievementUnlockedHandler(std::function<void(const AchievementInfo&)> handler) {
  std::lock_guard lock(g_mutex);
  g_handler = std::move(handler);
}

void WriteAchievementSummary() {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  WriteSummaryLocked();
}

}  // namespace rex::system::xam
