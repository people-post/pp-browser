#include "foundation/platform/ScreenKeyboard.h"

namespace pbr {

bool WatchScreenKeyboard(std::function<void()> /*on_change*/) {
  return false;
}

int ScreenKeyboardHeight() {
  return 0;
}

} // namespace pbr
