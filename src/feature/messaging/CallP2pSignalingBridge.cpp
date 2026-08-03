#include "feature/messaging/CallP2pSignalingBridge.h"

#include "base/runtime/AppRuntime.h"
#include "common/Utilities.h"

namespace pbr {
namespace {

constexpr int64_t kP2pConnectTimeoutMs = 15000;

} // namespace

CallP2pSignalingBridge::CallP2pSignalingBridge(CallP2pSignalingHost& host, CallSessionStore& sessions,
                                               CallMediaEngine& media)
    : host_(host), sessions_(sessions), media_(media) {
  redirectLogger("CallP2pSignalingBridge");
}

bool CallP2pSignalingBridge::IsP2pConnectFailed() const {
  return p2p_connect_failed_;
}

bool CallP2pSignalingBridge::P2pConnectMissingMic() const {
  return p2p_connect_missing_mic_;
}

void CallP2pSignalingBridge::ClearP2pConnectFailed() {
  p2p_connect_failed_ = false;
  p2p_connect_missing_mic_ = false;
}

void CallP2pSignalingBridge::MarkP2pConnectFailed(const std::string& /*reason*/) {
  if (p2p_connect_failed_ || media_.IsSfuMode() || host_.P2pIsAwaitingSfuRecovery()) {
    return;
  }
  p2p_connect_failed_ = true;
  p2p_connect_missing_mic_ = media_.IsActive() && !media_.HasLocalCapture();
  media_.ArmOfferRestart();
  log().info << "P2P connect failed call_id=" << media_call_id_
             << " missing_mic=" << (p2p_connect_missing_mic_ ? "1" : "0");
}

void CallP2pSignalingBridge::PollP2pConnectHealth() {
  if (p2p_connect_failed_ || media_.IsSfuMode() || host_.P2pIsAwaitingSfuRecovery() || !media_.IsActive()) {
    return;
  }
  if (media_.IsConnected()) {
    ClearP2pConnectFailed();
    return;
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return;
  }
  auto joined = sessions_.CountJoined(call_id);
  if (joined && *joined >= 3) {
    return; // group uses SFU attach-wait / ICE→SFU
  }
  if (media_.ConnectionState() == "failed") {
    MarkP2pConnectFailed("ice_failed");
    host_.P2pNotifyRingChanged();
    return;
  }
  const int64_t started = media_.StartedAtMs();
  if (started <= 0) {
    return;
  }
  if (util::NowUnixMs() - started < kP2pConnectTimeoutMs) {
    return;
  }
  MarkP2pConnectFailed("timeout");
  host_.P2pNotifyRingChanged();
}

Roe<void> CallP2pSignalingBridge::RetryP2pMedia(const std::string& call_id) {
  if (call_id.empty()) {
    return Error("call_id required");
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return Error("Call session not found");
  }
  auto peer = host_.P2pPeerIdentityForCall(call_id);
  if (!peer || !peer->has_value() || (**peer).empty()) {
    return Error("No peer for call retry");
  }
  ClearP2pConnectFailed();
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.Stop();
  }
  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  BindMediaCallbacks(**peer);
  log().info << "Retrying P2P media as offerer call_id=" << call_id;
  return media_.Start(call_id, CallMediaEngine::Role::Offerer);
}

void CallP2pSignalingBridge::NoteMediaAttempted(const std::string& call_id) {
  media_attempted_calls_.insert(call_id);
}

bool CallP2pSignalingBridge::MediaAttempted(const std::string& call_id) const {
  return media_attempted_calls_.find(call_id) != media_attempted_calls_.end();
}

void CallP2pSignalingBridge::BindMediaCallId(const std::string& call_id) {
  media_call_id_ = call_id;
}

void CallP2pSignalingBridge::ClearMediaPeerIdentity() {
  media_peer_identity_.clear();
}

const std::string& CallP2pSignalingBridge::MediaCallId() const {
  return media_call_id_;
}

void CallP2pSignalingBridge::BindMediaCallbacks(const std::string& peer_identity) {
  media_peer_identity_ = peer_identity;

  media_.SetOnLocalDescription([this](const CallMediaEngine::LocalDescription& local) {
    const std::string call_id = !media_call_id_.empty() ? media_call_id_ : media_.ActiveCallId();
    if (call_id.empty() || media_peer_identity_.empty()) {
      return;
    }
    const std::string peer = media_peer_identity_;
    AppRuntime::PostUI([this, call_id, peer, local]() {
      if (media_call_id_ != call_id && media_.ActiveCallId() != call_id) {
        return;
      }
      CallSdpDetail detail;
      detail.call_id = call_id;
      if (auto local_identity = host_.P2pLocalIdentity()) {
        detail.identity = *local_identity;
      }
      detail.sdp_type = local.type;
      detail.sdp = local.sdp;
      auto encoded = CallControlCodec::EncodeSdp(detail);
      if (!encoded) {
        return;
      }
      (void)host_.P2pSendDirect(peer, CallControlType::CallSdp, *encoded, "Call signaling");
    });
  });

  media_.SetOnIceCandidate([this](const CallMediaEngine::IceCandidate& ice) {
    const std::string call_id = !media_call_id_.empty() ? media_call_id_ : media_.ActiveCallId();
    if (call_id.empty() || media_peer_identity_.empty()) {
      return;
    }
    const std::string peer = media_peer_identity_;
    AppRuntime::PostUI([this, call_id, peer, ice]() {
      if (media_call_id_ != call_id && media_.ActiveCallId() != call_id) {
        return;
      }
      CallIceDetail detail;
      detail.call_id = call_id;
      if (auto local_identity = host_.P2pLocalIdentity()) {
        detail.identity = *local_identity;
      }
      detail.candidate = ice.candidate;
      detail.mid = ice.mid;
      auto encoded = CallControlCodec::EncodeIce(detail);
      if (!encoded) {
        return;
      }
      (void)host_.P2pSendDirect(peer, CallControlType::CallIce, *encoded, "Call signaling");
    });
  });

  media_.SetOnStateChanged([this](const std::string& state) {
    const std::string call_id = media_call_id_;
    if (state == "connected") {
      ClearP2pConnectFailed();
      if (media_.IsSfuMode()) {
        host_.P2pClearAwaitingSfuRecovery();
      }
      host_.P2pNotifyRingChanged();
      return;
    }
    if (state == "failed" && !media_.IsSfuMode() && !call_id.empty()) {
      auto joined = sessions_.CountJoined(call_id);
      if (joined && *joined >= 3) {
        if (host_.P2pIsAwaitingSfuRecovery()) {
          // Soft-migrate / StartSfu PC teardown often emits failed — do not re-enter.
          return;
        }
        host_.P2pOnGroupIceFailed(call_id);
        return;
      }
      MarkP2pConnectFailed("ice_failed");
      host_.P2pNotifyRingChanged();
      return;
    }
    host_.P2pNotifyRingChanged();
  });
}

void CallP2pSignalingBridge::ClearMediaCallbacks() {
  media_.SetOnLocalDescription({});
  media_.SetOnIceCandidate({});
  media_.SetOnStateChanged({});
  media_peer_identity_.clear();
  media_call_id_.clear();
}

Roe<void> CallP2pSignalingBridge::StartMediaAsOfferer(const std::string& call_id,
                                                      const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  BindMediaCallbacks(peer_identity);
  return media_.Start(call_id, CallMediaEngine::Role::Offerer);
}

Roe<void> CallP2pSignalingBridge::StartMediaAsAnswerer(const std::string& call_id,
                                                       const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  BindMediaCallbacks(peer_identity);
  return media_.Start(call_id, CallMediaEngine::Role::Answerer);
}

void CallP2pSignalingBridge::ScheduleStartMediaAsOfferer(const std::string& call_id,
                                                         const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  AppRuntime::PostUI([this, call_id, peer_identity]() {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
      return;
    }
    if (media_.IsActive() && media_.ActiveCallId() == call_id) {
      return;
    }
    if (auto started = StartMediaAsOfferer(call_id, peer_identity); !started) {
      log().warning << "StartMediaAsOfferer failed: " << started.error().message;
      host_.P2pSetLastMediaError(started.error().message);
      host_.P2pNotifyRingChanged();
      return;
    }
    AppRuntime::PostUI([this, call_id, peer_identity]() {
      if (!media_.IsActive() || media_.ActiveCallId() != call_id || media_.IsConnected() ||
          media_.IsSfuMode()) {
        return;
      }
      auto local = media_.CurrentLocalDescription();
      if (!local || local->sdp.empty()) {
        return;
      }
      CallSdpDetail detail;
      detail.call_id = call_id;
      if (auto local_identity = host_.P2pLocalIdentity()) {
        detail.identity = *local_identity;
      }
      detail.sdp_type = local->type;
      detail.sdp = local->sdp;
      auto encoded = CallControlCodec::EncodeSdp(detail);
      if (!encoded) {
        return;
      }
      log().info << "Re-sending local offer for answerer race call_id=" << call_id;
      (void)host_.P2pSendDirect(peer_identity, CallControlType::CallSdp, *encoded, "Call signaling");
    });
    host_.P2pNotifyRingChanged();
  });
}

void CallP2pSignalingBridge::ScheduleStartMediaAsAnswerer(const std::string& call_id,
                                                          const std::string& peer_identity) {
  media_attempted_calls_.insert(call_id);
  AppRuntime::PostUI([this, call_id, peer_identity]() {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
      return;
    }
    if (auto started = StartMediaAsAnswerer(call_id, peer_identity); !started) {
      log().warning << "StartMediaAsAnswerer failed: " << started.error().message;
      host_.P2pSetLastMediaError(started.error().message);
    }
    host_.P2pNotifyRingChanged();
  });
}

void CallP2pSignalingBridge::StopP2pMedia(const std::string& call_id) {
  ClearP2pConnectFailed();
  ClearMediaCallbacks();
  media_attempted_calls_.erase(call_id);
  // SDL / PC teardown is UI-only (same rule as libp2p StopLibp2pMedia).
  auto stop_engine = [this, call_id]() {
    if (media_.IsActive() && media_.ActiveCallId() == call_id) {
      media_.Stop();
    }
  };
  if (AppRuntime::CurrentlyOnUI()) {
    stop_engine();
  } else {
    AppRuntime::PostUI( std::move(stop_engine));
  }
}

Roe<void> CallP2pSignalingBridge::OnRemoteSdp(const CallSdpDetail& sdp, const std::string& sender_identity) {
  if (media_.IsSfuMode()) {
    return {};
  }
  auto applied = media_.SetRemoteDescription(sdp.sdp_type, sdp.sdp);
  const bool is_offer = (sdp.sdp_type == "offer" || sdp.sdp_type == "Offer");
  if (applied && !media_.IsActive() && is_offer) {
    const std::string call_id = sdp.call_id;
    if (!call_id.empty() && media_attempted_calls_.count(call_id) > 0) {
      ScheduleStartMediaAsAnswerer(call_id, sender_identity);
    }
  }
  return applied;
}

Roe<void> CallP2pSignalingBridge::OnRemoteIce(const CallIceDetail& ice) {
  if (media_.IsSfuMode()) {
    return {};
  }
  return media_.AddRemoteIceCandidate(ice.candidate, ice.mid);
}

} // namespace pbr
