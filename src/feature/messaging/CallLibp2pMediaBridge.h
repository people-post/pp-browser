#pragma once

#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallSessionStore.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/CallLifecycle.h"
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
                        CallMediaEngine& media, CallMediaDirectService& direct, IDialRegistry* dial,
                        ICircuitHopReach* circuit_reach);

  bool IsLibp2pConnectFailed() const;
  bool Libp2pConnectMissingMic() const;
  void ClearLibp2pConnectFailed();
  void PollLibp2pConnectHealth();

  /** True when libp2p call-media path is available (direct or circuit-brokered). */
  bool ShouldUseLibp2pForPeer(const std::string& peer_identity) const;

  Roe<void> RetryLibp2pMedia(const std::string& call_id);

  Roe<void> StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  Roe<void> StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);

  /** Answerer media waits for CallMediaKey (V015 epoch-1-on-accept); kick Start when key lands. */
  void OnMediaKeyReady(const std::string& call_id);

  void StopLibp2pMedia(const std::string& call_id);

  /**
   * Abort in-flight Connect and wait until the worker exits (or timeout).
   * Must run before destroying this bridge / CallMediaDirectService / libp2p host.
   */
  void PrepareForTeardown(int timeout_ms = 2000);

  /** True after PrepareForTeardown / StopLibp2pMedia — inbound hello wait must exit. */
  bool IsStopping() const { return stopping_.load(std::memory_order_acquire); }

  void NoteMediaAttempted(const std::string& call_id);
  bool MediaAttempted(const std::string& call_id) const;

  /** Keep dial/circuit pointers valid when MessagingHub rewires deps (N025 listen sync). */
  void SetReachDeps(IDialRegistry* dial, ICircuitHopReach* circuit_reach);

  void SetLifecycle(CallLifecycle* lifecycle);

private:
  Roe<void> BeginSession(const std::string& call_id, const std::string& peer_identity, bool offerer);
  Roe<void> EnsurePeerReachableOnIo(const std::string& peer_identity, uint64_t connect_gen);
  Roe<void> ConnectOffererWithRetry(const CallMediaDirectConnectParams& params,
                                    const CallMediaDirectCallbacks& cbs);
  Roe<ByteVector> LoadActiveMediaKey(const std::string& call_id) const;
  /** Direct stream up: mark media connected when capture is live, always advance lifecycle/chrome. */
  void CommitDirectConnected(const std::string& call_id);

  CallP2pSignalingHost& host_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  CallMediaEngine& media_;
  CallMediaDirectService& direct_;
  IDialRegistry* dial_ = nullptr;
  ICircuitHopReach* circuit_reach_ = nullptr;
  CallLifecycle* lifecycle_ = nullptr;

  std::string media_peer_identity_;
  std::string media_call_id_;
  std::string pending_answerer_call_id_;
  std::string pending_answerer_peer_;
  bool libp2p_connect_failed_ = false;
  bool libp2p_connect_missing_mic_ = false;
  /** Connect worker runs on Normal (not Critical) so hello/inbound are not starved. */
  std::atomic<bool> connect_worker_inflight_{false};
  /** Bumped in StopLibp2pMedia so in-flight Connect workers abort instead of racing Detach/Stop. */
  std::atomic<uint64_t> connect_generation_{0};
  std::atomic<bool> stopping_{false};
  std::unordered_set<std::string> media_attempted_calls_;
  std::atomic<uint32_t> audio_seq_{0};
};

} // namespace pbr
