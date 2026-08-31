#include "base/mesh/link/MeshPump.h"

namespace pbr::amp {

MeshPump::MeshPump(adp::Endpoint& endpoint, PeerLinkManager& links) : endpoint_(endpoint), links_(links) {}

void MeshPump::Pump() {
  endpoint_.Pump();
  links_.Tick();
}

void MeshPump::Tick() {
  endpoint_.Tick();
  links_.Tick();
}

} // namespace pbr::amp
