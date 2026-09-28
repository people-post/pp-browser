// Windows: IP helper change notifications (interface, unicast address, route) wake a snapshot of
// the adapters that carry a default gateway; cost from GetNetworkConnectivityHint (Windows 10 2004+,
// loaded at run time).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "foundation/platform/NetworkMonitorBackend.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pbr::detail {
namespace {

constexpr ULONG kIfTypeWifi = 71;       // IF_TYPE_IEEE80211
constexpr ULONG kIfTypeWwan = 243;      // IF_TYPE_WWANPP
constexpr ULONG kIfTypeWwan2 = 244;     // IF_TYPE_WWANPP2

/** Mirror of NL_NETWORK_CONNECTIVITY_HINT (netioapi.h, SDK 10.0.19041+). */
struct ConnectivityHint {
  int level;
  int cost;  // 0 unknown, 1 unrestricted, 2 fixed, 3 variable
  BOOLEAN approaching_data_limit;
  BOOLEAN over_data_limit;
  BOOLEAN roaming;
};
using ConnectivityHintFn = LONG(WINAPI*)(ConnectivityHint*);

bool IsExpensive() {
  static const auto fn = []() -> ConnectivityHintFn {
    HMODULE module = GetModuleHandleW(L"iphlpapi.dll");
    return module ? reinterpret_cast<ConnectivityHintFn>(
                        reinterpret_cast<void*>(GetProcAddress(module, "GetNetworkConnectivityHint")))
                  : nullptr;
  }();
  ConnectivityHint hint{};
  if (fn == nullptr || fn(&hint) != 0) {
    return false;
  }
  return hint.cost >= 2 || hint.approaching_data_limit || hint.over_data_limit || hint.roaming;
}

std::string AddressText(const SOCKADDR* addr) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (addr->sa_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(addr);
    if ((ntohl(v4->sin_addr.s_addr) >> 16) == 0xa9fe) {  // 169.254/16 link-local
      return {};
    }
    InetNtopA(AF_INET, &v4->sin_addr, buf, sizeof(buf));
  } else if (addr->sa_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(addr);
    if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr)) {
      return {};
    }
    InetNtopA(AF_INET6, &v6->sin6_addr, buf, sizeof(buf));
  }
  return buf;
}

NetworkState Snapshot() {
  NetworkState state;
  constexpr ULONG kFlags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                           GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buffer;
  ULONG rc = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buffer.resize(size);
    rc = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                              &size);
  }
  if (rc != NO_ERROR) {
    return state;
  }
  std::vector<std::string> parts;
  ULONG best_metric = ~0UL;
  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
       adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        adapter->FirstGatewayAddress == nullptr) {
      continue;  // only adapters that carry a default route
    }
    bool any = false;
    for (auto* uni = adapter->FirstUnicastAddress; uni != nullptr; uni = uni->Next) {
      if (uni->Address.lpSockaddr == nullptr) {
        continue;
      }
      const std::string addr = AddressText(uni->Address.lpSockaddr);
      if (!addr.empty()) {
        parts.push_back(std::string(adapter->AdapterName ? adapter->AdapterName : "?") + "/" + addr);
        any = true;
      }
    }
    if (!any) {
      continue;
    }
    const ULONG metric = adapter->Ipv4Metric != 0 ? adapter->Ipv4Metric : adapter->Ipv6Metric;
    if (metric < best_metric) {
      best_metric = metric;
      state.transport = adapter->IfType == kIfTypeWifi                                 ? NetworkTransport::Wifi
                        : adapter->IfType == kIfTypeWwan || adapter->IfType == kIfTypeWwan2 ? NetworkTransport::Cellular
                                                                                        : NetworkTransport::Other;
    }
  }
  state.online = !parts.empty();
  if (!state.online) {
    state.transport = NetworkTransport::Unknown;
  }
  state.expensive = state.online && (IsExpensive() || state.transport == NetworkTransport::Cellular);
  state.fingerprint = JoinSorted(std::move(parts));
  return state;
}

class WinNetworkMonitorBackend final : public INetworkMonitorBackend {
public:
  ~WinNetworkMonitorBackend() override { Stop(); }

  bool Start(StateSink sink) override {
    if (started_) {
      return true;
    }
    sink_ = std::move(sink);
    started_ = true;
    // Registrations before the first snapshot, so no change falls between them.
    (void)NotifyIpInterfaceChange(AF_UNSPEC, &OnInterface, this, FALSE, &interface_handle_);
    (void)NotifyUnicastIpAddressChange(AF_UNSPEC, &OnAddress, this, FALSE, &address_handle_);
    (void)NotifyRouteChange2(AF_UNSPEC, &OnRoute, this, FALSE, &route_handle_);
    Emit();
    return true;
  }

  void Stop() override {
    if (!started_) {
      return;
    }
    // CancelMibChangeNotify2 returns once no callback of that registration is running.
    for (HANDLE* handle : {&interface_handle_, &address_handle_, &route_handle_}) {
      if (*handle != nullptr) {
        CancelMibChangeNotify2(*handle);
        *handle = nullptr;
      }
    }
    started_ = false;
  }

private:
  static VOID NETIOAPI_API_ OnInterface(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
    static_cast<WinNetworkMonitorBackend*>(context)->Emit();
  }
  static VOID NETIOAPI_API_ OnAddress(PVOID context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
    static_cast<WinNetworkMonitorBackend*>(context)->Emit();
  }
  static VOID NETIOAPI_API_ OnRoute(PVOID context, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE) {
    static_cast<WinNetworkMonitorBackend*>(context)->Emit();
  }

  void Emit() {
    // The three registrations call back on system threads, possibly at once.
    std::lock_guard lock(emit_mu_);
    sink_(Snapshot());
  }

  StateSink sink_;
  std::mutex emit_mu_;
  bool started_ = false;
  HANDLE interface_handle_ = nullptr;
  HANDLE address_handle_ = nullptr;
  HANDLE route_handle_ = nullptr;
};

} // namespace

std::unique_ptr<INetworkMonitorBackend> CreateNetworkMonitorBackend() {
  return std::make_unique<WinNetworkMonitorBackend>();
}

} // namespace pbr::detail
