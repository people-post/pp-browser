#pragma once

#include <RmlUi/Core/FileInterface.h>

namespace pbr {

class PlatformServices {
public:
  static void Register();

  /// Non-null on Android/iOS; desktop uses RmlUi default FileInterface.
  static Rml::FileInterface* PackagedFileInterface();
};

} // namespace pbr
