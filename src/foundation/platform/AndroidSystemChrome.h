#pragma once

#include <string>

namespace pbr {

/** Android system navigation bar theming via MainActivity JNI. */
class AndroidSystemChrome {
public:
  /** Push appearance preference: "system", "light", or "dark". */
  static void SetAppearance(const std::string& appearance);
};

} // namespace pbr
