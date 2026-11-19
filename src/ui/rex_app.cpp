/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation — compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <chrono>

#include <rex/cvar.h>
#include <rex/diagnostics/freeze_logger.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace_player.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <imgui.h>

#include <cstdio>
#include <filesystem>

REXCVAR_DEFINE_STRING(user_data_root, "", "Runtime", "Override user data path");
REXCVAR_DEFINE_STRING(replay_trace, "", "GPU",
                      "Replay this single-frame GPU trace (.xtr) instead of launching the "
                      "title, write the presented frame to replay_trace_out, then exit");
REXCVAR_DEFINE_INT32(replay_trace_repeat, 1, "GPU",
                     "replay_trace: play the frame this many times (after the captured one) and "
                     "log the average host time per frame - a repeatable GPU-emulation benchmark");
REXCVAR_DEFINE_STRING(replay_trace_out, "", "GPU",
                      "Output image (.ppm) for replay_trace; default: trace path + .ppm");
REXCVAR_DEFINE_STRING(update_data_root, "", "Runtime", "Override update data path");
REXCVAR_DEFINE_BOOL(debug_ui, false, "UI/Debug",
                    "Enable developer overlays, hotkeys, and the Windows console")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex {

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {
  AddPositionalOption("game_directory");
}

bool ReXApp::OnInitialize() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  // Game directory: positional arg or default to exe_dir/assets
  std::filesystem::path game_dir;
  if (auto arg = GetArgument("game_directory")) {
    // The argument is UTF-8 (see windowed_app_main_win.cpp). On Windows
    // path(std::string) would decode it with the ANSI code page, corrupting
    // non-ASCII names, so convert explicitly.
    game_dir = rex::to_path(*arg);
  } else {
    game_dir = exe_dir / "assets";
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = rex::to_path(user_data_cvar);
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = rex::to_path(update_data_cvar);
  }

  // Allow subclass to override path defaults
  PathConfig path_config{game_dir, user_dir, update_dir};
  OnConfigurePaths(path_config);
  game_data_root_ = std::move(path_config.game_data_root);
  user_data_root_ = std::move(path_config.user_data_root);
  update_data_root_ = std::move(path_config.update_data_root);

  // Logging setup from CVARs
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info") {
    log_level_str = "trace";
  }
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, {});
  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  // Load saved config (CVARs) before anything reads them
  // The launch profile is a runtime choice, not a saved preference. Keep the
  // command-line/environment value authoritative if a config contains it.
  const bool debug_ui_requested = REXCVAR_GET(debug_ui);
  auto config_path = exe_dir / (std::string(GetName()) + ".toml");
  if (std::filesystem::exists(config_path)) {
    rex::cvar::LoadConfig(config_path);
    REXCVAR_SET(debug_ui, debug_ui_requested);
    REXLOG_INFO("Loaded config: {}", rex::path_to_utf8(config_path.filename()));
  }

  // Capture logs only when the debug console is available.
  if (REXCVAR_GET(debug_ui)) {
    log_sink_ = std::make_shared<rex::LogCaptureSink>();
    rex::AddSink(log_sink_);
  }
  REXLOG_INFO("[ui] debug_ui={}", REXCVAR_GET(debug_ui) ? "true" : "false");

  REXLOG_INFO("{} starting", GetName());
  REXLOG_INFO("  Game directory: {}", rex::path_to_utf8(game_data_root_));
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", rex::path_to_utf8(user_data_root_));
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", rex::path_to_utf8(update_data_root_));
  }

  // Create runtime
  runtime_ = std::make_unique<rex::Runtime>(game_data_root_, user_data_root_, update_data_root_);
  runtime_->set_app_context(&app_context());

  // Build runtime config with default platform backends
  rex::RuntimeConfig config;
#if REX_HAS_D3D12
  config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
#elif REX_HAS_VULKAN
  config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
#endif
  config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config.kernel_init = rex::kernel::InitializeKernel;

  // Allow subclass to customize config
  OnPreSetup(config);

  auto status = runtime_->Setup(ppc_info_.code_base, ppc_info_.code_size, ppc_info_.image_base,
                                ppc_info_.image_size, ppc_info_.func_mappings, std::move(config));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    if (!runtime_->graphics_system()) {
      // The graphics backend could not be initialized (for example no usable
      // Vulkan device). No window exists yet, so the platform entry point will
      // just exit; give the app a chance to explain why to the player.
      OnGraphicsSetupFailed(status);
    }
    return false;
  }

  std::string xex_image = "game:\\default.xex";

  // Allow subclass to override xex image
  OnLoadXexImage(xex_image);

  // Load XEX image
  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    REXLOG_ERROR("Failed to load XEX: {:08X}", status);
    return false;
  }

  OnPostLoadXexImage();

  // The executable module is now loaded, so the kernel state has the title ID
  // needed to select its persistent shader and pipeline storage. Initialize
  // it before the module is launched and can submit its first draws.
  auto* graphics_system =
      static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
  if (graphics_system && runtime_->kernel_state()) {
    graphics_system->InitializeShaderStorage(runtime_->user_data_root() / "cache",
                                             runtime_->kernel_state()->title_id(), true);
  }

  // Initialize rexcrt heap after LoadXexImage to avoid guest memory writes
  // corrupting the heap region. rexcrt_heap is set by codegen (REXCRT_HEAP)
  // when [rexcrt] contains heap functions -- originals are stripped so init
  // is required. Size is controlled by the rexcrt_heap_size_mb CVAR.
  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  // Notify subclass
  OnPostSetup();

  // Create window with the consumer-provided, player-visible title.
  window_ = rex::ui::Window::Create(app_context(), GetWindowTitle(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  // Attach window to input system so deferred drivers (e.g. MnK) can register
  if (runtime_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->Open();

  // Always expose the window to the runtime so hooks (native rendering, etc.)
  // can obtain the native handle even when the SDK graphics system is disabled.
  runtime_->set_display_window(window_.get());

  // Setup graphics presenter and ImGui
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_.get(), 64);
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        // Built-in overlays are developer UI and do not exist in the release profile.
        if (REXCVAR_GET(debug_ui)) {
          debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get());
          console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
          settings_overlay_ = std::make_unique<ui::SettingsDialog>(
              imgui_drawer_.get(), exe_dir / (std::string(GetName()) + ".toml"));
        }

        // Allow subclass to add custom dialogs
        OnCreateDialogs(imgui_drawer_.get());

        runtime_->set_imgui_drawer(imgui_drawer_.get());

        // Tell input drivers to suppress input when ImGui wants the mouse
        // (e.g. overlay is open). This controls MnK mouse capture.
        auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
        if (input_sys) {
          input_sys->SetActiveCallback([]() { return !ImGui::GetIO().WantCaptureMouse; });
        }
      }
    }
    window_->SetPresenter(presenter);

    // Single-frame GPU trace (.xtr) written to trace_gpu_prefix, for offline
    // inspection with the trace dump tool.
    if (REXCVAR_GET(debug_ui)) {
      rex::ui::RegisterBind("bind_gpu_frame_trace", "F9", "Capture a single-frame GPU trace",
                            [graphics_system] {
                              REXLOG_INFO("GPU frame trace requested");
                              graphics_system->RequestFrameTrace();
                            });
    }
  }

  if (!REXCVAR_GET(replay_trace).empty()) {
    // Trace replay: play one captured frame through the live GPU backend and
    // save what the presenter shows. The title is never launched.
    auto* replay_graphics = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
    module_thread_ = std::thread([this, replay_graphics]() {
      std::filesystem::path trace_path = rex::to_path(REXCVAR_GET(replay_trace));
      std::filesystem::path out_path = rex::to_path(REXCVAR_GET(replay_trace_out));
      if (out_path.empty()) {
        out_path = trace_path;
        out_path += ".ppm";
      }
      bool ok = false;
      if (replay_graphics && replay_graphics->presenter()) {
        auto player = std::make_unique<rex::graphics::TracePlayer>(replay_graphics);
        if (player->Open(trace_path.string()) && player->frame_count() > 0) {
          REXLOG_INFO("Replaying trace {} ({} frames)", trace_path.string(), player->frame_count());
          player->SeekFrame(0);
          player->SeekCommand(static_cast<int>(player->current_frame()->commands.size() - 1));
          player->WaitOnPlayback();
          const int32_t repeat = REXCVAR_GET(replay_trace_repeat);
          if (repeat > 1) {
            const auto start = std::chrono::steady_clock::now();
            for (int32_t i = 1; i < repeat; ++i) {
              player->ReplayCurrentFrame();
              player->WaitOnPlayback();
            }
            const double total_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - start)
                                        .count();
            REXLOG_INFO("Replay repeat: {} frames in {:.1f} ms, {:.3f} ms per frame", repeat - 1,
                        total_ms, total_ms / (repeat - 1));
          }
          ui::RawImage image;
          if (replay_graphics->presenter()->CaptureGuestOutput(image)) {
            if (FILE* f = std::fopen(out_path.string().c_str(), "wb")) {
              std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
              for (uint32_t y = 0; y < image.height; ++y) {
                const uint8_t* row = image.data.data() + y * image.stride;
                for (uint32_t x = 0; x < image.width; ++x) {
                  std::fwrite(row + x * 4, 1, 3, f);
                }
              }
              std::fclose(f);
              ok = true;
            }
          } else {
            REXLOG_ERROR("Replay: CaptureGuestOutput failed");
          }
        } else {
          REXLOG_ERROR("Replay: could not open trace {}", trace_path.string());
        }
      } else {
        REXLOG_ERROR("Replay: no graphics system / presenter");
      }
      REXLOG_INFO("Replay {}: {}", ok ? "wrote" : "FAILED", out_path.string());
      app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
    });
    return true;
  }

  // Launch module in background
  app_context().CallInUIThreadDeferred([this]() {
    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    OnPostLaunchModule(main_thread.get());
    main_thread->Resume();

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });

  return true;
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  // Disarm the always-on freeze watchdog before TerminateTitle() / teardown:
  // those can block past the present-gap, which used to make the still-armed
  // watchdog write a spurious freeze report ~10 s after the player closed the
  // window. Safe no-op when the logger was never started.
  // First write the exit snapshot while every guest thread is still alive: a
  // frozen picture with presents still running (GitHub #21/#25) ends with the
  // player closing the window, so this is the moment that state is visible.
  rex::diagnostics::freeze_logger::SnapshotOnClose();
  rex::diagnostics::freeze_logger::Stop();
  REXLOG_INFO("[freeze-logger] disarmed at window close");
  shutting_down_.store(true, std::memory_order_release);
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  app_context().QuitFromUIThread();
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

  rex::ui::UnregisterBind("bind_gpu_frame_trace");

  // ImGui cleanup (reverse of setup)
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(std::move(provider));
  }
}

}  // namespace rex
