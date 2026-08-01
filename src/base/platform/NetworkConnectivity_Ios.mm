#include "base/platform/NetworkConnectivity.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE

#include <ifaddrs.h>
#include <net/if.h>
#include <cstring>

namespace pbr {

NetworkTransport QueryIosNetworkTransport() {
  struct ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    return NetworkTransport::Unknown;
  }

  bool wifi_up = false;
  bool cellular_up = false;

  for (struct ifaddrs* ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || (ifa->ifa_flags & IFF_UP) == 0) {
      continue;
    }
    const sa_family_t family = ifa->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) {
      continue;
    }
    const char* name = ifa->ifa_name;
    if (name == nullptr) {
      continue;
    }
    if (std::strncmp(name, "en", 2) == 0) {
      wifi_up = true;
    } else if (std::strncmp(name, "pdp_ip", 6) == 0) {
      cellular_up = true;
    }
  }

  freeifaddrs(interfaces);

  if (wifi_up) {
    return NetworkTransport::Wifi;
  }
  if (cellular_up) {
    return NetworkTransport::Cellular;
  }
  return NetworkTransport::Unknown;
}

} // namespace pbr

#endif
#endif
