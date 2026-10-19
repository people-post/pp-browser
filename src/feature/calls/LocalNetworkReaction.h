#pragma once

#include "foundation/platform/NetworkMonitor.h"
#include "mesh/host/LocalNetworkChange.h"
#include "domain/messaging/CallMobility.h"
#include "mesh/host/MeshHost.h"
#include "common/MeshAlias.h"

namespace pbr {

class CallStack;

/** Platform network change → mesh vocabulary (attachment = default-route interfaces / addresses). */
mesh::LocalNetworkChange ToLocalNetworkChange(const NetworkChange& change);
/** Platform network state → mobility signals (k6). */
MobilityAttachment ToMobilityAttachment(const NetworkState& state);

/**
 * k5 / k6: fan a device network state out (any thread). The baseline (generation 0) only
 * classifies mobility; a change also lets the mesh re-validate links and addresses, and calls
 * re-anchor / restart their upgrade when the mesh moved. Either target may be null.
 */
void ReactToNetworkChange(const NetworkChange& change, mesh::MeshHost* mesh, CallStack* calls);

} // namespace pbr
