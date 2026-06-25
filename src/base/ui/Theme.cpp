#include "base/ui/Theme.h"

#include "base/platform/AssetIO.h"

namespace pbr {

bool Theme::LoadBase(const std::string& rcss_path) {
  return AssetIO::Exists(rcss_path);
}

} // namespace pbr
