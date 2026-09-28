#pragma once

#include "foundation/platform/NetworkMonitor.h"
#include "domain/mesh/host/LocalNetworkChange.h"

namespace pbr {

class MeshHost;
class CallStack;

/** Platform network change → mesh vocabulary (attachment = default-route interfaces / addresses). */
LocalNetworkChange ToLocalNetworkChange(const NetworkChange& change);

/**
 * k5: fan a device network change out (any thread): the mesh re-validates links and addresses,
 * calls re-anchor / restart their upgrade when the mesh moved. Either target may be null.
 */
void ReactToNetworkChange(const NetworkChange& change, MeshHost* mesh, CallStack* calls);

} // namespace pbr
