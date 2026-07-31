#pragma once

#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionStore.h"

#include "common/Error.h"
#include "common/Module.h"

#include <optional>
#include <string>
#include <unordered_set>

namespace pbr {

/** Narrow façade for P2P signaling side effects owned by CallSessionManager. */
class CallP2pSignalingHost {
public:
  virtual ~CallP2pSignalingHost() = default;
  virtual Roe<std::string> P2pLocalIdentity() const = 0;
  virtual Roe<void> P2pSendDirect(const std::string& peer_identity, CallControlType type,
                                  const std::string& detail_json, const std::string& display) = 0;
  virtual void P2pNotifyRingChanged() = 0;
  virtual void P2pSetLastMediaError(std::string message) = 0;
  virtual Roe<std::optional<std::string>> P2pPeerIdentityForCall(const std::string& call_id) const = 0;
  virtual bool P2pIsAwaitingSfuRecovery() const = 0;
  /** Group ICE fail — topology recovers via SFU (V025: never for N=2). */
  virtual void P2pOnGroupIceFailed(const std::string& call_id) = 0;
  virtual void P2pClearAwaitingSfuRecovery() = 0;
};

/**
 * 1:1 P2P offer/answer + SDP/ICE send + connect-fail / Retry (V014 + V025).
 * Does not decide SFU — topology owns soft-migrate / StartSfu.
 */
class CallP2pSignalingBridge : public Module {
public:
  CallP2pSignalingBridge(CallP2pSignalingHost& host, CallSessionStore& sessions, CallMediaEngine& media);

  bool IsP2pConnectFailed() const;
  bool P2pConnectMissingMic() const;
  void MarkP2pConnectFailed(const std::string& reason);
  void ClearP2pConnectFailed();
  void PollP2pConnectHealth();
  Roe<void> RetryP2pMedia(const std::string& call_id);

  Roe<void> StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  Roe<void> StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);

  void BindMediaCallbacks(const std::string& peer_identity);
  void ClearMediaCallbacks();
  void StopP2pMedia(const std::string& call_id);

  void NoteMediaAttempted(const std::string& call_id);
  bool MediaAttempted(const std::string& call_id) const;
  void BindMediaCallId(const std::string& call_id);
  void ClearMediaPeerIdentity();
  const std::string& MediaCallId() const;

  Roe<void> OnRemoteSdp(const CallSdpDetail& sdp, const std::string& sender_identity);
  Roe<void> OnRemoteIce(const CallIceDetail& ice);

private:
  CallP2pSignalingHost& host_;
  CallSessionStore& sessions_;
  CallMediaEngine& media_;
  std::string media_peer_identity_;
  std::string media_call_id_;
  bool p2p_connect_failed_ = false;
  bool p2p_connect_missing_mic_ = false;
  std::unordered_set<std::string> media_attempted_calls_;
};

} // namespace pbr
