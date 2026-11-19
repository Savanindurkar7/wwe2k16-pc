#pragma once

#include <cstdint>

namespace rex::input {

class TextEntryTarget {
 public:
  virtual ~TextEntryTarget() = default;

  virtual bool IsActive() = 0;
  virtual bool SelectChar(char16_t character) = 0;
};

}  // namespace rex::input
