// wwe2k16 - 60 FPS entrances / victory motions / cinematics, and late-frame flips.
//
// Addresses: functions matched in WWE 2K16 by tools/port_functions.py; the Xbox D3D device offsets
// used here are 14000 (present interval), 11028 (flip queue) and 16952..16960 (vblank bookkeeping):
//
//   0x83793780      engine timing block: +8 frame rate (60/30, PAL 50/25), +12 hold flag, +28..+64 time steps,
//                   +40 frame length in ms (16 / 33)
//   sub_82B1BED8    60 -> 30 (50 -> 25): loads the 30 FPS time steps. Called when an entrance, victory motion,
//                   replay or cinematic camera starts
//   sub_82B1BF78    30 -> 60 (25 -> 50), skipped while +12 is set
//   sub_82C214C0    D3D present interval setter, `stw r4,14000(r3)`
//   sub_82C296C0    D3D flip-target choice for a finished frame, r3 = frontbuffer | interval << 8 | threshold
//   sub_82C2C3F0    D3D GPU interrupt handler (source, device)
//
// Environment switches (default first):
//   WWE2K16_KEEP_60=0|1     console 30 FPS in entrances etc. | keep 60 FPS (default)
//   WWE2K16_LATE_FLIP=1|0   a late frame flips at once | waits for the next vblank (console behaviour)
//   WWE2K16_FRAME_CAP=0|30  no host cap (the game's own 60 / 30) | at most 30 presented frames per second
//   WWE2K16_SPEED_FIX=1|0   (Android default 1, PC default 0) the game's time step follows the real frame time
//
// Game speed fix (Android port; same approach as the WWE 2K17 port by Xav): the engine advances everything by the
// fixed step in the timing block once per frame (1/60 s, 1/30 s in entrances), so a phone rendering below 60 FPS runs
// the game in slow motion. Once per presented frame the real frame time is measured, clamped to 1/60..1/12 s,
// smoothed (ema += 0.3 * (dt - ema)) and written to the block's float fields: +28 step (s), +32 fps, +36 frame length
// (ms), +60 step*60, +64 1/(step*60). The integer fields (+8 rate, +40 frame length in ms) stay as the game sets them.
// The first frames are skipped, frames over 0.25 s (loading) are left out, and the fix switches itself off if the
// block does not hold the expected values (wrong address or layout), logging what it found.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include "ppc_compat.h"

#include "fps_overlay.h"
#include "android_driver.h"
#include "frame_monitor.h"

// WWE 2K16 retail addresses, matched to the WWE 2K16 functions by tools/port_functions.py; the timing block from the
// same instructions in the 60 -> 30 switch.
#ifdef WWE2K16_TU
// Title update 3 (wwe2k16_tu.exe): the same functions moved, matched by tools/port_functions.py.
#define WWE2K16_FN_ToThirty sub_82B23E80
#define WWE2K16_IMP_ToThirty __imp__sub_82B23E80
#define WWE2K16_FN_ToSixty sub_82B23F20
#define WWE2K16_IMP_ToSixty __imp__sub_82B23F20
#define WWE2K16_FN_SetPresentInterval sub_82C29578
#define WWE2K16_IMP_SetPresentInterval __imp__sub_82C29578
#define WWE2K16_FN_ChooseFlip sub_82C31778
#define WWE2K16_IMP_ChooseFlip __imp__sub_82C31778
#define WWE2K16_FN_GpuInterrupt sub_82C344A8
#define WWE2K16_IMP_GpuInterrupt __imp__sub_82C344A8
#define WWE2K16_TIMING_BLOCK 0x837A3850
#else
#define WWE2K16_FN_ToThirty sub_82B1BED8
#define WWE2K16_IMP_ToThirty __imp__sub_82B1BED8
#define WWE2K16_FN_ToSixty sub_82B1BF78
#define WWE2K16_IMP_ToSixty __imp__sub_82B1BF78
#define WWE2K16_FN_SetPresentInterval sub_82C214C0
#define WWE2K16_IMP_SetPresentInterval __imp__sub_82C214C0
#define WWE2K16_FN_ChooseFlip sub_82C296C0
#define WWE2K16_IMP_ChooseFlip __imp__sub_82C296C0
#define WWE2K16_FN_GpuInterrupt sub_82C2C3F0
#define WWE2K16_IMP_GpuInterrupt __imp__sub_82C2C3F0
#define WWE2K16_TIMING_BLOCK 0x83793780
#endif

// (r19) SDK patch 0009: 1 once frame generation gave up for the session. Weak: absent in an SDK without the patch.
extern "C" int wwe2k16_framegen_gave_up() __attribute__((weak));

namespace {

constexpr uint32_t kTimingBlock = WWE2K16_TIMING_BLOCK;
constexpr uint32_t kFrameRate = kTimingBlock + 8;

bool EnvDefaultOn(const char* name) {
  const char* value = std::getenv(name);
  return !(value != nullptr && std::strcmp(value, "0") == 0);
}

bool Keep60() {
  static const bool enabled = EnvDefaultOn("WWE2K16_KEEP_60");
  return enabled;
}

bool LateFlipImmediate() {
  return EnvDefaultOn("WWE2K16_LATE_FLIP");
}

bool SpeedFixEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("WWE2K16_SPEED_FIX");
#if defined(__ANDROID__)
    return !(value && std::strcmp(value, "0") == 0);
#else
    return value && std::strcmp(value, "1") == 0;
#endif
  }();
  return enabled;
}

uint32_t FrameCap() {
  static const uint32_t cap = [] {
    const char* value = std::getenv("WWE2K16_FRAME_CAP");
    return value ? uint32_t(std::atoi(value)) : 0u;
  }();
  return cap;
}

// --- Heat protection (r18) ------------------------------------------------------------------------------------------
// GameActivity feeds the game the Android thermal status (PowerManager, 0 NONE .. 6 SHUTDOWN) through the JNI call
// below. When the phone is hot the governor lowers the frame cap to the next value the game already paces cleanly
// (60 -> 45 -> 30), so a long session stays steady instead of collapsing (a Mali-G57 fell from ~58 to 10 FPS from
// heat at a 60 cap, vivo V2432, 2026-11-23). It only lowers, never above the player's own cap, and lifts again as the
// phone cools. WWE2K16_THERMAL_GUARD: 0 off, 1 normal (Adreno default), 2 earlier (Mali default, they throttle hard).
std::atomic<int> g_thermal_status{0};
// (r19) Android's heat forecast (PowerManager.getThermalHeadroom, 10 s ahead) as the same 0..6 scale, sent by
// GameActivity in BALANCED / COOL so the governor acts before the phone is hot, not after (1.2.0 testers: "runs smooth
// but the phone heats"). 0 when not used.
std::atomic<int> g_thermal_forecast{0};

int ThermalGuard() {
  static const int guard = [] {
    const char* v = std::getenv("WWE2K16_THERMAL_GUARD");
    return (v && *v) ? std::clamp(std::atoi(v), 0, 2) : 1;
  }();
  return guard;
}

// The highest cap the temperature allows right now (snapped to 30 / 45 / 60 so the game-speed fix stays on a step it
// handles). 60 = no heat limit.
uint32_t ThermalTarget() {
  const int guard = ThermalGuard();
  if (guard == 0) return 60;
  const int status = std::max(g_thermal_status.load(std::memory_order_relaxed),
                              g_thermal_forecast.load(std::memory_order_relaxed));
  if (guard == 2) {  // Mali: start earlier
    if (status >= 3) return 30;   // SEVERE+
    if (status >= 2) return 45;   // MODERATE
    return 60;
  }
  if (status >= 4) return 30;     // CRITICAL+
  if (status >= 3) return 45;     // SEVERE
  return 60;
}

// (r19) Hysteresis: a lowered ceiling is kept at least 20 s, then raised one step (30 -> 45 -> 60) at a time, so the
// frame rate no longer flips between 45 and 60 every few seconds as the reading hovers at a boundary (a Mali-G615:
// lowered, "cooled" 2 s later, lowered again, 2026-11-24).
uint32_t ThermalCeiling() {
  using clock = std::chrono::steady_clock;
  static uint32_t held = 60;
  static clock::time_point changed{};
  const uint32_t target = ThermalTarget();
  const auto now = clock::now();
  if (target < held) {
    held = target;
    changed = now;
  } else if (target > held && now - changed >= std::chrono::seconds(20)) {
    held = held == 30 ? 45u : 60u;
    changed = now;
  }
  return held;
}

// (r19) Frame generation forces a 30 cap (it doubles 30 to 60). When it gives up for the session (SDK patch 0009: the
// phone could not paint the in-between frames in time, twice) the cap goes back to the player's own frame rate
// (WWE2K16_FG_RESTORE_CAP: 0 = 60), so the game runs at what the phone can do instead of a plain 30.
uint32_t BaseCap() {
  static const uint32_t restore = [] {
    const char* v = std::getenv("WWE2K16_FG_RESTORE_CAP");
    return v ? uint32_t(std::atoi(v)) : 0u;
  }();
  static bool announced = false;
  if (std::getenv("WWE2K16_FRAMEGEN") && std::getenv("WWE2K16_FRAMEGEN")[0] == '1' && wwe2k16_framegen_gave_up &&
      wwe2k16_framegen_gave_up()) {
    if (!announced) {
      announced = true;
      REXLOG_INFO("[wwe2k16-fg] frame generation stopped: frame cap back to {} FPS", restore ? restore : 60u);
    }
    return restore ? restore : 60u;
  }
  return FrameCap() ? FrameCap() : 60u;
}

// The cap actually enforced this frame: the player's cap (60 when they chose "no host cap"), lowered by heat.
uint32_t EffectiveCap() {
  return std::min(BaseCap(), ThermalCeiling());
}

uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The D3D device, captured from the GPU interrupt handler's second argument.
uint32_t g_d3d_device = 0;

}  // namespace

// --- Entrance scene settings ----------------------------------------------------------------------------------------

// Entrances (and victory motions) are the game's 30 FPS phases: the engine asks for 30 at their start (ToThirty, kept at
// 60 here) and for 60 at their end (ToSixty). The performance log (2026-11-22) showed them as the slowest part (38-48
// FPS against 50-56 in the match: the whole arena and its crowd on screen). WWE2K16_ENTRANCE_CROWD (percent, the
// launcher's ENTRANCE CROWD) sets the crowd share for those phases only; the match keeps its own (CROWD). The SDK reads
// gpu_scene_crowd_density at every crowd draw, so the switch applies from the next frame.
namespace {
int EntranceCrowd() {
  static const int value = [] {
    const char* v = std::getenv("WWE2K16_ENTRANCE_CROWD");
    return (v && *v) ? std::clamp(std::atoi(v), 0, 100) : -1;  // -1 = same as the match
  }();
  return value;
}
int g_match_crowd = -1;
bool g_in_entrance = false;

void EntranceScene(bool entering) {
#if WWE2K16_SDK_K14
  const int entrance = EntranceCrowd();
  if (entrance < 0 || entering == g_in_entrance) return;
  if (g_match_crowd < 0) g_match_crowd = std::atoi(rex::cvar::GetFlagByName("gpu_scene_crowd_density").c_str());
  g_in_entrance = entering;
  const int value = entering ? std::min(entrance, g_match_crowd) : g_match_crowd;
  rex::cvar::SetFlagByName("gpu_scene_crowd_density", std::to_string(value));
  REXLOG_INFO("[wwe2k16-entrance] {}: crowd {}%", entering ? "entrance phase" : "match phase", value);
#else
  (void)entering;
#endif
}
}  // namespace

// --- Engine frame rate --------------------------------------------------------------------------------------------

// The engine's own switch to 30 FPS. Its animation runs on these time steps (not on wall-clock time), so skipping the
// switch keeps both the display rate and the simulation step at 60: entrances play at normal speed, twice as smooth.
// Forcing only the D3D present interval instead would keep the 1/30 s step: entrances at double speed.
extern "C" PPC_FUNC(WWE2K16_IMP_ToThirty);
extern "C" PPC_FUNC(WWE2K16_FN_ToThirty) {
  const uint32_t rate = PPC_LOAD_U32(kFrameRate);
  REXLOG_INFO("[wwe2k16-rate] halve request at rate {} ({})", rate, Keep60() ? "suppressed, staying at 60" : "applied");
  EntranceScene(true);
  if (Keep60()) {
    return;
  }
  WWE2K16_IMP_ToThirty(ctx, base);
}

// Logged only: marks the end of a 30 FPS phase in the console behaviour.
extern "C" PPC_FUNC(WWE2K16_IMP_ToSixty);
extern "C" PPC_FUNC(WWE2K16_FN_ToSixty) {
  REXLOG_INFO("[wwe2k16-rate] restore request at rate {}", PPC_LOAD_U32(kFrameRate));
  EntranceScene(false);
  WWE2K16_IMP_ToSixty(ctx, base);
}

// Logged only: one line per change of the game's requested present interval (1 = 60 FPS, 2 = 30 FPS), to see whether
// any path still asks for 30 FPS with the rate held at 60.
extern "C" PPC_FUNC(WWE2K16_FN_SetPresentInterval) {
  static uint32_t last = UINT32_MAX;
  const uint32_t interval = ctx.r4.u32;
  if (interval != last) {
    last = interval;
    REXLOG_INFO("[wwe2k16-interval] game set present interval {} (frame rate {})", interval,
                PPC_LOAD_U32(kFrameRate));
  }
  PPC_STORE_U32(ctx.r3.u32 + 14000, interval);
}

// --- Game speed fix and frame cap -----------------------------------------------------------------------------------

namespace {

float LoadF32(uint8_t* base, uint32_t address) { return std::bit_cast<float>(uint32_t(PPC_LOAD_U32(address))); }
void StoreF32(uint8_t* base, uint32_t address, float value) { PPC_STORE_U32(address, std::bit_cast<uint32_t>(value)); }

bool Near(float value, float expected) { return std::fabs(value - expected) <= std::fabs(expected) * 0.02f; }

struct SpeedFix {
  enum class State { kWaiting, kActive, kOff } state = State::kWaiting;
  uint64_t frames = 0;
  uint64_t last_ns = 0;
  double ema = 1.0 / 60.0;
  double log_sum = 0.0;
  uint32_t log_count = 0;
};

// The block holds the game's own values for its current rate (60 or 30, PAL 50 or 25): checked before any write.
bool BlockLooksRight(uint8_t* base) {
  const uint32_t rate = PPC_LOAD_U32(kTimingBlock + 8);
  if (rate != 60 && rate != 30 && rate != 50 && rate != 25) return false;
  const float step = 1.0f / float(rate);
  return Near(LoadF32(base, kTimingBlock + 28), step) && Near(LoadF32(base, kTimingBlock + 32), float(rate)) &&
         Near(LoadF32(base, kTimingBlock + 36), step * 1000.0f) &&
         Near(LoadF32(base, kTimingBlock + 60), step * 60.0f) &&
         Near(LoadF32(base, kTimingBlock + 64), 1.0f / (step * 60.0f));
}

void LogBlock(uint8_t* base, const char* what) {
  std::string words;
  for (uint32_t offset = 0; offset <= 64; offset += 4) {
    const uint32_t word = PPC_LOAD_U32(kTimingBlock + offset);
    words += fmt::format(" +{}={:08X}({:g})", offset, word, std::bit_cast<float>(word));
  }
  REXLOG_INFO("[wwe2k16-speed] {} timing block 0x{:08X}:{}", what, kTimingBlock, words);
}

void SpeedFixOnFrame(uint8_t* base, uint64_t now_ns) {
  static SpeedFix fix;
  if (fix.state == SpeedFix::State::kOff) return;
  const uint64_t prev_ns = fix.last_ns;
  fix.last_ns = now_ns;
  if (++fix.frames < 30 || !prev_ns) return;  // boot frames
  if (fix.state == SpeedFix::State::kWaiting) {
    if (!BlockLooksRight(base)) {
      if (fix.frames < 600) return;  // the game may fill the block a little later
      LogBlock(base, "not the expected values in the");
      REXLOG_WARN("[wwe2k16-speed] game speed fix OFF: the timing block does not match");
      fix.state = SpeedFix::State::kOff;
      return;
    }
    LogBlock(base, "game speed fix ON,");
    fix.state = SpeedFix::State::kActive;
  }
  const double dt = double(now_ns - prev_ns) * 1e-9;
  if (dt > 0.25) return;  // a loading frame: not fed into the step
  fix.ema += 0.3 * (std::clamp(dt, 1.0 / 60.0, 1.0 / 12.0) - fix.ema);
  const float step = float(fix.ema);
  StoreF32(base, kTimingBlock + 28, step);
  StoreF32(base, kTimingBlock + 32, 1.0f / step);
  StoreF32(base, kTimingBlock + 36, step * 1000.0f);
  StoreF32(base, kTimingBlock + 60, step * 60.0f);
  StoreF32(base, kTimingBlock + 64, 1.0f / (step * 60.0f));
  fix.log_sum += dt;
  if (++fix.log_count >= 900) {
    REXLOG_INFO("[wwe2k16-speed] step {:.2f} ms ({:.1f} fps), real frame avg {:.2f} ms over {} frames, game rate {}",
                fix.ema * 1000.0, 1.0 / fix.ema, fix.log_sum * 1000.0 / fix.log_count, fix.log_count,
                PPC_LOAD_U32(kFrameRate));
    fix.log_sum = 0.0;
    fix.log_count = 0;
  }
}

// WWE2K16_FRAME_CAP=30: the guest waits here until a 30 FPS frame's time is up (the speed fix then sees 33 ms frames
// and steps the game by 1/30 s, so it runs at normal speed).
void FrameCapOnFrame() {
  const uint32_t cap = EffectiveCap();
  // Log when heat moves the cap, so the perf log shows the governor working. (r19) Only heat: a cap below 60 that the
  // player chose (45 / 30 / frame generation) is not "phone hot" (1.2.0 logged "status 0: lowered to 30").
  {
    static uint32_t last_heat = 60;
    const uint32_t heat = ThermalCeiling();
    const uint32_t base = BaseCap();
    const uint32_t heat_cap = heat < base ? heat : 60u;
    if (heat_cap != last_heat) {
      if (heat_cap < 60) {
        REXLOG_INFO("[wwe2k16-thermal] phone hot (status {}, forecast {}): frame cap lowered to {} FPS",
                    g_thermal_status.load(), g_thermal_forecast.load(), heat_cap);
      } else {
        REXLOG_INFO("[wwe2k16-thermal] phone cooled: frame cap back to {} FPS", base);
      }
      last_heat = heat_cap;
    }
  }
  if (cap < 10 || cap >= 60) return;
  static uint64_t next_ns = 0;
  const uint64_t period_ns = 1'000'000'000ull / cap;
  uint64_t now = SteadyNs();
  if (next_ns && now < next_ns) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(next_ns - now));
    now = SteadyNs();
  }
  next_ns = (next_ns && now < next_ns + period_ns) ? next_ns + period_ns : now + period_ns;
}

}  // namespace

// --- Late frames ----------------------------------------------------------------------------------------------------

// After each Present the GPU waits until the frame's flip is processed. When a frame finishes after the vblank it was
// due at, Xbox D3D flips it immediately only if less than `threshold` % of the current vblank period has passed;
// otherwise the flip waits for the next vblank and the GPU stalls with it: a 17 ms frame costs 33 ms, a one-frame drop
// to 30 FPS. Heavy frames (camera cuts, close-up zooms with depth of field) hit exactly this. The host presents whole
// frames with FIFO vsync, so a 100 % threshold cannot tear: the late frame is shown at the next host vblank and the
// game carries on instead of losing a frame. Frames on time are unaffected (the 60 FPS cap stays).
namespace {
struct FlipStats {
  uint64_t window_start_ns = 0;
  uint32_t flips = 0;
  uint32_t late = 0;
  uint32_t late_bucket[4] = {};  // late by < 25 %, < 50 %, < 100 % of a vblank period, >= 1 period
};
}  // namespace

extern "C" PPC_FUNC(WWE2K16_IMP_ChooseFlip);
namespace wwe2k16 {
void ResolveCoherencyOnFrame();  // resolve_coherency.cpp
}

extern "C" PPC_FUNC(WWE2K16_FN_ChooseFlip) {
  wwe2k16::RecordGameFrame();
  FrameCapOnFrame();
  {
    const uint64_t now_ns = SteadyNs();
    if (SpeedFixEnabled()) SpeedFixOnFrame(base, now_ns);
    wwe2k16::frame_monitor::OnGameFrame(now_ns);
    wwe2k16::android_driver::OnFrame();
    wwe2k16::ResolveCoherencyOnFrame();
  }
  static uint32_t last_threshold = UINT32_MAX;
  static FlipStats stats;
  const uint32_t orig_threshold = ctx.r3.u32 & 0xFF;
  const uint32_t interval = (ctx.r3.u32 >> 8) & 0xF;

  // Only allow immediate late flip if interval <= 1 (60 FPS mode).
  // When interval >= 2 (30 FPS cinematics / entrances), overriding threshold to 100 causes frames
  // that finish after 1 vblank (16.7ms) to flip immediately instead of waiting for the full 2 vblanks (33.3ms),
  // which produces chaotic 38-44 FPS and destroys animation smoothness.
  const bool override_late_flip = (interval <= 1 && LateFlipImmediate());
  if (override_late_flip) {
    ctx.r3.u32 = (ctx.r3.u32 & ~0xFFu) | 100;
  }

  const uint32_t applied_threshold = ctx.r3.u32 & 0xFF;
  if (applied_threshold != last_threshold) {
    last_threshold = applied_threshold;
    REXLOG_INFO("[wwe2k16-flip] present-immediate threshold: game {}% -> applied {}% interval {} ({})",
                orig_threshold, applied_threshold, interval,
                override_late_flip ? "late frames flip immediately" : "kept");
  }
  // Per-second late-frame statistics, logged only for seconds that had late frames: a late frame is a visible hitch
  // even when the second still counts 60 flips. Device fields: +16952 vblank count, +16956 time of the last vblank
  // (mftb, 50 MHz), +16960 vblank of the last flip.
  const uint32_t device = g_d3d_device;
  if (device && interval) {
    const uint32_t vblank_count = PPC_LOAD_U32(device + 16952);
    const uint32_t due = PPC_LOAD_U32(device + 16960) + interval;
    ++stats.flips;
    if (int32_t(vblank_count - due) >= 0) {
      ++stats.late;
      const uint32_t periods_late = vblank_count - due;
      const uint32_t since_vblank_ticks = uint32_t(PPC_QUERY_TIMEBASE()) - PPC_LOAD_U32(device + 16956);
      const uint32_t percent = since_vblank_ticks / (50'000'000 / 60 / 100);
      ++stats.late_bucket[periods_late ? 3 : percent < 25 ? 0 : percent < 50 ? 1 : 2];
    }
    const uint64_t now = SteadyNs();
    if (!stats.window_start_ns) {
      stats.window_start_ns = now;
    } else if (now - stats.window_start_ns >= 1'000'000'000ull) {
      if (stats.late) {
        REXLOG_INFO("[wwe2k16-flip] flips={} late={} (<25%={} <50%={} <100%={} >=1 vblank={}) rate={}",
                    stats.flips, stats.late, stats.late_bucket[0], stats.late_bucket[1], stats.late_bucket[2],
                    stats.late_bucket[3], PPC_LOAD_U32(kFrameRate));
      }
      stats = FlipStats{};
      stats.window_start_ns = now;
    }
  }
  WWE2K16_IMP_ChooseFlip(ctx, base);
}

extern "C" PPC_FUNC(WWE2K16_IMP_GpuInterrupt);
extern "C" PPC_FUNC(WWE2K16_FN_GpuInterrupt) {
  g_d3d_device = ctx.r4.u32;
  WWE2K16_IMP_GpuInterrupt(ctx, base);
}

// r18: GameActivity reports the Android thermal status here (PowerManager.OnThermalStatusChangedListener), so the
// heat governor above can lower the frame cap while the phone is hot. 0 NONE .. 6 SHUTDOWN.
extern "C" JNIEXPORT void JNICALL Java_com_savanindurkar7_wwe2k16_GameActivity_nativeSetThermalStatus(JNIEnv*, jclass,
                                                                                                     jint status) {
  g_thermal_status.store(int(status), std::memory_order_relaxed);
}

// r19: the heat forecast (getThermalHeadroom mapped to the same 0..6 scale by GameActivity).
extern "C" JNIEXPORT void JNICALL Java_com_savanindurkar7_wwe2k16_GameActivity_nativeSetThermalForecast(JNIEnv*, jclass,
                                                                                                       jint status) {
  g_thermal_forecast.store(int(status), std::memory_order_relaxed);
}
