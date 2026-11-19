#include <cstdlib>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>

namespace {

class ScopedContinueEnvironment {
 public:
  ScopedContinueEnvironment() {
    if (const char* current = std::getenv("WWE2K16_MISSING_FN_CONTINUE")) {
      previous_ = current;
    }
#if defined(_WIN32)
    succeeded_ = _putenv_s("WWE2K16_MISSING_FN_CONTINUE", "1") == 0;
#else
    succeeded_ = setenv("WWE2K16_MISSING_FN_CONTINUE", "1", 1) == 0;
#endif
  }

  ~ScopedContinueEnvironment() {
#if defined(_WIN32)
    _putenv_s("WWE2K16_MISSING_FN_CONTINUE", previous_ ? previous_->c_str() : "");
#else
    if (previous_) {
      setenv("WWE2K16_MISSING_FN_CONTINUE", previous_->c_str(), 1);
    } else {
      unsetenv("WWE2K16_MISSING_FN_CONTINUE");
    }
#endif
  }

  bool succeeded() const { return succeeded_; }

 private:
  std::optional<std::string> previous_;
  bool succeeded_ = false;
};

}  // namespace

TEST_CASE("missing indirect PPC functions can continue and count each miss", "[system][ppc]") {
  ScopedContinueEnvironment continue_environment;
  REQUIRE(continue_environment.succeeded());

  PPCContext context{};
  context.lr = 0x82345678;
  context.ctr.u32 = 0x8328C3D0;

  const uint64_t before = PPCMissingIndirectFunctionCallCount();
  PPCHandleMissingIndirectFunction(context, nullptr, 0x8328C3D0);
  CHECK(PPCMissingIndirectFunctionCallCount() == before + 1);

  // Logging is one-shot per guest target, while the diagnostic counter counts
  // every missing dispatch (including repeated exploration calls).
  PPCHandleMissingIndirectFunction(context, nullptr, 0x8328C3D0);
  CHECK(PPCMissingIndirectFunctionCallCount() == before + 2);
}
