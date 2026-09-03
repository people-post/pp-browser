#include "foundation/platform/NetworkConnectivity.h"

#include "foundation/platform/Platform.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pbr {

#if defined(__ANDROID__)
NetworkTransport QueryAndroidNetworkTransport();
#elif defined(__APPLE__) && TARGET_OS_IPHONE
NetworkTransport QueryIosNetworkTransport();
#endif

NetworkTransport ActiveNetworkTransport() {
#if defined(__ANDROID__)
  return QueryAndroidNetworkTransport();
#elif defined(__APPLE__) && TARGET_OS_IPHONE
  return QueryIosNetworkTransport();
#else
  if (Platform::IsDesktop()) {
    return NetworkTransport::Wifi;
  }
  return NetworkTransport::Unknown;
#endif
}

bool IsOnWifi() {
  return ActiveNetworkTransport() == NetworkTransport::Wifi;
}

} // namespace pbr
