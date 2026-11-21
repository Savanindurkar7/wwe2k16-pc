// wwe2k16 - WWE 2K16 (Xbox 360) static recompilation with the ReXGlue SDK.
// Customise the app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#if __has_include(<rex/system/gpu_plugin.h>)
#include <rex/system/gpu_plugin.h>  // ReXGlue 0.10: the GPU is a plugin
#else
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/rex_app.h>
#if __has_include(<rex/system/xam/achievements.h>)
#include <rex/system/xam/achievements.h>
#define WWE2K16_HAVE_ACHIEVEMENT_API 1
#else
#define WWE2K16_HAVE_ACHIEVEMENT_API 0  // WWE 2K14 SDK: achievements are its own overlay
#endif
#include <rex/ui/keybinds.h>
#include "ppc_compat.h"  // WWE2K16_SDK_K14
#include <rex/input/input_system.h>

#include "achievement_sound.h"
#if WWE2K16_HAVE_ACHIEVEMENT_API
#include "achievement_toast.h"
#endif
#include "android_driver.h"
#include "disc_image.h"
#include "display_refresh_rate.h"
#include "fps_overlay.h"
#include "frame_monitor.h"
#include "launcher_settings.h"
#include "screenshot.h"
#include "native_renderer.h"
#include "script_input.h"
#include "touch_input.h"
#if WWE2K16_SDK_K14
#include "k14_tiling_hooks.h"  // the game's scene without MSAA (once, here: main.cpp only)
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

class Wwe2k16App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Wwe2k16App>(new Wwe2k16App(ctx, "wwe2k16", PPCImageConfig));
  }

#if !WWE2K16_SDK_K14
  std::string_view GetWindowTitle() const override { return "WWE 2K16"; }
#endif

  // Started without the launcher (no game folder argument, e.g. double-clicking the exe): use the settings last
  // saved in the launcher (launcher_settings.cpp).
  void OnConfigurePaths(rex::PathConfig& paths) override {
#if defined(__ANDROID__)
    // Android: the launcher app passes the game folder or .iso (positional argument), the user data folder
    // (--user_data_root) and every setting (arguments and WWE2K16_* variables); the PC launcher's launcher.json and
    // its exe switching do not apply.
    REXLOG_INFO("[wwe2k16] Android: game {}, user data {}", rex::path_to_utf8(paths.game_data_root),
                rex::path_to_utf8(paths.user_data_root));
#else
    wwe2k16::ApplyLauncherSettings(paths, GetArgument("game_directory").has_value());
#endif
    // The game can also be a disc image (.iso): mounted in place of the folder before default.xex loads.
    wwe2k16::UseDiscImage(paths.game_data_root, paths.user_data_root);
  }

  void OnLoadXexImage(std::string& xex_image) override {
    (void)xex_image;
    wwe2k16::MountDiscImage(runtime());
    wwe2k16::MountUtilityCache(runtime());
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The SDK picks D3D12 whenever it is built in; it hangs the GPU when the Play menu loads (DEVICE_HUNG,
    // 2026-10-16). Vulkan is the default; WWE2K16_GPU=d3d12 for comparison.
#if WWE2K16_SDK_K14
    // WWE 2K14 SDK: the GPU is the runtime-loaded "xenos" plugin (librexgpu-xenos.so next to libmain.so). Loaded here,
    // not after this hook, so its settings (Vulkan present modes, readback, the native renderer) exist before the
    // lines below set them; command-line values for them are replayed by the SDK as they register.
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    if (!config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, EnvIs("WWE2K16_GPU", "d3d12") ? "d3d12" : "vulkan");
      if (!config.graphics) config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "any");
    }
#endif
    // Renderer profile (native_renderer.h): classic / native / native_plus.
    wwe2k16::native_renderer::Apply();
#if !WWE2K16_SDK_K14
    if (!EnvIs("WWE2K16_GPU", "d3d12")) {
      config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
    }
#endif
    REXLOG_INFO("[wwe2k16] graphics backend: {}", EnvIs("WWE2K16_GPU", "d3d12") ? "D3D12" : "Vulkan");

    // Render target path: "fbo" (fast, default); WWE2K16_RT_PATH=fsi selects fragment shader interlock (exact EDRAM
    // emulation, slower) for comparisons.
    {
      const char* rt = std::getenv("WWE2K16_RT_PATH");
      rex::cvar::SetFlagByName("render_target_path_vulkan", rt && *rt ? rt : "fbo");
    }

    // Memexport readback (copying every shader memory write back for the CPU) stalls the GPU thread: entrances ran at
    // 9-16 presented FPS with it, a steady 30 without (2026-10-17). WWE2K16_MEMEXPORT_READBACK=1 turns it back on.
#if WWE2K16_SDK_K14
    // (0.10: a value given on the command line wins even when it equals the default, true.)
    if (rex::cvar::GetFlagSource("readback_memexport") == rex::cvar::Source::kDefault) {
#else
    if (!rex::cvar::HasNonDefaultValue("readback_memexport")) {
#endif
      rex::cvar::SetFlagByName("readback_memexport", EnvIs("WWE2K16_MEMEXPORT_READBACK", "1") ? "true" : "false");
    }

    // The game reads/writes the Xbox utility cache partition (cache:\EditEx.cac, cache:\CAR.cac:
    // Create mode / arena data). Back it with a folder in the user data root.
    rex::cvar::SetFlagByName("mount_cache", "true");

    // Enable keyboard and mouse controller emulation.
    rex::cvar::SetFlagByName("mnk_mode", "true");

    // Frame pacing. The SDK's Vulkan presenter prefers IMMEDIATE (no vsync: tears every frame), then
    // MAILBOX (tear-free fast vsync / triple buffering: drops stale frames and never stalls presentation).
    // Strict FIFO waits for host display's vblank: on a 75/120/144/240 Hz monitor, strict FIFO
    // halves 60 FPS to 37.5 / 30 FPS. Mailbox delivers tear-free 60 FPS on any refresh rate.
    // WWE2K16_VSYNC=0 enables immediate tearing mode.
    // WWE2K16_MAILBOX=0 forces strict FIFO vsync for testing.
    const bool vsync = !EnvIs("WWE2K16_VSYNC", "0");
    const bool allow_mailbox = !EnvIs("WWE2K16_MAILBOX", "0");
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", vsync ? "false" : "true");
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_mailbox", allow_mailbox ? "true" : "false");
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_fifo_relaxed", "true");
    rex::cvar::SetFlagByName("display_refresh_match", "false");
    // Keep the guest update clock at 60 Hz. When host-vblank pacing is available, a rational accumulator spreads
    // those updates over arbitrary display rates (75 Hz = 4 updates per 5 refreshes); stalled/unavailable host waits
    // fall back to the fixed-deadline timer. Switching the display to 60 Hz still gives the cleanest 1:1 cadence.
    rex::cvar::SetFlagByName("vsync_host_display", EnvIs("WWE2K16_HOST_VSYNC", "0") ? "false" : "true");
    // Swapchain image count:
    // Requesting 2 images provides low-latency double buffering, eliminating frame queue delay
    // for snappy, responsive controller and keyboard menu navigation.
    const bool low_latency = !EnvIs("WWE2K16_LOW_LATENCY", "0");
    rex::cvar::SetFlagByName("vulkan_swapchain_image_count", low_latency ? "2" : "0");

    const char* aaEnv = std::getenv("WWE2K16_AA");
    if (aaEnv && *aaEnv) {
      rex::cvar::SetFlagByName("swap_post_effect", aaEnv);
    }
    const char* anisoEnv = std::getenv("WWE2K16_ANISO");
    if (anisoEnv && *anisoEnv) {
      rex::cvar::SetFlagByName("anisotropic_override", anisoEnv);
    }

    const char* upscaleEnv = std::getenv("WWE2K16_UPSCALE");
    if (upscaleEnv && *upscaleEnv) {
      std::string upscale = upscaleEnv;
      for (char& c : upscale) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (upscale == "fsr" || upscale == "fsr1") {
        rex::cvar::SetFlagByName("present_effect", "fsr");
      } else if (upscale == "cas") {
        rex::cvar::SetFlagByName("present_effect", "cas");
      } else if (upscale == "bilinear" || upscale == "off" || upscale == "none") {
        rex::cvar::SetFlagByName("present_effect", "bilinear");
      } else {
        rex::cvar::SetFlagByName("present_effect", upscale);
      }
    }
    const char* fsrSharpness = std::getenv("WWE2K16_FSR_SHARPNESS");
    if (fsrSharpness && *fsrSharpness) {
      rex::cvar::SetFlagByName("present_fsr_sharpness_reduction", fsrSharpness);
    }
    const char* casSharpness = std::getenv("WWE2K16_CAS_SHARPNESS");
    if (casSharpness && *casSharpness) {
      rex::cvar::SetFlagByName("present_cas_additional_sharpness", casSharpness);
    }

    REXLOG_INFO("[wwe2k16] present: {}, guest vblank: {}, swapchain images: {}, shadows: {}, AA: {}, Aniso: {}, upscaler: {}",
                vsync ? (allow_mailbox ? "Mailbox (vsync)" : "FIFO (vsync)") : "immediate",
                rex::cvar::GetFlagByName("vsync_host_display") == "true" ? "host display" : "timer",
                rex::cvar::GetFlagByName("vulkan_swapchain_image_count") == "2" ? "2 (low latency)" : "3",
                !EnvIs("WWE2K16_SHADOWS", "0") ? "on" : "off",
                rex::cvar::GetFlagByName("swap_post_effect"),
                rex::cvar::GetFlagByName("anisotropic_override"),
                rex::cvar::GetFlagByName("present_effect"));

    // First-time shaders (a new camera shot, a finisher flash, the Breaking Point bar, a DLC model)
    // compile in the background. By default the SDK then throws the whole frame away, which shows as
    // a stutter; 22 of 25 measured hitches were exactly that. Presenting the frame with only the
    // not-yet-ready draw missing (for a frame or two, once per shader: they are cached on disk) is
    // far less visible. WWE2K16_SKIP_INCOMPLETE=1 restores frame dropping.
    rex::cvar::SetFlagByName("vulkan_async_skip_incomplete_frames",
                             EnvIs("WWE2K16_SKIP_INCOMPLETE", "1") ? "true" : "false");

    // Texture memory: the SDK's defaults (soft 384 MB, hard 768 MB, + 24 MB of render-to-texture per extra scale step)
    // date from 1 GB GPUs. At 3x (native 4K) post-processing, camera zooms, and major moves require multiple
    // 4K render targets simultaneously. Hard limit at 3x now 8192 + 256 * 8 = 10.2 GB.
    if (!rex::cvar::HasNonDefaultValue("texture_cache_memory_limit_soft")) {
      rex::cvar::SetFlagByName("texture_cache_memory_limit_soft", "4096");
    }
    if (!rex::cvar::HasNonDefaultValue("texture_cache_memory_limit_hard")) {
      rex::cvar::SetFlagByName("texture_cache_memory_limit_hard", "8192");
    }
    if (!rex::cvar::HasNonDefaultValue("texture_cache_memory_limit_render_to_texture")) {
      rex::cvar::SetFlagByName("texture_cache_memory_limit_render_to_texture", "256");
    }

    // WWE 2K16's depth buffers are float24, which Vulkan has no format for. Converting in the pixel
    // shader avoids z-fighting and shadow artefacts.
    if (!EnvIs("WWE2K16_DEPTH_F24_CONVERT", "0")) {
      rex::cvar::SetFlagByName("depth_float24_convert_in_pixel_shader", "true");
    }
    REXLOG_INFO("[wwe2k16] depth_float24_convert_in_pixel_shader={}",
                rex::cvar::GetFlagByName("depth_float24_convert_in_pixel_shader"));

#if defined(__ANDROID__)
    ApplyAndroidDefaults();
#endif
    // Frame monitor, freeze diagnostics and CPU core pinning (frame_monitor.h).
    wwe2k16::frame_monitor::Start();
  }

#if defined(__ANDROID__)
  // Phone defaults. The launcher's choices arrive as command-line cvars and win over these (HasNonDefaultValue).
  static void SetDefault(const char* name, const char* value) {
    if (!rex::cvar::HasNonDefaultValue(name)) rex::cvar::SetFlagByName(name, value);
  }
  void ApplyAndroidDefaults() {
    // The Adreno driver alone sends about a million debug messages per match; formatting them costs real time.
    SetDefault("vulkan_log_debug_messages", "false");
    // Phones share their memory with the CPU: the PC's 4 / 8 GB texture budget would push the game out of memory.
    rex::cvar::SetFlagByName("texture_cache_memory_limit_soft", "768");
    rex::cvar::SetFlagByName("texture_cache_memory_limit_hard", "1536");
    rex::cvar::SetFlagByName("texture_cache_memory_limit_render_to_texture", "64");
    // Keyboard and mouse emulation is meaningless on a phone.
    rex::cvar::SetFlagByName("mnk_mode", "false");
    // Custom Vulkan driver (launcher: Settings > GPU driver), before anything starts Vulkan.
    wwe2k16::android_driver::Install();
    REXLOG_INFO("[wwe2k16] Android: resolution scale {}x{}, texture min LOD {}, MSAA 2x {}, AA {}, aniso {}, "
                "present {}, letterbox {}, pipeline cache {}, async shaders {}, frame cap {}, keep 60 {}, speed fix {}, "
                "cores {}, driver {}",
                rex::cvar::GetFlagByName("draw_resolution_scale_x"), rex::cvar::GetFlagByName("draw_resolution_scale_y"),
                rex::cvar::GetFlagByName("texture_min_lod"), rex::cvar::GetFlagByName("native_2x_msaa"),
                rex::cvar::GetFlagByName("swap_post_effect"), rex::cvar::GetFlagByName("anisotropic_override"),
                rex::cvar::GetFlagByName("present_effect"), rex::cvar::GetFlagByName("present_letterbox"),
                rex::cvar::GetFlagByName("vulkan_pipeline_cache"), rex::cvar::GetFlagByName("async_shader_compilation"),
                EnvOr("WWE2K16_FRAME_CAP", "game"), EnvOr("WWE2K16_KEEP_60", "1"), EnvOr("WWE2K16_SPEED_FIX", "1"),
                EnvOr("WWE2K16_CPU_CORES", "all"), EnvOr("WWE2K16_DRIVER", "system"));
  }
#endif

  static const char* EnvOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
  }

  void OnPostSetup() override {
    rex::ui::RegisterBind("bind_toggle_fullscreen", "F11", "Toggle fullscreen / windowed", [this] {
#if defined(_WIN32)
      ToggleWindowMode();
#else
      if (auto* w = window()) {
        w->SetFullscreen(!w->IsFullscreen());
      }
#endif
    });
    // Full internal-resolution screenshot (a 4K render saves at 3840x2160 even on a 1080p screen).
    rex::ui::RegisterBind("bind_screenshot", "F12",
                          "Save a full-resolution screenshot to Documents\\wwe2k16\\Screenshots",
                          [this] { wwe2k16::TakeScreenshot(runtime()); });
#if defined(__ANDROID__)
    // On-screen touch controller (touch_input.h): merged with any real gamepad by the input system.
    if (runtime() && runtime()->input_system()) {
      static_cast<rex::input::InputSystem*>(runtime()->input_system())
          ->AddDriver(std::make_unique<wwe2k16::TouchInputDriver>(nullptr));
      REXLOG_INFO("[wwe2k16] touch controller ready");
    }
#endif
    // Scripted controller for automated tests (script_input.h).
    if (wwe2k16::ScriptInputDriver::Enabled() && runtime() && runtime()->input_system()) {
      static_cast<rex::input::InputSystem*>(runtime()->input_system())
          ->AddDriver(std::make_unique<wwe2k16::ScriptInputDriver>(nullptr));
      REXLOG_INFO("[wwe2k16] scripted input on (logs/test_input.txt)");
    }
  }

  // WWE2K16_DUMP_IMAGE=1: writes the loaded (decrypted, decompressed) game image to work\image.bin, flat from
  // 0x82000000, for tools/find_functions.py. Unmapped pages are left as zeros.
  void OnPostLoadXexImage() override {
    const char* dump = std::getenv("WWE2K16_DUMP_IMAGE");
    if (!dump || *dump != '1' || !runtime()) return;
#if defined(_WIN32)
    constexpr uint64_t kBase = 0x82000000, kSize = 0x02000000;
    uint8_t* base = runtime()->virtual_membase() + kBase;
    std::vector<uint8_t> image(kSize, 0);
    uint64_t last = 0;
    for (uint8_t* p = base; p < base + kSize;) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
      uint8_t* end = std::min(static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize, base + kSize);
      if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        std::memcpy(image.data() + (p - base), p, end - p);
        last = end - base;
      }
      p = end;
    }
    std::filesystem::create_directories("work");
    if (FILE* f = std::fopen("work/image.bin", "wb")) {
      std::fwrite(image.data(), 1, last, f);
      std::fclose(f);
      REXLOG_INFO("[wwe2k16] wrote work/image.bin ({} MB)", last >> 20);
    }
#endif
  }

  // Achievements: the SDK keeps unlocks in userdata\achievements\315A07D2.txt (achievements.cpp);
  // each new one shows an "Achievement unlocked" pop-up here.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
#if defined(_WIN32)
    SetupWindowMode();
    if (window()) display_refresh_rate_.SwitchTo60Hz(window()->GetNativeWindowHandle());
#endif
    fps_overlay_.Attach(drawer);
#if WWE2K16_HAVE_ACHIEVEMENT_API
    achievement_toasts_.Attach(&app_context(), drawer, window()->GetNativeWindowHandle());
    achievement_toasts_.SetSound(
        wwe2k16::LoadOggAsWav(rex::filesystem::GetExecutableFolder() / "achievement.ogg"));
    rex::system::xam::SetAchievementUnlockedHandler([this](const rex::system::xam::AchievementInfo& a) {
      achievement_toasts_.Push(a.name, a.description, a.icon_path);
    });
#endif
  }

  // The title's achievement table is readable once the module is set up for launch (not in
  // OnPreLaunchModule: the kernel has no executable module yet there). Writes the full list with
  // unlock state, plus the icons, for the launcher's Achievements panel.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
#if WWE2K16_HAVE_ACHIEVEMENT_API
    rex::system::xam::WriteAchievementSummary();
#endif
  }

  void OnShutdown() override {
    wwe2k16::frame_monitor::Stop();
    rex::ui::UnregisterBind("bind_toggle_fullscreen");
    rex::ui::UnregisterBind("bind_screenshot");
    fps_overlay_.Detach();
#if WWE2K16_HAVE_ACHIEVEMENT_API
    rex::system::xam::SetAchievementUnlockedHandler(nullptr);
    achievement_toasts_.Detach();
#endif
#if defined(_WIN32)
    display_refresh_rate_.Restore();
#endif
#if defined(_WIN32)
    std::thread([] {
      Sleep(2000);
      _exit(0);
    }).detach();
#endif
  }

 private:
#if WWE2K16_HAVE_ACHIEVEMENT_API
  wwe2k16::AchievementToasts achievement_toasts_;
#endif
  wwe2k16::FpsOverlay fps_overlay_;
  wwe2k16::DisplayRefreshRate display_refresh_rate_;

#if defined(_WIN32)
  bool is_borderless_ = false;
  RECT pre_borderless_rect_{};
  DWORD pre_borderless_style_ = 0;

  void SetupWindowMode() {
    HWND hwnd = static_cast<HWND>(window()->GetNativeWindowHandle());
    if (!hwnd) return;

    const char* modeEnv = std::getenv("WWE2K16_WINDOW_MODE");
    std::string mode = modeEnv ? modeEnv : "";
    if (mode.empty()) {
      mode = rex::cvar::GetFlagByName("fullscreen") == "true" ? "fullscreen" : "borderless";
    }

    if (mode == "borderless") {
      HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      if (GetMonitorInfoW(monitor, &mi)) {
        pre_borderless_style_ = GetWindowLongW(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &pre_borderless_rect_);

        DWORD style = pre_borderless_style_;
        style &= ~WS_OVERLAPPEDWINDOW;
        style |= WS_POPUP | WS_VISIBLE;
        SetWindowLongW(hwnd, GWL_STYLE, style);

        int x = mi.rcMonitor.left;
        int y = mi.rcMonitor.top;
        int w = mi.rcMonitor.right - mi.rcMonitor.left;
        int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

        SetWindowPos(hwnd, HWND_TOP, x, y, w, h,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        is_borderless_ = true;
        REXLOG_INFO("[wwe2k16] Borderless window mode applied: {}x{} at ({}, {})", w, h, x, y);
      }
    }

    RECT client_rect{};
    GetClientRect(hwnd, &client_rect);
    int client_w = client_rect.right - client_rect.left;
    int client_h = client_rect.bottom - client_rect.top;
    if (client_w <= 0 || client_h <= 0) {
      RECT win_rect{};
      GetWindowRect(hwnd, &win_rect);
      client_w = win_rect.right - win_rect.left;
      client_h = win_rect.bottom - win_rect.top;
    }

    int scale = 1;
    auto scale_str = rex::cvar::GetFlagByName("draw_resolution_scale");
    if (!scale_str.empty()) {
      scale = std::max(1, std::atoi(scale_str.c_str()));
    }
    const int render_w = 1280 * scale;
    const int render_h = 720 * scale;
    const std::string effect = rex::cvar::GetFlagByName("present_effect");
    REXLOG_INFO("[wwe2k16] Display Pipeline: Internal Render {}x{} (scale {}) -> Output {}x{} ({}) | Upscaler: {}",
                render_w, render_h, scale, client_w, client_h, mode,
                effect.empty() ? "bilinear" : effect);
  }

  void ToggleWindowMode() {
    HWND hwnd = static_cast<HWND>(window()->GetNativeWindowHandle());
    if (!hwnd) return;

    if (is_borderless_) {
      SetWindowLongW(hwnd, GWL_STYLE, pre_borderless_style_ ? pre_borderless_style_ : (WS_OVERLAPPEDWINDOW | WS_VISIBLE));
      int w = pre_borderless_rect_.right - pre_borderless_rect_.left;
      int h = pre_borderless_rect_.bottom - pre_borderless_rect_.top;
      if (w <= 0 || h <= 0) { w = 1280; h = 720; }
      SetWindowPos(hwnd, nullptr, pre_borderless_rect_.left, pre_borderless_rect_.top, w, h,
                   SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
      is_borderless_ = false;
      REXLOG_INFO("[wwe2k16] Switched from borderless to windowed");
    } else if (window()->IsFullscreen()) {
      window()->SetFullscreen(false);
    } else {
      HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      if (GetMonitorInfoW(monitor, &mi)) {
        pre_borderless_style_ = GetWindowLongW(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &pre_borderless_rect_);
        DWORD style = pre_borderless_style_;
        style &= ~WS_OVERLAPPEDWINDOW;
        style |= WS_POPUP | WS_VISIBLE;
        SetWindowLongW(hwnd, GWL_STYLE, style);
        int x = mi.rcMonitor.left;
        int y = mi.rcMonitor.top;
        int w = mi.rcMonitor.right - mi.rcMonitor.left;
        int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowPos(hwnd, HWND_TOP, x, y, w, h,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        is_borderless_ = true;
        REXLOG_INFO("[wwe2k16] Switched from windowed to borderless");
      }
    }
  }
#endif

  static bool EnvIs(const char* name, const char* value) {
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, value) == 0;
  }
};
