/**
 * @file        core/seh_posix.cpp
 * @brief       POSIX platform SEH implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/ppc/detail/seh.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>
#include <unistd.h>

#include <cstdlib>   // std::abort()
#include <cstring>   // std::memset()

#include <rex/ppc/exceptions.h>

namespace rex::ppc::detail {

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

// The signal actions that were installed before the PPC SEH layer claimed
// SIGSEGV/SIGBUS/SIGFPE/SIGILL. A fault that the PPC SEH layer declines (we
// are not inside SEH-protected guest code) must reach the handler that owned
// the signal before, for example an application crash reporter. Without this
// the handler installed by seh_initialize() buries any earlier handler and
// fatal, unhandled faults terminate silently via SIG_DFL.
static struct sigaction g_previous_sigsegv_action;
static struct sigaction g_previous_sigbus_action;
static struct sigaction g_previous_sigfpe_action;
static struct sigaction g_previous_sigill_action;

static const struct sigaction* PreviousActionForSignal(int sig) {
  switch (sig) {
    case SIGSEGV:
      return &g_previous_sigsegv_action;
    case SIGBUS:
      return &g_previous_sigbus_action;
    case SIGFPE:
      return &g_previous_sigfpe_action;
    case SIGILL:
      return &g_previous_sigill_action;
    default:
      return nullptr;
  }
}

// Terminate with the default disposition for the signal. Used both when there
// is no previous handler to chain to and when the chained handler returns
// without resolving the fault (returning from a fatal signal handler would
// re-execute the faulting instruction forever).
[[noreturn]] static void TerminateWithDefaultDisposition(int sig) {
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  sigaction(sig, &action, nullptr);
  sigset_t unblocked;
  sigemptyset(&unblocked);
  sigaddset(&unblocked, sig);
  sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
  raise(sig);
  _exit(128 + sig);
}

[[noreturn]] static void ChainToPreviousHandler(int sig, siginfo_t* info, void* context) {
  const struct sigaction* previous = PreviousActionForSignal(sig);
  if (previous) {
    if (previous->sa_handler == SIG_IGN || previous->sa_handler == SIG_DFL) {
      // No earlier handler: keep the default fatal behaviour.
      TerminateWithDefaultDisposition(sig);
    }
    if (previous->sa_flags & SA_SIGINFO) {
      previous->sa_sigaction(sig, info, context);
    } else {
      previous->sa_handler(sig);
    }
  }
  // The previous handler returned without resolving the fault.
  TerminateWithDefaultDisposition(sig);
}

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(uint32_t /*code*/, void* /*ep*/) {
  // Not used on POSIX - signal handlers throw directly
  return 0;
}

/// Signal handler for SIGSEGV/SIGBUS/SIGFPE/SIGILL
static void signal_handler(int sig, siginfo_t* info, void* context) {
  // Only handle if we're in SEH-protected code
  if (!tls_seh_active) {
    // Not in SEH region - pass the fault to the handler that owned the signal
    // before this layer (e.g. an application crash reporter), or terminate
    // with the default disposition if there was none.
    ChainToPreviousHandler(sig, info, context);
  }

  // Determine exception code based on signal
  SehException::Code code;
  switch (sig) {
    case SIGSEGV:
      code = SehException::ACCESS_VIOLATION;
      break;
    case SIGBUS:
      code = SehException::IN_PAGE_ERROR;
      break;
    case SIGFPE:
      code = SehException::FLOAT_DIVIDE_BY_ZERO;
      break;
    case SIGILL:
      code = SehException::ILLEGAL_INSTRUCTION;
      break;
    default:
      code = SehException::UNKNOWN;
      break;
  }

  // Get fault address
  uintptr_t address = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;

  // Store in thread state for potential rethrow
  tls_seh_state.code = static_cast<uint32_t>(code);
  tls_seh_state.info[0] = 0;
  tls_seh_state.info[1] = address;

  // Use libunwind to throw from signal context
  // This works because libunwind can unwind through signal frames
  throw SehException(code, address);
}

[[noreturn]] void seh_rethrow() {
  // Map stored code back to signal and re-raise
  uint32_t code = tls_seh_state.code;
  int sig;
  switch (code) {
    case SehException::ACCESS_VIOLATION:
      sig = SIGSEGV;
      break;
    case SehException::IN_PAGE_ERROR:
      sig = SIGBUS;
      break;
    case SehException::FLOAT_DIVIDE_BY_ZERO:
    case SehException::INTEGER_DIVIDE_BY_ZERO:
      sig = SIGFPE;
      break;
    case SehException::ILLEGAL_INSTRUCTION:
      sig = SIGILL;
      break;
    default:
      abort();
  }
  // Restore default handler and re-raise
  signal(sig, SIG_DFL);
  raise(sig);
  abort();  // Should not reach here
}

void seh_initialize() {
  if (g_seh_initialized.exchange(true)) {
    return;  // Already initialized
  }

  struct sigaction sa;
  sa.sa_sigaction = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;  // SA_NODEFER allows re-entry for nested exceptions

  // Save the actions being replaced so declined faults can be chained to them.
  sigaction(SIGSEGV, &sa, &g_previous_sigsegv_action);
  sigaction(SIGBUS, &sa, &g_previous_sigbus_action);
  sigaction(SIGFPE, &sa, &g_previous_sigfpe_action);
  sigaction(SIGILL, &sa, &g_previous_sigill_action);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::ppc::detail
