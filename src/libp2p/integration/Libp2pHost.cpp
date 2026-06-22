#include "libp2p/integration/Libp2pHost.h"

namespace pbr {

Libp2pHost::Libp2pHost() {
  // p2p is linked into the app target on all platforms.
  available_ = true;
}

Libp2pHost::~Libp2pHost() = default;

} // namespace pbr
