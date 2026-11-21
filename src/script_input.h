// wwe2k16 - scripted controller for automated tests (WWE2K16_TEST_INPUT=1, commands in logs\test_input.txt, see
// test_input.cpp). Added to the SDK's input system as an extra driver for controller 1; the system merges it with
// the real controller and keyboard, so it works without the window having focus.
#pragma once

#include <cstdlib>
#include <vector>

#include <rex/input/input_driver.h>
#if __has_include(<rex/input/device.h>)
#include <rex/input/device.h>
#endif

#include "test_input.h"

namespace wwe2k16 {

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;

class ScriptInputDriver : public rex::input::InputDriver {
 public:
  explicit ScriptInputDriver(rex::ui::Window* window) : InputDriver(window, 0) {}

  static bool Enabled() {
    const char* v = std::getenv("WWE2K16_TEST_INPUT");
    return v && *v == '1';
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

#if __has_include(<rex/input/device.h>)
  // ReXGlue 0.10: one synthetic device, merged into player 1.
  static constexpr DeviceId kDevice = static_cast<DeviceId>(0x53435249);  // 'SCRI'
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kDevice;
    info.name = "Test script";
    info.synthetic = true;
    out.push_back(info);
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) Caps(out_caps);
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_state) State(out_state);
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override { return X_ERROR_EMPTY; }
#else
  X_RESULT GetCapabilities(uint32_t user_index, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (user_index != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    Caps(out_caps);
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override {
    if (user_index != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    State(out_state);
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION*) override {
    return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t, X_INPUT_KEYSTROKE*) override { return X_ERROR_EMPTY; }
#endif

 private:
  static void Caps(X_INPUT_CAPABILITIES* out_caps) {
    *out_caps = {};
    out_caps->type = 0x01;      // gamepad
    out_caps->sub_type = 0x01;  // standard controller
  }
  void State(X_INPUT_STATE* out_state) {
    const TestInput in = PollTestInput();
    *out_state = {};
    out_state->packet_number = ++packet_;
    out_state->gamepad.buttons = in.buttons;
  }

  uint32_t packet_ = 0;
};

}  // namespace wwe2k16
