#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/** A stored peer signing key (base64) by identity kind and value; none when unknown. */
using StoredSigningKeyLookup =
    std::function<std::optional<std::string>(const std::string& identity_kind, const std::string& identity_value)>;

/**
 * Resolve tip.peer_id → device ML-DSA-65 public key for Spine B verify.
 *
 * Order: local device pk when tip_peer_id matches local_peer_id; else the stored key under kind
 * "peer_id", accepted only when it derives that peer id. Never asks for account-kind keys.
 */
std::optional<std::vector<uint8_t>> ResolvePeerAnnouncePublisherKey(
    std::string_view tip_peer_id, std::string_view local_peer_id,
    const std::vector<uint8_t>& local_device_public_key, const StoredSigningKeyLookup& stored_signing_key);

} // namespace pbr
