/**
 * @file        core/rate_census.cpp
 * @brief       Optional per-second runtime rate census.
 */

#include <rex/diagnostics/freeze_logger.h>
#include <rex/diagnostics/rate_census.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <thread>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

#if REX_PLATFORM_LINUX
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rex::diagnostics::rate_census {
namespace {

constexpr size_t kCpPhaseCount = size_t(CpPhase::kCount);
static_assert(kCpPhaseCount == sizeof(Sample::cp_phase_ms) / sizeof(double));

// Global lock waits in the current census second, per (waiter, owner) pair.
struct LockWait {
  uint32_t waiter = 0;
  uint32_t owner = 0;
  uint64_t ns = 0;
  uint64_t count = 0;
};

std::string ThreadName(uint32_t tid) {
#if REX_PLATFORM_LINUX
  std::ifstream comm("/proc/self/task/" + std::to_string(tid) + "/comm");
  std::string name;
  if (comm && std::getline(comm, name)) {
    return name;
  }
#endif
  return "?";
}

// WAIT_REG_MEM poll targets seen in the current census second.
struct WaitTarget {
  uint32_t key = 0;  // Poll address; bit 31 set for memory polls.
  uint64_t ns = 0;
  uint64_t count = 0;
  uint64_t iter_total = 0;
  uint64_t payload_ms_total = 0;      // Per-wait payload operand, summed.
  uint64_t payload_ms_sum_total = 0;  // Payload * iterations, summed.
  uint64_t payload_ms_max = 0;
  uint64_t payload_ms_sum_max = 0;
  uint64_t duration_ns_max = 0;
};

struct Counters {
  std::atomic<uint64_t> vblank{0};
  std::atomic<uint64_t> irq0{0};
  std::atomic<uint64_t> irq1{0};
  std::atomic<uint64_t> vdswap{0};
  std::atomic<uint64_t> present{0};
  std::atomic<uint64_t> gpu_busy_ns{0};
  std::atomic<uint64_t> gpu_wait_ns{0};
  std::atomic<uint64_t> gpu_wait_count{0};
  std::atomic<uint64_t> swap_wait_ns{0};
  std::atomic<uint64_t> submission_worker_busy_ns{0};
  std::atomic<uint64_t> submission_worker_jobs{0};
  std::atomic<uint32_t> vdswap_tid{0};
  std::atomic<uint64_t> swap_start_ns{0};
  std::atomic<uint64_t> cp_phase_ns[kCpPhaseCount] = {};
  std::atomic<uint64_t> cp_phase_calls[kCpPhaseCount] = {};
  std::atomic<uint64_t> cp_wait_reg_mem_count{0};
  std::atomic<uint64_t> present_interval[4] = {};  // 0, 1, 2, other
  std::atomic<uint64_t> descriptor_set_writes[2] = {};
  std::atomic<uint64_t> descriptor_set_reuses[2] = {};
};

struct Snapshot {
  uint64_t vblank;
  uint64_t irq0;
  uint64_t irq1;
  uint64_t vdswap;
  uint64_t present;
  uint64_t gpu_busy_ns;
  uint64_t gpu_wait_ns;
  uint64_t gpu_wait_count;
  uint64_t swap_wait_ns;
  uint64_t submission_worker_busy_ns;
  uint64_t submission_worker_jobs;
  uint32_t vdswap_tid;
  uint64_t cp_phase_ns[kCpPhaseCount];
  uint64_t cp_wait_reg_mem_count;
  uint64_t cp_phase_calls[kCpPhaseCount];
  uint64_t descriptor_set_writes[2];
  uint64_t descriptor_set_reuses[2];
};

uint64_t QueryHostNanoseconds() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
}

// Cheap nanosecond clock for the per-draw phase timers (two reads per draw): the time stamp counter scaled by a
// ratio calibrated once against steady_clock. steady_clock is QueryPerformanceCounter on Windows, noticeably more
// expensive per call. Same epoch and units as QueryHostNanoseconds, to within the calibration error.
#if defined(__x86_64__) || defined(_M_X64)
struct TscClock {
  uint64_t tsc0 = 0;
  uint64_t ns0 = 0;
  double ns_per_tick = 0.0;
  TscClock() {
    const uint64_t start_ns = QueryHostNanoseconds();
    const uint64_t start_tsc = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t end_ns = QueryHostNanoseconds();
    const uint64_t end_tsc = __rdtsc();
    if (end_tsc > start_tsc && end_ns > start_ns) {
      tsc0 = end_tsc;
      ns0 = end_ns;
      ns_per_tick = double(end_ns - start_ns) / double(end_tsc - start_tsc);
    }
  }
  uint64_t Now() const {
    if (ns_per_tick <= 0.0) {
      return QueryHostNanoseconds();
    }
    return ns0 + uint64_t(double(__rdtsc() - tsc0) * ns_per_tick);
  }
};
const TscClock& GetTscClock() {
  static const TscClock clock;
  return clock;
}
uint64_t QueryFastNanoseconds() { return GetTscClock().Now(); }
#else
uint64_t QueryFastNanoseconds() { return QueryHostNanoseconds(); }
#endif

struct ThreadTicks {
  std::string name;
  uint64_t utime = 0;
  uint64_t stime = 0;
};

using ThreadTickMap = std::unordered_map<uint32_t, ThreadTicks>;

bool ParseThreadStat(const std::filesystem::path& path, ThreadTicks& ticks_out) {
#if !REX_PLATFORM_LINUX
  (void)path;
  (void)ticks_out;
  return false;
#else
  std::ifstream stream(path);
  std::string line;
  if (!stream || !std::getline(stream, line)) {
    return false;
  }

  const size_t name_start = line.find('(');
  const size_t name_end = line.rfind(") ");
  if (name_start == std::string::npos || name_end <= name_start) {
    return false;
  }

  std::istringstream fields(line.substr(name_end + 2));
  std::string state;
  if (!(fields >> state)) {
    return false;
  }

  uint64_t utime = 0;
  uint64_t stime = 0;
  for (int field = 4; field <= 15; ++field) {
    std::string value;
    if (!(fields >> value)) {
      return false;
    }
    if (field == 14) {
      try {
        utime = std::stoull(value);
      } catch (const std::exception&) {
        return false;
      }
    } else if (field == 15) {
      try {
        stime = std::stoull(value);
      } catch (const std::exception&) {
        return false;
      }
    }
  }

  ticks_out.name = line.substr(name_start + 1, name_end - name_start - 1);
  ticks_out.utime = utime;
  ticks_out.stime = stime;
  return true;
#endif
}

ThreadTickMap ReadThreadTicks() {
  ThreadTickMap result;
#if REX_PLATFORM_LINUX
  std::error_code error;
  const std::filesystem::path task_root("/proc/self/task");
  for (const auto& entry : std::filesystem::directory_iterator(task_root, error)) {
    if (error) {
      break;
    }
    uint32_t tid = 0;
    try {
      tid = static_cast<uint32_t>(std::stoul(entry.path().filename().string()));
    } catch (const std::exception&) {
      continue;
    }
    ThreadTicks ticks;
    if (ParseThreadStat(entry.path() / "stat", ticks)) {
      result.emplace(tid, std::move(ticks));
    }
  }
#endif
  return result;
}

struct ThreadCpu {
  uint32_t tid = 0;
  std::string name;
  double percent = 0.0;
};

struct ThreadCpuReport {
  std::vector<ThreadCpu> top;
  ThreadCpu vdswap;
};

ThreadCpuReport CalculateThreadCpu(const ThreadTickMap& previous,
                                   const ThreadTickMap& current, uint64_t elapsed_ms,
                                   uint32_t vdswap_tid) {
  ThreadCpuReport report;
  if (elapsed_ms == 0) {
    return report;
  }

#if REX_PLATFORM_LINUX
  const long clock_ticks = std::max<long>(1, sysconf(_SC_CLK_TCK));
#else
  constexpr long clock_ticks = 100;
#endif
  for (const auto& [tid, ticks] : current) {
    const auto previous_it = previous.find(tid);
    if (previous_it == previous.end()) {
      continue;
    }
    const uint64_t current_ticks = ticks.utime + ticks.stime;
    const uint64_t previous_ticks = previous_it->second.utime + previous_it->second.stime;
    if (current_ticks < previous_ticks) {
      continue;
    }
    const uint64_t delta_ticks = current_ticks - previous_ticks;
    ThreadCpu cpu{
        tid,
        ticks.name,
        static_cast<double>(delta_ticks) * 100.0 * 1000.0 /
            (static_cast<double>(clock_ticks) * static_cast<double>(elapsed_ms)),
    };
    report.top.push_back(std::move(cpu));
  }

  if (vdswap_tid != 0) {
    const auto top_it = std::find_if(report.top.begin(), report.top.end(),
                                     [vdswap_tid](const ThreadCpu& cpu) {
                                       return cpu.tid == vdswap_tid;
                                     });
    if (top_it != report.top.end()) {
      report.vdswap = *top_it;
    } else {
      const auto current_it = current.find(vdswap_tid);
      if (current_it != current.end()) {
        report.vdswap.tid = vdswap_tid;
        report.vdswap.name = current_it->second.name;
      }
    }
  }

  std::sort(report.top.begin(), report.top.end(), [](const ThreadCpu& left, const ThreadCpu& right) {
    if (left.percent != right.percent) {
      return left.percent > right.percent;
    }
    return left.tid < right.tid;
  });
  if (report.top.size() > 5) {
    report.top.resize(5);
  }
  return report;
}

std::string FormatThreadCpu(const std::vector<ThreadCpu>& threads) {
  if (threads.empty()) {
    return "none";
  }
  std::string result;
  for (const ThreadCpu& thread : threads) {
    if (!result.empty()) {
      result += '|';
    }
    result += fmt::format("{}:{:.3f}", thread.name, thread.percent);
  }
  return result;
}

class RateCensus {
 public:
  ~RateCensus() { Stop(); }

  void Start(uint64_t vblank_interval_ticks, double refresh_rate_hz) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (started_) {
      return;
    }
    started_ = true;

    static const bool env_enabled = [] {
      const auto is_one = [](const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && std::string_view(value) == "1";
      };
      return is_one("WWE2K16_RATE_CENSUS");
    }();
    if (!env_enabled) {
      return;
    }

    vblank_interval_ticks_ = vblank_interval_ticks;
    refresh_rate_hz_ = refresh_rate_hz;
    {
      const char* sample = std::getenv("WWE2K16_LOCK_OWNER_SAMPLE");
      const char* profile = std::getenv("WWE2K16_THREAD_PROFILE");
      // SIGPROF is only safe to send when the thread profiler installed its handler.
      owner_sampling_ = sample && std::string_view(sample) == "1" && profile && *profile;
    }
    rex::thread::g_global_lock_census.store(true, std::memory_order_relaxed);
    enabled_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    logger_thread_ = std::thread(&RateCensus::Run, this);
  }

  void Stop() {
    std::thread logger_thread;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (!started_) {
        return;
      }
      running_.store(false, std::memory_order_release);
      logger_thread = std::move(logger_thread_);
    }

    if (logger_thread.joinable()) {
      logger_thread.join();
    }
    enabled_.store(false, std::memory_order_release);
  }

  std::optional<Sample> GetLatestSample() const {
    std::lock_guard<std::mutex> lock(sample_mutex_);
    return latest_sample_;
  }

  void RecordVblank() {
    if (enabled_.load(std::memory_order_relaxed)) {
      counters_.vblank.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void RecordInterrupt(uint32_t source) {
    if (!enabled_.load(std::memory_order_relaxed)) {
      return;
    }
    if (source == 0) {
      counters_.irq0.fetch_add(1, std::memory_order_relaxed);
    } else if (source == 1) {
      counters_.irq1.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void RecordVdSwap() {
    if (enabled_.load(std::memory_order_relaxed)) {
      counters_.vdswap.fetch_add(1, std::memory_order_relaxed);
      const uint32_t tid = rex::thread::current_thread_system_id();
      uint32_t first_tid = 0;
      counters_.vdswap_tid.compare_exchange_strong(first_tid, tid,
                                                    std::memory_order_relaxed);
      counters_.swap_start_ns.store(QueryHostNanoseconds(), std::memory_order_release);
    }
  }

  void RecordPresentInterval(uint32_t interval) {
    if (enabled_.load(std::memory_order_relaxed)) {
      counters_.present_interval[interval < 3 ? interval : 3].fetch_add(1, std::memory_order_relaxed);
    }
  }

  void RecordPresent() {
    if (enabled_.load(std::memory_order_relaxed)) {
      counters_.present.fetch_add(1, std::memory_order_relaxed);
      const uint64_t swap_start = counters_.swap_start_ns.exchange(0, std::memory_order_acq_rel);
      const uint64_t now = QueryHostNanoseconds();
      if (swap_start != 0 && now > swap_start) {
        counters_.swap_wait_ns.fetch_add(now - swap_start, std::memory_order_relaxed);
      }
    }
  }

  void RecordGpuBusyStart() {
    if (enabled_.load(std::memory_order_relaxed) && !gpu_busy_active_) {
      gpu_busy_active_ = true;
      gpu_busy_start_ns_ = QueryHostNanoseconds();
    }
  }

  void RecordGpuBusyEnd() {
    if (!gpu_busy_active_) {
      return;
    }
    const uint64_t now = QueryHostNanoseconds();
    if (now > gpu_busy_start_ns_) {
      counters_.gpu_busy_ns.fetch_add(now - gpu_busy_start_ns_, std::memory_order_relaxed);
    }
    gpu_busy_active_ = false;
    gpu_busy_start_ns_ = 0;
  }

  void RecordGpuWaitStart() {
    if (!enabled_.load(std::memory_order_relaxed) || gpu_wait_active_) {
      return;
    }
    const uint64_t now = QueryHostNanoseconds();
    gpu_wait_active_ = true;
    gpu_wait_start_ns_ = now;
    counters_.gpu_wait_count.fetch_add(1, std::memory_order_relaxed);
    if (gpu_busy_active_) {
      if (now > gpu_busy_start_ns_) {
        counters_.gpu_busy_ns.fetch_add(now - gpu_busy_start_ns_, std::memory_order_relaxed);
      }
      gpu_busy_active_ = false;
      gpu_busy_start_ns_ = 0;
      gpu_busy_paused_ = true;
    }
  }

  void RecordGpuWaitEnd() {
    if (!gpu_wait_active_) {
      return;
    }
    const uint64_t now = QueryHostNanoseconds();
    if (now > gpu_wait_start_ns_) {
      counters_.gpu_wait_ns.fetch_add(now - gpu_wait_start_ns_, std::memory_order_relaxed);
    }
    gpu_wait_active_ = false;
    gpu_wait_start_ns_ = 0;
    if (gpu_busy_paused_ && enabled_.load(std::memory_order_relaxed)) {
      gpu_busy_active_ = true;
      gpu_busy_start_ns_ = now;
    }
    gpu_busy_paused_ = false;
  }

  void RecordSubmissionWorkerBusyStart() {
    if (enabled_.load(std::memory_order_relaxed) && !submission_worker_busy_active_) {
      submission_worker_busy_active_ = true;
      submission_worker_busy_start_ns_ = QueryHostNanoseconds();
    }
  }

  void RecordSubmissionWorkerBusyEnd() {
    if (!submission_worker_busy_active_) {
      return;
    }
    const uint64_t now = QueryHostNanoseconds();
    if (now > submission_worker_busy_start_ns_) {
      counters_.submission_worker_busy_ns.fetch_add(
          now - submission_worker_busy_start_ns_, std::memory_order_relaxed);
    }
    submission_worker_busy_active_ = false;
    submission_worker_busy_start_ns_ = 0;
  }

  void RecordSubmissionWorkerJob() {
    if (enabled_.load(std::memory_order_relaxed)) {
      counters_.submission_worker_jobs.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void RecordDescriptorSetUpdate(bool is_vertex, bool reused) {
    if (!enabled_.load(std::memory_order_relaxed)) {
      return;
    }
    const size_t stage = is_vertex ? 0 : 1;
    (reused ? counters_.descriptor_set_reuses[stage] : counters_.descriptor_set_writes[stage])
        .fetch_add(1, std::memory_order_relaxed);
  }

  bool IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }

  // Exclusive phase timing: entering a nested phase charges the elapsed time
  // to the enclosing phase, exiting resumes the enclosing phase's clock.
  void CpPhaseEnter(CpPhase phase, uint64_t now_ns) {
    if (cp_phase_depth_ > 0) {
      ChargeTopPhase(now_ns);
    }
    if (cp_phase_depth_ < kMaxPhaseDepth) {
      cp_phase_stack_[cp_phase_depth_] = phase;
    }
    if (size_t(phase) < kCpPhaseCount) {
      counters_.cp_phase_calls[size_t(phase)].fetch_add(1, std::memory_order_relaxed);
    }
    ++cp_phase_depth_;
    cp_phase_start_ns_ = now_ns;
  }

  void CpPhaseExit(uint64_t now_ns) {
    if (cp_phase_depth_ == 0) {
      return;
    }
    ChargeTopPhase(now_ns);
    --cp_phase_depth_;
    cp_phase_start_ns_ = now_ns;
  }

  void RecordWaitRegMem(bool is_memory, uint32_t poll_addr, uint64_t wait_ns,
                        uint64_t payload_ms, uint64_t iterations,
                        uint64_t payload_ms_sum) {
    if (!enabled_.load(std::memory_order_relaxed)) {
      return;
    }
    counters_.cp_wait_reg_mem_count.fetch_add(1, std::memory_order_relaxed);
    const uint32_t key = (poll_addr & 0x7FFFFFFFu) | (is_memory ? 0x80000000u : 0u);
    std::lock_guard<std::mutex> lock(wait_targets_mutex_);
    auto target_it = wait_targets_.find(key);
    if (target_it == wait_targets_.end()) {
      if (wait_targets_.size() >= 64) {
        return;
      }
      target_it = wait_targets_.emplace(key, WaitTarget{key}).first;
    }
    WaitTarget& target = target_it->second;
    target.ns += wait_ns;
    ++target.count;
    target.iter_total += iterations;
    target.payload_ms_total += payload_ms;
    target.payload_ms_sum_total += payload_ms_sum;
    target.payload_ms_max = std::max(target.payload_ms_max, payload_ms);
    target.payload_ms_sum_max = std::max(target.payload_ms_sum_max, payload_ms_sum);
    target.duration_ns_max = std::max(target.duration_ns_max, wait_ns);
  }

  void OnGlobalLockContended(uint32_t owner_tid) {
#if REX_PLATFORM_LINUX
    if (!owner_sampling_ || owner_tid == 0) {
      return;
    }
    // At most one owner sample per 2 ms.
    const uint64_t now = QueryHostNanoseconds();
    uint64_t last = last_owner_sample_ns_.load(std::memory_order_relaxed);
    if (now - last < 2'000'000 ||
        !last_owner_sample_ns_.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
      return;
    }
    syscall(SYS_tgkill, getpid(), pid_t(owner_tid), SIGPROF);
#else
    (void)owner_tid;
#endif
  }

  void RecordGlobalLockWait(uint32_t owner_tid, uint64_t wait_ns) {
    const uint32_t waiter = rex::thread::current_thread_system_id();
    std::lock_guard<std::mutex> lock(lock_waits_mutex_);
    for (LockWait& entry : lock_waits_) {
      if (entry.waiter == waiter && entry.owner == owner_tid) {
        entry.ns += wait_ns;
        ++entry.count;
        return;
      }
    }
    if (lock_waits_.size() < 256) {
      lock_waits_.push_back({waiter, owner_tid, wait_ns, 1});
    }
  }

 private:
  static constexpr uint32_t kMaxPhaseDepth = 8;

  std::string TakeLockWaits(uint64_t elapsed_ms) {
    std::vector<LockWait> waits;
    {
      std::lock_guard<std::mutex> lock(lock_waits_mutex_);
      waits.swap(lock_waits_);
    }
    if (waits.empty()) {
      return "none";
    }
    std::sort(waits.begin(), waits.end(),
              [](const LockWait& a, const LockWait& b) { return a.ns > b.ns; });
    if (waits.size() > 6) {
      waits.resize(6);
    }
    std::string result;
    for (const LockWait& w : waits) {
      if (!result.empty()) {
        result += '|';
      }
      result += fmt::format("{}<-{}({}):{:.3f}:{}", ThreadName(w.waiter), ThreadName(w.owner), w.owner,
                            double(w.ns) / 1'000'000.0 * 1000.0 / double(elapsed_ms), w.count);
    }
    return result;
  }

  void ChargeTopPhase(uint64_t now_ns) {
    if (cp_phase_depth_ == 0 || cp_phase_depth_ > kMaxPhaseDepth || now_ns <= cp_phase_start_ns_) {
      return;
    }
    const size_t index = size_t(cp_phase_stack_[cp_phase_depth_ - 1]);
    if (index < kCpPhaseCount) {
      counters_.cp_phase_ns[index].fetch_add(now_ns - cp_phase_start_ns_,
                                             std::memory_order_relaxed);
    }
  }

  std::vector<WaitTarget> TakeWaitTargets() {
    std::vector<WaitTarget> targets;
    {
      std::lock_guard<std::mutex> lock(wait_targets_mutex_);
      targets.reserve(wait_targets_.size());
      for (const auto& [key, target] : wait_targets_) {
        (void)key;
        targets.push_back(target);
      }
      wait_targets_.clear();
    }
    std::sort(targets.begin(), targets.end(),
              [](const WaitTarget& a, const WaitTarget& b) { return a.ns > b.ns; });
    return targets;
  }

  std::string FormatWaitTargets(const std::vector<WaitTarget>& targets,
                                uint64_t elapsed_ms) const {
    if (targets.empty()) {
      return "none";
    }
    std::string result;
    const size_t target_count = std::min<size_t>(targets.size(), 4);
    for (size_t i = 0; i < target_count; ++i) {
      const WaitTarget& target = targets[i];
      if (!result.empty()) {
        result += '|';
      }
      result += fmt::format("{}{:X}:{:.3f}:{}", (target.key & 0x80000000u) ? "mem" : "reg",
                            target.key & 0x7FFFFFFFu,
                            double(target.ns) / 1'000'000.0 * 1000.0 / double(elapsed_ms),
                            target.count);
    }
    return result;
  }

  std::string FormatWaitStats(const std::vector<WaitTarget>& targets) const {
    if (targets.empty()) {
      return "addr=none waits=0 iter_total=0 payload_ms_mean=0 payload_ms_max=0 "
             "payload_ms_sum_mean=0 payload_ms_sum_max=0 dur_ms_mean=0 dur_ms_max=0";
    }

    std::string result;
    for (const WaitTarget& target : targets) {
      if (!result.empty()) {
        result += '|';
      }
      const bool is_memory = (target.key & 0x80000000u) != 0;
      const uint32_t address = target.key & 0x7FFFFFFFu;
      const std::string address_text =
          is_memory ? fmt::format("{:08X}", address) : fmt::format("reg{:X}", address);
      const double wait_count = static_cast<double>(target.count);
      result += fmt::format(
          "addr={} waits={} iter_total={} payload_ms_mean={:.3f} payload_ms_max={} "
          "payload_ms_sum_mean={:.3f} payload_ms_sum_max={} dur_ms_mean={:.3f} "
          "dur_ms_max={:.3f}",
          address_text, target.count, target.iter_total,
          wait_count != 0.0 ? static_cast<double>(target.payload_ms_total) / wait_count : 0.0,
          target.payload_ms_max,
          wait_count != 0.0 ? static_cast<double>(target.payload_ms_sum_total) / wait_count : 0.0,
          target.payload_ms_sum_max,
          wait_count != 0.0 ? static_cast<double>(target.ns) / 1'000'000.0 / wait_count : 0.0,
          static_cast<double>(target.duration_ns_max) / 1'000'000.0);
    }
    return result;
  }

  Snapshot ReadCounters() const {
    Snapshot snapshot = ReadScalarCounters();
    for (size_t i = 0; i < kCpPhaseCount; ++i) {
      snapshot.cp_phase_ns[i] = counters_.cp_phase_ns[i].load(std::memory_order_relaxed);
      snapshot.cp_phase_calls[i] = counters_.cp_phase_calls[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 2; ++i) {
      snapshot.descriptor_set_writes[i] =
          counters_.descriptor_set_writes[i].load(std::memory_order_relaxed);
      snapshot.descriptor_set_reuses[i] =
          counters_.descriptor_set_reuses[i].load(std::memory_order_relaxed);
    }
    return snapshot;
  }

  Snapshot ReadScalarCounters() const {
    return {
        counters_.vblank.load(std::memory_order_relaxed),
        counters_.irq0.load(std::memory_order_relaxed),
        counters_.irq1.load(std::memory_order_relaxed),
        counters_.vdswap.load(std::memory_order_relaxed),
        counters_.present.load(std::memory_order_relaxed),
        counters_.gpu_busy_ns.load(std::memory_order_relaxed),
        counters_.gpu_wait_ns.load(std::memory_order_relaxed),
        counters_.gpu_wait_count.load(std::memory_order_relaxed),
        counters_.swap_wait_ns.load(std::memory_order_relaxed),
        counters_.submission_worker_busy_ns.load(std::memory_order_relaxed),
        counters_.submission_worker_jobs.load(std::memory_order_relaxed),
        counters_.vdswap_tid.load(std::memory_order_relaxed),
        {},
        counters_.cp_wait_reg_mem_count.load(std::memory_order_relaxed),
        {},
        {},
        {},
    };
  }

  static double PerSecond(uint64_t delta, uint64_t elapsed_ms) {
    if (!elapsed_ms) {
      return 0.0;
    }
    return static_cast<double>(delta) * 1000.0 / static_cast<double>(elapsed_ms);
  }

  void Run() {
    rex::thread::set_current_thread_name("Rate Census");

    uint64_t last_host_ms = rex::chrono::Clock::QueryHostUptimeMillis();
    uint64_t last_guest_ticks = rex::chrono::Clock::QueryGuestTickCount();
    uint64_t last_guest_ms = rex::chrono::Clock::QueryGuestUptimeMillis();
    Snapshot last = ReadCounters();
    ThreadTickMap previous_thread_ticks = ReadThreadTicks();
    auto next_sample = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    while (running_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_until(next_sample);
      next_sample += std::chrono::seconds(1);
      if (!running_.load(std::memory_order_acquire)) {
        break;
      }

      const uint64_t host_ms = rex::chrono::Clock::QueryHostUptimeMillis();
      const uint64_t guest_ticks = rex::chrono::Clock::QueryGuestTickCount();
      const uint64_t guest_ms = rex::chrono::Clock::QueryGuestUptimeMillis();
      const Snapshot current = ReadCounters();
      const uint64_t elapsed_ms = host_ms > last_host_ms ? host_ms - last_host_ms : 0;
      if (!elapsed_ms) {
        continue;
      }

      const ThreadTickMap current_thread_ticks = ReadThreadTicks();
      const ThreadCpuReport thread_cpu = CalculateThreadCpu(
          previous_thread_ticks, current_thread_ticks, elapsed_ms, current.vdswap_tid);
      Sample sample;
      sample.t_ms = host_ms;
      sample.vblank = PerSecond(current.vblank - last.vblank, elapsed_ms);
      sample.irq0 = PerSecond(current.irq0 - last.irq0, elapsed_ms);
      sample.irq1 = PerSecond(current.irq1 - last.irq1, elapsed_ms);
      sample.vdswap = PerSecond(current.vdswap - last.vdswap, elapsed_ms);
      sample.present = PerSecond(current.present - last.present, elapsed_ms);
      sample.guest_ticks_per_s = PerSecond(guest_ticks - last_guest_ticks, elapsed_ms);
      sample.guest_ms_per_s = PerSecond(guest_ms - last_guest_ms, elapsed_ms);
      sample.vblank_interval_ticks = vblank_interval_ticks_;
      sample.refresh_hz = refresh_rate_hz_;
      sample.gpu_busy_ms =
          static_cast<double>(current.gpu_busy_ns - last.gpu_busy_ns) / 1'000'000.0 *
          1000.0 / static_cast<double>(elapsed_ms);
      sample.gpu_wait_ms =
          static_cast<double>(current.gpu_wait_ns - last.gpu_wait_ns) / 1'000'000.0 *
          1000.0 / static_cast<double>(elapsed_ms);
      sample.gpu_wait_count = current.gpu_wait_count - last.gpu_wait_count;
      sample.swap_wait_ms =
          static_cast<double>(current.swap_wait_ns - last.swap_wait_ns) / 1'000'000.0 *
          1000.0 / static_cast<double>(elapsed_ms);
      sample.submission_worker_busy_ms =
          static_cast<double>(current.submission_worker_busy_ns -
                              last.submission_worker_busy_ns) /
          1'000'000.0 * 1000.0 / static_cast<double>(elapsed_ms);
      sample.submission_worker_jobs_per_s =
          PerSecond(current.submission_worker_jobs - last.submission_worker_jobs, elapsed_ms);
      sample.vdswap_tid = current.vdswap_tid;
      sample.vdswap_thread_cpu_pct = thread_cpu.vdswap.percent;
      for (size_t i = 0; i < kCpPhaseCount; ++i) {
        sample.cp_phase_ms[i] = static_cast<double>(current.cp_phase_ns[i] - last.cp_phase_ns[i]) /
                                1'000'000.0 * 1000.0 / static_cast<double>(elapsed_ms);
      }
      sample.cp_wait_reg_mem_count = current.cp_wait_reg_mem_count - last.cp_wait_reg_mem_count;
      const std::vector<WaitTarget> wait_records = TakeWaitTargets();
      const std::string wait_targets = FormatWaitTargets(wait_records, elapsed_ms);
      const std::string lock_waits = TakeLockWaits(elapsed_ms);
      uint64_t intervals[4];
      for (size_t i = 0; i < 4; ++i) {
        const uint64_t now_count = counters_.present_interval[i].load(std::memory_order_relaxed);
        intervals[i] = now_count - last_present_interval_[i];
        last_present_interval_[i] = now_count;
      }
      uint64_t phase_calls[kCpPhaseCount];
      for (size_t i = 0; i < kCpPhaseCount; ++i) {
        phase_calls[i] = current.cp_phase_calls[i] - last.cp_phase_calls[i];
      }
      uint64_t descriptor_set_writes[2];
      uint64_t descriptor_set_reuses[2];
      for (size_t i = 0; i < 2; ++i) {
        descriptor_set_writes[i] = current.descriptor_set_writes[i] - last.descriptor_set_writes[i];
        descriptor_set_reuses[i] = current.descriptor_set_reuses[i] - last.descriptor_set_reuses[i];
      }
      {
        std::lock_guard<std::mutex> lock(sample_mutex_);
        latest_sample_ = sample;
      }

      const uint64_t steady_ns = QueryHostNanoseconds();
      REXLOG_INFO(
          "[rate-census] steady_ns={} t_ms={} vblank={:.3f} irq0={:.3f} irq1={:.3f} "
          "vdswap={:.3f} present={:.3f} guest_ticks_per_s={:.3f} "
          "guest_ms_per_s={:.3f} vblank_interval_ticks={} refresh_hz={:.3f} "
          "gpu_busy_ms={:.3f} gpu_wait_ms={:.3f} gpu_wait_count={} "
          "swap_wait_ms={:.3f} vdswap_tid={} vdswap_thread_cpu_pct={:.3f}",
          steady_ns, sample.t_ms, sample.vblank, sample.irq0, sample.irq1, sample.vdswap,
          sample.present, sample.guest_ticks_per_s, sample.guest_ms_per_s,
          sample.vblank_interval_ticks, sample.refresh_hz, sample.gpu_busy_ms,
          sample.gpu_wait_ms, sample.gpu_wait_count, sample.swap_wait_ms,
          sample.vdswap_tid, sample.vdswap_thread_cpu_pct);
      REXLOG_INFO("[rate-census-submit] t_ms={} worker_busy_ms={:.3f} worker_jobs_per_s={:.3f}",
                  sample.t_ms, sample.submission_worker_busy_ms,
                  sample.submission_worker_jobs_per_s);
      REXLOG_INFO(
          "[rate-census-cpu] t_ms={} vdswap_tid={} vdswap_thread={} "
          "vdswap_thread_cpu_pct={:.3f} top={}",
          sample.t_ms, sample.vdswap_tid,
          thread_cpu.vdswap.name.empty() ? "unknown" : thread_cpu.vdswap.name,
          sample.vdswap_thread_cpu_pct, FormatThreadCpu(thread_cpu.top));
      REXLOG_INFO(
          "[rate-census-cp] t_ms={} wait_reg_mem_ms={:.3f} draw_ms={:.3f} copy_ms={:.3f} "
          "swap_ms={:.3f} submit_ms={:.3f} submit_drain_ms={:.3f} wait_reg_mem_count={} "
          "draws={} copies={} swaps={} submits={} submit_drains={} wait_targets={}",
          sample.t_ms, sample.cp_phase_ms[size_t(CpPhase::kWaitRegMem)],
          sample.cp_phase_ms[size_t(CpPhase::kDraw)], sample.cp_phase_ms[size_t(CpPhase::kCopy)],
          sample.cp_phase_ms[size_t(CpPhase::kSwap)], sample.cp_phase_ms[size_t(CpPhase::kSubmit)],
          sample.cp_phase_ms[size_t(CpPhase::kSubmitDrain)], sample.cp_wait_reg_mem_count,
          phase_calls[size_t(CpPhase::kDraw)], phase_calls[size_t(CpPhase::kCopy)],
          phase_calls[size_t(CpPhase::kSwap)], phase_calls[size_t(CpPhase::kSubmit)],
          phase_calls[size_t(CpPhase::kSubmitDrain)], wait_targets);
      REXLOG_INFO("[rate-census-wait] t_ms={} {}", sample.t_ms,
                  FormatWaitStats(wait_records));
      REXLOG_INFO("[rate-census-lock] t_ms={} waits={}", sample.t_ms, lock_waits);
      REXLOG_INFO("[rate-census-swap] t_ms={} present_interval0={} interval1={} interval2={} other={}",
                  sample.t_ms, intervals[0], intervals[1], intervals[2], intervals[3]);
      REXLOG_INFO(
          "[rate-census-desc] t_ms={} vertex_written={} vertex_reused={} pixel_written={} "
          "pixel_reused={}",
          sample.t_ms, descriptor_set_writes[0], descriptor_set_reuses[0],
          descriptor_set_writes[1], descriptor_set_reuses[1]);

      last_host_ms = host_ms;
      last_guest_ticks = guest_ticks;
      last_guest_ms = guest_ms;
      last = current;
      previous_thread_ticks = current_thread_ticks;
    }
  }

  Counters counters_;
  std::atomic<bool> enabled_{false};
  std::atomic<bool> running_{false};
  std::mutex lifecycle_mutex_;
  mutable std::mutex sample_mutex_;
  bool started_ = false;
  std::thread logger_thread_;
  std::optional<Sample> latest_sample_;
  uint64_t vblank_interval_ticks_ = 0;
  double refresh_rate_hz_ = 0.0;
  bool gpu_busy_active_ = false;
  uint64_t gpu_busy_start_ns_ = 0;
  bool gpu_busy_paused_ = false;
  bool gpu_wait_active_ = false;
  uint64_t gpu_wait_start_ns_ = 0;
  bool submission_worker_busy_active_ = false;
  uint64_t submission_worker_busy_start_ns_ = 0;
  // Phase stack, touched only by the GPU command thread.
  CpPhase cp_phase_stack_[kMaxPhaseDepth] = {};
  uint32_t cp_phase_depth_ = 0;
  uint64_t cp_phase_start_ns_ = 0;
  uint64_t last_present_interval_[4] = {};
  std::mutex lock_waits_mutex_;
  std::vector<LockWait> lock_waits_;
  bool owner_sampling_ = false;
  std::atomic<uint64_t> last_owner_sample_ns_{0};
  std::mutex wait_targets_mutex_;
  std::unordered_map<uint32_t, WaitTarget> wait_targets_;
};

RateCensus g_rate_census;

}  // namespace

void Start(uint64_t vblank_interval_ticks, double refresh_rate_hz) {
#if defined(__x86_64__) || defined(_M_X64)
  GetTscClock();  // One-time 20 ms calibration here rather than on the first draw.
#endif
  g_rate_census.Start(vblank_interval_ticks, refresh_rate_hz);
}

void Stop() { g_rate_census.Stop(); }

std::optional<Sample> GetLatestSample() { return g_rate_census.GetLatestSample(); }

void RecordVblank() { g_rate_census.RecordVblank(); }

void RecordInterrupt(uint32_t source) { g_rate_census.RecordInterrupt(source); }

void RecordVdSwap() {
  freeze_logger::RecordVdSwap();
  g_rate_census.RecordVdSwap();
}

void RecordPresentInterval(uint32_t interval) { g_rate_census.RecordPresentInterval(interval); }

void RecordPresent() {
  freeze_logger::RecordPresent();
  g_rate_census.RecordPresent();
}

void RecordGpuBusyStart() { g_rate_census.RecordGpuBusyStart(); }

void RecordGpuBusyEnd() { g_rate_census.RecordGpuBusyEnd(); }

void RecordGpuWaitStart() { g_rate_census.RecordGpuWaitStart(); }

void RecordGpuWaitEnd() { g_rate_census.RecordGpuWaitEnd(); }

void RecordSubmissionWorkerBusyStart() { g_rate_census.RecordSubmissionWorkerBusyStart(); }

void RecordSubmissionWorkerBusyEnd() { g_rate_census.RecordSubmissionWorkerBusyEnd(); }

void RecordSubmissionWorkerJob() { g_rate_census.RecordSubmissionWorkerJob(); }

void RecordDescriptorSetUpdate(bool is_vertex, bool reused) {
  g_rate_census.RecordDescriptorSetUpdate(is_vertex, reused);
}

bool IsEnabled() { return g_rate_census.IsEnabled(); }

uint64_t NowNs() { return QueryFastNanoseconds(); }

void CpPhaseEnter(CpPhase phase, uint64_t now_ns) { g_rate_census.CpPhaseEnter(phase, now_ns); }

void CpPhaseExit(uint64_t now_ns) { g_rate_census.CpPhaseExit(now_ns); }

void OnGlobalLockContended(uint32_t owner_tid) { g_rate_census.OnGlobalLockContended(owner_tid); }

void RecordGlobalLockWait(uint32_t owner_tid, uint64_t wait_ns) {
  g_rate_census.RecordGlobalLockWait(owner_tid, wait_ns);
}

void RecordWaitRegMem(bool is_memory, uint32_t poll_addr, uint64_t wait_ns,
                      uint64_t payload_ms, uint64_t iterations,
                      uint64_t payload_ms_sum) {
  g_rate_census.RecordWaitRegMem(is_memory, poll_addr, wait_ns, payload_ms,
                                 iterations, payload_ms_sum);
}

}  // namespace rex::diagnostics::rate_census
