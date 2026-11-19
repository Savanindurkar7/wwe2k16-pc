#pragma once
#include <rex/ui/windowed_app_context.h>
#include <SDL3/SDL.h>
#include <atomic>
namespace rex { namespace ui {
class AndroidWindowedAppContext final : public WindowedAppContext {
 public:
  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;
  bool ShowSimpleMessageBox(const char* title, const char* message) override;
  void RunMainSDLLoop();
 private:
  std::atomic_bool quit_requested_{false};
};
} }
