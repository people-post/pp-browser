#pragma once

#include "libp2p/integration/host/Reachability.h"

namespace pbr {

class IdentifyIntegrationService;
class Libp2pHost;

/**
 * Publish reachability-derived listen set via Identify (media-hop L2).
 * No-op when `publish_enabled` is false (e.g. Client role or media_relay off).
 */
void PublishAdvertisedListenSet(Libp2pHost& host, IdentifyIntegrationService& identify,
                                const ReachabilitySnapshot& snapshot,
                                const std::string& bound_listen_multiaddr,
                                const std::string& local_peer_id_base58, bool publish_enabled);

} // namespace pbr
