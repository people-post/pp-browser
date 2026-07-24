#include "base/platform/PlatformStartupHints.h"

#include "base/platform/Platform.h"

namespace pbr {

std::string_view InitFailureHint() {
  switch (Platform::Detect()) {
  case PlatformKind::Android:
    return " Check logcat for SDL/OpenGL errors.";
  case PlatformKind::IOS:
    return " Check Console / Application Support/.../pp-browser-debug.log for SDL/OpenGL errors.";
  case PlatformKind::Desktop:
  default:
    return " If no window appears, reconfigure from a clean build: "
           "rm -rf build && cmake -B build -S . && cmake --build build. "
           "Ensure DISPLAY is set. On Linux install: libx11-dev and libgl-dev (see docs/ops/BUILD.md).";
  }
}

} // namespace pbr
