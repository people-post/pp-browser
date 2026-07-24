#pragma once

#include "base/people/ContactTypes.h"

namespace pbr {

enum class PeerProtocolCompat {
  Compatible = 0,
  /** Local protocol_gen is below peer's min_peer_protocol_gen — we must update. */
  LocalTooOld,
  /** Peer's protocol_gen is below our kMinPeerProtocolGen — peer should update. */
  PeerTooOld,
};

/** Absent capability fields are treated as protocol_gen / min_peer_protocol_gen = 1. */
PeerProtocolCompat EvaluatePeerProtocolCompat(const DirectoryHit& peer, int local_protocol_gen,
                                              int local_min_peer_protocol_gen);

} // namespace pbr
