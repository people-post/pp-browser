#pragma once

#include "lib/messaging/SentCopy.h"

#include "common/directory/IContactTrustAccess.h"
#include "foundation/crypto/ReplayWindow.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "lib/messaging/AccountCard.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/DeliveryNotice.h"
#include "lib/messaging/E2eIngestClassifier.h"
#include "lib/messaging/PublicPskLockCoordinator.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/GroupRosterSyncCodec.h"
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
  /** G012: we own this group and a member left — re-sign its roster and send it to the members. */
  std::optional<std::string> resign_roster_group_id;
  /** G012: we are this group's new owner; sign its roster with the transfer we were handed. */
  std::optional<std::string> took_ownership_group_id;
  std::optional<GroupOwnerTransfer> took_ownership_transfer;
  /**
   * A notice from another device of our own account verified (its card taken): the account's shared state may have
   * changed there — the locker is worth a sync (M023 §7).
   */
  bool from_sibling = false;
  /** G013: a group message arrived while our check went unanswered — ask again (its sender is online). */
  std::optional<std::string> restart_roster_check_group_id;
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
 * This device's account id and KEM keys (people's): what decrypting, PSK rotation and group membership
 * need of the local identity — the account's KEM opens account copies, the device's its device pairs (M021).
 */
struct LocalKeys {
  std::function<Roe<std::string>()> account_id;
  std::function<Roe<ByteVector>()> kem_private_key;
  /** This device's PeerId: copies for it are its own; other devices' are its siblings'. */
  std::function<Roe<std::string>()> device_peer_id;
  std::function<Roe<ByteVector>()> device_kem_private_key;
};

/** An account's newest kept account card — its device list (docs/contracts/ACCOUNT_CARD.md); none when not known. */
using AccountCardLookup = std::function<std::optional<AccountCard>(const std::string& account_id)>;

/** v6 receive pipeline steps 0–12: an inbound envelope verified, decrypted, applied and stored. */
class RelayReceivePipeline : public Module {
public:
  RelayReceivePipeline(IThreadStore& store, AccountCardLookup account_cards, IPskSessionStore& psk_store,
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
  /**
   * A sender's account card that verified the envelope it came with, newer than any kept
   * (docs/contracts/ACCOUNT_CARD.md): keep it.
   */
  void SetOnSenderCard(std::function<void(const AccountCard&)> callback) { on_sender_card_ = std::move(callback); }
  /**
   * The issue time a carried card of an account must reach to speak for it (PeerCardStore::Floor): a restored
   * device keeps no cards, only how new the ones it had were. None: 0.
   */
  void SetAccountCardFloor(std::function<int64_t(const std::string& account_id)> floor) {
    account_card_floor_ = std::move(floor);
  }
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
  std::function<void(const AccountCard&)> on_sender_card_;
  std::function<int64_t(const std::string&)> account_card_floor_;
  std::function<void(const GroupMembershipCodec::MemberJoinedEntry&)> on_roster_member_;

  RelayReceiveOutcome ProcessDirectEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                            bool authorized_older_backfill, MessageTransport transport);
  /**
   * The control lane: verified, decrypted under the pair's e2e_public session, handed to the family
   * that sends it on the lane while fresh and not seen before. Never stored.
   */
  RelayReceiveOutcome ProcessControlEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id);
  /**
   * A sibling's control notice (M022: our own account writes to itself), signature already checked: a sent-message
   * copy (M021 d3) is opened and filed as a message we sent; anything else is only its card (the device list).
   */
  void ApplySiblingNotice(const RelayEnvelope& envelope, const std::string& local_account_id,
                          const DirectChatTarget& inbound_target, RelayReceiveOutcome& outcome);
  /** File a sibling's sent message in its chat (public direct, or group), once per message id. */
  void FileSentCopy(const SentCopy& copy, const RelayEnvelope& envelope, RelayReceiveOutcome& outcome);
  RelayReceiveOutcome ProcessGroupEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                           bool authorized_older_backfill, MessageTransport transport);
  /** A sender's numbering in a thread: a group thread holds one per member (and per pair epoch). */
  struct ReplayKey {
    std::string thread_id;
    uint32_t session_epoch = 0;
    std::string sender;

    bool operator==(const ReplayKey& other) const {
      return thread_id == other.thread_id && session_epoch == other.session_epoch && sender == other.sender;
    }
  };

  struct ReplayKeyHash {
    size_t operator()(const ReplayKey& key) const {
      return std::hash<std::string>{}(key.thread_id) ^ (static_cast<size_t>(key.session_epoch) << 1) ^
             (std::hash<std::string>{}(key.sender) << 2);
    }
  };

  /**
   * Checked with the sending device's key from the sender's account card (M021): the card the envelope
   * carries when it proves itself and is newer than the one kept, else the kept one. A device the card does
   * not list is refused.
   */
  Roe<bool> VerifySignature(const RelayEnvelope& envelope, const DirectChatTarget& target) const;
  /** Which copy of a message an envelope is, to this device (M021). */
  enum class CopyTo { ThisDevice, Account, Sibling };
  CopyTo WhoseCopy(const RelayEnvelope& envelope) const;
  /** The KEM secret that opens a copy's key_init: this device's for its device pair, the account's for an account copy. */
  Roe<ByteVector> CopyKemPrivateKey(const RelayEnvelope& envelope) const;
  std::optional<std::string> FindMessageIdAtSeq(const std::string& thread_id, const uint32_t session_epoch,
                                                const std::string& seq_owner_contact_id,
                                                const uint64_t sender_seq, const std::string& sender_device) const;
  ReplayWindow& ReplayWindowFor(const std::string& thread_id, uint32_t session_epoch, const std::string& sender);
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
  /** G012: keep the owner-signed roster a roster event carries, when it verifies and is newer. */
  void KeepCarriedStatement(const ThreadMessage& message) const;
  /**
   * G012: a group_roster_statement from the owner. (The member check's request and reply ride the control
   * lane: MakeGroupRosterSyncFamily.)
   */
  Roe<void> ApplyRosterStatement(const ThreadMessage& message, const std::string& actor_identity,
                                 const std::string& local_account, RelayReceiveOutcome* outcome) const;
  Roe<void> ValidateInboundPskRotate(const RelayEnvelope& envelope, const ThreadMessage& message) const;
  Roe<void> ApplyInboundPskRotate(const std::string& thread_id, const RelayEnvelope& envelope,
                                 const ThreadMessage& message);

  IThreadStore& store_;
  AccountCardLookup account_cards_;
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
