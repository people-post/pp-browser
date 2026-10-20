#pragma once

#include "foundation/platform/NetworkMonitor.h"
#include "mesh/host/LocalNetworkChange.h"
#include "mesh/host/MeshHost.h"

#include <optional>
#include "common/MeshAlias.h"

namespace pbr {

/** Platform network change → mesh vocabulary (attachment = default-route interfaces / addresses). */
mesh::LocalNetworkChange ToLocalNetworkChange(const NetworkChange& change);

/**
 * k5: the mesh's side of a device network change (any thread). The baseline (generation 0) is only
 * logged and yields none; a change lets the mesh (when up) re-validate its links and addresses and
 * is returned for the participants.
 */
std::optional<mesh::LocalNetworkChange> ReactMeshToNetworkChange(const NetworkChange& change, mesh::MeshHost* mesh);

} // namespace pbr
