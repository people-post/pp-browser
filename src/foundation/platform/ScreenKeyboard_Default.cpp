#include "foundation/platform/ScreenKeyboard.h"

namespace pbr {

bool WatchScreenKeyboard(std::function<void()> /*on_change*/) {
  return false;
}

int ScreenKeyboardHeight() {
  return 0;
}

std::string TakeScreenKeyboardEvents() {
  return {};
}

std::string DescribeScreenKeyboard() {
  return {};
}

} // namespace pbr
