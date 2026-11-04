#pragma once

#include "common/Logger.h"
#include "common/NetworkChange.h"
#include "mesh/host/LocalNetworkChange.h"
#include "mesh/host/MeshHost.h"
#include "mesh/host/UdpSocketProbe.h"

#include <optional>
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"

namespace pbr {

/** Platform network change → mesh vocabulary (attachment = default-route interfaces / addresses). */
mesh::LocalNetworkChange ToLocalNetworkChange(const NetworkChange& change);

/**
 * k5: the mesh's side of a device network change (any thread). The baseline (generation 0) is only
 * logged and yields none; a change lets the mesh (when up) re-validate its links and addresses and
 * is returned for the participants.
 */
std::optional<mesh::LocalNetworkChange> ReactMeshToNetworkChange(const NetworkChange& change, mesh::MeshHost* mesh);

/**
 * #416: what the mesh found of its UDP socket (on a network change, or the app's return to the
 * foreground) — a Diag line, and the error to the developer log. The mesh's IO lane; does not block.
 */
void ReportUdpSocketCheck(logging::Logger& log, const mesh::UdpSocketCheck& check);

} // namespace pbr
