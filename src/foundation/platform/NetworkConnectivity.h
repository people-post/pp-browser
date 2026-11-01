#pragma once

#include "common/NetworkChange.h"

namespace pbr {

/** Best-effort active transport; Unknown when not mobile or unavailable. */
NetworkTransport ActiveNetworkTransport();

/** True when ActiveNetworkTransport() == Wifi (or desktop — not used for ephemeral listen). */
bool IsOnWifi();

} // namespace pbr
