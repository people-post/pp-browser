#pragma once

#include "base/mesh/link/Types.h"

#include "common/PbrCompat.h"

namespace pbr::test {

/** Derives libp2p-style base58 PeerIds for AMP mesh tests (requires pp_base_p2p). */
amp::PeerLinkConfig AmpMeshTestLinkConfig();

} // namespace pbr::test
