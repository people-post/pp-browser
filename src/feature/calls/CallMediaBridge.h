#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "feature/calls/CallLifecycle.h"
#include "feature/calls/CallMediaHost.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"

#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * 1:1 call media (m1 / V026). Uses CallMediaEngine SFU-mode capture/playback
 * with Opus frames over ICallMediaTransport (Amp; [A020]).
 */
class CallMediaBridge : public Module {
public:
  CallMediaBridge(CallMediaHost& host, CallSessionStore& sessions, CallMediaKeyStore& media_keys,
                        CallMediaEngine& media, ICallMediaTransport& direct, IDialRegistry* dial,
                        ICircuitHopReach* circuit_reach);

  bool IsMeshConnectFailed() const;
  bool MeshConnectMissingMic() const;
  void ClearMeshConnectFailed();
  void PollMeshConnectHealth();

  /** True when mesh call-media path is available (direct or circuit-brokered). */
  bool ShouldUseMeshForPeer(const std::string& peer_identity) const;

  Roe<void> RetryMeshMedia(const std::string& call_id);

  Roe<void> StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  Roe<void> StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);

  /** Answerer media waits for CallMediaKey (V015 epoch-1-on-accept); kick Start when key lands. */
  void OnMediaKeyReady(const std::string& call_id);

  void StopMeshMedia(const std::string& call_id);

  /**
   * SoftMigrate: close 1:1 call-media stream without CallMediaEngine::Stop so SFU capture continues.
   */
  void ReleaseDirectTransport();

  /**
   * CallAccept/Invite taught PeerId→relay: (works for non-contacts). Rebind deferred inbound
   * on_audio stream_id when it matches the pending inbound PeerId.
   */
  void NotePeerIdRelayMapping(const std::string& peer_id, const std::string& relay_identity);

  /**
   * Abort in-flight Connect and wait until the worker exits (or timeout).
   * Must run before destroying this bridge / CallMediaDirectService / mesh host.
   */
  void PrepareForTeardown(int timeout_ms = 2000);

  /** True after PrepareForTeardown / StopMeshMedia — inbound hello wait must exit. */
  bool IsStopping() const { return stopping_.load(std::memory_order_acquire); }

  void NoteMediaAttempted(const std::string& call_id);
  bool MediaAttempted(const std::string& call_id) const;

  /** Keep dial/circuit pointers valid when ConversationsHub rewires deps (N025 listen sync). */
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
  void DeliverInboundDirectMedia(const std::string& call_id, uint8_t channel,
                                 const std::vector<uint8_t>& payload);

  CallMediaHost& host_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  CallMediaEngine& media_;
  ICallMediaTransport& direct_;
  IDialRegistry* dial_ = nullptr;
  ICircuitHopReach* circuit_reach_ = nullptr;
  CallLifecycle* lifecycle_ = nullptr;

  std::string media_peer_identity_;
  std::string media_call_id_;
  std::string pending_answerer_call_id_;
  std::string pending_answerer_peer_;
  /** Inbound hello PeerId while stream_id deferred (non-contact / pre-Accept). */
  std::string inbound_deferred_peer_id_;
  bool mesh_connect_failed_ = false;
  bool mesh_connect_missing_mic_ = false;
  /** Connect worker runs on Normal (not Critical) so hello/inbound are not starved. */
  std::atomic<bool> connect_worker_inflight_{false};
  /** Bumped in StopMeshMedia so in-flight Connect workers abort instead of racing Detach/Stop. */
  std::atomic<uint64_t> connect_generation_{0};
  std::atomic<bool> stopping_{false};
  std::unordered_set<std::string> media_attempted_calls_;
  std::atomic<uint32_t> audio_seq_{0};
  /** 1:1 inbound remote mixer stream; 0 = defer until relay: identity known (BeginSession). */
  std::atomic<uint32_t> inbound_remote_stream_{0};
};

} // namespace pbr
