#pragma once

#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionStore.h"
#include "base/messaging/PeerCapsLogic.h"
#include "base/messaging/SoftMigrateLogic.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "feature/messaging/CallMediaKeyStore.h"
#include "feature/messaging/CallTopologyRelayDeps.h"

#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Narrow façade for SFU / soft-migrate side effects owned by CallSessionManager. */
class CallTopologyHost {
public:
  virtual ~CallTopologyHost() = default;
  virtual Roe<std::string> TopologyLocalIdentity() const = 0;
  virtual Roe<void> TopologyLeaveCall(const std::string& call_id) = 0;
  virtual Roe<void> TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                           const std::string& detail_json, const std::string& display,
                                           const std::string& skip_identity) = 0;
  virtual Roe<void> TopologySendDirect(const std::string& peer_identity, CallControlType type,
                                       const std::string& detail_json, const std::string& display) = 0;
  virtual void TopologyNotifyRingChanged() = 0;
  virtual void TopologySetLastMediaError(std::string message) = 0;
  /** Ephemeral in-call progress (hop pick / switch) — not an error banner. */
  virtual void TopologySetMediaActivity(std::string message) = 0;
  virtual void TopologyClearMediaActivity() = 0;
  virtual void TopologyNoteMediaAttempted(const std::string& call_id) = 0;
  virtual void TopologyBindMediaCallId(const std::string& call_id) = 0;
  virtual void TopologyClearMediaPeerIdentity() = 0;
  /**
   * SoftMigrate to media_relay: drop 1:1 call-media stream without CallMediaEngine::Stop
   * (capture must keep running into SFU send).
   */
  virtual void TopologyReleaseDirectMedia() = 0;
  /** Guest WaitForAttach — force relay inbox poll for CallSfuAttach. */
  virtual void TopologyRequestInboxSync() = 0;
};

/**
 * SFU soft-migrate / attach-wait / hop pick (V021 + V025).
 * Pure who-picks / wait / fan-out live in base SoftMigrateLogic / SfuAttachWaitLogic /
 * SfuAttachFanout; this adapter owns IO + AppRuntime posting.
 */
class CallTopologyController : public Module {
public:
  struct MediaRelayDeps {
    IMediaRelayClient* relay = nullptr;
    IDialRegistry* dial = nullptr;
    ICircuitHopReach* circuit_reach = nullptr;
    std::vector<std::string> bootstrap_peers;
    bool prefer_contacts = true;
    /** Cached mesh_node listings (n-dir). */
    std::function<std::vector<MeshDirectoryNode>()> list_directory_nodes;
    /** DHT peer_routing cache (n2-caps). */
    std::function<std::vector<MeshDirectoryNode>()> list_dht_nodes;
    /** When false, org seed hops are omitted (bridge score / n-dir). */
    std::function<bool()> seed_dial_ok;
    /**
     * PreferLocalMediaHop / AttachAsLocalHop — durable Node only (desktop/org).
     * Must stay false for mobile ephemeral media_relay (V027): phones must not SoftMigrate
     * themselves into the SFU host role (dogfood crash + Connection reset for peers).
     */
    bool prefer_local_as_hop = false;
    /** For same-/24 hop ranking only (wildcard bind cleared). */
    std::string local_listen_multiaddr;
    /**
     * Dialable listen multiaddrs for PreferLocalMediaHop CallSfuAttach fan-out
     * (LAN IPs + /p2p/<self>, same shape as call invite listen_multiaddrs).
     * SoftMigrate prefers `resolve_local_advertise` when set (live listen state).
     */
    std::vector<std::string> local_advertise_multiaddrs;
    std::function<std::vector<std::string>()> resolve_local_advertise;
    /**
     * V030: true when peer advertised media_relay on call caps (or equivalent cache).
     * SoftMigrate keeps OrgSeed always; contacts require this. Null → no contact hops.
     */
    std::function<bool(const std::string& peer_id)> peer_has_media_relay;
    /** PeerIds with media_relay=true ads (inject into SoftMigrate when missing from contacts). */
    std::function<std::vector<std::string>()> list_media_relay_peers;
  };

  CallTopologyController(CallTopologyHost& host, CallSessionStore& sessions, ContactsStore& contacts,
                         CallMediaEngine& media);

  void SetMediaRelayDeps(MediaRelayDeps deps);
  /** Required for SFU E2E AEAD (V032). */
  void SetMediaKeyStore(CallMediaKeyStore* keys);

  bool IsAwaitingSfuRecovery() const;
  bool IsSfuAttached() const;
  bool IsOnSfuForCall(const std::string& call_id) const;
  /** Soft-migrate / attach-wait in flight (suppress ICE→SFU re-entry + stale LeaveCall). */
  bool IsSoftMigrateInFlight() const;
  bool IsSfuAttachWaitActive() const;

  bool HasMediaRelayHopCandidates() const;
  std::vector<MeshHopCandidate> RankedMediaHopCandidates() const;
  /** Resolve dialable multiaddr for a hop PeerId (contacts ∪ seeds ∪ L1 address book). */
  std::string ResolveHopMultiaddr(const std::string& hop_peer_id) const;

  void BeginSfuAttachWait(const std::string& call_id);
  void ClearSfuAttachWait();
  void PollPendingSfuAttach();

  void ClearAwaitingSfuRecovery();
  void OnMediaStopped(const std::string& call_id);

  void EjectParticipantAfterMigrateFailure(const std::string& call_id, const std::string& identity,
                                           const std::string& reason);

  Roe<void> MaybeSoftMigrateToSfu(const std::string& call_id, SoftMigrateTrigger trigger,
                                  const std::string& prefer_hop_peer_id = {},
                                  uint64_t expected_gen = 0);
  Roe<void> AttachLocalToSfu(const std::string& call_id, const CallSfuAttachDetail& attach);

  /** Group (N≥3) ICE failed — recover via soft-migrate (posted to UI by caller if needed). */
  void TryRecoverViaSfu(const std::string& call_id);

  /**
   * After local AcceptInvite: attach via hint, soft-migrate, or clear wait for 1:1.
   * Returns true if an SFU path was scheduled (caller should not start P2P).
   */
  bool OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                           const std::optional<std::string>& sfu_hint);

  /**
   * After inbound CallAccept raised joined count: soft-migrate or clear SFU wait.
   * Returns true if SFU path was taken (caller should not start P2P offerer).
   */
  bool OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                            const std::string& joiner_identity);

  /**
   * After CallRoster updated local joined count (mid-call invite path): initiator may SoftMigrate.
   */
  void OnJoinedCountObserved(const std::string& call_id, size_t n_joined);

  Roe<void> OnInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach);
  /** Guest attach failed with hop preferences (V029) — initiator only. */
  void OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail);
  /** Owner refused guest after empty hop intersection (V029). */
  void OnInboundHopRefuse(const CallHopRefuseDetail& detail);

  void RefreshAdaptation(const std::string& call_id);
  void RefreshAdaptation(const std::string& call_id, bool camera_user_wants);
  /** Re-Subscribe hop streams for all currently Joined peers (late join / roster). */
  void SyncSfuSubscriptions(const std::string& call_id);
  uint32_t PublisherStreamIdForLocal() const;
  /** Hop health when SFU attached (V032). */
  CallHopHealth HopHealth() const;

private:
  void ReportSfuAttachFailedToInitiator(const std::string& call_id, const std::string& failed_hop,
                                        const std::string& error);
  void RefuseGuestNoSharedHop(const std::string& call_id, const std::string& guest_identity);
  std::vector<std::string> DialableHopPeerIds() const;
  bool IsMigrateGenerationCurrent(uint64_t gen) const;
  /** Apply deferred CallSfuAttach after SoftMigrate finishes (must run on UI). */
  void FlushPendingInboundSfuAttach();
  void SubscribePublisherStream(uint32_t stream_id);
  /** First subscribe: ask publisher for an IDR (V034). */
  void MaybeRequestPublisherKeyframe(uint32_t stream_id);
  /** Learn publisher_stream_id from CallSfuAttach even when roster lacks that peer (dogfood). */
  void NoteRemotePublisherFromAttach(const CallSfuAttachDetail& attach);
  /** Fan-out our publisher stream so peers with incomplete Joined roster still subscribe. */
  void AnnounceLocalPublisher(const std::string& call_id, const CallSfuAttachDetail& hop_attach);
  /**
   * Guest media-relay duplex died mid-call — re-AcceptAndAttach without restarting capture.
   * UI-thread entry; work runs on a worker.
   */
  void OnGuestSfuTransportLost();
  /** Quote + AcceptAndAttach + reader + subscribe; keeps existing StartSfu send path. */
  Roe<void> ReattachGuestSfuTransport(const std::string& call_id, const CallSfuAttachDetail& attach);

  CallTopologyHost& host_;
  CallSessionStore& sessions_;
  ContactsStore& contacts_;
  CallMediaEngine& media_;
  CallMediaKeyStore* media_keys_ = nullptr;
  MediaRelayDeps relay_deps_;
  int64_t last_quote_a_up_bps_ = 0;
  bool sfu_attached_ = false;
  bool awaiting_sfu_recovery_ = false;
  bool soft_migrate_in_flight_ = false;
  /** Generation that currently owns soft_migrate_in_flight_ (stale workers must not clear newer). */
  uint64_t soft_migrate_flight_gen_ = 0;
  std::atomic<uint64_t> migrate_generation_{0};
  /** Serializes AttachLocalToSfu (concurrent SoftMigrate + inbound CallSfuAttach). */
  std::mutex sfu_attach_mu_;
  /** Inbound CallSfuAttach while SoftMigrate PickHop is mid-AcceptAndAttach — apply after. */
  std::optional<CallSfuAttachDetail> pending_inbound_sfu_attach_;
  std::string pending_inbound_sfu_attach_call_id_;
  uint32_t local_publisher_stream_id_ = 0;
  /** Remote publisher streams learned from CallSfuAttach (roster may lag). */
  std::unordered_set<uint32_t> remote_publisher_stream_ids_;
  std::unordered_set<uint32_t> video_refresh_sent_streams_;
  std::string sfu_attach_wait_call_id_;
  int64_t sfu_attach_wait_deadline_ms_ = 0;
  /** Last successful remote-hop attach (guest reattach after duplex death). */
  std::optional<CallSfuAttachDetail> active_guest_sfu_attach_;
  std::string active_sfu_call_id_;
  int sfu_guest_reattach_attempts_ = 0;
  bool guest_reattach_in_flight_ = false;
  static constexpr int kMaxGuestSfuReattachAttempts = 3;
};

} // namespace pbr
