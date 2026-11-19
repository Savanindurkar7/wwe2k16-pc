/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Sanjay Govind, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <filesystem>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>
#include <xinput.h>  // NOLINT(build/include_order)

#include <algorithm>

REXCVAR_DEFINE_STRING(pad_remap, "", "Input", "Controller button remap: FROM=TO pairs, e.g. A=B,B=A");

namespace rex::input::xinput {

XinputInputDriver::XinputInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      module_(nullptr),
      XInputGetCapabilities_(nullptr),
      XInputGetState_(nullptr),
      XInputGetStateEx_(nullptr),
      XInputGetKeystroke_(nullptr),
      XInputSetState_(nullptr),
      XInputEnable_(nullptr) {}

XinputInputDriver::~XinputInputDriver() {
  if (module_) {
    FreeLibrary((HMODULE)module_);
    module_ = nullptr;
    XInputGetCapabilities_ = nullptr;
    XInputGetState_ = nullptr;
    XInputGetStateEx_ = nullptr;
    XInputGetKeystroke_ = nullptr;
    XInputSetState_ = nullptr;
    XInputEnable_ = nullptr;
  }
}

X_STATUS XinputInputDriver::Setup() {
  HMODULE module = LoadLibraryW(L"xinput1_4.dll");
  if (!module) {
    return X_STATUS_DLL_NOT_FOUND;
  }

  // Support guide button with XInput using XInputGetStateEx
  // https://source.winehq.org/git/wine.git/?a=commit;h=de3591ca9803add117fbacb8abe9b335e2e44977
  auto const XInputGetStateEx = (LPCSTR)100;

  // Required.
  auto xigc = GetProcAddress(module, "XInputGetCapabilities");
  auto xigs = GetProcAddress(module, "XInputGetState");
  auto xigsEx = GetProcAddress(module, XInputGetStateEx);
  auto xigk = GetProcAddress(module, "XInputGetKeystroke");
  auto xiss = GetProcAddress(module, "XInputSetState");

  // Not required.
  auto xie = GetProcAddress(module, "XInputEnable");

  // Only fail when we don't have the bare essentials;
  if (!xigc || !xigs || !xigk || !xiss) {
    FreeLibrary(module);
    return X_STATUS_PROCEDURE_NOT_FOUND;
  }

  module_ = module;
  XInputGetCapabilities_ = xigc;
  XInputGetState_ = xigs;
  XInputGetStateEx_ = xigsEx;
  XInputGetKeystroke_ = xigk;
  XInputSetState_ = xiss;
  XInputEnable_ = xie;

  return X_STATUS_SUCCESS;
}

constexpr uint64_t SKIP_INVALID_CONTROLLER_TIME = 1100;
static uint64_t last_invalid_time[4];

static DWORD should_skip(uint32_t user_index) {
  uint64_t time = last_invalid_time[user_index];
  if (time) {
    uint64_t deltatime = rex::chrono::Clock::QueryHostUptimeMillis() - time;

    if (deltatime < SKIP_INVALID_CONTROLLER_TIME) {
      return ERROR_DEVICE_NOT_CONNECTED;
    }
    last_invalid_time[user_index] = 0;
  }
  return 0;
}

static void set_skip(uint32_t user_index) {
  last_invalid_time[user_index] = rex::chrono::Clock::QueryHostUptimeMillis();
}

X_RESULT XinputInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                            X_INPUT_CAPABILITIES* out_caps) {
  DWORD skipper = should_skip(user_index);
  if (skipper) {
    return skipper;
  }
  XINPUT_CAPABILITIES native_caps;
  auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilities_;
  DWORD result = xigc(user_index, flags & ~X_INPUT_DEVTYPE::XINPUT_DEVTYPE_KEYBOARD, &native_caps);
  if (result) {
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
      set_skip(user_index);
    }
    return result;
  }

  out_caps->type = native_caps.Type;
  out_caps->sub_type = native_caps.SubType;
  out_caps->flags = native_caps.Flags;
  out_caps->gamepad.buttons = native_caps.Gamepad.wButtons;
  out_caps->gamepad.left_trigger = native_caps.Gamepad.bLeftTrigger;
  out_caps->gamepad.right_trigger = native_caps.Gamepad.bRightTrigger;
  out_caps->gamepad.thumb_lx = native_caps.Gamepad.sThumbLX;
  out_caps->gamepad.thumb_ly = native_caps.Gamepad.sThumbLY;
  out_caps->gamepad.thumb_rx = native_caps.Gamepad.sThumbRX;
  out_caps->gamepad.thumb_ry = native_caps.Gamepad.sThumbRY;
  out_caps->vibration.left_motor_speed = native_caps.Vibration.wLeftMotorSpeed;
  out_caps->vibration.right_motor_speed = native_caps.Vibration.wRightMotorSpeed;

  return result;
}

// Controller button remap: comma-separated FROM=TO pairs of A B X Y LB RB LT RT START BACK LS RS UP DOWN LEFT RIGHT
// (e.g. "A=B,B=A"). Buttons that are not listed keep their own function.
namespace {
struct PadInput {
  const char* name;
  uint16_t bit;  // 0 for the triggers
  int trigger;   // 1 left, 2 right
};
constexpr PadInput kPadInputs[] = {
    {"A", 0x1000, 0},    {"B", 0x2000, 0},   {"X", 0x4000, 0},   {"Y", 0x8000, 0},    {"LB", 0x0100, 0},
    {"RB", 0x0200, 0},   {"LT", 0, 1},       {"RT", 0, 2},       {"START", 0x0010, 0}, {"BACK", 0x0020, 0},
    {"LS", 0x0040, 0},   {"RS", 0x0080, 0},  {"UP", 0x0001, 0},  {"DOWN", 0x0002, 0}, {"LEFT", 0x0004, 0},
    {"RIGHT", 0x0008, 0},
};
constexpr int kPadInputCount = int(std::size(kPadInputs));

int FindPadInput(const std::string& name) {
  for (int i = 0; i < kPadInputCount; ++i)
    if (name == kPadInputs[i].name)
      return i;
  return -1;
}

void ApplyPadRemap(X_INPUT_GAMEPAD& pad) {
  static std::string parsed_from;
  static std::array<int, kPadInputCount> target{};
  static bool active = false;
  const std::string config = REXCVAR_GET(pad_remap);
  if (config != parsed_from || parsed_from.empty()) {
    parsed_from = config;
    active = false;
    for (int i = 0; i < kPadInputCount; ++i)
      target[i] = i;
    size_t start = 0;
    while (start < config.size()) {
      size_t end = config.find(',', start);
      if (end == std::string::npos)
        end = config.size();
      const std::string pair = config.substr(start, end - start);
      const size_t eq = pair.find('=');
      if (eq != std::string::npos) {
        const int from = FindPadInput(pair.substr(0, eq));
        const int to = FindPadInput(pair.substr(eq + 1));
        if (from >= 0 && to >= 0 && from != to) {
          target[from] = to;
          active = true;
        }
      }
      start = end + 1;
    }
  }
  if (!active)
    return;
  uint16_t known = 0;
  for (const auto& in : kPadInputs)
    known |= in.bit;
  uint16_t buttons = pad.buttons & ~known;  // guide and anything else stays as it is
  uint8_t lt = 0, rt = 0;
  for (int i = 0; i < kPadInputCount; ++i) {
    const PadInput& in = kPadInputs[i];
    const uint8_t value = in.trigger == 1   ? uint8_t(pad.left_trigger)
                          : in.trigger == 2 ? uint8_t(pad.right_trigger)
                          : (pad.buttons & in.bit) ? uint8_t(255)
                                                   : uint8_t(0);
    if (!value)
      continue;
    const PadInput& out = kPadInputs[target[i]];
    if (out.trigger == 1)
      lt = std::max(lt, value);
    else if (out.trigger == 2)
      rt = std::max(rt, value);
    else if (value > 30)
      buttons |= out.bit;
  }
  pad.buttons = buttons;
  pad.left_trigger = lt;
  pad.right_trigger = rt;
}
}  // namespace

X_RESULT XinputInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  DWORD skipper = should_skip(user_index);
  if (skipper) {
    return skipper;
  }

  // Added padding in case we are using XInputGetStateEx
  struct {
    XINPUT_STATE state;
    unsigned int dwPaddingReserved;
  } native_state;

  // If the guide button is enabled use XInputGetStateEx, otherwise use the
  // default XInputGetState.
  auto xigs = REXCVAR_GET(guide_button) ? (decltype(&XInputGetState))XInputGetStateEx_
                                        : (decltype(&XInputGetState))XInputGetState_;

  DWORD result = xigs(user_index, &native_state.state);
  if (result) {
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
      set_skip(user_index);
    }
    return result;
  }

  out_state->packet_number = native_state.state.dwPacketNumber;
  out_state->gamepad.buttons = native_state.state.Gamepad.wButtons;
  out_state->gamepad.left_trigger = native_state.state.Gamepad.bLeftTrigger;
  out_state->gamepad.right_trigger = native_state.state.Gamepad.bRightTrigger;
  out_state->gamepad.thumb_lx = native_state.state.Gamepad.sThumbLX;
  out_state->gamepad.thumb_ly = native_state.state.Gamepad.sThumbLY;
  out_state->gamepad.thumb_rx = native_state.state.Gamepad.sThumbRX;
  out_state->gamepad.thumb_ry = native_state.state.Gamepad.sThumbRY;
  ApplyPadRemap(out_state->gamepad);

  return result;
}

X_RESULT XinputInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  DWORD skipper = should_skip(user_index);
  if (skipper) {
    return skipper;
  }
  XINPUT_VIBRATION native_vibration;
  native_vibration.wLeftMotorSpeed = vibration->left_motor_speed;
  native_vibration.wRightMotorSpeed = vibration->right_motor_speed;
  auto xiss = (decltype(&XInputSetState))XInputSetState_;
  DWORD result = xiss(user_index, &native_vibration);
  if (result == ERROR_DEVICE_NOT_CONNECTED) {
    set_skip(user_index);
  }
  return result;
}

X_RESULT XinputInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                         X_INPUT_KEYSTROKE* out_keystroke) {
  // We may want to filter flags/user_index before sending to native.
  // flags is reserved on desktop.
  DWORD result;
  bool user_any = users == 0xFF;
  // XInputGetKeystroke on Windows has a bug where it will return
  // ERROR_SUCCESS (0) even if the device is not connected:
  // https://stackoverflow.com/questions/23669238/xinputgetkeystroke-returning-error-success-while-controller-is-unplugged
  //
  // So we first check if the device is connected via XInputGetCapabilities, so
  // we are not passing back an uninitialized X_INPUT_KEYSTROKE structure.
  // If any user (0xFF) is polled this bug does not occur but GetCapabilities
  // would fail so we need to skip it.
  if (!user_any) {
    XINPUT_CAPABILITIES caps;
    auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilities_;
    result = xigc(users, 0, &caps);
    if (result) {
      return result;
    }
  }

  XINPUT_KEYSTROKE native_keystroke;
  auto xigk = (decltype(&XInputGetKeystroke))XInputGetKeystroke_;
  result = xigk(users, 0, &native_keystroke);
  if (result) {
    return result;
  }

  out_keystroke->virtual_key = native_keystroke.VirtualKey;
  out_keystroke->unicode = native_keystroke.Unicode;
  out_keystroke->flags = native_keystroke.Flags;
  out_keystroke->user_index = native_keystroke.UserIndex;
  out_keystroke->hid_code = native_keystroke.HidCode;
  // X_ERROR_EMPTY if no new keys
  // X_ERROR_DEVICE_NOT_CONNECTED if no device
  // X_ERROR_SUCCESS if key
  return result;
}
}  // namespace rex::input::xinput
