#pragma once

#include "common/directory/IContactTrustAccess.h"
#include "foundation/crypto/ReplayWindow.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/DeliveryNotice.h"
#include "lib/messaging/E2eIngestClassifier.h"
#include "lib/messaging/PublicPskLockCoordinator.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/GroupRosterStore.h"
#include "common/thread/IThreadStore.h"
#include "lib/messaging/PeerSigningKeyStore.h"
#include "common/thread/ThreadTypes.h"
#include "common/chat/SystemControl.h"
#include "common/Module.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

class ControlLaneSeenStore;

class GroupInviteGate;

struct RelayReceiveOutcome {
  bool persisted = false;
  bool thread_changed = false;
  /** The sender's signature verified (only then may its signed claims, e.g. its relay route, be learned). */
  bool sender_verified = false;
  IngestDecision decision = IngestDecision::HardReject;
  /** Set when `persisted` is true. */
  std::string thread_id;
  /** The stored (decrypted) message when `persisted` — previews and attachment downloads read this. */
  std::optional<ThreadMessage> persisted_message;
  /**
   * When true, MeshDelivery must not bump thread preview/unread or fire OS notices
   * (call MediaKey / SFU / mid-call roster, etc.).
   */
  bool suppress_inbox_chrome = false;
  /**
   * When set, the local user (owner) should publish owner-signed member_joined after ingest.
   * Filled on successful group_invite_accept apply.
   */
  std::optional<std::string> publish_member_joined_group_id;
  std::optional<std::string> publish_member_joined_member_identity;
  uint64_t publish_member_joined_epoch = 0;
  /**
   * When an envelope from a known peer could not be read / applied: why (the GUI words it), with an
   * optional thread to attach a local system line. `receive_failure_detail` is for logs.
   */
  std::optional<ReceiveFailure> receive_failure;
  /** ApplyFailed: the system-control family whose update failed. */
  std::string receive_failure_family;
  /**
   * A "delete for everyone" from the sender: either the retract itself, or a message that arrived after its
   * retract and was stored as a tombstone. No preview / unread bump, no attachment download.
   */
  bool deleted_by_sender = false;
  std::optional<std::string> receive_failure_thread_id;
  std::optional<std::string> receive_failure_sender;
  std::string receive_failure_detail;
};

/**
 * How far a control-lane message's signed time may be from ours (devices' clocks drift); the
 * seen-store keeps ids at least this long, so a replay inside it is a duplicate.
 */
inline constexpr int64_t kControlLaneSignedSkewMs = 60 * 60 * 1000;

/**
 * Whether a control-lane message may still apply: within the signed-time bound and, when it came
 * through the relay, waiting there no longer than `max_age_ms` (relay clock).
 */
bool ControlLaneFresh(const RelayEnvelope& envelope, int64_t max_age_ms, int64_t now_ms);

/**
 * This device's account id and hybrid KEM private key (people's): what decrypting, PSK rotation and
 * group membership need of the local identity.
 */
struct LocalKeys {
  std::function<Roe<std::string>()> account_id;
  std::function<Roe<ByteVector>()> kem_private_key;
};

/** v6 receive pipeline steps 0–12: an inbound envelope verified, decrypted, applied and stored. */
class RelayReceivePipeline : public Module {
public:
  RelayReceivePipeline(IThreadStore& store, IPeerSigningKeyResolver& signing_keys, IPskSessionStore& psk_store,
                       LocalKeys local_keys, GroupRosterStore& group_roster,
                       GroupInviteGate* invite_gate = nullptr);

  /** The system-control families features bind (call control, …). Not owned; null: none. */
  void SetSystemControls(const SystemControlRegistry* controls) { controls_ = controls; }
  /** Where control-lane ids are remembered (at most once per message); none: no dedupe. */
  void SetControlLaneSeen(ControlLaneSeenStore* seen) { control_lane_seen_ = seen; }
  /**
   * projects/privacy T3: direct messages (chat and call control) from a Blocked sender are dropped
   * on receipt. Not owned; null = no blocking.
   */
  void SetContactTrust(IContactTrustAccess* trust) { contact_trust_ = trust; }
  /** A sender's key card that verified the envelope it came with (docs/contracts/ACCOUNT_CARD.md): keep it. */
  void SetOnSenderKeys(std::function<void(const KeyCard&)> callback) { on_sender_keys_ = std::move(callback); }
  /**
   * Another member as the group owner's roster (member_joined) gives them: their card — keep it if it
   * proves itself (docs/contracts/ACCOUNT_CARD.md) — and their relay route.
   */
  void SetOnRosterMember(std::function<void(const GroupMembershipCodec::MemberJoinedEntry&)> callback) {
    on_roster_member_ = std::move(callback);
  }
  /** Called after a sender's retract wiped one of their messages here (`original` = the row before). */
  void SetOnMessageRetracted(std::function<void(const std::string& thread_id, const ThreadMessage& original)> callback) {
    on_message_retracted_ = std::move(callback);
  }

  RelayReceiveOutcome ProcessEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                      bool authorized_older_backfill = false,
                                      MessageTransport transport = MessageTransport::Relay);

private:
  /** A retract from the target's own sender tombstones the target (when it is here already). */
  void ApplyInboundRetract(const ThreadMessage& persisted, RelayReceiveOutcome& outcome);
  /** A message whose retract arrived first is stored as a tombstone straight away. */
  void TombstoneIfRetractedBeforeArrival(ThreadMessage& persisted, RelayReceiveOutcome& outcome);
  std::function<void(const std::string&, const ThreadMessage&)> on_message_retracted_;
  std::function<void(const KeyCard&)> on_sender_keys_;
  std::function<void(const GroupMembershipCodec::MemberJoinedEntry&)> on_roster_member_;

  RelayReceiveOutcome ProcessDirectEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                            bool authorized_older_backfill, MessageTransport transport);
  /**
   * The control lane: verified, decrypted under the pair's e2e_public session, handed to the family
   * that sends it on the lane while fresh and not seen before. Never stored.
   */
  RelayReceiveOutcome ProcessControlEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id);
  RelayReceiveOutcome ProcessGroupEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                           bool authorized_older_backfill, MessageTransport transport);
  struct ReplayKey {
    std::string thread_id;
    uint32_t session_epoch = 0;

    bool operator==(const ReplayKey& other) const {
      return thread_id == other.thread_id && session_epoch == other.session_epoch;
    }
  };

  struct ReplayKeyHash {
    size_t operator()(const ReplayKey& key) const {
      return std::hash<std::string>{}(key.thread_id) ^ (static_cast<size_t>(key.session_epoch) << 1);
    }
  };

  Roe<bool> VerifySignature(const RelayEnvelope& envelope, const DirectChatTarget& target) const;
  std::optional<std::string> FindMessageIdAtSeq(const std::string& thread_id, const uint32_t session_epoch,
                                                const std::string& seq_owner_contact_id,
                                                const uint64_t sender_seq) const;
  ReplayWindow& ReplayWindowFor(const std::string& thread_id, const uint32_t session_epoch);
  Roe<void> ApplyInboundMembershipMessage(ThreadMessage& message, const std::string& actor_identity,
                                          RelayReceiveOutcome* outcome) const;
  /** A bound family's side effects for `message` (none when no family owns it). */
  Roe<void> ApplyInboundControl(ThreadMessage& message, const RelayEnvelope& envelope,
                                RelayReceiveOutcome* outcome_on_failure = nullptr,
                                const std::string& thread_id = {}) const;
  /** Membership updates apply out of order: they run even when the ordered stream drops the message
   *  (authenticated, not in order). */
  bool AppliesOutOfOrder(const ThreadMessage& message) const;
  /** Apply what applies out of order; failures are logged, the message is not stored. */
  void ApplyOutOfOrderControls(ThreadMessage& message, const RelayEnvelope& envelope, IngestDecision decision,
                               RelayReceiveOutcome& outcome) const;
  /** A quiet control message bumps no unread count, preview or notice. */
  void MaybeSuppressInboxChrome(RelayReceiveOutcome& outcome, const ThreadMessage& message) const;
  Roe<void> ValidateInboundPskRotate(const RelayEnvelope& envelope, const ThreadMessage& message) const;
  Roe<void> ApplyInboundPskRotate(const std::string& thread_id, const RelayEnvelope& envelope,
                                 const ThreadMessage& message);

  IThreadStore& store_;
  IPeerSigningKeyResolver& signing_keys_;
  IPskSessionStore& psk_store_;
  LocalKeys local_keys_;
  GroupRosterStore& group_roster_;
  GroupInviteGate* invite_gate_ = nullptr;
  const SystemControlRegistry* controls_ = nullptr;
  ControlLaneSeenStore* control_lane_seen_ = nullptr;

  IContactTrustAccess* contact_trust_ = nullptr;
  bool SenderBlocked(const std::string& sender) const;
  PublicPskLockCoordinator public_lock_;
  std::unordered_map<ReplayKey, ReplayWindow, ReplayKeyHash> replay_windows_;
};

} // namespace pbr
