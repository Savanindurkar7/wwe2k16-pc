#include <rex/ui/windowed_app_context_android.h>
namespace rex { namespace ui {
void AndroidWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  SDL_Event event{};
  event.type = SDL_EVENT_USER;
  SDL_PushEvent(&event);
}
void AndroidWindowedAppContext::PlatformQuitFromUIThread() {
  quit_requested_.store(true);
  SDL_Event event{};
  event.type = SDL_EVENT_USER;
  SDL_PushEvent(&event);
}
bool AndroidWindowedAppContext::ShowSimpleMessageBox(const char* title, const char* message) {
  return SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, nullptr);
}
void AndroidWindowedAppContext::RunMainSDLLoop() {
  while (!quit_requested_.load()) {
    SDL_Event event{};
    if (!SDL_WaitEvent(&event)) {
      ExecutePendingFunctionsFromUIThread();
      continue;
    }
    ExecutePendingFunctionsFromUIThread();
    if (event.type == SDL_EVENT_QUIT ||
        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      QuitFromUIThread();
    }
  }
}
}
}  // namespace ui
