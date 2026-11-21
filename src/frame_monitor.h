// wwe2k16 - frame monitor and freeze diagnostics (Android port; works on any POSIX build).
//
// Measures before fixing (the ~10 s freeze when a match starts): every presented game frame is written to
// <log dir>/frames.csv with what the SDK spent time on during it (pipeline creation, texture loads, guest file
// reads: rex/perf/stall_counters.h). A frame over 200 ms adds a [wwe2k16-stall] line to the log with those numbers.
// While no frame arrives for 400 ms a watchdog samples every thread of the process (name, state, CPU time used in the
// last 100 ms, kernel wait channel) and logs the busiest ones: that shows whether the GPU thread is compiling, the
// game's loader thread is reading files, or everything is waiting on something.
//
// The same watchdog thread also keeps the game's threads on the chosen CPU cores (WWE2K16_CPU_CORES) and reports
// frame work to Android's performance hint API (WWE2K16_PERF_HINT).
//
//   WWE2K16_LOG_DIR=<dir>          where frames.csv goes (Android: the launcher's logs folder); unset = off
//   WWE2K16_FRAME_MONITOR=0|1|2    on-screen monitor: off | FPS | FPS + frame time and loading work
//   WWE2K16_CPU_CORES=all|auto|big all cores | not the little cores | only the fastest cores
//   WWE2K16_PERF_HINT=1|0          Android 12+ performance hint session for the frame thread

#pragma once

#include <cstdint>
#include <string>

namespace wwe2k16::frame_monitor {

// Once per presented game frame, on the guest thread that presents it.
void OnGameFrame(uint64_t now_ns);

// Starts the watchdog thread (stall sampling, core pinning). Safe to call more than once.
void Start();
void Stop();

// For the on-screen monitor: the latest values, smoothed.
struct Snapshot {
  float fps = 0.0f;
  float frame_ms = 0.0f;
  float worst_ms = 0.0f;           // worst frame in the last second
  uint32_t pipelines_per_s = 0;    // pipelines built in the last second
  uint32_t texture_loads_per_s = 0;
  uint32_t file_kb_per_s = 0;
};
Snapshot GetSnapshot();
int OverlayMode();  // WWE2K16_FRAME_MONITOR

}  // namespace wwe2k16::frame_monitor
