#include "base/net/PeerProtocolCompat.h"

namespace pbr {

PeerProtocolCompat EvaluatePeerProtocolCompat(const DirectoryHit& peer, int local_protocol_gen,
                                              int local_min_peer_protocol_gen) {
  const int peer_gen = peer.protocol_gen.value_or(1);
  const int peer_min = peer.min_peer_protocol_gen.value_or(1);
  if (local_protocol_gen < peer_min) {
    return PeerProtocolCompat::LocalTooOld;
  }
  if (peer_gen < local_min_peer_protocol_gen) {
    return PeerProtocolCompat::PeerTooOld;
  }
  return PeerProtocolCompat::Compatible;
}

} // namespace pbr
