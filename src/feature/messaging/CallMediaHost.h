#pragma once

#include "base/messaging/CallControlCodec.h"

#include "common/Error.h"

#include <optional>
#include <string>

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
  /**
   * Map inbound call-media libp2p PeerId → call-roster `relay:` identity.
   * Do not use P2pPeerIdentityForCall for this — that returns an arbitrary remote and
   * mis-keys PreferLocal 1:1 audio onto another peer's SFU stream_id.
   */
  virtual Roe<std::optional<std::string>> P2pRelayIdentityForLibp2pPeerId(
      const std::string& call_id, const std::string& peer_id) const = 0;
  virtual bool P2pIsAwaitingSfuRecovery() const = 0;
  /**
   * True when 1:1 call-media close is expected during SoftMigrate (N≥3, sfu_hint, or attach-wait).
   * Guests must not flip ConnectFailed before AttachLocalToSfu completes.
   */
  virtual bool P2pExpectGroupSfuMigration(const std::string& call_id) const = 0;
  /** Arm attach-wait chrome / ignore window when 1:1 drops ahead of CallSfuAttach. */
  virtual void P2pNoteExpectSfuAttach(const std::string& call_id) = 0;
  /** True after AttachLocalToSfu / media_relay SoftMigrate — not 1:1 libp2p SFU-mode capture. */
  virtual bool P2pIsSfuAttached() const = 0;
  virtual void P2pClearAwaitingSfuRecovery() = 0;
  /** Offerer Connect retries — resend epoch media key (answerer often Defers waiting on relay). */
  virtual void P2pResendMediaKey(const std::string& call_id, const std::string& peer_identity) = 0;
  /** Answerer MediaPending — force relay inbox poll for CallMediaKey. */
  virtual void P2pRequestInboxSync() = 0;
};

} // namespace pbr
