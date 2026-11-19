#pragma once

#include <android/native_window.h>

#include <cstdint>

#include <rex/platform.h>
#include <rex/ui/surface.h>

namespace rex {
namespace ui {

class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window)
      : window_(window) {
    if (window_) {
      ANativeWindow_acquire(window_);
    }
  }

  ~AndroidNativeWindowSurface() override {
    if (window_) {
      ANativeWindow_release(window_);
    }
  }

  TypeIndex GetType() const override {
    return kTypeIndex_AndroidNativeWindow;
  }

  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out,
                   uint32_t& height_out) const override {
    if (!window_) {
      width_out = 0;
      height_out = 0;
      return false;
    }

    width_out = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
    height_out = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
    return width_out != 0 && height_out != 0;
  }

 private:
  ANativeWindow* window_ = nullptr;
};

}  // namespace ui
}  // namespace rex
