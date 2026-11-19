#pragma once
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
namespace rex {
template <typename Signature> class MoveOnlyFunction;
template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)> {
  struct Base {
    virtual ~Base() = default;
    virtual R call(Args&&... args) = 0;
  };
  template <typename F> struct Holder final : Base {
    template <typename U> explicit Holder(U&& f) : fn(std::forward<U>(f)) {}
    R call(Args&&... args) override {
      return std::invoke(fn, std::forward<Args>(args)...);
    }
    F fn;
  };
 public:
  MoveOnlyFunction() = default;
  MoveOnlyFunction(std::nullptr_t) noexcept {}
  MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
  MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
  MoveOnlyFunction(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;
  template <typename F, typename D = std::decay_t<F>,
    std::enable_if_t<!std::is_same_v<D, MoveOnlyFunction> &&
      std::is_invocable_r_v<R, D&, Args...>, int> = 0>
  MoveOnlyFunction(F&& f) : target(std::make_unique<Holder<D>>(std::forward<F>(f))) {}
  MoveOnlyFunction& operator=(std::nullptr_t) noexcept {
    target.reset(); return *this;
  }
  explicit operator bool() const noexcept { return !!target; }
  R operator()(Args... args) const {
    if (!target) throw std::bad_function_call();
    return target->call(std::forward<Args>(args)...);
  }
 private:
  std::unique_ptr<Base> target;
};
}
