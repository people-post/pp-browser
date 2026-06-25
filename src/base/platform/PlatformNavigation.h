#pragma once

#include <functional>

namespace pbr {

class PlatformNavigation {
public:
  static void SetDismissHandler(std::function<bool()> handler);
  static bool OnDismissKey();
  static bool OnSystemBack();
};

} // namespace pbr
