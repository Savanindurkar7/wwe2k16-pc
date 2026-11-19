/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/thread/mutex.h>

#if defined(__linux__)
#include <pthread.h>
#endif

#include <rex/diagnostics/rate_census.h>

namespace rex::thread {

std::atomic<bool> g_global_lock_census{false};

std::unique_lock<std::recursive_mutex> global_critical_region::AcquireCensused() {
  std::recursive_mutex& m = mutex();
  if (m.try_lock()) {
    return std::unique_lock<std::recursive_mutex>(m, std::adopt_lock);
  }
  uint32_t owner = 0;
#if defined(__GLIBC__)
  // glibc records the owning thread id in the mutex (diagnostic read only).
  owner = uint32_t(reinterpret_cast<pthread_mutex_t*>(m.native_handle())->__data.__owner);
#endif
  diagnostics::rate_census::OnGlobalLockContended(owner);
  const uint64_t start = diagnostics::rate_census::NowNs();
  m.lock();
  diagnostics::rate_census::RecordGlobalLockWait(owner, diagnostics::rate_census::NowNs() - start);
  return std::unique_lock<std::recursive_mutex>(m, std::adopt_lock);
}

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

}  // namespace rex::thread
