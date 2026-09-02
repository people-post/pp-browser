#pragma once

#include "base/data/Config.h"

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
 *   PP_NODE_ADVERTISE_MULTIADDRS
 *   PP_NODE_MESH_PUBLISH
 *   PP_NODE_REGISTRATION_BASE_URL
 *   (PP_NODE_IDENTITY_SEED is applied in NodeBootstrap, not here)
 */
void ApplyPpNodeConfigEnvOverlays(AppConfig& config);

} // namespace pbr
