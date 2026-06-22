#pragma once

#include <string>

namespace pbr {

class AssetIO {
public:
  static bool ReadText(const std::string& path, std::string& out);
  static bool Exists(const std::string& path);
};

} // namespace pbr
