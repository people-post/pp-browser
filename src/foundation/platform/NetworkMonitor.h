#pragma once

#include "common/NetworkChange.h"
#include "foundation/platform/NetworkConnectivity.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace pbr {

namespace detail {
class INetworkMonitorBackend;
}

/**
 * OS network-change events. Backends: Linux netlink, Darwin `NWPathMonitor`, Windows IP helper
 * notifications, Android `ConnectivityManager` default-network callback.
 *
 * The first state a backend reports reaches the listener as the baseline (generation 0); afterwards
 * only material changes — `NetworkState` differs from the last one reported. The listener runs
 * on a backend thread: post to the owner before touching state, and never call `Stop` from it.
 */
class NetworkMonitor {
public:
  using Listener = std::function<void(const NetworkChange&)>;

  /** `with_os_backend` false: no OS source — tests feed states through `OnState`. */
  explicit NetworkMonitor(bool with_os_backend = true);
  ~NetworkMonitor();
  NetworkMonitor(const NetworkMonitor&) = delete;
  NetworkMonitor& operator=(const NetworkMonitor&) = delete;

  /** Starts the OS source. False when the platform has none (events never come). */
  bool Start(Listener listener);
  /** No listener call is running or will run once this returns. Idempotent. */
  void Stop();

  NetworkState Current() const;
  uint64_t Generation() const;

  /** A fresh state from the backend (or a test). Reports it when it is a material change. */
  void OnState(const NetworkState& state);

private:
  std::unique_ptr<detail::INetworkMonitorBackend> backend_;
  /** Serializes `OnState` (listener included), so `Stop` can wait for a call in flight. */
  std::mutex listener_mu_;
  Listener listener_;
  mutable std::mutex state_mu_;
  bool running_ = false;
  bool have_baseline_ = false;
  NetworkState state_;
  uint64_t generation_ = 0;
};

} // namespace pbr
