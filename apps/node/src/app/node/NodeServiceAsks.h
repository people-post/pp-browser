#pragma once

#include "app/node/NodeConfig.h"
#include "common/Error.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * For every known service but the ledger (its ask is --ledger-role): `PP_NODE_<SERVICE>_ENABLED` overrides
 * `node.<service>.enabled` (1/0, true/false, yes/no, on/off; anything else leaves it). projects/node-relay NR001.
 */
void ApplyPpNodeServiceEnvOverlays(NodeConfig& config, const std::vector<std::string>& known);

/**
 * NR001's rule 4. A service this build lacks (`known` but not `built`) that the config asks for (`node.<s>.enabled`
 * true) fails the start, so an operator never believes a service runs that does not; one with a section but no ask is
 * only ignored. Returns the ignored ones (for one log line each).
 */
Roe<std::vector<std::string>> CheckPpNodeServiceAsks(const NodeConfig& config, const std::vector<std::string>& built,
                                                     const std::vector<std::string>& known);

} // namespace pbr
