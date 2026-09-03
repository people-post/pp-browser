#include "foundation/platform/PlatformUserHints.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pbr {
namespace PlatformUserHints {

std::string_view P2pNetworkHintKey() {
#if defined(__ANDROID__)
  return "hints.network.android_permissions";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  return "hints.network.local_ios";
#elif defined(__APPLE__)
  return "hints.network.local_macos";
#elif defined(_WIN32)
  return "hints.network.firewall_windows";
#else
  return "hints.network.lan_linux";
#endif
}

std::string_view MicBlockedHintKey() {
  return "hints.mic_blocked";
}

} // namespace PlatformUserHints
} // namespace pbr
