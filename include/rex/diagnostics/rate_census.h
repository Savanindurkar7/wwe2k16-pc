/**
 * @file        rex/diagnostics/rate_census.h
 * @brief       Optional per-second runtime rate census.
 */

#pragma once

#include <cstdint>
#include <optional>

namespace rex::diagnostics::rate_census {

struct Sample {
  uint64_t t_ms = 0;
  double vblank = 0.0;
  double irq0 = 0.0;
  double irq1 = 0.0;
  double vdswap = 0.0;
  double present = 0.0;
  double guest_ticks_per_s = 0.0;
  double guest_ms_per_s = 0.0;
  uint64_t vblank_interval_ticks = 0;
  double refresh_hz = 0.0;
  double gpu_busy_ms = 0.0;
  double gpu_wait_ms = 0.0;
  uint64_t gpu_wait_count = 0;
  double swap_wait_ms = 0.0;
  double submission_worker_busy_ms = 0.0;
  double submission_worker_jobs_per_s = 0.0;
  uint32_t vdswap_tid = 0;
  double vdswap_thread_cpu_pct = 0.0;
  // Exclusive wall time per second spent by the GPU command thread in each
  // CpPhase (nested phases are subtracted from their parent).
  double cp_phase_ms[6] = {};
  uint64_t cp_wait_reg_mem_count = 0;
};

// Command processor phases timed by ScopedCpPhase.
enum class CpPhase : uint32_t {
  kWaitRegMem = 0,  // Guest WAIT_REG_MEM not satisfied on the first poll.
  kDraw,            // IssueDraw minus nested phases.
  kCopy,            // IssueCopy (resolves) minus nested phases.
  kSwap,            // IssueSwap minus nested phases.
  kSubmit,          // EndSubmission (host queue submit).
  kSubmitDrain,     // CP time waiting for the asynchronous submit worker.
  kCount,
};

// Starts the census when WWE2K16_RATE_CENSUS is exactly "1". The environment is
// sampled once by this call; event recording is otherwise a no-op when the
// census is disabled.
void Start(uint64_t vblank_interval_ticks, double refresh_rate_hz);

// Stops the reporting thread. Safe to call when Start was never enabled.
void Stop();

// Returns the most recent complete one-second sample, if one has been
// published. The sample is updated by the census thread once per second.
std::optional<Sample> GetLatestSample();

void RecordVblank();
void RecordInterrupt(uint32_t source);
void RecordVdSwap();
// Presentation interval requested by the game for the current swap.
void RecordPresentInterval(uint32_t interval);
void RecordPresent();
void RecordGpuBusyStart();
void RecordGpuBusyEnd();
void RecordGpuWaitStart();
void RecordGpuWaitEnd();
void RecordSubmissionWorkerBusyStart();
void RecordSubmissionWorkerBusyEnd();
void RecordSubmissionWorkerJob();
// Records one successful texture descriptor-set update or reuse for the
// vertex (true) or pixel (false) shader stage.
void RecordDescriptorSetUpdate(bool is_vertex, bool reused);

bool IsEnabled();
uint64_t NowNs();
void CpPhaseEnter(CpPhase phase, uint64_t now_ns);
void CpPhaseExit(uint64_t now_ns);
// Records one unsatisfied WAIT_REG_MEM: poll target (register index, or
// physical address with is_memory), total time it blocked, the per-iteration
// sleep payload in milliseconds, and the number and sum of poll iterations.
void RecordWaitRegMem(bool is_memory, uint32_t poll_addr, uint64_t wait_ns,
                      uint64_t payload_ms, uint64_t iterations,
                      uint64_t payload_ms_sum);
// Global critical region contention (called by global_critical_region when the
// census is on): owner is the host tid holding the lock (0 if unknown).
// OnGlobalLockContended may send SIGPROF to the owner (WWE2K16_LOCK_OWNER_SAMPLE=1,
// only when a SIGPROF handler is installed, i.e. WWE2K16_THREAD_PROFILE is set) so
// the thread profiler records what the owner is doing.
void OnGlobalLockContended(uint32_t owner_tid);
void RecordGlobalLockWait(uint32_t owner_tid, uint64_t wait_ns);

// Times a command processor phase when the census is enabled; a no-op
// otherwise. Must be used on the GPU command thread only.
class ScopedCpPhase {
 public:
  explicit ScopedCpPhase(CpPhase phase) : active_(IsEnabled()) {
    if (active_) {
      CpPhaseEnter(phase, NowNs());
    }
  }
  ~ScopedCpPhase() {
    if (active_) {
      CpPhaseExit(NowNs());
    }
  }
  ScopedCpPhase(const ScopedCpPhase&) = delete;
  ScopedCpPhase& operator=(const ScopedCpPhase&) = delete;

 private:
  bool active_;
};

}  // namespace rex::diagnostics::rate_census
