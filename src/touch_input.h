// wwe2k16 - on-screen touch controller (Android). The launcher's game screen (TouchControls.java) draws an Xbox 360
// style pad over the game and sends its state here through JNI; this driver hands it to the SDK's input system as
// controller 1. The input system merges every driver, so a real gamepad keeps working next to it.

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/input/input_driver.h>

namespace wwe2k16 {

class TouchInputDriver : public rex::input::InputDriver {
 public:
  explicit TouchInputDriver(rex::ui::Window* window) : InputDriver(window, 0) {}

  rex::X_STATUS Setup() override;
  rex::X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                                rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  rex::X_RESULT GetState(uint32_t user_index, rex::input::X_INPUT_STATE* out_state) override;
  rex::X_RESULT SetState(uint32_t user_index, rex::input::X_INPUT_VIBRATION* vibration) override;
  rex::X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, rex::input::X_INPUT_KEYSTROKE* out) override;

 private:
  uint32_t packet_ = 0;
};

// The pad's state, written by the Java side (any thread), read by the driver.
struct TouchPadState {
  std::atomic<bool> enabled{false};
  std::atomic<uint32_t> buttons{0};
  std::atomic<int32_t> lx{0}, ly{0}, rx{0}, ry{0};  // -32768..32767
  std::atomic<uint32_t> lt{0}, rt{0};               // 0..255
  std::atomic<uint32_t> version{0};
  std::atomic<uint32_t> rumble{0};                  // big << 16 | small, from the game (0..65535 each)
};
TouchPadState& GetTouchPad();

}  // namespace wwe2k16
