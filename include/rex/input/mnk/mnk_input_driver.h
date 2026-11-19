/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window_listener.h>

#include <cstdint>
#include <chrono>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnKeyChar(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

 private:
  uint32_t UserIndex() const;
  bool IsEnabled() const;
  void CenterCursor();
  void UpdateMouseCapture();
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t virtual_key, bool down, uint16_t unicode = 0,
                        bool repeat = false);
  void EnqueueTextCharacter(char16_t character);
  void EnqueueTextButton(uint16_t button);

  struct TextEntryInput {
    enum class Type { kCharacter, kButton } type;
    char16_t character = 0;
    uint16_t button = 0;
  };

  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};
  bool paste_key_down_ = false;

  // Mouse delta tracking
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  bool mouse_enabled_ = false;
  bool has_focus_ = true;

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;
  // Independent queue for the game's pad-driven on-screen keyboard.
  std::queue<TextEntryInput> text_entry_queue_;
  uint16_t text_entry_held_button_ = 0;
  std::chrono::steady_clock::time_point text_entry_phase_end_{};

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
