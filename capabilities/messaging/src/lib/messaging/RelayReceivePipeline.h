#pragma once

#include "lib/messaging/ControlLaneHold.h"
#include "lib/messaging/SentCopy.h"

#include "common/directory/IContactTrustAccess.h"
#include "lib/delivery/E2eIngestClassifier.h"
#include "lib/delivery/IPeerSessionStore.h"
#include "lib/people/AccountCard.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/delivery/DeliveryNotice.h"
#include "lib/delivery/LocalKeys.h"
#include "lib/delivery/E2eIngestClassifier.h"
#include "lib/delivery/PublicPskLockCoordinator.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/GroupRosterSyncCodec.h"
#include "common/thread/IThreadStore.h"
#include "lib/people/PeerSigningKeyStore.h"
#include "common/thread/ThreadTypes.h"
#include "common/chat/SystemControl.h"
#include "common/Module.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IControlLaneSeenStore;

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
   * An event in a group that is about this account (it was removed; its accept met a full group): the GROUP thread
   * counts it unread and notifies, though the direct message that carried it stays quiet. Other people's joins,
   * leaves and renames are not.
   */
  enum class AboutUs { None, Removed, GroupFull };
  AboutUs about_us = AboutUs::None;
  std::string about_us_group_thread_id;
  /** A message another of our devices sent, filed here (M021 d3): no inbox chrome, but its attachment is fetched. */
  bool sibling_copy = false;
  /**
   * A chat record from another account with copies for our other devices but none for this one (M021 §9): the
   * sending device may not know this device yet — or sent our copy over a link. The caller waits, then asks.
   */
  struct MissingCopy {
    std::string sender_account;
    std::string sender_device;
    std::string message_id;
    std::string group_id;
  };
  std::optional<MissingCopy> missing_copy;
  /**
   * A verified control-lane message from this account that this device could not read (M024): a record with
   * copies for our other devices only, or an account copy that did not open. Its sender may hold an older list of
   * our devices: the caller tells it our card (rate-limited), so its next one has a copy for this device. The
   * message itself is not asked for again — signaling is stale by then.
   */
  std::optional<std::string> show_card_to;
  /**
   * A control-lane message that could not be checked for want of its sender's card (none kept, or the sending device
   * not on it): held for the card (ControlLaneHold), then checked again (ReplayHeldControl).
   */
  bool held_for_card = false;
  /**
   * The sender verified, and lane messages of its wait for its card (ControlLaneHold): the caller has them checked
   * again (ReplayHeldControl), off this ingest.
   */
  std::optional<std::string> replay_held_from;
  /**
   * When set, the local user (owner) should publish owner-signed member_joined after ingest.
   * Filled on successful group_invite_accept apply.
   */
  std::optional<std::string> publish_member_joined_group_id;
  /** G014: an accept came when the group was full — the owner tells the invitee (`group_invite_refused`). */
  std::optional<std::string> refuse_invite_group_id;
  std::string refuse_invite_nonce;
  std::string refuse_invite_member;
  /** Why (`group_full`, or `group_dissolved` for an accept that came after the owner ended the group, G016). */
  std::string refuse_invite_reason;
  std::optional<std::string> publish_member_joined_member_identity;
  uint64_t publish_member_joined_epoch = 0;
  /** The invite the accept answered (the join bundle names it, G019). */
  std::string publish_member_joined_invite_nonce;
  /**
   * G019: an accept of the invite `admit_invite_nonce` we sent as a member or admin — sign the `add` of `admit_member`
   * and send it (GroupMembershipWorkflow::AdmitInvitee).
   */
  std::optional<std::string> admit_group_id;
  std::string admit_invite_nonce;
  std::string admit_member;
  /** G018 item 4: we own this group and hold changes on top of its statement — fold them (debounced). */
  std::optional<std::string> fold_group_id;
  /**
   * G018 item 5: a group copy's sender holds a newer roster than ours (a newer statement, or other changes) — ask it
   * (`catch_up_ask`) for what we lack. At most once per group per CatchUpLimiter::kCatchUpGapMs.
   */
  std::optional<std::string> catch_up_group_id;
  std::string catch_up_ask;
  /** G012: we are this group's new owner; sign its roster with the transfer we were handed. */
  std::optional<std::string> took_ownership_group_id;
  std::optional<GroupOwnerTransfer> took_ownership_transfer;
  /**
   * A notice from another device of our own account verified (its card taken): the account's shared state may have
   * changed there — the locker is worth a sync (M023 §7).
   */
  bool from_sibling = false;
  /**
   * G016: a `group_dissolved` ended a group we are in just now. The group's chat (this thread) gets the one
   * notice — unread count, preview, an OS notice if away — and the DM that carried it none (`suppress_inbox_chrome`).
   */
  std::optional<std::string> dissolved_group_thread_id;
  /** G016: invites to the dissolved group, now void: (inviter, nonce) — the invite cards in the DMs to close. */
  std::vector<std::pair<std::string, std::string>> voided_invite_cards;
  /** A newer invite from the same inviter to the same group replaced these: (inviter, nonce), their cards to close. */
  std::vector<std::pair<std::string, std::string>> superseded_invite_cards;
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

/** How a sender's signature check went (RelayReceivePipeline::CheckSignature). */
struct SenderSignatureCheck {
  enum class Result {
    Verified,
    /** No card kept for the sender, and none riding that speaks for it: nothing to check with yet. */
    NoCard,
    /** The card checked with does not list the sending device (a device linked since). */
    DeviceNotOnCard,
    /**
     * Checked with the sending device's key from the card, and the signature does not verify: a forgery, a changed
     * record — or a sender whose signed bytes differ (an app before sign v4, 2026-11-06).
     */
    Mismatch,
    /** Not checkable at all: no sending device of an account, a malformed signature or key (`error` says). */
    Invalid,
  };
  Result result = Result::Invalid;
  /** The card checked with (or the one held, when it lacks the device): its issued_at; none with no card. */
  std::optional<int64_t> card_at;
  /** That card rode the envelope (newer than any kept); else it is the kept one. */
  bool card_carried = false;
  /** A card rode the envelope but did not speak for the sender (not its, not signed, not newer than kept / floor). */
  bool carried_refused = false;
  std::string error;
};

/** An account's newest kept account card — its device list (docs/contracts/ACCOUNT_CARD.md); none when not known. */
using AccountCardLookup = std::function<std::optional<AccountCard>(const std::string& account_id)>;

/** v6 receive pipeline steps 0–12: an inbound envelope verified, decrypted, applied and stored. */
class RelayReceivePipeline : public Module {
public:
  RelayReceivePipeline(IThreadStore& store, AccountCardLookup account_cards, IPeerSessionStore& psk_store,
                       LocalKeys local_keys, IGroupRosterStore& group_roster, GroupInviteGate* invite_gate = nullptr);

  /** The system-control families features bind (call control, …). Not owned; null: none. */
  void SetSystemControls(const SystemControlRegistry* controls) { controls_ = controls; }
  /** Where control-lane ids are remembered (at most once per message); none: no dedupe. */
  void SetControlLaneSeen(IControlLaneSeenStore* seen) { control_lane_seen_ = seen; }
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
   * A verified envelope whose sender says its account card is newer (`sender_card_at`) than the one it was
   * checked with (M021 d6): `(account, sending device, issued_at)` — the caller fetches the newer card.
   */
  void SetOnSenderCardVersion(
      std::function<void(const std::string& account, const std::string& device, int64_t issued_at)> callback) {
    on_sender_card_version_ = std::move(callback);
  }
  /**
   * An envelope that could not be checked: no card kept for its sender, or the sending device not on it, and no
   * card riding it (lane messages carry ours only when the peer may lack it). `(account, sending device,
   * issued_at)` from its `sender_card_at` — the caller asks that device for the card. Unverified: rate-limit it.
   */
  void SetOnUnknownSenderDevice(
      std::function<void(const std::string& account, const std::string& device, int64_t issued_at)> callback) {
    on_unknown_sender_device_ = std::move(callback);
  }
  /**
   * The issue time a carried card of an account must reach to speak for it (PeerCards::Floor): a restored
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
  void SetOnMessageRetracted(
      std::function<void(const std::string& thread_id, const ThreadMessage& original)> callback) {
    on_message_retracted_ = std::move(callback);
  }

  RelayReceiveOutcome ProcessEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                      bool authorized_older_backfill = false,
                                      MessageTransport transport = MessageTransport::Relay);

  /** A held lane message checked again (ReplayHeldControl): the envelope as received, and how it went now. */
  struct HeldControlReplay {
    RelayEnvelope envelope;
    RelayReceiveOutcome outcome;
  };
  /**
   * `sender`'s lane messages held for its card (ControlLaneHold), checked again now that a card of its may be kept
   * (`replay_held_from`, a card kept another way): each as on arrival — freshness counting its wait, first sight,
   * the family's own checks — oldest first. One still not checkable waits again until its time is up.
   */
  std::vector<HeldControlReplay> ReplayHeldControl(const std::string& sender, const std::string& local_relay_user_id);
  /** Whether lane messages of `sender`'s wait for its card. */
  bool HoldsControlFrom(const std::string& sender) const;

private:
  /** A retract from the target's own sender tombstones the target (when it is here already). */
  void ApplyInboundRetract(const ThreadMessage& persisted, RelayReceiveOutcome& outcome);
  /** A message whose retract arrived first is stored as a tombstone straight away. */
  void TombstoneIfRetractedBeforeArrival(ThreadMessage& persisted, RelayReceiveOutcome& outcome);
  std::function<void(const std::string&, const ThreadMessage&)> on_message_retracted_;
  std::function<void(const AccountCard&)> on_sender_card_;
  std::function<void(const std::string&, const std::string&, int64_t)> on_sender_card_version_;
  std::function<void(const std::string&, const std::string&, int64_t)> on_unknown_sender_device_;
  std::function<int64_t(const std::string&)> account_card_floor_;
  std::function<void(const GroupMembershipCodec::MemberJoinedEntry&)> on_roster_member_;

  RelayReceiveOutcome ProcessDirectEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                            bool authorized_older_backfill, MessageTransport transport);
  /**
   * The control lane: verified, decrypted under the pair's e2e_public session, handed to the family
   * that sends it on the lane while fresh and not seen before. Never stored.
   */
  RelayReceiveOutcome ProcessControlEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                             std::optional<int64_t> held_since_ms = std::nullopt);
  /**
   * The lane message's sender check (CheckSignature). When it fails, the warning and the `control.unverified` line
   * say why; one that could not be checked for want of the sender's card is held for it (`held_since_ms`: a replayed
   * one's first hold) — a signature that does not verify is not.
   */
  bool CheckControlSender(const RelayEnvelope& envelope, const DirectChatTarget& target,
                          std::optional<int64_t> held_since_ms, RelayReceiveOutcome& outcome);
  /** The failed check's warning (developer log) and `control.unverified` line. */
  void ReportUnverifiedControl(const RelayEnvelope& envelope, const SenderSignatureCheck& check, bool held) const;
  /** Hold a lane message for its sender's card (ControlLaneHold); false when its time is up (held already: waits). */
  bool HoldForCard(const RelayEnvelope& envelope, std::optional<int64_t> held_since_ms);
  /**
   * A control record from another account with copies for our other devices only (M024): once it verifies, its
   * sender is told our card (`show_card_to`) — it may not know this device yet.
   */
  void NoteMissingControlCopy(const RelayEnvelope& received, const std::string& local_account,
                              RelayReceiveOutcome& outcome);
  /**
   * A verified lane copy that did not open under `target_key`: a shareable `control.unreadable` diag line and a
   * developer-log warning, both with the session we hold and the one the copy offers (AutoKeySessionId, epoch). An
   * account copy also tells the sender our card (`show_card_to`). The lane moves on: an unreadable copy will not
   * become readable, and the next message must not wait for it.
   */
  void ReportUnreadableControl(const RelayEnvelope& envelope, const PeerSessionKey& target_key, const Error& error,
                               RelayReceiveOutcome& outcome) const;
  /**
   * A sibling's control notice (M022: our own account writes to itself), signature already checked: a sent-message
   * copy (M021 d3) is opened and filed as a message we sent; anything else is only its card (the device list).
   */
  void ApplySiblingNotice(const RelayEnvelope& envelope, const std::string& local_account_id,
                          const DirectChatTarget& inbound_target, RelayReceiveOutcome& outcome);
  /** File a sibling's sent message in its chat (public direct, or group), once per message id. */
  void FileSentCopy(const SentCopy& copy, const RelayEnvelope& envelope, RelayReceiveOutcome& outcome);
  /** G014 invitee side: the owner refused our accept (the group was full) — drop the group we made on accept. */
  Roe<void> ApplyInviteRefused(const ThreadMessage& message, const std::string& actor_identity,
                               const std::string& local_account, RelayReceiveOutcome* outcome) const;
  RelayReceiveOutcome ProcessGroupEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                           bool authorized_older_backfill, MessageTransport transport);
  /** The groups `sender` and we are both on this device's roster of (a group copy names its group only inside). */
  std::vector<std::string> GroupsSharedWith(const std::string& sender, const std::string& local_account) const;
  /**
   * Which of `candidates` a group record's stream id is the pair's stream of (G018 item 7) — a hint only (the
   * stream id is not signed): which chat to tell of a copy we could not read, which group a missing copy was for.
   */
  static std::optional<std::string> GroupOfStream(const RelayEnvelope& envelope,
                                                  const std::vector<std::string>& candidates,
                                                  const std::string& local_account);
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
  /** VerifySignature, with why it failed (the card it checked with; a missing card or device apart from a mismatch). */
  SenderSignatureCheck CheckSignature(const RelayEnvelope& envelope, const DirectChatTarget& target) const;
  /** Which copy of a message an envelope is, to this device (M021). */
  enum class CopyTo { ThisDevice, Account, Sibling };
  CopyTo WhoseCopy(const RelayEnvelope& envelope) const;
  /** The KEM secret that opens a copy's key_init: this device's for its device pair, the account's for an account copy. */
  Roe<ByteVector> CopyKemPrivateKey(const RelayEnvelope& envelope) const;
  std::optional<std::string> FindMessageIdAtSeq(const std::string& thread_id, const uint32_t session_epoch,
                                                const std::string& seq_owner_contact_id, const uint64_t sender_seq,
                                                const std::string& sender_device) const;
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
  /**
   * G012: keep the owner-signed roster a roster event carries, when it verifies and is newer, and recompute the
   * roster over it (G018, group_roster_sync::ApplyView).
   */
  void KeepCarriedStatement(const ThreadMessage& message, const std::string& local_account,
                            RelayReceiveOutcome* outcome) const;
  /**
   * After a recompute (G018): the lines for the changes that count now; when a change took us out, the group goes
   * read-only here (removed, G020 3); when we own the group and hold changes, the owner folds them.
   */
  void FinishView(const std::string& group_id, group_roster_sync::GroupViewApplied& applied,
                  const std::string& local_account, RelayReceiveOutcome* outcome) const;
  /** G020 3: we were removed from the group — read-only here, no send target. */
  void MarkRemovedHere(const std::string& group_id) const;
  /**
   * G018: a member's or admin's signed change (`group_change`), from a member: kept when it proves itself (or no card
   * lists its device yet: kept, and the card asked for), the roster recomputed, its line written.
   */
  Roe<void> ApplyGroupChange(ThreadMessage& message, const std::string& actor_identity,
                             const std::string& local_account, RelayReceiveOutcome* outcome) const;
  /**
   * G019: the newcomer an `add` brought in (its card and route ride the `group_change`): kept once the add counts
   * here; the stored copy keeps no card.
   */
  void KeepChangeNewcomer(ThreadMessage& message, const GroupChange& change, const std::string& local_account) const;
  /**
   * G019, the newcomer's side: the join bundle (`member_joined` naming us) from the inviter of an invite we accepted —
   * the owner's statement (verified for the owner the invite named), the changes it carries (each proving itself), the
   * roster computed from them, which must have us; then the members' cards and routes for those it lists. Nothing in
   * it is taken on the inviter's word.
   */
  Roe<void> ApplyJoinBundle(ThreadMessage& message, const GroupMembershipCodec::MemberJoinedPayload& joined,
                            const std::string& actor_identity, const std::string& local_account,
                            RelayReceiveOutcome* outcome) const;
  /**
   * A member_joined's cards and routes, kept for the members `listed` says the roster has (never on the sender's word
   * alone); the stored copy keeps none.
   */
  void KeepBundleCards(ThreadMessage& message, const GroupMembershipCodec::MemberJoinedPayload& joined,
                       const std::string& local_account, const std::function<bool(const std::string&)>& listed) const;
  /** The accept of an invite this device sent (owner, member or admin, G019). */
  Roe<void> ApplyInviteAccepted(const GroupMembershipCodec::InviteResponsePayload& response,
                                const std::string& actor_identity, const std::string& local_account,
                                RelayReceiveOutcome* outcome) const;
  /** G018 item 5: a copy whose sender holds a newer roster than ours asks for a catch-up (rate-limited). */
  void NoteCopyState(const std::string& group_id, const GroupCopyState& theirs, const std::string& sender,
                     RelayReceiveOutcome& outcome);
  /**
   * G017: the owner made a member admin (or member again), or set the preset. The owner-signed roster it carries is
   * what applies (KeepAndCatchUp); the event, refused unless it agrees with that roster, is the group chat's line.
   */
  Roe<void> ApplyGroupSettingChanged(const ThreadMessage& message, GroupMembershipControlType kind,
                                     const std::string& actor_identity, const std::string& local_account) const;
  /**
   * G012: a group_roster_statement from the owner. (The member check's request and reply ride the control
   * lane: MakeGroupRosterSyncFamily.)
   */
  Roe<void> ApplyRosterStatement(const ThreadMessage& message, const std::string& actor_identity,
                                 const std::string& local_account, RelayReceiveOutcome* outcome) const;
  /** G016: the owner ended a group we are in — empty its roster, mark it dissolved, keep its chat with one line. */
  Roe<void> ApplyGroupDissolved(const ThreadMessage& message, const std::string& actor_identity,
                                const std::string& local_account, RelayReceiveOutcome* outcome) const;
  Roe<void> ValidateInboundPskRotate(const RelayEnvelope& envelope, const ThreadMessage& message) const;
  Roe<void> ApplyInboundPskRotate(const std::string& thread_id, const RelayEnvelope& envelope,
                                  const ThreadMessage& message);

  IThreadStore& store_;
  AccountCardLookup account_cards_;
  IPeerSessionStore& psk_store_;
  LocalKeys local_keys_;
  IGroupRosterStore& group_roster_;
  GroupInviteGate* invite_gate_ = nullptr;
  const SystemControlRegistry* controls_ = nullptr;
  IControlLaneSeenStore* control_lane_seen_ = nullptr;

  IContactTrustAccess* contact_trust_ = nullptr;
  bool SenderBlocked(const std::string& sender) const;
  PublicPskLockCoordinator public_lock_;
  group_roster_sync::CatchUpLimiter catch_up_limiter_;
  std::unordered_map<ReplayKey, ReplayWindow, ReplayKeyHash> replay_windows_;
  /** Lane messages waiting for their sender's card; ingest runs on more than one thread (relay poll, links). */
  mutable std::mutex control_hold_mutex_;
  ControlLaneHold control_hold_;
};

} // namespace pbr
