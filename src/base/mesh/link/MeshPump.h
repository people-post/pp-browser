#pragma once

#include "lib/amp/L1/Endpoint.h"
#include "base/mesh/link/PeerLinkManager.h"

namespace pbr::amp {

/** Io-thread driver: ADP Endpoint pump/tick for PeerLinkManager links. */
class MeshPump {
public:
  MeshPump(adp::Endpoint& endpoint, PeerLinkManager& links);

  void Pump();
  void Tick();

private:
  adp::Endpoint& endpoint_;
  PeerLinkManager& links_;
};

} // namespace pbr::amp
