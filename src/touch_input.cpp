// wwe2k16 - on-screen touch controller (Android). See touch_input.h.

#include "touch_input.h"

#include <algorithm>
#include <cmath>

#include <rex/logging.h>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace wwe2k16 {

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;

TouchPadState& GetTouchPad() {
  static TouchPadState state;
  return state;
}

X_STATUS TouchInputDriver::Setup() { return X_STATUS_SUCCESS; }

X_RESULT TouchInputDriver::GetCapabilities(uint32_t user_index, uint32_t, X_INPUT_CAPABILITIES* out_caps) {
  if (user_index != 0 || !GetTouchPad().enabled.load(std::memory_order_relaxed)) return X_ERROR_DEVICE_NOT_CONNECTED;
  *out_caps = {};
  out_caps->type = 0x01;      // gamepad
  out_caps->sub_type = 0x01;  // standard controller
  out_caps->gamepad.buttons = 0xF3FF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
  out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
  out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
  out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  TouchPadState& pad = GetTouchPad();
  if (user_index != 0 || !pad.enabled.load(std::memory_order_relaxed)) return X_ERROR_DEVICE_NOT_CONNECTED;
  *out_state = {};
  out_state->packet_number = pad.version.load(std::memory_order_acquire);
  out_state->gamepad.buttons = uint16_t(pad.buttons.load(std::memory_order_relaxed));
  out_state->gamepad.left_trigger = uint8_t(pad.lt.load(std::memory_order_relaxed));
  out_state->gamepad.right_trigger = uint8_t(pad.rt.load(std::memory_order_relaxed));
  out_state->gamepad.thumb_lx = int16_t(pad.lx.load(std::memory_order_relaxed));
  out_state->gamepad.thumb_ly = int16_t(pad.ly.load(std::memory_order_relaxed));
  out_state->gamepad.thumb_rx = int16_t(pad.rx.load(std::memory_order_relaxed));
  out_state->gamepad.thumb_ry = int16_t(pad.ry.load(std::memory_order_relaxed));
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  TouchPadState& pad = GetTouchPad();
  if (user_index != 0 || !pad.enabled.load(std::memory_order_relaxed)) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (vibration) {
    pad.rumble.store((uint32_t(vibration->left_motor_speed) << 16) | vibration->right_motor_speed,
                     std::memory_order_relaxed);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetKeystroke(uint32_t, uint32_t, X_INPUT_KEYSTROKE*) { return X_ERROR_EMPTY; }

}  // namespace wwe2k16

#if defined(__ANDROID__)

namespace {
int32_t Axis(jfloat v) {
  const float c = std::clamp(float(v), -1.0f, 1.0f);
  return int32_t(std::lround(c * (c < 0 ? 32768.0f : 32767.0f)));
}
}  // namespace

// TouchControls.nativeSetPad(buttons, lx, ly, rx, ry, lt, rt): sticks -1..1 (up = +1), triggers 0..1.
extern "C" JNIEXPORT void JNICALL Java_com_savanindurkar7_wwe2k16_TouchControls_nativeSetPad(
    JNIEnv*, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat lt, jfloat rt) {
  auto& pad = wwe2k16::GetTouchPad();
  pad.buttons.store(uint32_t(buttons) & 0xFFFF, std::memory_order_relaxed);
  pad.lx.store(Axis(lx), std::memory_order_relaxed);
  pad.ly.store(Axis(ly), std::memory_order_relaxed);
  pad.rx.store(Axis(rx), std::memory_order_relaxed);
  pad.ry.store(Axis(ry), std::memory_order_relaxed);
  pad.lt.store(uint32_t(std::clamp(float(lt), 0.0f, 1.0f) * 255.0f), std::memory_order_relaxed);
  pad.rt.store(uint32_t(std::clamp(float(rt), 0.0f, 1.0f) * 255.0f), std::memory_order_relaxed);
  pad.version.fetch_add(1, std::memory_order_release);
}

// TouchControls.nativeSetEnabled(on): the pad is visible (off while a real gamepad is connected, in AUTO mode).
extern "C" JNIEXPORT void JNICALL Java_com_savanindurkar7_wwe2k16_TouchControls_nativeSetEnabled(JNIEnv*, jclass,
                                                                                               jboolean on) {
  auto& pad = wwe2k16::GetTouchPad();
  if (!on) {
    pad.buttons.store(0);
    pad.lx.store(0);
    pad.ly.store(0);
    pad.rx.store(0);
    pad.ry.store(0);
    pad.lt.store(0);
    pad.rt.store(0);
    pad.version.fetch_add(1, std::memory_order_release);
  }
  pad.enabled.store(on != JNI_FALSE, std::memory_order_relaxed);
  REXLOG_INFO("[wwe2k16-touch] touch controls {}", on ? "shown" : "hidden");
}

// TouchControls.nativeGetRumble(): the game's vibration request (big << 16 | small).
extern "C" JNIEXPORT jint JNICALL Java_com_savanindurkar7_wwe2k16_TouchControls_nativeGetRumble(JNIEnv*, jclass) {
  return jint(wwe2k16::GetTouchPad().rumble.load(std::memory_order_relaxed));
}

#endif
