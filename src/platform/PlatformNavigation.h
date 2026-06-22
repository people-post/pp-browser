#pragma once

namespace Rml {
class Context;
}

namespace pbr {

class PlatformNavigation {
public:
  static bool OnDismissKey();
  static bool OnSystemBack();
};

} // namespace pbr
