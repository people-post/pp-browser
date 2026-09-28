// macOS / iOS: Network.framework NWPathMonitor. The path gives status, interface kinds and cost;
// the fingerprint adds the path's interfaces' addresses (getifaddrs).

#include "foundation/platform/NetworkMonitorBackend.h"

#import <Network/Network.h>
#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <string>
#include <vector>

namespace pbr::detail {
namespace {

std::string AddressText(const sockaddr* addr) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (addr->sa_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(addr);
    if ((ntohl(v4->sin_addr.s_addr) >> 16) == 0xa9fe) {  // 169.254/16 link-local
      return {};
    }
    inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof(buf));
  } else if (addr->sa_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(addr);
    if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr)) {
      return {};
    }
    inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof(buf));
  }
  return buf;
}

NetworkState Snapshot(nw_path_t path) {
  NetworkState state;
  state.online = nw_path_get_status(path) == nw_path_status_satisfied;
  if (!state.online) {
    return state;
  }
  if (nw_path_uses_interface_type(path, nw_interface_type_wifi)) {
    state.transport = NetworkTransport::Wifi;
  } else if (nw_path_uses_interface_type(path, nw_interface_type_cellular)) {
    state.transport = NetworkTransport::Cellular;
  } else {
    state.transport = NetworkTransport::Other;
  }
  state.expensive = nw_path_is_expensive(path);
  if (@available(macOS 10.15, iOS 13.0, *)) {
    state.expensive = state.expensive || nw_path_is_constrained(path);  // Low Data Mode
  }

  __block std::vector<std::string> names;
  nw_path_enumerate_interfaces(path, ^bool(nw_interface_t iface) {
    if (const char* name = nw_interface_get_name(iface)) {
      names.emplace_back(name);
    }
    return true;
  });
  std::vector<std::string> parts;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (const ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
      if (it->ifa_addr == nullptr || it->ifa_name == nullptr || (it->ifa_flags & IFF_UP) == 0) {
        continue;
      }
      const std::string name = it->ifa_name;
      if (std::find(names.begin(), names.end(), name) == names.end()) {
        continue;
      }
      const std::string addr = AddressText(it->ifa_addr);
      if (!addr.empty()) {
        parts.push_back(name + "/" + addr);
      }
    }
    freeifaddrs(list);
  }
  if (parts.empty()) {
    parts = names;  // addresses unknown: interface identity only
  }
  state.fingerprint = JoinSorted(std::move(parts));
  return state;
}

class DarwinNetworkMonitorBackend final : public INetworkMonitorBackend {
public:
  ~DarwinNetworkMonitorBackend() override { Stop(); }

  bool Start(StateSink sink) override {
    if (monitor_ != nullptr) {
      return true;
    }
    queue_ = dispatch_queue_create("pp.network-monitor", DISPATCH_QUEUE_SERIAL);
    monitor_ = nw_path_monitor_create();
    if (queue_ == nullptr || monitor_ == nullptr) {
      Release();
      return false;
    }
    nw_path_monitor_set_queue(monitor_, queue_);
    StateSink on_state = std::move(sink);
    nw_path_monitor_set_update_handler(monitor_, ^(nw_path_t path) {
      on_state(Snapshot(path));
    });
    nw_path_monitor_start(monitor_);
    return true;
  }

  void Stop() override {
    if (monitor_ == nullptr) {
      return;
    }
    nw_path_monitor_cancel(monitor_);
    // Updates run on queue_: once an empty block has run there, none is running or pending.
    dispatch_sync(queue_, ^{
    });
    Release();
  }

private:
  void Release() {
#if !__has_feature(objc_arc)
    if (monitor_ != nullptr) {
      nw_release(monitor_);
    }
    if (queue_ != nullptr) {
      dispatch_release(queue_);
    }
#endif
    monitor_ = nullptr;
    queue_ = nullptr;
  }

  dispatch_queue_t queue_ = nullptr;
  nw_path_monitor_t monitor_ = nullptr;
};

} // namespace

std::unique_ptr<INetworkMonitorBackend> CreateNetworkMonitorBackend() {
  return std::make_unique<DarwinNetworkMonitorBackend>();
}

} // namespace pbr::detail
