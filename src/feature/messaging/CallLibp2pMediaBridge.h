#pragma once

#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallSessionStore.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/CallP2pSignalingBridge.h"
#include "feature/messaging/CallTopologyRelayDeps.h"
#include "libp2p/integration/host/CallMediaDirectService.h"

#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace pbr {

/**
 * 1:1 libp2p direct call media (m1 / V026). Uses CallMediaEngine SFU-mode capture/playback
 * with Opus frames over CallMediaDirectService (AEAD under call media key).
 */
class CallLibp2pMediaBridge : public Module {
public:
  CallLibp2pMediaBridge(CallP2pSignalingHost& host, CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                        CallMediaEngine& media, CallMediaDirectService& direct, IDialRegistry* dial);

  bool IsLibp2pConnectFailed() const;
  bool Libp2pConnectMissingMic() const;
  void ClearLibp2pConnectFailed();
  void PollLibp2pConnectHealth();

  bool ShouldUseLibp2pForPeer(const std::string& peer_identity) const;

  Roe<void> StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  Roe<void> StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);

  void StopLibp2pMedia(const std::string& call_id);

  void NoteMediaAttempted(const std::string& call_id);
  bool MediaAttempted(const std::string& call_id) const;

private:
  Roe<void> BeginSession(const std::string& call_id, const std::string& peer_identity, bool offerer);
  Roe<ByteVector> LoadActiveMediaKey(const std::string& call_id) const;

  CallP2pSignalingHost& host_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  CallMediaEngine& media_;
  CallMediaDirectService& direct_;
  IDialRegistry* dial_ = nullptr;

  std::string media_peer_identity_;
  std::string media_call_id_;
  bool libp2p_connect_failed_ = false;
  bool libp2p_connect_missing_mic_ = false;
  std::unordered_set<std::string> media_attempted_calls_;
  std::atomic<uint32_t> audio_seq_{0};
};

} // namespace pbr
