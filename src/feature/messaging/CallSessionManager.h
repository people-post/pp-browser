#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "base/media/CallMediaAdaptation.h"
#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "base/people/MeshHopPolicy.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/P2pMessagingService.h"
#include "libp2p/integration/host/MediaRelayService.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include "common/Module.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/**
 * Call session lifecycle + pairwise signaling + media bring-up (a2 / V014).
 * a4: soft-migrate onto blind media_relay when N≥3 (V020–V024).
 */
class CallSessionManager : public Module {
public:
  using RingChangedFn = std::function<void()>;

  struct MediaRelayDeps {
    MediaRelayService* relay = nullptr;
    PeerSessionManager* sessions = nullptr;
    std::vector<std::string> bootstrap_peers;
    bool prefer_contacts = true;
  };

  CallSessionManager(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                     CallSessionStore& sessions, CallMediaKeyStore& media_keys, P2pMessagingService& p2p,
                     IPskSessionStore& psk_store, CallMediaEngine& media);

  void SetOnRingChanged(RingChangedFn callback);
  void SetMediaRelayDeps(MediaRelayDeps deps);

  /** Start a 1:1 (or multi-invite) call linked to origin_thread_id. Invites peer identities. */
  Roe<CallSession> StartCall(const std::string& origin_thread_id, CallMediaMode mode,
                             const std::vector<std::string>& invitee_identities);

  Roe<void> AcceptInvite(const std::string& call_id);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id);

  /** Mid-call guest invite (pairwise only; no group membership). */
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);

  Roe<std::vector<PendingCallInvite>> ListPendingInvites();
  Roe<std::optional<CallSession>> ActiveLocalCall() const;
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();

  /** First non-local participant identity for a call (display / peer label). */
  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;

  /** Peer (non-local) video_enabled from roster; nullopt if no peer row. */
  Roe<std::optional<bool>> PeerVideoEnabledForCall(const std::string& call_id) const;

  /** Joined participant rows for in-call roster UI. */
  Roe<std::vector<CallParticipant>> ListJoinedParticipants(const std::string& call_id) const;

  /** True while attempting ICE-fail → SFU recovery (suppress auto-leave). */
  bool IsAwaitingSfuRecovery() const;

  /**
   * Leave if we accepted into an N≥3 call but never received CallSfuAttach / hop attach
   * (soft-migrate failed on the coordinator, or no media_relay hop). Call from UI refresh.
   */
  void PollPendingSfuAttach();

  Roe<std::optional<CallSession>> SessionForCall(const std::string& call_id) const;

  /** Expire stale pending invites; notify UI if any changed. */
  void SweepExpiredInvites();

  /**
   * Media never survives process death. End any Joined/Ringing local call rows and
   * pending invites left on disk after a force-quit so chrome does not stick on
   * "Calling…" with no peer.
   */
  void AbandonOrphanedCallsAfterRestart();

  /** True if this process already called Start/StartSfu for call_id (even if media later stopped). */
  bool MediaAttemptedThisProcess(const std::string& call_id) const;

  /** Apply inbound pairwise call system control (after DM persist). */
  Roe<void> ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity);

  CallMediaEngine& Media();

  /** Local mute / camera — updates participant row + roster fan-out (V019 content policy). */
  Roe<void> SetLocalAudioMuted(bool muted);
  Roe<void> SetLocalVideoEnabled(bool enabled);

  /** Pop last media start failure (for UI toast). Empty if none. */
  std::optional<std::string> TakeLastMediaError();

  /** Drop media callbacks before destroying this manager (avoids UAF on engine Stop). */
  void ClearMediaCallbacks();

private:
  Roe<std::string> LocalRelayIdentity() const;
  Roe<void> SendCallDirectMessage(const std::string& peer_identity, CallControlType type,
                                  const std::string& detail_json, const std::string& display);
  Roe<void> AppendOriginHistory(const std::string& thread_id, CallControlType type, const std::string& text,
                                const std::string& detail_json);
  Roe<void> FanOutToJoined(const std::string& call_id, CallControlType type, const std::string& detail_json,
                           const std::string& display, const std::string& skip_identity);
  Roe<void> MaybeRotateMediaKey(const std::string& call_id, const std::string& leaver_identity);
  Roe<void> EndCallLocal(CallSession& session, const std::optional<int64_t>& duration_ms);
  Roe<CallRosterDetail> BuildRosterDetail(const std::string& call_id) const;
  void NotifyRingChanged();

  Roe<ByteVector> ResolvePeerSessionKey(const std::string& peer_identity) const;
  Roe<void> SendMediaKeyToPeer(const std::string& call_id, const std::string& peer_identity,
                               uint32_t media_epoch, const std::string& media_key_id, const ByteVector& key_bytes);
  Roe<void> StartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  Roe<void> StartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  /** Post media bring-up to UI so Accept / inbound CallAccept never block the click or ingest path. */
  void ScheduleStartMediaAsOfferer(const std::string& call_id, const std::string& peer_identity);
  void ScheduleStartMediaAsAnswerer(const std::string& call_id, const std::string& peer_identity);
  void BindMediaCallbacks(const std::string& peer_identity);
  void StopMediaIfCall(const std::string& call_id);
  Roe<void> LeaveCallIfActiveExcept(const std::string& keep_call_id);

  Roe<void> MaybeSoftMigrateToSfu(const std::string& call_id);
  Roe<void> AttachLocalToSfu(const std::string& call_id, const CallSfuAttachDetail& attach);
  uint32_t PublisherStreamIdForLocal() const;
  void RefreshAdaptation(const std::string& call_id);

  /** Ranked contact∪seed hops available for media_relay (empty ⇒ cannot soft-migrate). */
  std::vector<MeshHopCandidate> RankedMediaHopCandidates() const;
  bool HasMediaRelayHopCandidates() const;
  void BeginSfuAttachWait(const std::string& call_id);
  void ClearSfuAttachWait();
  /** Soft-migrate failed after a mid-call accept: keep 1:1 P2P, eject the new joiner. */
  void EjectParticipantAfterMigrateFailure(const std::string& call_id, const std::string& identity,
                                           const std::string& reason);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  CallSessionStore& sessions_;
  CallMediaKeyStore& media_keys_;
  P2pMessagingService& p2p_;
  IPskSessionStore& psk_store_;
  CallMediaEngine& media_;
  MediaRelayDeps relay_deps_;
  RingChangedFn on_ring_changed_;
  std::string media_peer_identity_;
  /** Bound at StartMediaAs* so PC/state callbacks never need ActiveCallId under the engine lock. */
  std::string media_call_id_;
  std::optional<std::string> last_media_error_;
  bool sfu_attached_ = false;
  bool awaiting_sfu_recovery_ = false;
  uint32_t local_publisher_stream_id_ = 0;
  std::unordered_set<std::string> media_attempted_calls_;
  /** AcceptInvite / SoftMigrate: wait for CallSfuAttach or local attach before giving up. */
  std::string sfu_attach_wait_call_id_;
  int64_t sfu_attach_wait_deadline_ms_ = 0;
};

} // namespace pbr
