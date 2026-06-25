#include "platform/AndroidAssetLocator.h"

namespace pbr {

std::string AndroidAssetLocator::Resolve(const std::string& relative) const {
  // Gradle packs repo assets/ at the APK asset root; SDL resolves these via AAssetManager.
  return relative;
}

} // namespace pbr
