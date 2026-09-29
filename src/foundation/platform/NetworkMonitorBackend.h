#pragma once

// Private to foundation/platform: the per-OS source behind NetworkMonitor.

#include "foundation/platform/NetworkMonitor.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pbr::detail {

class INetworkMonitorBackend {
public:
  using StateSink = std::function<void(const NetworkState&)>;
  virtual ~INetworkMonitorBackend() = default;
  /** Report the current state soon after starting, then whenever it may have changed. */
  virtual bool Start(StateSink sink) = 0;
  /** No `sink` call is running or will run once this returns. */
  virtual void Stop() = 0;
};

/** The platform's backend (NetworkMonitor_<Os>.cpp); null when the platform has none. */
std::unique_ptr<INetworkMonitorBackend> CreateNetworkMonitorBackend();

/** Interfaces of IPv4 default routes in `/proc/net/route` text (Linux). */
std::vector<std::string> ParseProcNetRouteDefaultIfaces(std::string_view text);
/** Interfaces of IPv6 default routes in `/proc/net/ipv6_route` text (Linux); `lo` skipped. */
std::vector<std::string> ParseProcNetIpv6RouteDefaultIfaces(std::string_view text);

/** Sorted, de-duplicated `a,b,c` — stable fingerprints whatever order the OS lists things in. */
std::string JoinSorted(std::vector<std::string> parts);

} // namespace pbr::detail
