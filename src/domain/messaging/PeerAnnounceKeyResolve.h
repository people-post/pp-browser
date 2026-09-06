#pragma once

#include "domain/messaging/PeerSigningKeyStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * Resolve tip.peer_id → device ML-DSA-65 public key for Spine B verify.
 *
 * Order: local device pk when tip_peer_id matches local_peer_id; else
 * PeerSigningKeyStore under kind "peer_id". Does not consult account-kind rows.
 */
std::optional<std::vector<uint8_t>> ResolvePeerAnnouncePublisherKey(
    std::string_view tip_peer_id, std::string_view local_peer_id,
    const std::vector<uint8_t>& local_device_public_key, const PeerSigningKeyStore& signing_key_store);

} // namespace pbr
