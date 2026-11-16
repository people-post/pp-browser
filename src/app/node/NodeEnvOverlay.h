#pragma once

#include "foundation/data/Config.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/** Parse ops bool envs: true/1/yes/on vs false/0/no/off (case-insensitive). */
std::optional<bool> ParsePpNodeBoolEnv(std::string_view value);

/** Split comma-separated bootstrap multiaddrs; empty tokens dropped. */
std::vector<std::string> ParsePpNodeBootstrapPeersCsv(std::string_view csv);

/**
 * Apply PP_NODE_* deploy overlays onto loaded config (file/defaults).
 * Full precedence: CLI → env → config file → defaults.
 *
 * Env keys:
 *   PP_NODE_DATA_DIR
 *   PP_NODE_AMP_UDP_PORT
 *   PP_NODE_BOOTSTRAP_PEERS   (comma-separated multiaddrs)
 *   PP_NODE_CAP_CIRCUIT_RELAY
 *   PP_NODE_CAP_MEDIA_RELAY
 *   PP_NODE_CAP_DHT
 *   PP_NODE_CAPACITY_LINKS / _RESERVATIONS / _BRIDGES / _GROUP_CALLS   (mesh.capacity; a positive count)
 *   PP_NODE_ADVERTISE_MULTIADDRS
 *   PP_NODE_MESH_PUBLISH
 *   PP_NODE_REGISTRATION_BASE_URL  (clears registration.peer_id: another relay's)
 *   PP_NODE_REGISTRATION_PEER_ID   (registration.peer_id: the PeerId that relay answers to, multi-relay MR008)
 *   PP_NODE_RELAY_CONTENT_SHA256   (relay.content_sha256: send x-amz-content-sha256 on relay POST/PUT)
 *   (PP_NODE_IDENTITY_SEED is applied in NodeBootstrap, not here)
 * NodeBootstrap then defaults the registration relay's PeerId (DefaultNodeRegistrationPeerId).
 */
void ApplyPpNodeConfigEnvOverlays(AppConfig& config);

/**
 * The relay the node registers at is expected to answer to `registration.peer_id` (MR008); empty while it is the
 * compiled prod registration URL (`compiled_prod_relay`, the compiled origin + /api/relay): the compiled prod relay's
 * PeerId. Another URL without one keeps none (the node pins that relay's first answer, with a warning).
 */
void DefaultNodeRegistrationPeerId(ServiceEndpointConfig& registration, const std::string& compiled_prod_relay,
                                   const std::string& compiled_prod_peer_id);

} // namespace pbr
