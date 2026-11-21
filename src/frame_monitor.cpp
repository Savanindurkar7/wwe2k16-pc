// wwe2k16 - frame monitor and freeze diagnostics. See frame_monitor.h.

#include "frame_monitor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/perf/stall_counters.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#define WWE2K16_PROCFS 1
#endif

namespace wwe2k16::frame_monitor {
namespace {

constexpr uint64_t kStallFrameNs = 200'000'000;   // a frame this long gets a [wwe2k16-stall] line
constexpr uint64_t kWatchdogGapNs = 400'000'000;  // no frame for this long: sample the threads

struct Totals {
  uint64_t pipelines = 0, pipeline_ns = 0, textures = 0, texture_ns = 0, reads = 0, read_bytes = 0, read_ns = 0;
};

Totals ReadTotals() {
  auto& c = rex::perf::GetStallCounters();
  Totals t;
  t.pipelines = c.pipelines_created.load(std::memory_order_relaxed);
  t.pipeline_ns = c.pipeline_create_ns.load(std::memory_order_relaxed);
  t.textures = c.texture_loads.load(std::memory_order_relaxed);
  t.texture_ns = c.texture_load_ns.load(std::memory_order_relaxed);
  t.reads = c.file_reads.load(std::memory_order_relaxed);
  t.read_bytes = c.file_read_bytes.load(std::memory_order_relaxed);
  t.read_ns = c.file_read_ns.load(std::memory_order_relaxed);
  return t;
}

const char* Env(const char* name) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : nullptr;
}

std::atomic<uint64_t> g_last_frame_ns{0};
std::atomic<uint64_t> g_frame_count{0};

std::mutex g_snapshot_mutex;
Snapshot g_snapshot;

// --- frames.csv -----------------------------------------------------------------------------------------------------

struct CsvWriter {
  FILE* file = nullptr;
  bool tried = false;
  uint64_t start_ns = 0;
  uint64_t last_flush_ns = 0;

  void Open(uint64_t now_ns) {
    tried = true;
    const char* dir = Env("WWE2K16_LOG_DIR");
    if (!dir) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = std::filesystem::path(dir) / "frames.csv";
    file = std::fopen(path.string().c_str(), "w");
    if (!file) return;
    start_ns = now_ns;
    std::fprintf(file,
                 "t_s,frame_ms,pipelines,pipeline_ms,texture_loads,texture_ms,file_reads,file_kb,file_ms\n");
    REXLOG_INFO("[wwe2k16-frames] per-frame log: {}", path.string());
  }
};

CsvWriter g_csv;

// Per-second window for the snapshot.
struct Window {
  uint64_t start_ns = 0;
  uint32_t frames = 0;
  uint64_t worst_ns = 0;
  Totals start_totals;
};

Window g_window;
Totals g_prev_totals;
uint64_t g_prev_frame_ns = 0;
double g_frame_ms_ema = 0.0;

// --- Android performance hint (API 33) --------------------------------------------------------------------------

struct PerfHint {
#if defined(__ANDROID__)
  using GetManagerFn = void* (*)();
  using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
  using ReportFn = int (*)(void*, int64_t);
  using UpdateTargetFn = int (*)(void*, int64_t);
  ReportFn report = nullptr;
  UpdateTargetFn update_target = nullptr;
  void* session = nullptr;
  bool tried = false;
  int64_t target_ns = 16'666'667;

  void Init() {
    tried = true;
    const char* setting = Env("WWE2K16_PERF_HINT");
    if (setting && std::strcmp(setting, "0") == 0) return;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    auto get_manager = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
    auto create = reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHint_createSession"));
    report = reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    update_target = reinterpret_cast<UpdateTargetFn>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    if (!get_manager || !create || !report) {
      REXLOG_INFO("[wwe2k16-hint] Android performance hint API not available (Android 12 or older)");
      return;
    }
    void* manager = get_manager();
    const int32_t tid = int32_t(gettid());
    if (const char* cap = Env("WWE2K16_FRAME_CAP"); cap && std::atoi(cap) == 30) target_ns = 33'333'333;
    session = manager ? create(manager, &tid, 1, target_ns) : nullptr;
    REXLOG_INFO("[wwe2k16-hint] performance hint session {} (target {:.1f} ms)", session ? "started" : "not available",
                double(target_ns) / 1e6);
  }
#endif
  void OnFrame(uint64_t frame_ns) {
#if defined(__ANDROID__)
    if (!tried) Init();
    if (session && report && frame_ns > 0 && frame_ns < 1'000'000'000ull) report(session, int64_t(frame_ns));
#else
    (void)frame_ns;
#endif
  }
};

PerfHint g_perf_hint;

// --- Threads: sampling and core pinning (procfs) ----------------------------------------------------------------------

#if defined(WWE2K16_PROCFS)

struct ThreadStat {
  std::string name;
  char state = '?';
  uint64_t cpu_ticks = 0;
  std::string wchan;
};

bool ReadThreadStat(int tid, ThreadStat& out) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  std::getline(in, line);
  const size_t open = line.find('('), close = line.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close + 2 >= line.size()) return false;
  out.name = line.substr(open + 1, close - open - 1);
  std::istringstream rest(line.substr(close + 2));
  std::string field;
  std::vector<std::string> fields;
  while (rest >> field && fields.size() < 13) fields.push_back(field);
  if (fields.size() < 13) return false;
  out.state = fields[0].empty() ? '?' : fields[0][0];
  out.cpu_ticks = std::strtoull(fields[11].c_str(), nullptr, 10) + std::strtoull(fields[12].c_str(), nullptr, 10);
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/wchan", tid);
  std::ifstream w(path);
  if (w) std::getline(w, out.wchan);
  return true;
}

std::vector<int> ListThreads() {
  std::vector<int> tids;
  if (DIR* dir = opendir("/proc/self/task")) {
    while (dirent* entry = readdir(dir)) {
      const int tid = std::atoi(entry->d_name);
      if (tid > 0) tids.push_back(tid);
    }
    closedir(dir);
  }
  return tids;
}

// One sample: CPU used by each thread over `window`, busiest first.
void SampleThreads(uint64_t stall_ms, int sample_index) {
  std::map<int, ThreadStat> before;
  for (int tid : ListThreads()) {
    ThreadStat s;
    if (ReadThreadStat(tid, s)) before[tid] = s;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  struct Row {
    int tid;
    ThreadStat stat;
    uint64_t ticks;
  };
  std::vector<Row> rows;
  for (auto& [tid, b] : before) {
    ThreadStat a;
    if (!ReadThreadStat(tid, a)) continue;
    rows.push_back({tid, a, a.cpu_ticks - std::min(a.cpu_ticks, b.cpu_ticks)});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.ticks > y.ticks; });
  const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
  std::string busy;
  int shown = 0;
  for (const Row& r : rows) {
    if (shown >= 8) break;
    if (r.ticks == 0 && shown >= 3) break;
    busy += fmt::format(" | {}({}) {} {}%{}{}", r.stat.name, r.tid, r.stat.state, r.ticks * 1000 / hz / 1,
                        r.stat.wchan.empty() || r.stat.wchan == "0" ? "" : " wait:",
                        r.stat.wchan == "0" ? "" : r.stat.wchan);
    ++shown;
  }
  const Totals t = ReadTotals();
  REXLOG_INFO("[wwe2k16-stall] no frame for {} ms (sample {}): pipelines {} ({} ms), textures {} ({} ms), "
              "file reads {} ({} KB, {} ms); busiest threads (CPU ms in 100 ms):{}",
              stall_ms, sample_index, t.pipelines - g_prev_totals.pipelines,
              (t.pipeline_ns - g_prev_totals.pipeline_ns) / 1'000'000, t.textures - g_prev_totals.textures,
              (t.texture_ns - g_prev_totals.texture_ns) / 1'000'000, t.reads - g_prev_totals.reads,
              (t.read_bytes - g_prev_totals.read_bytes) / 1024, (t.read_ns - g_prev_totals.read_ns) / 1'000'000,
              busy);
}

// CPU cores by speed: little / mid / big from each core's top frequency.
struct CoreSets {
  cpu_set_t fast{};   // the fastest cluster(s): >= 85 % of the top frequency
  cpu_set_t not_little{};
  int fast_count = 0, not_little_count = 0, total = 0;
  bool valid = false;
};

CoreSets DetectCores() {
  CoreSets sets;
  CPU_ZERO(&sets.fast);
  CPU_ZERO(&sets.not_little);
  std::vector<std::pair<int, long>> cores;
  const long count = sysconf(_SC_NPROCESSORS_CONF);
  long top = 0, low = 0;
  for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE; ++cpu) {
    char path[96];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    std::ifstream in(path);
    long khz = 0;
    if (!(in >> khz)) continue;
    cores.push_back({cpu, khz});
    top = std::max(top, khz);
    low = low ? std::min(low, khz) : khz;
  }
  if (cores.empty() || !top) return sets;
  for (auto& [cpu, khz] : cores) {
    ++sets.total;
    if (khz * 100 >= top * 85) {
      CPU_SET(cpu, &sets.fast);
      ++sets.fast_count;
    }
    if (khz > low || low == top) {
      CPU_SET(cpu, &sets.not_little);
      ++sets.not_little_count;
    }
  }
  sets.valid = true;
  return sets;
}

struct Pinning {
  enum class Mode { kAll, kAuto, kBig } mode = Mode::kAll;
  CoreSets cores;
  const cpu_set_t* target = nullptr;
  std::set<int> pinned;
  bool logged = false;

  void Init() {
    const char* setting = Env("WWE2K16_CPU_CORES");
    if (!setting || std::strcmp(setting, "all") == 0) return;
    mode = std::strcmp(setting, "big") == 0 ? Mode::kBig : Mode::kAuto;
    cores = DetectCores();
    if (!cores.valid) return;
    if (mode == Mode::kBig && cores.fast_count >= 2) target = &cores.fast;
    else if (cores.not_little_count >= 2 && cores.not_little_count < cores.total) target = &cores.not_little;
    else if (cores.fast_count >= 2) target = &cores.fast;
  }

  // New threads appear while the game loads: everything not pinned yet is pinned.
  void Apply() {
    if (!target) return;
    int newly = 0;
    for (int tid : ListThreads()) {
      if (pinned.count(tid)) continue;
      if (sched_setaffinity(tid, sizeof(cpu_set_t), target) == 0) ++newly;
      pinned.insert(tid);
    }
    if (newly && !logged) {
      logged = true;
      REXLOG_INFO("[wwe2k16-cores] game threads kept on {} of {} cores ({})", CPU_COUNT(target), cores.total,
                  mode == Mode::kBig ? "fastest cores" : "little cores left out");
    }
  }
};

#endif  // WWE2K16_PROCFS

// --- Watchdog thread ------------------------------------------------------------------------------------------------

std::mutex g_watchdog_mutex;
std::condition_variable g_watchdog_cv;
std::thread g_watchdog;
bool g_watchdog_stop = false;

void WatchdogMain() {
#if defined(WWE2K16_PROCFS)
  Pinning pinning;
  pinning.Init();
  int stall_samples = 0;
  uint64_t last_pin_ns = 0;
#endif
  std::unique_lock<std::mutex> lock(g_watchdog_mutex);
  while (!g_watchdog_stop) {
    g_watchdog_cv.wait_for(lock, std::chrono::milliseconds(200));
    if (g_watchdog_stop) break;
    lock.unlock();
#if defined(WWE2K16_PROCFS)
    const uint64_t now = rex::perf::StallNowNs();
    if (now - last_pin_ns > 2'000'000'000ull) {
      pinning.Apply();
      last_pin_ns = now;
    }
    const uint64_t last = g_last_frame_ns.load(std::memory_order_relaxed);
    // Only once the game is presenting (the boot is one long gap anyway) and at most 20 samples per stall.
    if (last && g_frame_count.load(std::memory_order_relaxed) > 60 && now - last > kWatchdogGapNs) {
      if (stall_samples < 20) SampleThreads((now - last) / 1'000'000, ++stall_samples);
    } else {
      stall_samples = 0;
    }
#endif
    lock.lock();
  }
}

}  // namespace

int OverlayMode() {
  static const int mode = [] {
    const char* v = Env("WWE2K16_FRAME_MONITOR");
    if (!v) {
      v = Env("WWE2K16_FPS_COUNTER");  // the PC launcher's switch
      return (v && v[0] != '0') ? 1 : 0;
    }
    return std::clamp(std::atoi(v), 0, 2);
  }();
  return mode;
}

void OnGameFrame(uint64_t now_ns) {
  g_last_frame_ns.store(now_ns, std::memory_order_relaxed);
  const uint64_t frame_index = g_frame_count.fetch_add(1, std::memory_order_relaxed);
  const Totals totals = ReadTotals();
  if (!g_prev_frame_ns) {
    g_prev_frame_ns = now_ns;
    g_prev_totals = totals;
    g_window.start_ns = now_ns;
    g_window.start_totals = totals;
    return;
  }
  const uint64_t frame_ns = now_ns - g_prev_frame_ns;
  g_prev_frame_ns = now_ns;
  g_perf_hint.OnFrame(frame_ns);

  if (!g_csv.tried) g_csv.Open(now_ns);
  if (g_csv.file) {
    std::fprintf(g_csv.file, "%.3f,%.2f,%llu,%.1f,%llu,%.1f,%llu,%llu,%.1f\n", double(now_ns - g_csv.start_ns) * 1e-9,
                 double(frame_ns) * 1e-6, (unsigned long long)(totals.pipelines - g_prev_totals.pipelines),
                 double(totals.pipeline_ns - g_prev_totals.pipeline_ns) * 1e-6,
                 (unsigned long long)(totals.textures - g_prev_totals.textures),
                 double(totals.texture_ns - g_prev_totals.texture_ns) * 1e-6,
                 (unsigned long long)(totals.reads - g_prev_totals.reads),
                 (unsigned long long)((totals.read_bytes - g_prev_totals.read_bytes) / 1024),
                 double(totals.read_ns - g_prev_totals.read_ns) * 1e-6);
    if (now_ns - g_csv.last_flush_ns > 1'000'000'000ull) {
      std::fflush(g_csv.file);
      g_csv.last_flush_ns = now_ns;
    }
  }
  if (frame_ns > kStallFrameNs && frame_index > 60) {
    REXLOG_INFO("[wwe2k16-stall] frame took {} ms: pipelines built {} ({} ms), textures loaded {} ({} ms), "
                "file reads {} ({} KB, {} ms)",
                frame_ns / 1'000'000, totals.pipelines - g_prev_totals.pipelines,
                (totals.pipeline_ns - g_prev_totals.pipeline_ns) / 1'000'000, totals.textures - g_prev_totals.textures,
                (totals.texture_ns - g_prev_totals.texture_ns) / 1'000'000, totals.reads - g_prev_totals.reads,
                (totals.read_bytes - g_prev_totals.read_bytes) / 1024,
                (totals.read_ns - g_prev_totals.read_ns) / 1'000'000);
  }
  g_prev_totals = totals;

  // Snapshot for the on-screen monitor, once per second.
  const double ms = double(frame_ns) * 1e-6;
  g_frame_ms_ema = g_frame_ms_ema > 0.0 ? g_frame_ms_ema + 0.1 * (ms - g_frame_ms_ema) : ms;
  ++g_window.frames;
  g_window.worst_ns = std::max(g_window.worst_ns, frame_ns);
  if (now_ns - g_window.start_ns >= 1'000'000'000ull) {
    const double seconds = double(now_ns - g_window.start_ns) * 1e-9;
    Snapshot s;
    s.fps = float(g_window.frames / seconds);
    s.frame_ms = float(g_frame_ms_ema);
    s.worst_ms = float(double(g_window.worst_ns) * 1e-6);
    s.pipelines_per_s = uint32_t(totals.pipelines - g_window.start_totals.pipelines);
    s.texture_loads_per_s = uint32_t(totals.textures - g_window.start_totals.textures);
    s.file_kb_per_s = uint32_t((totals.read_bytes - g_window.start_totals.read_bytes) / 1024);
    {
      std::lock_guard<std::mutex> lock(g_snapshot_mutex);
      g_snapshot = s;
    }
    g_window = Window{};
    g_window.start_ns = now_ns;
    g_window.start_totals = totals;
  }
}

Snapshot GetSnapshot() {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  return g_snapshot;
}

void Start() {
  std::lock_guard<std::mutex> lock(g_watchdog_mutex);
  if (g_watchdog.joinable()) return;
  g_watchdog_stop = false;
  g_watchdog = std::thread(WatchdogMain);
}

void Stop() {
  {
    std::lock_guard<std::mutex> lock(g_watchdog_mutex);
    if (!g_watchdog.joinable()) return;
    g_watchdog_stop = true;
  }
  g_watchdog_cv.notify_all();
  g_watchdog.join();
  if (g_csv.file) {
    std::fclose(g_csv.file);
    g_csv.file = nullptr;
  }
}

}  // namespace wwe2k16::frame_monitor
