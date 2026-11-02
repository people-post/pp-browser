#pragma once

#include <cstdint>
#include <string>

namespace pbr {

// The device's network as values (projects/capability-repos CR015, CR018): the OS monitor that reports it is the
// app's (foundation/platform NetworkMonitor); the mesh and the calls stack react to it.

/** The active data link (mobile gating, N025); desktop builds report Wifi. */
enum class NetworkTransport {
  Unknown,
  Wifi,
  Cellular,
  Other,
};

/** What the device's network attachment looks like (call-path-resilience k5 / M7). */
struct NetworkState {
  /** A usable default route exists. */
  bool online = false;
  /** Transport of the default route; Other = wired / VPN / unknown kind. */
  NetworkTransport transport = NetworkTransport::Unknown;
  /** Metered or expensive (cellular, hotspot, Windows variable cost). */
  bool expensive = false;
  /**
   * The backend's identity of the attachment: default-route interfaces and their addresses (plus the
   * OS network handle where there is one). A different value means our addresses or route changed.
   */
  std::string fingerprint;

  bool operator==(const NetworkState&) const = default;
};

/**
 * A reported network state. `generation` 0 is the baseline (the state at start: `previous` is
 * empty — not a change); material changes count from 1.
 */
struct NetworkChange {
  uint64_t generation = 0;
  NetworkState previous;
  NetworkState current;
};

} // namespace pbr
