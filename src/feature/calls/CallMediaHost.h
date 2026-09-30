#pragma once

#include "domain/messaging/CallControlCodec.h"
#include "feature/calls/LiveCall.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Narrow façade for call media side effects owned by CallSessionManager. */
class CallMediaHost {
public:
  virtual ~CallMediaHost() = default;
  virtual Roe<std::string> P2pLocalIdentity() const = 0;
  virtual Roe<void> P2pSendDirect(const std::string& peer_identity, CallControlType type,
                                  const std::string& detail_json, const std::string& display) = 0;
  virtual void P2pNotifyRingChanged() = 0;
  virtual void P2pSetLastMediaError(std::string message) = 0;
  virtual Roe<std::optional<std::string>> P2pPeerIdentityForCall(const std::string& call_id) const = 0;
  /** The call as it lives on this device (peers, who placed it, open or ended); null if unknown. */
  virtual const LiveCall* P2pLiveCall(const std::string& call_id) const = 0;
  /** The call's media coordinator (engine + seat use); null for a call not admitted here. */
  virtual CallMediaCoordinator* P2pCallMedia(const std::string& call_id) = 0;
  /**
   * Map inbound call-media mesh PeerId → call-roster `relay:` identity.
   * Do not use P2pPeerIdentityForCall for this — that returns an arbitrary remote and
   * mis-keys PreferLocal 1:1 audio onto another peer's SFU stream_id.
   */
  virtual Roe<std::optional<std::string>> RelayIdentityForMeshPeerId(
      const std::string& call_id, const std::string& peer_id) const = 0;
  /**
   * Map call-roster account: → mesh PeerId for dial/Ensure (Invite/Accept libp2p_peer_id /
   * contact PeerId). Empty if unknown — dial may still use account: alias when registered.
   */
  virtual Roe<std::optional<std::string>> MeshPeerIdForAccount(const std::string& account) const = 0;
  /** Offerer Connect retries — resend epoch media key (answerer often Defers waiting on relay). */
  virtual void P2pResendMediaKey(const std::string& call_id, const std::string& peer_identity) = 0;
  /** Answerer MediaPending — force relay inbox poll for CallMediaKey. */
  virtual void P2pRequestInboxSync() = 0;
  /**
   * An inbound call-media hello for `call_id` was accepted from `identity` (roster account) /
   * `peer_id` (mesh). B30: may stand in for a CallAccept the relay has not delivered yet.
   */
  virtual void P2pNoteInboundHello(const std::string& /*call_id*/, const std::string& /*identity*/,
                                   const std::string& /*peer_id*/) {}
  /** The group hop carries the active call's media (1:1 frames are dropped). Any thread. */
  virtual bool HopCarriesMedia() const { return false; }
};

} // namespace pbr
