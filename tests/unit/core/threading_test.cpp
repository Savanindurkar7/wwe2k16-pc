/**
 * @file        tests/unit/core/threading_test.cpp
 * @brief       Unit tests for create-suspended and signal-path thread control.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

#include <rex/platform.h>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <cstdlib>
#endif

#include <catch2/catch_test_macros.hpp>

#include <rex/thread.h>

namespace {

using namespace std::chrono_literals;
using rex::thread::Thread;
using rex::thread::Wait;
using rex::thread::WaitResult;

constexpr size_t kCreateResumeIterations = 5000;
constexpr auto kThreadWaitTimeout = 100ms;

void SetStartWindowHook() {
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  REQUIRE(setenv("REXGLUE_TEST_THREAD_START_DELAY_US", "1000", 1) == 0);
#endif
}

void ClearStartWindowHook() {
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  REQUIRE(unsetenv("REXGLUE_TEST_THREAD_START_DELAY_US") == 0);
#endif
}

bool WaitFor(const std::function<bool()>& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + kThreadWaitTimeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

void RunCreateSuspendedResumeLoop(bool yield_before_resume) {
  SetStartWindowHook();

  std::atomic<uint32_t> ran{0};
  bool all_threads_completed = true;
  for (size_t i = 0; i < kCreateResumeIterations; ++i) {
    auto thread = Thread::Create(
        Thread::CreationParameters{.create_suspended = true}, [&ran] {
          ran.fetch_add(1, std::memory_order_release);
        });
    REQUIRE(thread != nullptr);

    if (yield_before_resume) {
      rex::thread::MaybeYield();
    }

    const bool resumed = thread->Resume();
    const auto wait_result = Wait(thread.get(), false, kThreadWaitTimeout);
    if (!resumed) {
      all_threads_completed = false;
      CHECK(resumed);
      const bool recovered = WaitFor([&] { return thread->Resume(); });
      CHECK(recovered);
      if (recovered) {
        CHECK(Wait(thread.get(), false, kThreadWaitTimeout) == WaitResult::kSuccess);
      }
      break;
    }

    if (wait_result != WaitResult::kSuccess) {
      all_threads_completed = false;
      CHECK(wait_result == WaitResult::kSuccess);
      const bool recovered = WaitFor([&] { return thread->Resume(); });
      CHECK(recovered);
      if (recovered) {
        CHECK(Wait(thread.get(), false, kThreadWaitTimeout) == WaitResult::kSuccess);
      }
      break;
    }
    if (ran.load(std::memory_order_acquire) != i + 1) {
      all_threads_completed = false;
      CHECK(ran.load(std::memory_order_acquire) == i + 1);
      break;
    }
  }

  CHECK(all_threads_completed);
  CHECK(ran.load(std::memory_order_acquire) == kCreateResumeIterations);
  ClearStartWindowHook();
}

}  // namespace

TEST_CASE("create-suspended thread resumes immediately", "[threading][thread_start]") {
  RunCreateSuspendedResumeLoop(false);
}

TEST_CASE("create-suspended thread resumes after a yield", "[threading][thread_start]") {
  RunCreateSuspendedResumeLoop(true);
}

TEST_CASE("suspend after create-suspended start uses the semaphore path", "[threading]") {
  std::atomic<bool> stop{false};
  std::atomic<bool> entered{false};
  std::atomic<uint32_t> ticks{0};

  auto thread = Thread::Create(
      Thread::CreationParameters{.create_suspended = true}, [&] {
        entered.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
          ticks.fetch_add(1, std::memory_order_relaxed);
          rex::thread::MaybeYield();
        }
      });
  REQUIRE(thread != nullptr);

  // Let the create-suspended start wait establish its count before releasing
  // it. This also makes the old semaphore-token defect deterministic.
  std::this_thread::sleep_for(5ms);
  REQUIRE(thread->Resume());
  REQUIRE(WaitFor([&] { return entered.load(std::memory_order_acquire); }));

  REQUIRE(thread->Suspend());
  std::this_thread::sleep_for(10ms);
  const auto paused_ticks = ticks.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(10ms);
  CHECK(ticks.load(std::memory_order_relaxed) == paused_ticks);

  REQUIRE(thread->Resume());
  CHECK(WaitFor([&] { return ticks.load(std::memory_order_relaxed) > paused_ticks; }));

  stop.store(true, std::memory_order_release);
  REQUIRE(Wait(thread.get(), false, kThreadWaitTimeout) == WaitResult::kSuccess);
}
