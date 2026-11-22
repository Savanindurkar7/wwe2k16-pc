// wwe2k16 - DLC packages added in the launcher (r17).
//
// The launcher copies the DLC files the player picks (Xbox 360 content packages: LIVE / PIRS / CON, usually named
// with 42 hex characters) into <user data>/dlc_import/. Before the game's first instruction runs (OnPostLaunchModule:
// the title id is known, the guest thread not yet resumed), each one is unpacked by the SDK's
// ContentManager::InstallContent into
//   <user data>/0000000000000000/<title id>/00000002/<package name>/   (+ a .header file with its name and licenses)
// which is where the game's XamContentCreateEnumerator looks for marketplace content (content type 2). The copy in
// dlc_import is deleted when the install succeeds; a package that fails is moved to dlc_import/failed/ and the reason
// is logged ([wwe2k16-dlc] lines) and written to dlc_import/result.txt for the launcher.
//
// Kill switch: WWE2K16_DLC=0.

#include "dlc_install.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/logging.h>
#include "ppc_compat.h"  // WWE2K16_SDK_K14

#if WWE2K16_SDK_K14 && __has_include(<rex/system/xam/content_manager.h>)
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
using rex::X_RESULT;  // (X_ERROR_SUCCESS expands to X_RESULT)
#define WWE2K16_HAVE_DLC_INSTALL 1
#else
#define WWE2K16_HAVE_DLC_INSTALL 0
#endif

namespace wwe2k16::dlc {

namespace {

namespace fs = std::filesystem;

// The first 4 bytes of an Xbox 360 content package.
bool LooksLikePackage(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  char magic[4] = {};
  if (!in.read(magic, 4)) return false;
  return (magic[0] == 'L' && magic[1] == 'I' && magic[2] == 'V' && magic[3] == 'E') ||
         (magic[0] == 'P' && magic[1] == 'I' && magic[2] == 'R' && magic[3] == 'S') ||
         (magic[0] == 'C' && magic[1] == 'O' && magic[2] == 'N' && magic[3] == ' ');
}

}  // namespace

void InstallPending(rex::Runtime* runtime) {
#if WWE2K16_HAVE_DLC_INSTALL
  if (const char* v = std::getenv("WWE2K16_DLC"); v && v[0] == '0') return;
  if (!runtime || !runtime->kernel_state() || !runtime->kernel_state()->content_manager()) return;
  const fs::path root = runtime->user_data_root();
  if (root.empty()) return;
  const fs::path inbox = root / "dlc_import";
  std::error_code ec;
  if (!fs::is_directory(inbox, ec)) return;

  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(inbox, ec)) {
    if (e.is_regular_file(ec) && e.path().filename() != "result.txt" && e.path().filename().string()[0] != '.') {
      files.push_back(e.path());
    }
  }
  if (files.empty()) return;

  std::ofstream result(inbox / "result.txt", std::ios::app);
  auto* content = runtime->kernel_state()->content_manager();
  for (const fs::path& f : files) {
    const std::string name = f.filename().string();
    const auto size_mb = fs::file_size(f, ec) >> 20;
    if (!LooksLikePackage(f)) {
      REXLOG_WARN("[wwe2k16-dlc] {} ({} MB) is not an Xbox 360 content package (no LIVE / PIRS / CON header): skipped",
                  name, size_mb);
      result << "FAILED " << name << ": not an Xbox 360 content package\n";
      fs::create_directories(inbox / "failed", ec);
      fs::rename(f, inbox / "failed" / name, ec);
      continue;
    }
    REXLOG_INFO("[wwe2k16-dlc] installing {} ({} MB)...", name, size_mb);
    const rex::X_RESULT r = content->InstallContent(f);
    if (r == X_ERROR_SUCCESS) {
      REXLOG_INFO("[wwe2k16-dlc] {} installed", name);
      result << "OK " << name << "\n";
      fs::remove(f, ec);
    } else {
      REXLOG_WARN("[wwe2k16-dlc] {} could not be installed (error 0x{:08X}): moved to dlc_import/failed", name,
                  uint32_t(r));
      result << "FAILED " << name << ": install error 0x" << std::hex << uint32_t(r) << std::dec << "\n";
      fs::create_directories(inbox / "failed", ec);
      fs::rename(f, inbox / "failed" / name, ec);
    }
  }
#else
  (void)runtime;
#endif
}

}  // namespace wwe2k16::dlc
