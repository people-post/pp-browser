#pragma once

#include "domain/messaging/CallHopAttachLogic.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Broadcast frame AEAD context (media-client-layers L006 / L013): `broadcast-media|program|join`.
 * Pass to `SealMediaRelayFrame` / `OpenMediaRelayFrame`; never the call label.
 */
inline std::string BroadcastMediaFrameContext(const std::string& program_id, const std::string& join_handle) {
  return "broadcast-media|" + program_id + "|" + join_handle;
}

/**
 * The `media_relay` stream a broadcaster publishes on: derived from its mesh PeerId, which every
 * viewer learns from the signed tip (calls derive theirs from the Account id, which a tip lacks).
 */
inline uint32_t BroadcastPublisherStreamId(const std::string& publisher_peer_id) {
  return PublisherStreamIdForIdentity(publisher_peer_id);
}

/** A fresh 32-byte show key (B004: one stable key per show, new on every go-live). */
std::vector<uint8_t> NewBroadcastMediaKey();
/** A fresh opaque join handle for one show of `program_id` (`live:<program>:<random hex>`). */
std::string NewBroadcastJoinHandle(const std::string& program_id);

} // namespace pbr
