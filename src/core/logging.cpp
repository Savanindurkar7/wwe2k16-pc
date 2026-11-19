/**
 * @file        core/logging.cpp
 * @brief       Logging infrastructure implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_STRING(log_level, "info", "Log",
                      "Global log level: trace, debug, info, warn, error, critical, off")
    .allowed({"trace", "debug", "info", "warn", "error", "critical", "off"});

REXCVAR_DEFINE_STRING(log_file, "", "Log", "Log file path (empty = no file logging)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(log_verbose, false, "Log", "Enable verbose logging (sets level to trace)")
    .debug_only();

namespace rex {

namespace {

#if REX_PLATFORM_WIN32
// spdlog's basic_file_sink opens the log file with narrow fopen, which
// interprets the name in the active ANSI code page. A log path that lives
// under a directory whose name is outside that code page (Cyrillic, CJK, ...)
// therefore cannot be opened. This sink is the same file sink backed by a
// FILE* opened through rex::filesystem::OpenFile (_wfopen), so a UTF-8 path
// converted with rex::to_path works.
class Utf8PathFileSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  Utf8PathFileSink(const std::string& utf8_path, bool truncate) : utf8_path_(utf8_path) {
    path_ = rex::to_path(utf8_path);
    std::error_code ec;
    if (!path_.parent_path().empty()) {
      std::filesystem::create_directories(path_.parent_path(), ec);
    }
    file_ = rex::filesystem::OpenFile(path_, truncate ? "wb" : "ab");
    if (!file_) {
      throw spdlog::spdlog_ex("Failed opening log file " + utf8_path + " for writing", errno);
    }
  }

  ~Utf8PathFileSink() override {
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if (std::fwrite(formatted.data(), 1, formatted.size(), file_) != formatted.size()) {
      throw spdlog::spdlog_ex("Failed writing to log file " + utf8_path_, errno);
    }
  }

  void flush_() override {
    if (file_) {
      std::fflush(file_);
    }
  }

 private:
  std::string utf8_path_;
  std::filesystem::path path_;
  FILE* file_ = nullptr;
};
#endif  // REX_PLATFORM_WIN32

// Shared file-sink factory: wide-safe on Windows, plain UTF-8 bytes elsewhere.
spdlog::sink_ptr MakeFileSink(const std::string& utf8_path, bool truncate) {
#if REX_PLATFORM_WIN32
  return std::make_shared<Utf8PathFileSink>(utf8_path, truncate);
#else
  return std::make_shared<spdlog::sinks::basic_file_sink_mt>(utf8_path, truncate);
#endif
}

// Category registry — indices 0..kBuiltinCount-1 are SDK built-ins.
std::vector<LogCategoryEntry> g_registry;

// Shared default sinks (stored for pattern changes)
spdlog::sink_ptr g_console_sink;
spdlog::sink_ptr g_file_sink;
std::string g_file_path;

// Extra global sinks added via AddSink()
std::vector<spdlog::sink_ptr> g_extra_sinks;

// Initialization state
bool g_initialized = false;
std::mutex g_mutex;

// Stored config (for resolving category_levels/category_sinks on late registration)
LogConfig g_config;

// Collect the default sinks into a vector for logger construction
std::vector<spdlog::sink_ptr> BuildDefaultSinks() {
  std::vector<spdlog::sink_ptr> sinks;
  if (g_console_sink)
    sinks.push_back(g_console_sink);
  if (g_file_sink)
    sinks.push_back(g_file_sink);
  for (auto& s : g_extra_sinks)
    sinks.push_back(s);
  return sinks;
}

// Build sinks for a specific category (handles per-category sinks from config)
std::vector<spdlog::sink_ptr> BuildCategorySinks(const std::string& name) {
  auto it = g_config.category_sinks.find(name);
  if (it != g_config.category_sinks.end()) {
    if (g_config.category_sinks_exclusive) {
      // Replace default sinks entirely
      return it->second;
    }
    // Additive: default sinks + category-specific sinks
    auto sinks = BuildDefaultSinks();
    for (auto& s : it->second)
      sinks.push_back(s);
    return sinks;
  }
  return BuildDefaultSinks();
}

// Resolve per-category level from config, or return default
spdlog::level::level_enum ResolveCategoryLevel(const std::string& name) {
  auto it = g_config.category_levels.find(name);
  if (it != g_config.category_levels.end())
    return it->second;
  return g_config.default_level;
}

// Create a logger and register it
std::shared_ptr<spdlog::logger> CreateCategoryLogger(const std::string& name) {
  auto sinks = BuildCategorySinks(name);
  auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
  logger->set_level(ResolveCategoryLevel(name));
  logger->flush_on(g_config.flush_level);
  spdlog::register_logger(logger);
  return logger;
}

}  // namespace

// ---- Built-in category short names (must match log:: constant IDs) ----
static constexpr const char* kBuiltinNames[] = {"core", "cpu", "apu", "gpu", "krnl", "sys", "fs"};

void InitLogging(const LogConfig& config) {
  std::lock_guard lock(g_mutex);

  if (g_initialized) {
    // Re-initialization: add any newly requested default sinks, then update config and levels.
    g_config = config;

    std::vector<spdlog::sink_ptr> new_sinks;
    spdlog::sink_ptr replaced_file_sink;
    if (config.log_to_console && !g_console_sink) {
      auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      sink->set_level(spdlog::level::trace);
      sink->set_pattern(config.console_pattern);
      g_console_sink = sink;
      new_sinks.push_back(std::move(sink));
    }

    if (config.log_file && (!g_file_sink || g_file_path != config.log_file)) {
      auto sink = MakeFileSink(config.log_file, true);
      sink->set_level(spdlog::level::trace);
      sink->set_pattern(config.file_pattern);
      replaced_file_sink = std::move(g_file_sink);
      g_file_sink = sink;
      g_file_path = config.log_file;
      new_sinks.push_back(std::move(sink));
    }

    for (auto& entry : g_registry) {
      if (entry.logger) {
        auto& sinks = entry.logger->sinks();
        if (replaced_file_sink) {
          std::erase(sinks, replaced_file_sink);
        }
        for (const auto& sink : new_sinks) {
          if (std::find(sinks.begin(), sinks.end(), sink) == sinks.end()) {
            sinks.push_back(sink);
          }
        }
        entry.logger->set_level(ResolveCategoryLevel(entry.name));
        entry.logger->flush_on(config.flush_level);
      }
    }
    return;
  }

  g_config = config;

  // Create console sink
  if (config.log_to_console) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.console_pattern);
    g_console_sink = sink;
  }

  // Create file sink
  if (config.log_file) {
    auto sink = MakeFileSink(config.log_file, true);
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.file_pattern);
    g_file_sink = sink;
    g_file_path = config.log_file;
  }

  // Populate extra global sinks from config
  g_extra_sinks = config.extra_sinks;

  // Ensure registry has room for built-in categories (don't truncate
  // any consumer categories that were registered during static init).
  if (g_registry.size() < log::kBuiltinCount) {
    g_registry.resize(log::kBuiltinCount);
  }
  for (uint16_t i = 0; i < log::kBuiltinCount; ++i) {
    g_registry[i].name = kBuiltinNames[i];
    g_registry[i].logger = CreateCategoryLogger(kBuiltinNames[i]);
  }

  // Set core as default logger for spdlog
  spdlog::set_default_logger(g_registry[0].logger);

  g_initialized = true;
  // Warnings and below reach the file within a second (errors flush immediately).
  spdlog::flush_every(std::chrono::seconds(1));

  // Create loggers for any categories registered during static init
  // (they have a name but no logger since sinks didn't exist yet).
  for (size_t i = log::kBuiltinCount; i < g_registry.size(); ++i) {
    if (!g_registry[i].name.empty() && !g_registry[i].logger) {
      g_registry[i].logger = CreateCategoryLogger(g_registry[i].name);
    }
  }
}

void InitLogging(const char* log_file, spdlog::level::level_enum level) {
  LogConfig config;
  config.log_file = log_file;
  config.default_level = level;
  InitLogging(config);
}

void ShutdownLogging() {
  std::lock_guard lock(g_mutex);
  if (!g_initialized)
    return;

  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->flush();
  }

  spdlog::shutdown();

  g_registry.clear();
  g_console_sink.reset();
  g_file_sink.reset();
  g_file_path.clear();
  g_extra_sinks.clear();
  g_initialized = false;
}

void FlushLogging() {
  std::lock_guard lock(g_mutex);
  if (!g_initialized) {
    return;
  }
  for (auto& entry : g_registry) {
    if (entry.logger) {
      entry.logger->flush();
    }
  }
}

std::string GetLogFilePath() {
  std::lock_guard lock(g_mutex);
  return g_file_path;
}

LogCategoryId RegisterLogCategory(const char* name) {
  std::lock_guard lock(g_mutex);

  // Reserve indices 0..kBuiltinCount-1 for SDK built-in categories so
  // consumer categories never collide with built-in IDs even when
  // registered during static initialization (before InitLogging).
  if (g_registry.size() < log::kBuiltinCount) {
    g_registry.resize(log::kBuiltinCount);
  }

  // Check for duplicates
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }

  uint16_t id = static_cast<uint16_t>(g_registry.size());
  LogCategoryEntry entry;
  entry.name = name;
  if (g_initialized) {
    entry.logger = CreateCategoryLogger(name);
  }
  g_registry.push_back(std::move(entry));
  return LogCategoryId{id};
}

std::optional<LogCategoryId> FindCategory(const std::string& name) {
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }
  return std::nullopt;
}

std::span<const LogCategoryEntry> GetAllCategories() {
  // No lock: only safe to call from main thread or after init.
  return {g_registry.data(), g_registry.size()};
}

spdlog::logger* GetLoggerRaw(LogCategoryId category) {
  if (!g_initialized)
    InitLogging();
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger.get();
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger(LogCategoryId category) {
  if (!g_initialized)
    InitLogging();
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger;
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return GetLogger(log::Core);
}

void SetCategoryLevel(LogCategoryId category, spdlog::level::level_enum level) {
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->set_level(level);
    }
  }
}

void SetAllLevels(spdlog::level::level_enum level) {
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->set_level(level);
  }
}

void RegisterLogLevelCallback() {
  rex::cvar::RegisterChangeCallback("log_level", [](std::string_view, std::string_view value) {
    if (auto level = ParseLogLevel(std::string(value))) {
      SetAllLevels(*level);
      REXLOG_DEBUG("Log level changed to {}", value);
    }
  });
}

void AddSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  g_extra_sinks.push_back(sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->sinks().push_back(sink);
  }
}

void AddSink(LogCategoryId category, spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->sinks().push_back(sink);
    }
  }
}

void RemoveSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  std::erase(g_extra_sinks, sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      std::erase(entry.logger->sinks(), sink);
  }
}

void RemoveSink(LogCategoryId category, spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      std::erase(logger->sinks(), sink);
    }
  }
}

void SetConsolePattern(const std::string& pattern) {
  if (g_console_sink)
    g_console_sink->set_pattern(pattern);
}

void SetFilePattern(const std::string& pattern) {
  if (g_file_sink)
    g_file_sink->set_pattern(pattern);
}

// ==========================================================================
// CLI Helpers
// ==========================================================================

std::optional<spdlog::level::level_enum> ParseLogLevel(const std::string& level_str) {
  static const std::unordered_map<std::string, spdlog::level::level_enum> level_map = {
      {"trace", spdlog::level::trace},  {"debug", spdlog::level::debug},
      {"info", spdlog::level::info},    {"warn", spdlog::level::warn},
      {"warning", spdlog::level::warn}, {"error", spdlog::level::err},
      {"err", spdlog::level::err},      {"critical", spdlog::level::critical},
      {"off", spdlog::level::off},
  };

  std::string lower = level_str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  auto it = level_map.find(lower);
  if (it != level_map.end())
    return it->second;
  return std::nullopt;
}

spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level) {
  return ParseLogLevel(level_str).value_or(default_level);
}

LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels) {
  LogConfig config;
  config.log_file = log_file;

  // Build-type default
  config.default_level = kDefaultLogLevel;

  // Environment variable
  if (const char* env_level = std::getenv("REX_LOG_LEVEL")) {
    if (auto level = ParseLogLevel(env_level))
      config.default_level = *level;
  } else if (const char* env_level2 = std::getenv("SPDLOG_LEVEL")) {
    if (auto level = ParseLogLevel(env_level2))
      config.default_level = *level;
  }

  // CLI global level overrides environment
  if (!cli_level.empty()) {
    if (auto level = ParseLogLevel(cli_level))
      config.default_level = *level;
  }

  // Per-category CLI levels (string-keyed)
  for (const auto& [cat_name, level_str] : category_levels) {
    if (level_str.empty())
      continue;
    if (auto level = ParseLogLevel(level_str)) {
      config.category_levels[cat_name] = *level;
    }
  }

  return config;
}

// ==========================================================================
// Guest Thread ID
// ==========================================================================

uint32_t GetLogGuestThreadId() {
  // TODO: Link to actual guest context when available
  return 0;
}

// ==========================================================================
// Deprecated Compatibility Layer
// ==========================================================================

// Map old LogCategory enum to new LogCategoryId
static LogCategoryId CategoryToId(LogCategory category) {
  auto idx = static_cast<uint16_t>(std::to_underlying(category));
  return LogCategoryId{idx};
}

std::optional<LogCategory> CategoryFromName(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  static const std::unordered_map<std::string, LogCategory> name_map = {
      {"core", LogCategory::Core},     {"cpu", LogCategory::CPU},
      {"ppc", LogCategory::CPU},       {"apu", LogCategory::APU},
      {"audio", LogCategory::APU},     {"gpu", LogCategory::GPU},
      {"graphics", LogCategory::GPU},  {"kernel", LogCategory::Kernel},
      {"krnl", LogCategory::Kernel},   {"runtime", LogCategory::Kernel},
      {"system", LogCategory::System}, {"sys", LogCategory::System},
      {"fs", LogCategory::FS},         {"filesystem", LogCategory::FS},
      {"vfs", LogCategory::FS},
  };

  auto it = name_map.find(lower);
  if (it != name_map.end())
    return it->second;
  return std::nullopt;
}

std::shared_ptr<spdlog::logger> GetLogger(LogCategory category) {
  return GetLogger(CategoryToId(category));
}

void SetCategoryLevel(LogCategory category, spdlog::level::level_enum level) {
  SetCategoryLevel(CategoryToId(category), level);
}

}  // namespace rex
