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
 *   PP_NODE_REGISTRATION_BASE_URL  (clears registration.relay_id: another relay's)
 *   PP_NODE_REGISTRATION_RELAY_ID  (registration.relay_id: the relay id that relay proves, `relay:…`, MR008)
 *   PP_NODE_RELAY_CONTENT_SHA256   (relay.content_sha256: send x-amz-content-sha256 on relay POST/PUT)
 *   (PP_NODE_IDENTITY_SEED is applied in NodeBootstrap, not here)
 * NodeBootstrap then defaults the registration relay's id (DefaultNodeRegistrationRelayId).
 */
void ApplyPpNodeConfigEnvOverlays(AppConfig& config);

/**
 * The relay the node registers at is expected to prove `registration.relay_id` (MR008, SIGNED_FORMATS § 3.3); empty
 * while it is the compiled prod registration URL (`compiled_prod_relay`, the compiled origin + /api/relay): the
 * compiled prod relay's id (`pending` while its operator set is not made: the node does not register). Another URL
 * without one keeps none (a developer build pins that relay's first valid certificate, with a warning).
 */
void DefaultNodeRegistrationRelayId(ServiceEndpointConfig& registration, const std::string& compiled_prod_relay,
                                    const std::string& compiled_prod_relay_id);

/** A relay base URL as the node compares it: trimmed, no trailing `/`, scheme and host lower case, no default port. */
std::string NodeRelayUrlKey(const std::string& url);

/**
 * Why the node may not register at `registration` (MR008 item 5): a registration URL whose `relay_id` (after
 * DefaultNodeRegistrationRelayId) is the build's placeholder or not a relay id (any build), or is empty outside a
 * developer build — the message names the setting; "" when it may (or there is no registration URL). A developer
 * build pins that relay on its first valid certificate instead, warned.
 */
std::string NodeRegistrationIdProblem(const ServiceEndpointConfig& registration, bool developer_build);

} // namespace pbr
