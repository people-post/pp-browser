#pragma once

#include "lib/calls/LiveCall.h"
#include "feature/calls/CallMedia.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What a call's 1:1 path asks the device (V057 2): queries only. What it tells the device — the
 * chrome changed, a media error, a key resend, an inbox sync, an inbound hello — goes up as
 * `DirectReport` events on the calls owner's queue. Implemented by CallDevice.
 */
class CallDirectPathQueries {
public:
  virtual ~CallDirectPathQueries() = default;
  /** The call as it lives on this device (peers, who placed it, open or ended); null if unknown. */
  virtual const LiveCall* LiveCallOf(const std::string& call_id) const = 0;
  /** The call's `CallMedia` (seat, the other path's state); null for a call not admitted here. */
  virtual CallMedia* CallMediaOf(const std::string& call_id) = 0;
  /** The call's first remote participant (a 1:1 call's peer). */
  virtual Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const = 0;
  /**
   * Map inbound call-media mesh PeerId → call-roster `relay:` identity.
   * Do not use PeerIdentityForCall for this — that returns an arbitrary remote and
   * mis-keys PreferLocal 1:1 audio onto another peer's SFU stream_id.
   */
  virtual Roe<std::optional<std::string>> RelayIdentityForMeshPeerId(const std::string& call_id,
                                                                     const std::string& peer_id) const = 0;
  /**
   * Map call-roster account: → mesh PeerId for dial/Ensure (Invite/Accept libp2p_peer_id /
   * contact PeerId). Empty if unknown — dial may still use account: alias when registered.
   */
  virtual Roe<std::optional<std::string>> MeshPeerIdForAccount(const std::string& account) const = 0;
};

} // namespace pbr
