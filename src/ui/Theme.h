#pragma once

#include <string>

namespace ppbrowser {

class Theme {
public:
  static bool LoadBase(const std::string& rcss_path);
};

} // namespace ppbrowser
