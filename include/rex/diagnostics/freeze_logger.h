/**
 * @file        rex/diagnostics/freeze_logger.h
 * @brief       Always-on freeze logger: a low-rate present watchdog plus a
 *              lock-free ring of kernel synchronization events, written to a
 *              report file when the game stops presenting. Intended for normal
 *              player builds so a freeze anywhere records its own cause.
 */
#pragma once

#include <cstdint>
#include <cstdio>

namespace rex::diagnostics::freeze_logger {

// Kernel synchronization event kinds recorded in the ring buffer.
enum class SyncType : uint32_t {
  kUnknown = 0,
  kEventCreate,
  kEventSet,
  kEventReset,
  kEventPulse,
  kSemaphoreRelease,
  kWaitBegin,
  kWaitEnd,
  kThreadCreate,
  kThreadExit,
};

// Starts the always-on logger: installs the one-shot capture signal handler and
// a 1 Hz watchdog thread. No-op when already running, or when disabled with
// WWE2K16_FREEZE_LOG=0 / REX_FREEZE_LOG_DISABLE=1.
// Cheap and safe to call once.
void Start();

// Stops the watchdog (used at shutdown). Safe to call when never started.
void Stop();

// Writes an exit snapshot (same contents as a freeze report, file
// wwe2k16-exit-<stamp>.txt, newest 3 kept) when the player closes the window.
// Catches freezes where the picture stops but presents continue (GitHub
// #21/#25), which the present-gap watchdog cannot see. Call before Stop() and
// before the title is terminated. No-op when disabled or nothing was presented.
void SnapshotOnClose();

// True when the logger is running (not disabled by environment).
bool IsEnabled();

// Hot-path recorders. All are no-ops when the logger is disabled. They never
// allocate, never take a lock, and only touch atomics + a fixed slot.
void RecordPresent();
void RecordVdSwap();
void RecordSyncEvent(SyncType type, uint32_t handle, uint32_t object_type, const char* name);

// Records the current guest thread as blocked on `handle` (object_type/name) and
// then as unblocked. `guest_lr` is the guest link register at the wait call, a
// useful proxity for the blocked guest PC (the recompiled runtime keeps no PC).
void RecordWaitBegin(uint32_t handle, uint32_t object_type, const char* name, uint64_t guest_lr);
void RecordWaitEnd(uint32_t handle);

// Registers the calling host thread as a guest thread slot (for the report) and
// unregisters it. Called from XThread's host trampoline.
void RegisterThread(uint32_t guest_thread_id, const char* name);
void UnregisterThread();

// Total number of sync events pushed into the ring (overhead accounting).
uint64_t RecordedEventCount();

// --- Freeze-report extras -----------------------------------------------------

// Optional guest-state provider installed by the game. When set, it is invoked
// while the report file is open (before the log tail) so it can append
// guest-memory-derived state (game objects, present-device fields, fixed
// .data globals). It runs on the watchdog thread, which is NOT a
// bound guest ThreadState, so the implementation must use a captured guest
// memory base / Memory pointer rather than REX_KERNEL_MEMORY().
using GuestStateDumpFn = void (*)(FILE* file, uint64_t now_ns);
void SetGuestStateDumpFn(GuestStateDumpFn fn);

// The access-violation recovery paths call this once per recovered page. Cheap
// relaxed atomics + one timestamp; safe from any thread. The report prints the
// per-kind count plus the last guest address / protect / age.
enum class MemoryRecoveryKind : uint32_t {
  kGuestVirtualStaleProtect = 0,
  kReleasedPhysical,
  kStalePhysical,
  kCount,
};
void RecordMemoryRecovery(MemoryRecoveryKind kind, uint32_t guest_address, uint32_t protect);

}  // namespace rex::diagnostics::freeze_logger
