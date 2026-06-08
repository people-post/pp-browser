#include "ui/Theme.h"

#include <filesystem>

namespace pbr {

bool Theme::LoadBase(const std::string& rcss_path) {
  return std::filesystem::exists(rcss_path);
}

} // namespace pbr
