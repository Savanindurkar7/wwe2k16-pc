#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <rex/thread/fiber.h>
#include <algorithm>
#include <cstdlib>
#include <cstdint>

namespace rex::thread {

extern "C" void rex_fiber_switch(void* from, const void* to);

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  if (tls_current_) return tls_current_;
  auto* fiber = new Fiber();
  fiber->is_thread_fiber_ = true;
  tls_current_ = fiber;
  return fiber;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  if (!entry) return nullptr;

  auto* fiber = new Fiber();
  fiber->entry_ = entry;
  fiber->arg_ = arg;

  try {
    fiber->stack_.resize(std::max(stack_size, size_t(64 * 1024)));
  } catch (...) {
    delete fiber;
    return nullptr;
  }

  uintptr_t top = reinterpret_cast<uintptr_t>(
      fiber->stack_.data() + fiber->stack_.size());
  top &= ~uintptr_t(15);
  fiber->context_.sp = top;
  fiber->context_.x19_x30[11] =
      reinterpret_cast<uintptr_t>(&Fiber::Trampoline);

  return fiber;
}

void Fiber::Trampoline() {
  Fiber* fiber = tls_current_;
  if (!fiber || !fiber->entry_) std::abort();
  fiber->entry_(fiber->arg_);
  std::abort();
}

void Fiber::SwitchTo(Fiber* target) {
  if (!target || target == tls_current_) return;
  Fiber* from = tls_current_;
  if (!from) std::abort();
  tls_current_ = target;
  rex_fiber_switch(&from->context_, &target->context_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  }
  delete this;
}

}  // namespace rex::thread

#endif
