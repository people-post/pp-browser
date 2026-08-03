#pragma once

#include "base/media/CallMediaEngine.h"
#include "base/messaging/CallControlCodec.h"
#include "base/messaging/CallSessionStore.h"
#include "base/messaging/SoftMigrateLogic.h"
#include "base/people/ContactsStore.h"
#include "base/people/MeshHopPolicy.h"
#include "feature/messaging/CallTopologyRelayDeps.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
  virtual void TopologyNoteMediaAttempted(const std::string& call_id) = 0;
  virtual void TopologyBindMediaCallId(const std::string& call_id) = 0;
  virtual void TopologyClearMediaPeerIdentity() = 0;
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
    std::string local_listen_multiaddr;
  };

  CallTopologyController(CallTopologyHost& host, CallSessionStore& sessions, ContactsStore& contacts,
                         CallMediaEngine& media);

  void SetMediaRelayDeps(MediaRelayDeps deps);

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

  Roe<void> MaybeSoftMigrateToSfu(const std::string& call_id, SoftMigrateTrigger trigger);
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

  void RefreshAdaptation(const std::string& call_id);
  uint32_t PublisherStreamIdForLocal() const;

private:
  CallTopologyHost& host_;
  CallSessionStore& sessions_;
  ContactsStore& contacts_;
  CallMediaEngine& media_;
  MediaRelayDeps relay_deps_;
  bool sfu_attached_ = false;
  bool awaiting_sfu_recovery_ = false;
  bool soft_migrate_in_flight_ = false;
  uint64_t migrate_generation_ = 0;
  uint32_t local_publisher_stream_id_ = 0;
  std::string sfu_attach_wait_call_id_;
  int64_t sfu_attach_wait_deadline_ms_ = 0;
};

} // namespace pbr
