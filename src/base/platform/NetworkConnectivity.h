#pragma once

namespace pbr {

/** Active data link for mobile gating (N025). Desktop builds return Wifi. */
enum class NetworkTransport {
  Unknown,
  Wifi,
  Cellular,
  Other,
};

/** Best-effort active transport; Unknown when not mobile or unavailable. */
NetworkTransport ActiveNetworkTransport();

/** True when ActiveNetworkTransport() == Wifi (or desktop — not used for ephemeral listen). */
bool IsOnWifi();

} // namespace pbr
