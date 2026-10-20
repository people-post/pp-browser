#pragma once

#include "foundation/platform/NetworkMonitor.h"
#include "mesh/host/LocalNetworkChange.h"
#include "domain/calls/CallMobility.h"
#include "common/MeshAlias.h"

namespace pbr {

class CallStack;

/** Platform network state → mobility signals (k6). */
MobilityAttachment ToMobilityAttachment(const NetworkState& state);

/**
 * k5 / k6: the calls' side of a device network change (any thread). `local` is what the mesh made
 * of it, null for the baseline (generation 0), which only classifies mobility; a change lets calls
 * re-anchor / restart their upgrade when the mesh moved.
 */
void ReactCallsToNetworkChange(const NetworkChange& change, const mesh::LocalNetworkChange* local, CallStack& calls);

} // namespace pbr
