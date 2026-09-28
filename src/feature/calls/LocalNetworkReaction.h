#pragma once

#include "foundation/platform/NetworkMonitor.h"
#include "domain/mesh/host/LocalNetworkChange.h"
#include "domain/messaging/CallMobility.h"

namespace pbr {

class MeshHost;
class CallStack;

/** Platform network change → mesh vocabulary (attachment = default-route interfaces / addresses). */
LocalNetworkChange ToLocalNetworkChange(const NetworkChange& change);
/** Platform network state → mobility signals (k6). */
MobilityAttachment ToMobilityAttachment(const NetworkState& state);

/**
 * k5 / k6: fan a device network state out (any thread). The baseline (generation 0) only
 * classifies mobility; a change also lets the mesh re-validate links and addresses, and calls
 * re-anchor / restart their upgrade when the mesh moved. Either target may be null.
 */
void ReactToNetworkChange(const NetworkChange& change, MeshHost* mesh, CallStack* calls);

} // namespace pbr
