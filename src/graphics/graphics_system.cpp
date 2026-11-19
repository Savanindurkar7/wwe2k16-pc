/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/graphics_system.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/diagnostics/rate_census.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_STRING(trace_gpu_prefix, "", "GPU", "GPU trace file prefix");

REXCVAR_DEFINE_BOOL(trace_gpu_stream, false, "GPU", "Enable GPU trace streaming");

REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU", "Persist Vulkan shader and pipeline storage")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(display_pacing_stats, false, "GPU",
                    "Log per second how many new frames reached each host display vblank (Windows)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(vsync_host_display_phase_percent, 5, "GPU",
                     "With vsync_host_display: fire the guest vblank this percentage of a display period after the "
                     "display vblank (0 = at the vblank)");

REXCVAR_DEFINE_BOOL(vsync_host_display, false, "GPU",
                    "Fire guest vblanks on the host display's vertical blanks when the display refresh is a "
                    "multiple of the guest refresh (Windows); otherwise a timer is used");

namespace {
rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::Setup(runtime::FunctionDispatcher* function_dispatcher,
                               system::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context, bool with_presentation) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;
  app_context_ = app_context;

  // Create presenter if presentation is requested and provider is available
  if (with_presentation && provider_) {
    // Safe if either the UI thread call or the presenter creation fails.
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() {
        presenter_ =
            provider_->CreatePresenter([this](bool is_responsible, bool statically_from_ui_thread) {
              OnHostGpuLossFromAnyThread(is_responsible);
            });
      });
    } else {
      // May be needed for offscreen use, such as capturing the guest output
      // image.
      presenter_ =
          provider_->CreatePresenter([this](bool is_responsible, bool statically_from_ui_thread) {
            OnHostGpuLossFromAnyThread(is_responsible);
          });
    }
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  system::X_VIDEO_MODE census_video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&census_video_mode);
  double census_refresh_rate_hz = std::max(1.0, double(float(census_video_mode.refresh_rate)));
  uint64_t census_guest_tick_frequency = chrono::Clock::guest_tick_frequency();
  uint64_t census_vsync_interval_ticks = std::max(
      uint64_t(1), uint64_t(double(census_guest_tick_frequency) / census_refresh_rate_hz));
  diagnostics::rate_census::Start(census_vsync_interval_ticks, census_refresh_rate_hz);

  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        system::X_VIDEO_MODE video_mode;
        kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);
        bool vsync_enabled = REXCVAR_GET(vsync);
        uint64_t interval_ticks = vsync_enabled ? vsync_interval_ticks : no_vsync_interval_ticks;
        uint64_t next_deadline = chrono::Clock::QueryGuestTickCount() + interval_ticks;
        // Avoid replaying missed vblanks after a long stall or suspend.
        constexpr uint64_t kResyncAfterIntervals = 3;
        // vsync_host_display: distribute the fixed-rate guest vblanks across host display vblanks. Integer-multiple
        // refreshes emit every Nth host vblank; other rates use a rational accumulator (for example, 4 guest vblanks
        // per 5 host vblanks at 75 Hz). This avoids a free-running guest timer drifting against the display.
        // Fall back to the deadline timer if the host wait fails or stalls.
        using SteadyClock = std::chrono::steady_clock;
        auto host_refresh_check_time = SteadyClock::time_point();
        auto host_sync_suspended_until = SteadyClock::time_point();
        auto last_host_vblank_time = SteadyClock::time_point();
        uint32_t host_hz = 0;
        uint32_t host_guest_numerator = 0;
        uint32_t host_guest_denominator = 0;
        uint32_t host_guest_accumulator = 0;
        uint32_t logged_host_guest_numerator = UINT32_MAX;
        uint32_t logged_host_guest_denominator = UINT32_MAX;
        uint32_t logged_host_hz = UINT32_MAX;
        while (vsync_worker_running_) {
          const bool current_vsync_enabled = REXCVAR_GET(vsync);
          if (current_vsync_enabled != vsync_enabled) {
            vsync_enabled = current_vsync_enabled;
            interval_ticks = vsync_enabled ? vsync_interval_ticks : no_vsync_interval_ticks;
            next_deadline = chrono::Clock::QueryGuestTickCount() + interval_ticks;
            last_host_vblank_time = SteadyClock::time_point();
            host_guest_accumulator = 0;
          }

          if (vsync_enabled && REXCVAR_GET(vsync_host_display) && presenter()) {
            auto now = SteadyClock::now();
            if (now >= host_refresh_check_time) {
              host_refresh_check_time = now + std::chrono::seconds(1);
              const uint32_t detected_host_hz = presenter()->GetHostDisplayRefreshHz();
              if (detected_host_hz != host_hz) {
                host_hz = detected_host_hz;
                host_guest_accumulator = 0;
                last_host_vblank_time = SteadyClock::time_point();
                const uint32_t guest_hz = std::max(1u, uint32_t(std::lround(refresh_rate_hz)));
                const uint32_t divisor = host_hz ? std::gcd(guest_hz, host_hz) : 1;
                host_guest_numerator = host_hz ? guest_hz / divisor : 0;
                host_guest_denominator = host_hz ? host_hz / divisor : 0;
              }
              if (host_guest_numerator != logged_host_guest_numerator ||
                  host_guest_denominator != logged_host_guest_denominator || host_hz != logged_host_hz) {
                logged_host_guest_numerator = host_guest_numerator;
                logged_host_guest_denominator = host_guest_denominator;
                logged_host_hz = host_hz;
                REXGPU_INFO("[host-vsync] display {} Hz: {}", host_hz,
                            host_guest_numerator
                                ? fmt::format("guest vblank ratio {}/{} display vblank(s)", host_guest_numerator,
                                              host_guest_denominator)
                                : std::string("unavailable, deadline timer vblank"));
              }
            }
            if (host_sync_suspended_until != SteadyClock::time_point() && now >= host_sync_suspended_until) {
              host_sync_suspended_until = SteadyClock::time_point();
              last_host_vblank_time = SteadyClock::time_point();
              host_guest_accumulator = 0;
            }
            if (host_hz && now >= host_sync_suspended_until) {
              const auto wait_start = SteadyClock::now();
              const bool waited = presenter()->WaitForHostDisplayVBlank();
              now = SteadyClock::now();
              const auto host_period = std::chrono::duration_cast<SteadyClock::duration>(
                  std::chrono::duration<double>(1.0 / double(host_hz)));
              if (waited && now - wait_start > host_period * 4) {
                // Display asleep or the output is stalling: don't hold the guest to it.
                REXGPU_WARN("[host-vsync] display vblank wait took {} ms, timer vblank for 5 s",
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - wait_start).count());
                host_sync_suspended_until = now + std::chrono::seconds(5);
              } else if (waited && now - last_host_vblank_time >= host_period / 2) {
                uint32_t elapsed_host_vblanks = 1;
                if (last_host_vblank_time != SteadyClock::time_point()) {
                  const auto elapsed = now - last_host_vblank_time;
                  const auto estimated_periods = std::chrono::duration<double>(elapsed).count() * double(host_hz);
                  elapsed_host_vblanks = std::max(1u, uint32_t(std::lround(estimated_periods)));
                }
                last_host_vblank_time = now;
                if (elapsed_host_vblanks > 4) {
                  REXGPU_WARN("[host-vsync] missed {} display vblanks; deadline timer for 5 s",
                              elapsed_host_vblanks);
                  host_sync_suspended_until = now + std::chrono::seconds(5);
                  host_guest_accumulator = 0;
                } else {
                  host_guest_accumulator += host_guest_numerator * elapsed_host_vblanks;
                }
                const uint32_t guest_vblanks_due =
                    host_sync_suspended_until == SteadyClock::time_point()
                        ? host_guest_accumulator / host_guest_denominator
                        : 0;
                if (host_sync_suspended_until == SteadyClock::time_point()) {
                  host_guest_accumulator %= host_guest_denominator;
                }
                if (guest_vblanks_due) {
                  // Phase offset: the guest frame is handed to the presenter right after the display vblank; firing
                  // it exactly at the vblank races with the display/compositor latching the image. A fractional
                  // refresh ratio may make multiple guest ticks due after one delayed wake, so deliver those ticks.
                  const int32_t phase_percent =
                      std::clamp(REXCVAR_GET(vsync_host_display_phase_percent), 0, 90);
                  if (phase_percent) {
                    std::this_thread::sleep_until(now + host_period * phase_percent / 100);
                  }
                  for (uint32_t i = 0; i < guest_vblanks_due; ++i) {
                    MarkVblank();
                  }
                }
                // Keep the timer path's phase close if the display wait later becomes unavailable.
                next_deadline = chrono::Clock::QueryGuestTickCount() + interval_ticks;
                continue;
              }
            }
          }
          uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          if (current_time >= next_deadline) {
            MarkVblank();
            const uint64_t late_ticks = current_time - next_deadline;
            if (late_ticks / interval_ticks >= kResyncAfterIntervals) {
              next_deadline = current_time + interval_ticks;
            } else {
              next_deadline += interval_ticks;
            }
          }

          if (current_time < next_deadline) {
            const uint64_t guest_ticks_per_second = std::max(uint64_t(1), guest_tick_frequency);
            const uint64_t remaining_ticks = next_deadline - current_time;
            const uint64_t sleep_microseconds =
                (remaining_ticks / guest_ticks_per_second) * 1000000 +
                ((remaining_ticks % guest_ticks_per_second) * 1000000) /
                    guest_ticks_per_second;
            rex::thread::Sleep(
                std::chrono::microseconds(std::max(uint64_t(1), sleep_microseconds)));
          } else {
            rex::thread::Sleep(std::chrono::microseconds(1));
          }
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
#if REX_PLATFORM_WIN32
  if (REXCVAR_GET(display_pacing_stats) && presenter()) {
    display_pacing_running_ = true;
    display_pacing_thread_ = std::thread(&GraphicsSystem::DisplayPacingThread, this);
  }
#endif  // REX_PLATFORM_WIN32
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  if (REXCVAR_GET(trace_gpu_stream)) {
    BeginTracing();
  }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  diagnostics::rate_census::Stop();

  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (display_pacing_thread_.joinable()) {
    display_pacing_running_ = false;
    display_pacing_thread_.join();
  }

  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  std::string log_path = rex::GetLogFilePath();
  if (log_path.empty()) {
    log_path = rex::path_to_utf8(rex::filesystem::GetExecutableFolder() / "wwe2k16.log");
  }
  const std::string message =
      "The graphics driver stopped responding, so the game must close.\n\n"
      "Please update your graphics driver. If you use Intel graphics, download the driver from "
      "Intel's website, not Windows Update.\n\n"
      "Please attach this log to your bug report:\n" +
      log_path;
  REXLOG_ERROR("Fatal GPU failure: {}", message);
  rex::FlushLogging();

  // Platform-native message boxes must run on the main/UI thread. A fatal
  // graphics submission can leave guest threads running while a normal UI-loop
  // quit is unwinding, so terminate the process with an ordinary exit code as
  // soon as the user dismisses the dialog rather than allowing a second crash.
  if (app_context_) {
    app_context_->CallInUIThreadSynchronous([this, &message] {
      if (!app_context_->ShowSimpleMessageBox("Graphics error", message.c_str())) {
        REXLOG_ERROR("Could not display the fatal GPU message box");
      }
      rex::FlushLogging();
      std::_Exit(EXIT_FAILURE);
    });
  }

  REXLOG_ERROR("Fatal GPU failure could not be marshaled to the UI thread; exiting normally");
  rex::FlushLogging();
  std::_Exit(EXIT_FAILURE);
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  diagnostics::rate_census::RecordInterrupt(source);
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

void GraphicsSystem::DisplayPacingThread() {
  using Clock = std::chrono::steady_clock;
  auto window_start = Clock::now();
  auto last_vblank = Clock::time_point();
  uint64_t last_frames = presenter()->guest_frames_consumed();
  uint64_t vblanks = 0, repeats = 0, singles = 0, multiples = 0, long_gaps = 0;
  double max_gap_ms = 0.0;
  while (display_pacing_running_) {
    if (!presenter()->WaitForHostDisplayVBlank()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      last_vblank = Clock::time_point();
      continue;
    }
    // Count half a period away from where guest frames are handed over (right after the display-locked guest
    // vblank, vsync_host_display_phase_percent after the display vblank); counting at the hand-over itself races
    // with it and misattributes frames.
    const uint32_t mid_hz = std::max(1u, last_host_refresh_hz_.load(std::memory_order_relaxed));
    const int32_t sample_percent =
        (std::clamp(REXCVAR_GET(vsync_host_display_phase_percent), 0, 90) + 50) % 100;
    if (sample_percent) {
      std::this_thread::sleep_for(std::chrono::microseconds(10000 * sample_percent / mid_hz));
    }
    const auto now = Clock::now();
    const uint64_t frames = presenter()->guest_frames_consumed();
    const uint64_t new_frames = frames - last_frames;
    last_frames = frames;
    if (last_vblank != Clock::time_point()) {
      const double gap_ms = std::chrono::duration<double, std::milli>(now - last_vblank).count();
      max_gap_ms = std::max(max_gap_ms, gap_ms);
      // A wake-up more than 1.5 periods after the previous one missed a vblank (this thread was late): its frame
      // count covers two vblanks, so it is not classified.
      const uint32_t hz = std::max(1u, last_host_refresh_hz_.load(std::memory_order_relaxed));
      if (gap_ms > 1500.0 / hz) {
        ++long_gaps;
      } else {
        ++vblanks;
        if (new_frames == 0) {
          ++repeats;
        } else if (new_frames == 1) {
          ++singles;
        } else {
          ++multiples;
        }
      }
    }
    last_vblank = now;
    if (now - window_start >= std::chrono::seconds(1)) {
      last_host_refresh_hz_.store(presenter()->GetHostDisplayRefreshHz(), std::memory_order_relaxed);
      REXGPU_INFO("[display-pacing] hz={} vblanks={} new0={} new1={} new2p={} late_wakeups={} max_gap_ms={:.2f} "
                  "presents={} guest_frames={}",
                  last_host_refresh_hz_.load(std::memory_order_relaxed), vblanks, repeats, singles, multiples,
                  long_gaps, max_gap_ms, presenter()->host_present_count(), frames);
      vblanks = repeats = singles = multiples = long_gaps = 0;
      max_gap_ms = 0.0;
      window_start = now;
    }
  }
}

void GraphicsSystem::MarkVblank() {
  diagnostics::rate_census::RecordVblank();

  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  if (!REXCVAR_GET(store_shaders)) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::EndTracing() {
  command_processor_->EndTracing();
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
