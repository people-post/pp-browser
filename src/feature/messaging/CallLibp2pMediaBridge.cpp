#include "feature/messaging/CallLibp2pMediaBridge.h"

#include "base/platform/BrowserThread.h"
#include "common/Utilities.h"

namespace pbr {
namespace {

constexpr int64_t kLibp2pConnectTimeoutMs = 15000;

} // namespace

CallLibp2pMediaBridge::CallLibp2pMediaBridge(CallP2pSignalingHost& host, CallSessionStore& sessions,
                                             CallMediaKeyStore& media_keys, CallMediaEngine& media,
                                             CallMediaDirectService& direct, IDialRegistry* dial)
    : host_(host), sessions_(sessions), media_keys_(media_keys), media_(media), direct_(direct), dial_(dial) {
  redirectLogger("CallLibp2pMediaBridge");

  direct_.SetInboundHandler([this](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    auto session = sessions_.LoadSession(params.call_id);
    if (!session || !session->has_value()) {
      return;
    }
    if (auto key = media_keys_.LoadEpochKey(params.call_id, params.media_epoch); key && key->has_value()) {
      params.media_key = **key;
    }
    const std::string call_id = params.call_id;
    cbs.on_connected = [this, call_id]() {
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id]() {
        if (media_.IsActive() && media_.ActiveCallId() == call_id) {
          host_.P2pNotifyRingChanged();
        }
      });
    };
    cbs.on_audio = [this, call_id](const std::vector<uint8_t>& opus) {
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, opus]() {
        if (!media_.IsActive() || media_.ActiveCallId() != call_id) {
          return;
        }
        CallMediaEngine::SfuPacket pkt;
        pkt.channel_id = 0;
        pkt.payload = opus;
        media_.OnSfuPacket(pkt);
      });
    };
    cbs.on_failed = [this, call_id](const std::string& reason) {
      BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, reason]() {
        if (media_.ActiveCallId() != call_id) {
          return;
        }
        libp2p_connect_failed_ = true;
        host_.P2pSetLastMediaError(reason);
        host_.P2pNotifyRingChanged();
      });
    };
  });
}

bool CallLibp2pMediaBridge::IsLibp2pConnectFailed() const {
  return libp2p_connect_failed_;
}

bool CallLibp2pMediaBridge::Libp2pConnectMissingMic() const {
  return libp2p_connect_missing_mic_;
}

void CallLibp2pMediaBridge::ClearLibp2pConnectFailed() {
  libp2p_connect_failed_ = false;
  libp2p_connect_missing_mic_ = false;
}

void CallLibp2pMediaBridge::PollLibp2pConnectHealth() {
  if (libp2p_connect_failed_ || host_.P2pIsAwaitingSfuRecovery() || !media_.IsActive() || !media_.IsSfuMode()) {
    return;
  }
  if (media_.IsConnected()) {
    ClearLibp2pConnectFailed();
    return;
  }
  const std::string call_id = media_.ActiveCallId();
  if (call_id.empty()) {
    return;
  }
  auto joined = sessions_.CountJoined(call_id);
  if (joined && *joined >= 3) {
    return;
  }
  const int64_t started = media_.StartedAtMs();
  if (started <= 0) {
    return;
  }
  if (util::NowUnixMs() - started < kLibp2pConnectTimeoutMs) {
    return;
  }
  libp2p_connect_failed_ = true;
  libp2p_connect_missing_mic_ = !media_.HasLocalCapture();
  host_.P2pNotifyRingChanged();
}

bool CallLibp2pMediaBridge::ShouldUseLibp2pForPeer(const std::string& peer_identity) const {
  return dial_ && dial_->IsDialable(peer_identity);
}

Roe<ByteVector> CallLibp2pMediaBridge::LoadActiveMediaKey(const std::string& call_id) const {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("call session not found");
  }
  auto key = media_keys_.LoadEpochKey(call_id, (*session)->media_epoch);
  if (!key) {
    return key.error();
  }
  if (!key->has_value()) {
    return Error("call media key not ready");
  }
  return **key;
}

Roe<void> CallLibp2pMediaBridge::BeginSession(const std::string& call_id, const std::string& peer_identity,
                                              bool offerer) {
  auto key = LoadActiveMediaKey(call_id);
  if (!key) {
    return key.error();
  }

  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("call session not found");
  }

  media_attempted_calls_.insert(call_id);
  media_call_id_ = call_id;
  media_peer_identity_ = peer_identity;
  audio_seq_.store(0);
  ClearLibp2pConnectFailed();

  if (media_.IsActive()) {
    media_.Stop();
  }
  direct_.Detach();

  const uint32_t media_epoch = (*session)->media_epoch;
  const ByteVector media_key = *key;

  media_.SetOnStateChanged([this](const std::string& state) {
    if (state == "connected") {
      ClearLibp2pConnectFailed();
      host_.P2pNotifyRingChanged();
      return;
    }
    host_.P2pNotifyRingChanged();
  });

  const std::string captured_call_id = call_id;
  const std::string captured_peer = peer_identity;
  if (auto started = media_.StartSfu(call_id, [this, captured_call_id, captured_peer, media_epoch, media_key](
                                                  const CallMediaEngine::SfuPacket& pkt) {
        if (pkt.channel_id != 0) {
          return;
        }
        const uint32_t seq = audio_seq_.fetch_add(1) + 1;
        (void)direct_.SendAudio(pkt.payload, seq, pkt.mark);
      });
      !started) {
    return started;
  }

  CallMediaDirectConnectParams params;
  params.peer_key = peer_identity;
  params.call_id = call_id;
  params.media_epoch = media_epoch;
  params.media_key = media_key;
  params.offerer = offerer;

  CallMediaDirectCallbacks cbs;
  cbs.on_connected = [this]() {
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
      ClearLibp2pConnectFailed();
      host_.P2pNotifyRingChanged();
    });
  };
  cbs.on_audio = [this, captured_call_id](const std::vector<uint8_t>& opus) {
    BrowserThread::PostTask(BrowserThreadId::UI, [this, captured_call_id, opus]() {
      if (!media_.IsActive() || media_.ActiveCallId() != captured_call_id) {
        return;
      }
      CallMediaEngine::SfuPacket pkt;
      pkt.channel_id = 0;
      pkt.payload = opus;
      media_.OnSfuPacket(pkt);
    });
  };
  cbs.on_failed = [this, captured_call_id](const std::string& reason) {
    BrowserThread::PostTask(BrowserThreadId::UI, [this, captured_call_id, reason]() {
      if (media_.ActiveCallId() != captured_call_id) {
        return;
      }
      libp2p_connect_failed_ = true;
      host_.P2pSetLastMediaError(reason);
      host_.P2pNotifyRingChanged();
    });
  };

  if (offerer) {
    BrowserThread::PostTask(BrowserThreadId::IO, [this, params, cbs]() {
      Roe<void> connected = direct_.Connect(params, cbs);
      BrowserThread::PostTask(BrowserThreadId::UI, [this, connected, call_id = params.call_id]() {
        if (!connected) {
          libp2p_connect_failed_ = true;
          host_.P2pSetLastMediaError(connected.error().message);
          host_.P2pNotifyRingChanged();
        }
      });
    });
  }

  libp2p_connect_missing_mic_ = false;
  host_.P2pNotifyRingChanged();
  return {};
}

Roe<void> CallLibp2pMediaBridge::StartMediaAsOfferer(const std::string& call_id,
                                                     const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, true);
}

Roe<void> CallLibp2pMediaBridge::StartMediaAsAnswerer(const std::string& call_id,
                                                      const std::string& peer_identity) {
  return BeginSession(call_id, peer_identity, false);
}

void CallLibp2pMediaBridge::ScheduleStartMediaAsOfferer(const std::string& call_id,
                                                        const std::string& peer_identity) {
  BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer_identity]() {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
      return;
    }
    if (media_.IsActive() && media_.ActiveCallId() == call_id) {
      return;
    }
    if (auto started = StartMediaAsOfferer(call_id, peer_identity); !started) {
      host_.P2pSetLastMediaError(started.error().message);
      host_.P2pNotifyRingChanged();
    }
  });
}

void CallLibp2pMediaBridge::ScheduleStartMediaAsAnswerer(const std::string& call_id,
                                                         const std::string& peer_identity) {
  BrowserThread::PostTask(BrowserThreadId::UI, [this, call_id, peer_identity]() {
    auto session = sessions_.LoadSession(call_id);
    if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
      return;
    }
    if (auto started = StartMediaAsAnswerer(call_id, peer_identity); !started) {
      host_.P2pSetLastMediaError(started.error().message);
      host_.P2pNotifyRingChanged();
    }
  });
}

void CallLibp2pMediaBridge::StopLibp2pMedia(const std::string& call_id) {
  if (media_.IsActive() && media_.ActiveCallId() == call_id) {
    media_.Stop();
  }
  direct_.Detach();
  media_peer_identity_.clear();
  media_call_id_.clear();
  ClearLibp2pConnectFailed();
  media_attempted_calls_.erase(call_id);
}

void CallLibp2pMediaBridge::NoteMediaAttempted(const std::string& call_id) {
  media_attempted_calls_.insert(call_id);
}

bool CallLibp2pMediaBridge::MediaAttempted(const std::string& call_id) const {
  return media_attempted_calls_.count(call_id) > 0;
}

} // namespace pbr
