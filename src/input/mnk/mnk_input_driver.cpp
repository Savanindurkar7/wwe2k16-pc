/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <chrono>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/clipboard.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#if REX_PLATFORM_WIN32
#include <rex/ui/window_win.h>
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(mnk_mode, true, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_BOOL(mnk_mouse, false, "Input", "Mouse drives the right stick (captures the cursor)");
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK").range(0, 3);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);

REXCVAR_DEFINE_STRING(keybind_a, "L,Return", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "K,Escape", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "J", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "Space", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "Shift", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "I", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "O", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "U", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_up, "Numpad8,T", "Input/Keybinds/Controller", "Right stick up");
REXCVAR_DEFINE_STRING(keybind_rstick_down, "Numpad2,G", "Input/Keybinds/Controller", "Right stick down");
REXCVAR_DEFINE_STRING(keybind_rstick_left, "Numpad4,F", "Input/Keybinds/Controller", "Right stick left");
REXCVAR_DEFINE_STRING(keybind_rstick_right, "Numpad6,H", "Input/Keybinds/Controller", "Right stick right");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "V", "Input/Keybinds/Controller", "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Up,1", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Down,2", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Left,3", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Right,4", "Input/Keybinds/Controller", "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "Return", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

namespace {

constexpr size_t kMaxKeystrokeQueueSize = 256;
constexpr size_t kMaxPasteCharacterCount = 128;
constexpr size_t kMaxTextEntryQueueSize = 512;
constexpr std::chrono::milliseconds kTextEntryHold{50};
constexpr std::chrono::milliseconds kTextEntryGap{50};

bool IsFilteredTextCharacter(uint16_t unicode) {
  // Match OnKeyChar's single-line text filtering, including UTF-16 surrogate
  // halves which are not independently useful as guest text input.
  return unicode < 0x20 || (unicode >= 0x7F && unicode <= 0x9F) ||
         (unicode >= 0xD800 && unicode <= 0xDFFF);
}

bool IsControlKeyDown(const bool (&key_down)[256]) {
  return key_down[static_cast<uint16_t>(VirtualKey::kControl)] ||
         key_down[static_cast<uint16_t>(VirtualKey::kLControl)] ||
         key_down[static_cast<uint16_t>(VirtualKey::kRControl)];
}

struct ParsedBinding {
  std::vector<VirtualKey> keys;
};

struct BindingCache {
  std::array<ParsedBinding, 25> bindings;
};

std::shared_ptr<const BindingCache> g_binding_cache;
std::mutex g_binding_cache_mutex;
std::once_flag g_binding_cache_init;

ParsedBinding ParseBinding(const std::string& binding) {
  ParsedBinding parsed;
  size_t start = 0;
  while (start <= binding.size()) {
    size_t end = binding.find(',', start);
    if (end == std::string::npos) end = binding.size();
    std::string name = binding.substr(start, end - start);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    const VirtualKey key = rex::ui::ParseVirtualKey(name);
    auto add = [&](VirtualKey candidate) {
      if (candidate != VirtualKey::kNone &&
          std::find(parsed.keys.begin(), parsed.keys.end(), candidate) == parsed.keys.end()) {
        parsed.keys.push_back(candidate);
      }
    };
    add(key);
    if (key == VirtualKey::kShift) {
      add(VirtualKey::kLShift);
      add(VirtualKey::kRShift);
    } else if (key == VirtualKey::kControl) {
      add(VirtualKey::kLControl);
      add(VirtualKey::kRControl);
    }
    start = end + 1;
  }
  return parsed;
}

constexpr std::array<const char*, 25> kBindingNames = {
    "keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_shoulder",
    "keybind_right_shoulder", "keybind_left_trigger", "keybind_right_trigger", "keybind_start",
    "keybind_back", "keybind_lstick_press", "keybind_rstick_press", "keybind_lstick_up",
    "keybind_lstick_down", "keybind_lstick_left", "keybind_lstick_right", "keybind_rstick_up",
    "keybind_rstick_down", "keybind_rstick_left", "keybind_rstick_right", "keybind_dpad_up",
    "keybind_dpad_down", "keybind_dpad_left", "keybind_dpad_right", "keybind_guide"};

std::array<std::string, 25> ReadBindingValues() {
  return {REXCVAR_GET(keybind_a), REXCVAR_GET(keybind_b), REXCVAR_GET(keybind_x),
          REXCVAR_GET(keybind_y), REXCVAR_GET(keybind_left_shoulder),
          REXCVAR_GET(keybind_right_shoulder), REXCVAR_GET(keybind_left_trigger),
          REXCVAR_GET(keybind_right_trigger), REXCVAR_GET(keybind_start), REXCVAR_GET(keybind_back),
          REXCVAR_GET(keybind_lstick_press), REXCVAR_GET(keybind_rstick_press),
          REXCVAR_GET(keybind_lstick_up), REXCVAR_GET(keybind_lstick_down),
          REXCVAR_GET(keybind_lstick_left), REXCVAR_GET(keybind_lstick_right),
          REXCVAR_GET(keybind_rstick_up), REXCVAR_GET(keybind_rstick_down),
          REXCVAR_GET(keybind_rstick_left), REXCVAR_GET(keybind_rstick_right),
          REXCVAR_GET(keybind_dpad_up), REXCVAR_GET(keybind_dpad_down),
          REXCVAR_GET(keybind_dpad_left), REXCVAR_GET(keybind_dpad_right), REXCVAR_GET(keybind_guide)};
}

void InitializeBindingCache() {
  std::call_once(g_binding_cache_init, [] {
    auto cache = std::make_shared<BindingCache>();
    const auto values = ReadBindingValues();
    for (size_t i = 0; i < values.size(); ++i) cache->bindings[i] = ParseBinding(values[i]);
    std::atomic_store(&g_binding_cache, std::shared_ptr<const BindingCache>(cache));
    for (size_t i = 0; i < kBindingNames.size(); ++i) {
      rex::cvar::RegisterChangeCallback(kBindingNames[i], [i](std::string_view, std::string_view value) {
        std::lock_guard lock(g_binding_cache_mutex);
        auto current = std::atomic_load(&g_binding_cache);
        if (!current) return;
        auto updated = std::make_shared<BindingCache>(*current);
        updated->bindings[i] = ParseBinding(std::string(value));
        std::atomic_store(&g_binding_cache, std::shared_ptr<const BindingCache>(updated));
      });
    }
  });
}

std::shared_ptr<const BindingCache> GetBindingCache() {
  InitializeBindingCache();
  return std::atomic_load(&g_binding_cache);
}

bool IsBindPressed(const bool (&key_down)[256], const ParsedBinding& binding) {
  for (VirtualKey key : binding.keys) {
    const uint16_t index = static_cast<uint16_t>(key);
    if (index < 256 && key_down[index]) return true;
  }
  return false;
}

struct PadKeyList {
  std::array<VirtualKey, 24> values{};
  size_t count = 0;
  const VirtualKey* begin() const { return values.data(); }
  const VirtualKey* end() const { return values.data() + count; }
  void push_back(VirtualKey key) {
    if (count < values.size()) values[count++] = key;
  }
};

PadKeyList GetPadVirtualKeys(VirtualKey key) {
  static constexpr std::array<std::pair<size_t, VirtualKey>, 24> pad_bindings = {{
      {0, VirtualKey::kXInputPadA}, {1, VirtualKey::kXInputPadB}, {2, VirtualKey::kXInputPadX},
      {3, VirtualKey::kXInputPadY}, {4, VirtualKey::kXInputPadLShoulder},
      {5, VirtualKey::kXInputPadRShoulder}, {6, VirtualKey::kXInputPadLTrigger},
      {7, VirtualKey::kXInputPadRTrigger}, {8, VirtualKey::kXInputPadStart},
      {9, VirtualKey::kXInputPadBack}, {10, VirtualKey::kXInputPadLThumbPress},
      {11, VirtualKey::kXInputPadRThumbPress}, {12, VirtualKey::kXInputPadLThumbUp},
      {13, VirtualKey::kXInputPadLThumbDown}, {14, VirtualKey::kXInputPadLThumbLeft},
      {15, VirtualKey::kXInputPadLThumbRight}, {16, VirtualKey::kXInputPadRThumbUp},
      {17, VirtualKey::kXInputPadRThumbDown}, {18, VirtualKey::kXInputPadRThumbLeft},
      {19, VirtualKey::kXInputPadRThumbRight}, {20, VirtualKey::kXInputPadDpadUp},
      {21, VirtualKey::kXInputPadDpadDown}, {22, VirtualKey::kXInputPadDpadLeft},
      {23, VirtualKey::kXInputPadDpadRight}}};
  const auto cache = GetBindingCache();
  PadKeyList mapped;
  if (!cache) return mapped;
  for (const auto& [index, pad_key] : pad_bindings) {
    const auto& keys = cache->bindings[index].keys;
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) mapped.push_back(pad_key);
  }
  return mapped;
}

}  // namespace

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  if (attached_window_) {
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

X_STATUS MnkInputDriver::Setup() {
  InitializeBindingCache();
  REXLOG_INFO("MnK input driver initialized");
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    attached_window_ = window;
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    if (mouse_captured_) {
      mouse_captured_ = false;
      attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      attached_window_->ReleaseMouse();
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

// Virtual keys consumed by the guest's front-end keyboard UI through
// XamInputGetKeystroke(Ex): the standard VK navigation/action set (the guest
// pump dispatches on VK codes 3..40 and consumes the VK_SHIFT key-up). Only
// these are turned into keystroke events; all other keys stay state-only.
static bool IsUiKeystrokeKey(uint16_t vk) {
  switch (static_cast<VirtualKey>(vk)) {
    case VirtualKey::kCancel:
    case VirtualKey::kBack:
    case VirtualKey::kTab:
    case VirtualKey::kReturn:
    case VirtualKey::kShift:
    case VirtualKey::kCapital:
    case VirtualKey::kEscape:
    case VirtualKey::kSpace:
    case VirtualKey::kLeft:
    case VirtualKey::kUp:
    case VirtualKey::kRight:
    case VirtualKey::kDown:
    case VirtualKey::kW:
    case VirtualKey::kA:
    case VirtualKey::kS:
    case VirtualKey::kD:
    case VirtualKey::kJ:
    case VirtualKey::kK:
    case VirtualKey::kL:
    case VirtualKey::kI:
      return true;
    default:
      return false;
  }
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  UpdateMouseCapture();

  if (!is_active() || !has_focus_) {
    std::lock_guard lock(state_mutex_);
    while (!text_entry_queue_.empty()) {
      text_entry_queue_.pop();
    }
    text_entry_held_button_ = 0;
    text_entry_phase_end_ = {};
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);

  TextEntryTarget* const text_entry_target = this->text_entry_target();
  const bool text_entry_active = text_entry_target && text_entry_target->IsActive();
  if (!text_entry_active) {
    while (!text_entry_queue_.empty()) {
      text_entry_queue_.pop();
    }
    text_entry_held_button_ = 0;
    text_entry_phase_end_ = {};
  } else {
    uint16_t buttons = 0;
    if (key_down_[static_cast<uint16_t>(VirtualKey::kUp)])
      buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    if (key_down_[static_cast<uint16_t>(VirtualKey::kDown)])
      buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    if (key_down_[static_cast<uint16_t>(VirtualKey::kLeft)])
      buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    if (key_down_[static_cast<uint16_t>(VirtualKey::kRight)])
      buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

    // Each action is a pad press held for kTextEntryHold and followed by kTextEntryGap with no
    // action, so the title sees a clean down/up edge however often it polls per frame.
    const auto now = std::chrono::steady_clock::now();
    if (text_entry_held_button_ != 0) {
      if (now < text_entry_phase_end_) {
        buttons |= text_entry_held_button_;
      } else {
        text_entry_held_button_ = 0;
        text_entry_phase_end_ = now + kTextEntryGap;
      }
    } else if (now >= text_entry_phase_end_) {
      uint16_t action_button = 0;
      while (!text_entry_queue_.empty() && action_button == 0) {
        const TextEntryInput input = text_entry_queue_.front();
        text_entry_queue_.pop();
        if (input.type == TextEntryInput::Type::kButton) {
          action_button = input.button;
          continue;
        }
        if (input.character == u' ') {
          action_button = X_INPUT_GAMEPAD_Y;
          continue;
        }
        // Preserve the character's case: the title's on-screen keyboard may
        // render a lower-case layer (e.g. dialogue text) or an upper-case layer
        // (e.g. name fields), so the Shift/Caps Lock result must reach
        // SelectChar unchanged.
        if (text_entry_target->SelectChar(input.character)) {
          action_button = X_INPUT_GAMEPAD_A;
        }
      }
      if (action_button != 0) {
        buttons |= action_button;
        text_entry_held_button_ = action_button;
        text_entry_phase_end_ = now + kTextEntryHold;
      }
    }

    ++packet_number_;
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
      out_state->gamepad.buttons = buttons;
    }
    return X_ERROR_SUCCESS;
  }

  const auto bindings = GetBindingCache();
  if (!bindings) return X_ERROR_FUNCTION_FAILED;
  uint16_t buttons = 0;
  if (IsBindPressed(key_down_, bindings->bindings[0]))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down_, bindings->bindings[1]))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down_, bindings->bindings[2]))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down_, bindings->bindings[3]))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down_, bindings->bindings[4]))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down_, bindings->bindings[5]))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down_, bindings->bindings[10]))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down_, bindings->bindings[11]))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down_, bindings->bindings[9]))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(key_down_, bindings->bindings[8]))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, bindings->bindings[24]))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down_, bindings->bindings[20]))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down_, bindings->bindings[21]))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down_, bindings->bindings[22]))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down_, bindings->bindings[23]))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  const bool lt_pressed = IsBindPressed(key_down_, bindings->bindings[6]);
  const bool rt_pressed = IsBindPressed(key_down_, bindings->bindings[7]);
  uint8_t lt = lt_pressed ? 0xFF : 0;
  uint8_t rt = rt_pressed ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed(key_down_, bindings->bindings[14]))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down_, bindings->bindings[15]))
    lx += INT16_MAX;
  if (IsBindPressed(key_down_, bindings->bindings[12]))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, bindings->bindings[13]))
    ly -= INT16_MAX;

  double sensitivity = REXCVAR_GET(mnk_sensitivity);
  constexpr double kBaseScale = 200.0;
  int32_t rx = 0;
  int32_t ry = 0;
  if (IsBindPressed(key_down_, bindings->bindings[18]))
    rx -= INT16_MAX;
  if (IsBindPressed(key_down_, bindings->bindings[19]))
    rx += INT16_MAX;
  if (IsBindPressed(key_down_, bindings->bindings[16]))
    ry += INT16_MAX;
  if (IsBindPressed(key_down_, bindings->bindings[17]))
    ry -= INT16_MAX;
  rx += static_cast<int32_t>(mouse_dx_ * sensitivity * kBaseScale);
  ry += static_cast<int32_t>(-mouse_dy_ * sensitivity * kBaseScale);
  mouse_dx_ = 0;
  mouse_dy_ = 0;

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // XamInputGetKeystroke is commonly called with XUSER_INDEX_ANY (0xFF) by
  // front-end menus. Keep the keyboard-emulated controller visible for that
  // query as SDL's physical-controller driver does.
  constexpr uint32_t kAnyUser = 0xFF;
  if (!IsEnabled() || (user_index != UserIndex() && user_index != kAnyUser)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
    if (user_index == kAnyUser) out_keystroke->user_index = static_cast<uint8_t>(UserIndex());
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t virtual_key, bool down, uint16_t unicode,
                                      bool repeat) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = virtual_key;
  ks.unicode = unicode;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  if (down && repeat) {
    ks.flags = static_cast<uint16_t>(ks.flags) | X_INPUT_KEYSTROKE_REPEAT;
  }
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  if (keystroke_queue_.size() >= kMaxKeystrokeQueueSize) {
    keystroke_queue_.pop();
  }
  keystroke_queue_.push(ks);
}

void MnkInputDriver::EnqueueTextCharacter(char16_t character) {
  if (text_entry_queue_.size() >= kMaxTextEntryQueueSize) {
    text_entry_queue_.pop();
  }
  text_entry_queue_.push(
      TextEntryInput{TextEntryInput::Type::kCharacter, character, 0});
}

void MnkInputDriver::EnqueueTextButton(uint16_t button) {
  if (text_entry_queue_.size() >= kMaxTextEntryQueueSize) {
    text_entry_queue_.pop();
  }
  text_entry_queue_.push(TextEntryInput{TextEntryInput::Type::kButton, 0, button});
}

void MnkInputDriver::CenterCursor() {
  if (!attached_window_)
    return;
  int32_t cx = static_cast<int32_t>(attached_window_->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(attached_window_->GetActualLogicalHeight() / 2);
  prev_mouse_x_ = cx;
  prev_mouse_y_ = cy;
#if REX_PLATFORM_WIN32
  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (win32_window && win32_window->hwnd()) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(win32_window->hwnd(), &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::UpdateMouseCapture() {
  if (!attached_window_)
    return;

  const bool mouse_enabled = REXCVAR_GET(mnk_mouse);
  bool should_capture = IsEnabled() && mouse_enabled && has_focus_ && is_active();

  if (!mouse_enabled && mouse_enabled_) {
    std::lock_guard lock(state_mutex_);
    mouse_dx_ = 0;
    mouse_dy_ = 0;
    SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
    SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
    SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
  }
  mouse_enabled_ = mouse_enabled;

  if (should_capture && !mouse_captured_) {
    mouse_captured_ = true;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    attached_window_->CaptureMouse();
    // Reset deltas to avoid a spike on capture start
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  } else if (!should_capture && mouse_captured_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    attached_window_->ReleaseMouse();
  }

  // Re-center cursor each frame while captured to prevent edge clamping
  if (mouse_captured_) {
    CenterCursor();
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  bool ctrl_v = false;
  bool paste = false;
  {
    std::lock_guard lock(state_mutex_);
    ctrl_v = vk == static_cast<uint16_t>(VirtualKey::kV) &&
             (e.is_ctrl_pressed() || IsControlKeyDown(key_down_));
    paste = ctrl_v && !paste_key_down_;
    if (vk == static_cast<uint16_t>(VirtualKey::kV)) {
      paste_key_down_ = ctrl_v;
    }
    // Do not let a Ctrl+V paste temporarily activate a user-configured V pad
    // binding, or emit V itself as a UI keystroke. Repeated Ctrl+V keydowns are
    // ignored until the matching key-up so a held shortcut pastes only once.
    SetKeyState(vk, !ctrl_v);
    if (!ctrl_v && IsUiKeystrokeKey(vk)) {
      EnqueueKeystroke(vk, true, 0, e.prev_state());
    }
    if (!ctrl_v) {
      for (VirtualKey pad_vk : GetPadVirtualKeys(static_cast<VirtualKey>(vk))) {
        EnqueueKeystroke(static_cast<uint16_t>(pad_vk), true, 0, e.prev_state());
      }
    }
    if (!ctrl_v && !e.prev_state()) {
      switch (static_cast<VirtualKey>(vk)) {
        case VirtualKey::kBack:
          EnqueueTextButton(X_INPUT_GAMEPAD_X);
          break;
        case VirtualKey::kReturn:
          EnqueueTextButton(X_INPUT_GAMEPAD_START);
          break;
        case VirtualKey::kEscape:
          EnqueueTextButton(X_INPUT_GAMEPAD_B);
          break;
        default:
          break;
      }
    }
  }

  if (!paste) {
    return;
  }

  // On GTK, this handler runs on the UI thread that receives key events.
  const auto clipboard_text = rex::ui::GetClipboardText();
  if (!clipboard_text) {
    return;
  }

  std::lock_guard lock(state_mutex_);
  if (!has_focus_ || !IsEnabled()) {
    return;
  }
  const size_t character_count = std::min(clipboard_text->size(), kMaxPasteCharacterCount);
  for (size_t i = 0; i < character_count; ++i) {
    const uint16_t unicode = static_cast<uint16_t>((*clipboard_text)[i]);
    if (IsFilteredTextCharacter(unicode)) {
      continue;
    }
    EnqueueTextCharacter(static_cast<char16_t>(unicode));
    if (unicode == static_cast<uint16_t>(VirtualKey::kSpace)) {
      // Match typed Space: the guest inserts it through the VK_SPACE action.
      EnqueueKeystroke(static_cast<uint16_t>(VirtualKey::kSpace), true);
    } else {
      EnqueueKeystroke(static_cast<uint16_t>(VirtualKey::kPacket), true, unicode);
    }
  }
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  if (vk == static_cast<uint16_t>(VirtualKey::kV)) {
    paste_key_down_ = false;
  }
  if (!IsEnabled())
    return;
  SetKeyState(vk, false);
  if (IsUiKeystrokeKey(vk)) {
    EnqueueKeystroke(vk, false);
  }
  for (VirtualKey pad_vk : GetPadVirtualKeys(static_cast<VirtualKey>(vk))) {
    EnqueueKeystroke(static_cast<uint16_t>(pad_vk), false);
  }
}

void MnkInputDriver::OnKeyChar(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;

  const uint16_t unicode = static_cast<uint16_t>(e.virtual_key());
  if (IsFilteredTextCharacter(unicode)) {
    return;
  }

  std::lock_guard lock(state_mutex_);
  if (!has_focus_)
    return;

  // GTK (verified under XTest) produces a printable 'v' character event for Ctrl+V instead
  // of the usual U+0016 control character. Neither form should type the shortcut.
  if ((unicode == u'v' || unicode == u'V') &&
      (e.is_ctrl_pressed() || IsControlKeyDown(key_down_))) {
    return;
  }

  const auto repeat_count = std::clamp(e.repeat_count(), 1,
                                       static_cast<int>(kMaxKeystrokeQueueSize));
  for (int i = 0; i < repeat_count; ++i) {
    EnqueueTextCharacter(static_cast<char16_t>(unicode));
  }

  // VK_SPACE already has a dedicated KEYDOWN path in the guest's UI dispatcher,
  // which inserts one space. Keep that legacy keystroke path, while the separate
  // text-entry queue above uses Space to pulse Y.
  if (unicode == static_cast<uint16_t>(VirtualKey::kSpace))
    return;

  // The game reserves VK 3..40 for UI actions. VK_PACKET keeps translated
  // printable text on the guest's unicode insertion path, including punctuation
  // whose character value overlaps one of those action VKs.
  for (int i = 0; i < repeat_count; ++i) {
    EnqueueKeystroke(static_cast<uint16_t>(VirtualKey::kPacket), true, unicode,
                     e.prev_state() || i > 0);
  }
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !REXCVAR_GET(mnk_mouse) || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !REXCVAR_GET(mnk_mouse))
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !REXCVAR_GET(mnk_mouse) || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  int32_t x = e.x();
  int32_t y = e.y();
  mouse_dx_ += x - prev_mouse_x_;
  mouse_dy_ += y - prev_mouse_y_;
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = false;
  std::memset(key_down_, 0, sizeof(key_down_));
  paste_key_down_ = false;
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  if (mouse_captured_ && attached_window_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    attached_window_->ReleaseMouse();
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = true;
}

}  // namespace rex::input::mnk
