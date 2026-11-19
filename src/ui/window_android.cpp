#include <rex/ui/window.h>
#include <rex/ui/surface_android.h>
#include <SDL3/SDL.h>

namespace rex {
namespace ui {

class AndroidWindow final : public Window {
 public:
  AndroidWindow(WindowedAppContext& context, std::string_view title,
                uint32_t width, uint32_t height)
      : Window(context, title, width, height) {}

  ~AndroidWindow() override {
    EnterDestructor();
    if (window_) SDL_DestroyWindow(window_);
  }

 protected:
  bool OpenImpl() override {
    const std::string title(GetTitle());
    window_ = SDL_CreateWindow(
        title.c_str(), static_cast<int>(GetDesiredLogicalWidth()),
        static_cast<int>(GetDesiredLogicalHeight()),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window_) return false;

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    if (width > 0 && height > 0) {
      WindowDestructionReceiver receiver(this);
      OnActualSizeUpdate(static_cast<uint32_t>(width),
                         static_cast<uint32_t>(height), receiver);
      OnFocusUpdate(true, receiver);
    }
    return true;
  }

  void RequestCloseImpl() override {
    if (!window_) return;
    WindowDestructionReceiver receiver(this);
    OnBeforeClose(receiver);
    if (receiver.IsWindowDestroyed()) return;
    SDL_Window* window = window_;
    window_ = nullptr;
    SDL_DestroyWindow(window);
    OnAfterClose();
  }

  std::unique_ptr<Surface> CreateSurfaceImpl(
      Surface::TypeFlags allowed_types) override {
    if (!(allowed_types & Surface::kTypeFlag_AndroidNativeWindow) ||
        !window_) {
      return nullptr;
    }

    auto* native_window = static_cast<ANativeWindow*>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
        nullptr));
    if (!native_window) return nullptr;

    return std::make_unique<AndroidNativeWindowSurface>(native_window);
  }

  void RequestPaintImpl() override {}

 private:
  SDL_Window* window_ = nullptr;
};

std::unique_ptr<Window> Window::Create(
    WindowedAppContext& context, const std::string_view title,
    uint32_t width, uint32_t height) {
  return std::make_unique<AndroidWindow>(context, title, width, height);
}

}  // namespace ui
}  // namespace rex
