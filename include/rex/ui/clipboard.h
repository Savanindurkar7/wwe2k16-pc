#pragma once

#include <optional>
#include <string>

namespace rex::ui {

// Returns clipboard text as UTF-16, or nullopt when text cannot be retrieved.
std::optional<std::u16string> GetClipboardText();

}  // namespace rex::ui
