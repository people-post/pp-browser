#pragma once

#include <ui/base/FileInterface.h>

namespace pbr {

class PlatformHooks {
public:
  static void Register();

  /// Non-null on Android/iOS; desktop uses RmlUi default FileInterface.
  static ui::FileInterface* PackagedFileInterface();
};

} // namespace pbr
