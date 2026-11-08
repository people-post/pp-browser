#include "lib/messaging/RelayReceivePipeline.h"

#include <algorithm>

#include "lib/messaging/SentCopy.h"

#include "lib/people/AccountCard.h"

#include "lib/delivery/DeliveryNotice.h"
#include "lib/messaging/GroupChangeLines.h"
#include "lib/messaging/GroupRosterSync.h"

#include <cstdlib>
#include <sstream>

#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ReactionTypes.h"

#include "lib/delivery/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/chat/ChatPayloadTypes.h"
#include "lib/delivery/ChatPayloadValidator.h"
#include "lib/delivery/E2ePublicSessionLogic.h"
#include "lib/delivery/E2eRelayPayloadCodec.h"
#include "lib/delivery/EnvelopeSigner.h"
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterVersion.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "common/directory/ShortRelayId.h"
#include "common/chat/MessagingLimits.h"
#include "common/chat/RelayStreamKey.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/delivery/PskRotateCodec.h"
#include "lib/delivery/RelayWirePayload.h"
#include "lib/messaging/SyncStateCodec.h"
#include "lib/messaging/GroupInviteGate.h"
#include "lib/delivery/PublicPskLockCoordinator.h"

#include "common/Diag.h"
#include "common/Logger.h"
#include "common/Utilities.h"
#include "lib/delivery/IControlLaneSeenStore.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

Roe<void> RejectIfAnnotationCapExceeded(IThreadStore& store, const ThreadMessage& persisted) {
  if (persisted.content_type != ChatContentType::Annotation) {
    return {};
  }
  const std::string target_id = persisted.target_message_id.value_or("");
  if (target_id.empty()) {
    return {};
  }
  if (auto fields = ChatPayloadCodec::DecodeAnnotationJson(persisted.payload_json);
      fields && fields->annotation_type == kAnnotationTypeRetract) {
    return {}; // the cap guards against reaction floods; it must never stop a delete
  }
  auto count = store.CountAnnotationsForTarget(persisted.thread_id, target_id);
  if (!count) {
    return count.error();
  }
  if (static_cast<size_t>(*count) >= kMaxAnnotationsPerTarget) {
    return Error("Annotation cap exceeded for target");
  }
  return {};
}

bool IsEnvelopeFromPeer(const Thread& thread, const RelayEnvelope& envelope) {
  if (!thread.peer_identity_value.empty()) {
    return envelope.sender_contact_id == thread.peer_identity_value ||
           envelope.sender_relay_id == thread.peer_identity_value;
  }
  return false;
}

DirectChatTarget InboundTargetFromEnvelope(const RelayEnvelope& envelope) {
  DirectChatTarget target;
  target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  target.peer_identity_value = envelope.sender_contact_id;
  target.channel = envelope.route.channel;
  return target;
}

/**
 * The session a copy is read under (M021): the pair with the sending device for a copy to this device,
 * the account-level session for an account copy.
 */
PeerSessionKey CopyTargetKey(const RelayEnvelope& envelope, const DirectChatTarget& sender, const CryptoChannel channel) {
  PeerSessionKey key;
  key.peer_identity_kind = sender.peer_identity_kind;
  key.peer_identity_value = sender.peer_identity_value;
  key.channel = channel;
  if (!envelope.recipient_device.empty()) {
    key.peer_device = envelope.sender_device;
  }
  return key;
}

/**
 * Whose numbering a copy is in (M021): the sending device's in its pair with us; for an account copy that
 * device's account copies (two linked devices both write to a contact that knows neither yet — each numbers
 * its own). A private chat is one device's (M014): the account's numbering, which its integrity checks read.
 */
std::string CopyNumbering(const RelayEnvelope& envelope) {
  if (!envelope.recipient_device.empty()) {
    return envelope.sender_device;
  }
  if (envelope.route.kind == kRelayRouteDirect && envelope.route.channel == ThreadChannel::E2e) {
    return {};
  }
  return envelope.sender_device + "@account";
}

/** A row's sending device: the device pair's, none for an account copy. */
std::optional<std::string> RowDevice(const std::string& numbering) {
  return numbering.empty() ? std::nullopt : std::optional<std::string>(numbering);
}

void MarkReceiveFailure(RelayReceiveOutcome& outcome, const std::string& sender, const ReceiveFailure reason,
                        const std::string& detail, const std::optional<std::string>& thread_id = std::nullopt,
                        const std::string& family = {}) {
  outcome.receive_failure = reason;
  outcome.receive_failure_family = family;
  outcome.receive_failure_detail = detail.empty() ? std::string(ReceiveFailureWire(reason)) : detail;
  if (!sender.empty()) {
    outcome.receive_failure_sender = sender;
  }
  if (thread_id && !thread_id->empty()) {
    outcome.receive_failure_thread_id = *thread_id;
  }
}

/** An invite refused for want of a contact is told to the user apart from other membership failures. */
ReceiveFailure MembershipFailure(const Error& err) {
  return err.message == kGroupInviteNeedsContactError ? ReceiveFailure::GroupInviteNeedsContact
                                                       : ReceiveFailure::MembershipUpdateFailed;
}

/** Membership DMs must apply even when the 1:1 seq stream is SoftCompromised. */
bool IsGroupMembershipControlMessage(const ThreadMessage& message) {
  return GroupMembershipCodec::ControlTypeFromMessage(message).has_value();
}

} // namespace

RelayReceivePipeline::RelayReceivePipeline(IThreadStore& store, AccountCardLookup account_cards,
                                           IPeerSessionStore& psk_store, LocalKeys local_keys,
                                           IGroupRosterStore& group_roster, GroupInviteGate* invite_gate)
    : store_(store), account_cards_(std::move(account_cards)), psk_store_(psk_store), local_keys_(std::move(local_keys)),
      group_roster_(group_roster), invite_gate_(invite_gate), public_lock_(store, psk_store) {
  redirectLogger("RelayReceivePipeline");
}

Roe<void> RelayReceivePipeline::ApplyInboundControl(ThreadMessage& message, const RelayEnvelope& envelope,
                                                    RelayReceiveOutcome* outcome_on_failure,
                                                    const std::string& thread_id) const {
  if (!controls_) {
    return {};
  }
  const auto family = controls_->Find(message);
  if (!family || !family->apply) {
    return {};
  }
  SystemControlContext context{envelope.sender_contact_id, envelope.relay_created_at_ms, envelope.relay_server_time_ms};
  auto applied = family->apply(message, context);
  if (!applied && outcome_on_failure) {
    MarkReceiveFailure(*outcome_on_failure, envelope.sender_contact_id, ReceiveFailure::ApplyFailed,
                       applied.error().message, thread_id, family->name);
  }
  return applied;
}

bool RelayReceivePipeline::AppliesOutOfOrder(const ThreadMessage& message) const {
  return IsGroupMembershipControlMessage(message);
}

void RelayReceivePipeline::ApplyOutOfOrderControls(ThreadMessage& message, const RelayEnvelope& envelope,
                                                   const IngestDecision decision, RelayReceiveOutcome& outcome) const {
  if (IsGroupMembershipControlMessage(message)) {
    log().info << "Apply membership out of order (" << static_cast<int>(decision)
               << ") message_id=" << envelope.message_id;
    (void)ApplyInboundMembershipMessage(message, envelope.sender_contact_id, &outcome);
  }
}

void RelayReceivePipeline::KeepCarriedStatement(const ThreadMessage& message, const std::string& local_account,
                                                RelayReceiveOutcome* outcome) const {
  auto detail = group_roster_sync_codec::DetailOf(message);
  auto statement = detail ? group_roster_sync_codec::StatementIn(*detail) : std::nullopt;
  if (!statement) {
    return;  // an event from an app before G012
  }
  group_roster_sync::GroupViewApplied applied;
  if (auto kept =
          group_roster_sync::KeepAndCatchUp(group_roster_, *statement, account_cards_, util::NowUnixMs(), &applied);
      !kept) {
    log().warning << "Roster statement not kept group_id=" << statement->group_id << ": " << kept.error().message;
    return;
  }
  FinishView(statement->group_id, applied, local_account, outcome);
}

void RelayReceivePipeline::MarkRemovedHere(const std::string& group_id) const {
  auto lock = IGroupRosterStore::LockForUpdate();
  (void)group_roster_.ClearGroupTarget(group_id);
  if (auto metadata = group_roster_.LoadMetadata(group_id);
      metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Active) {
    GroupMetadata removed = **metadata;
    removed.sync_state = GroupSyncState::Removed;
    (void)group_roster_.UpsertMetadata(removed);
  }
}

void RelayReceivePipeline::FinishView(const std::string& group_id, group_roster_sync::GroupViewApplied& applied,
                                      const std::string& local_account, RelayReceiveOutcome* outcome) const {
  auto metadata = group_roster_.LoadMetadata(group_id);
  if (!metadata || !*metadata) {
    return;
  }
  if (applied.new_title) {
    (void)group_change_lines::FollowTitle(store_, group_id, *applied.new_title); // a rename that counts (G017)
  }
  auto about_us =
      group_change_lines::Append(store_, group_id, applied.newly_applied, (*metadata)->roster_epoch, local_account);
  if (!about_us) {
    log().warning << "group change lines not written group_id=" << group_id << ": " << about_us.error().message;
  }
  const bool we_left = !local_account.empty() &&
                       std::find(applied.gone.begin(), applied.gone.end(), local_account) != applied.gone.end();
  if (we_left) {
    MarkRemovedHere(group_id);
    if (outcome && about_us && *about_us) {
      outcome->about_us = RelayReceiveOutcome::AboutUs::Removed;
      outcome->about_us_group_thread_id = **about_us;
    }
    return;
  }
  if (outcome && (*metadata)->owner_identity == local_account) {
    if (auto held = group_roster_.ListGroupChanges(group_id); held && !held->empty()) {
      outcome->fold_group_id = group_id;
    }
  }
}

void RelayReceivePipeline::KeepChangeNewcomer(ThreadMessage& message, const GroupChange& change,
                                              const std::string& local_account) const {
  auto detail = group_roster_sync_codec::DetailOf(message);
  auto newcomer = detail ? group_roster_sync_codec::NewcomerIn(*detail) : std::nullopt;
  if (!newcomer) {
    return;
  }
  auto in = group_roster_.IsMember(change.group_id, change.member);
  if (change.kind == GroupChangeKind::Add && newcomer->member_identity == change.member &&
      change.member != local_account && in && *in && on_roster_member_) {
    on_roster_member_({.member_identity = newcomer->member_identity,
                       .card = newcomer->card,
                       .relay_user_id = newcomer->relay_user_id});
  }
  // What is stored keeps the change, not the card (it is kept as a card).
  Object payload;
  payload.set("control_type", GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupChange));
  payload.set("detail", group_roster_sync_codec::EncodeChange(change));
  message.payload_json = DumpJson(payload);
}

Roe<void> RelayReceivePipeline::ApplyGroupChange(ThreadMessage& message, const std::string& actor_identity,
                                                 const std::string& local_account, RelayReceiveOutcome* outcome) const {
  if (outcome) {
    outcome->suppress_inbox_chrome = true; // the DM carries it; the group's chat gets the line
  }
  auto detail = group_roster_sync_codec::DetailOf(message);
  if (!detail) {
    return Error("group_change: no detail");
  }
  auto change = group_roster_sync_codec::DecodeChange(*detail);
  if (!change) {
    return change.error();
  }
  auto member = group_roster_.IsMember(change->group_id, local_account);
  if (!member) {
    return member.error();
  }
  if (local_account.empty() || !*member) {
    return {}; // not our group (any more)
  }
  // From the signer's device, or a member handing it on: the change speaks for its signer only (checked below).
  if (actor_identity != change->signer) {
    if (auto sender = group_roster_.IsMember(change->group_id, actor_identity); !sender || !*sender) {
      return Error("group_change: handed on by someone outside the group");
    }
  }
  const int64_t now_ms = util::NowUnixMs();
  auto received = group_roster_sync::ReceiveChange(group_roster_, *change, account_cards_, now_ms);
  if (!received) {
    return received.error();
  }
  if (received->held == group_roster_sync::ChangeHeld::Refused) {
    return Error("group_change: it does not prove itself (forged, damaged, or the group holds too many)");
  }
  if (received->held == group_roster_sync::ChangeHeld::NoCard && on_unknown_sender_device_) {
    on_unknown_sender_device_(change->signer, change->signer_device, now_ms); // verified once its card is here
  }
  if (received->held == group_roster_sync::ChangeHeld::Proved && change->signer_card && on_sender_card_ &&
      change->signer_card->keys.account_id == change->signer && VerifyAccountCard(*change->signer_card, now_ms)) {
    on_sender_card_(*change->signer_card); // newer than ours, it is kept
  }
  log().info << "group change " << group_change::KindToString(change->kind) << " group_id=" << change->group_id
             << " by=" << change->signer
             << (received->held == group_roster_sync::ChangeHeld::NoCard ? " (no card yet)" : "");
  FinishView(change->group_id, received->applied, local_account, outcome);
  KeepChangeNewcomer(message, *change, local_account);
  return {};
}

Roe<void> RelayReceivePipeline::ApplyJoinBundle(ThreadMessage& message,
                                                const GroupMembershipCodec::MemberJoinedPayload& joined,
                                                const std::string& actor_identity, const std::string& local_account,
                                                RelayReceiveOutcome* outcome) const {
  auto metadata = group_roster_.LoadMetadata(joined.group_id);
  if (!metadata || !*metadata) {
    return Error("join bundle: a group we do not hold");
  }
  // From the inviter of an invite of ours we accepted — or, from an owner that names none, the owner we know.
  bool from_inviter = false;
  if (!joined.invite_nonce.empty()) {
    auto pending = group_roster_.LoadPendingInvite(joined.invite_nonce);
    from_inviter = pending && *pending && !(*pending)->outbound && (*pending)->inviter_identity == actor_identity &&
                   (*pending)->group_id == joined.group_id && (*pending)->invitee_identity == local_account;
  }
  if (!from_inviter && (*metadata)->owner_identity != actor_identity) {
    return Error("join bundle: not from the one who invited us");
  }
  auto detail = group_roster_sync_codec::DetailOf(message);
  auto statement = detail ? group_roster_sync_codec::StatementIn(*detail) : std::nullopt;
  if (!statement) {
    if ((*metadata)->owner_identity != actor_identity) {
      return Error("join bundle: no owner statement");
    }
    // The owner's own word, as members take its member_joined (an owner that could not sign): the roster it states.
    if (auto applied = ApplyMemberJoinedToRoster(group_roster_, joined, actor_identity); !applied) {
      return applied.error();
    }
    KeepBundleCards(message, joined, local_account, [this, &joined](const std::string& identity) {
      auto in = group_roster_.IsMember(joined.group_id, identity);
      return in && *in;
    });
    return {};
  }
  const int64_t now_ms = util::NowUnixMs();
  const group_roster_sync_codec::CarriedChanges carried = group_roster_sync_codec::ChangesIn(*detail, joined.group_id);
  // The owner's statement, checked for the owner the invite named, and the changes, each proving itself, must give a
  // roster with us in it (G018 item 6) — before anything is kept: the inviter can make up neither.
  if (auto proves = group_roster_sync::CheckJoin(group_roster_, *statement, carried.changes, account_cards_, now_ms,
                                                 local_account);
      !proves) {
    return Error("join bundle: " + proves.error().message);
  }
  if (auto kept = group_roster_sync::KeepStatement(group_roster_, *statement); !kept) {
    return Error("join bundle: " + kept.error().message);
  }
  // What happened before we joined gets no line here.
  if (auto held = group_roster_sync::HoldCarried(group_roster_, carried.changes, account_cards_, now_ms, true); !held) {
    return held.error();
  }
  auto applied = group_roster_sync::ApplyView(group_roster_, joined.group_id, account_cards_, now_ms);
  if (!applied) {
    return applied.error();
  }
  if (!applied->view || !applied->view->Lists(local_account)) {
    return Error("join bundle: the roster it proves does not have us");
  }
  KeepBundleCards(message, joined, local_account,
                  [&view = *applied->view](const std::string& identity) { return view.Lists(identity); });
  if (carried.more && outcome) {
    outcome->catch_up_group_id = joined.group_id; // the rest of the changes, from the inviter
    outcome->catch_up_ask = actor_identity;
  }
  FinishView(joined.group_id, *applied, local_account, outcome);
  log().info << "joined group_id=" << joined.group_id << " from=" << actor_identity
             << " members=" << applied->view->members.size() << " changes=" << carried.changes.size();
  return {};
}

void RelayReceivePipeline::KeepBundleCards(ThreadMessage& message,
                                           const GroupMembershipCodec::MemberJoinedPayload& joined,
                                           const std::string& local_account,
                                           const std::function<bool(const std::string&)>& listed) const {
  bool carried_cards = false;
  for (const GroupMembershipCodec::MemberJoinedEntry& entry : joined.members) {
    carried_cards = carried_cards || entry.card.has_value();
    // Only for those the roster we computed lists: the inviter's word adds no one.
    if (entry.member_identity != local_account && listed(entry.member_identity) &&
        (entry.card || !entry.relay_user_id.empty()) && on_roster_member_) {
      on_roster_member_(entry);
    }
  }
  if (!carried_cards) {
    return;
  }
  // What is stored keeps the roster, not the cards (kept as cards).
  if (auto stripped = GroupMembershipCodec::WithoutCards(joined); stripped) {
    Object payload;
    payload.set("control_type", GroupMembershipControlTypeToWire(GroupMembershipControlType::MemberJoined));
    payload.set("detail", *stripped);
    message.payload_json = DumpJson(payload);
  }
}

void RelayReceivePipeline::NoteCopyState(const std::string& group_id, const GroupCopyState& theirs,
                                         const std::string& sender, RelayReceiveOutcome& outcome) {
  const GroupCopyState ours = group_roster_sync::CopyState(group_roster_, group_id);
  if (!group_roster_sync::NeedsCatchUp(ours, theirs) || !catch_up_limiter_.Allow(group_id, util::NowUnixMs())) {
    return;
  }
  log().info << "group copy from " << sender << " holds a newer roster (statement " << theirs.statement_epoch
             << ", ours " << ours.statement_epoch << ") group_id=" << group_id;
  outcome.catch_up_group_id = group_id;
  outcome.catch_up_ask = sender;
}

Roe<void> RelayReceivePipeline::ApplyGroupSettingChanged(const ThreadMessage& message,
                                                         const GroupMembershipControlType kind,
                                                         const std::string& actor_identity,
                                                         const std::string& local_account) const {
  std::string group_id;
  std::string detail_json;
  std::string display;
  std::function<bool(const GroupRosterStatement&)> agrees;
  if (kind == GroupMembershipControlType::MemberRoleChanged) {
    auto changed = GroupMembershipCodec::DecodeMemberRoleChangedFromMessage(message);
    if (!changed) {
      return changed.error();
    }
    group_id = changed->group_id;
    auto encoded = GroupMembershipCodec::EncodeMemberRoleChanged(*changed);
    if (!encoded) {
      return encoded.error();
    }
    detail_json = *encoded;
    display = changed->role == MemberRole::Admin ? "Member made admin" : "Member no longer admin";
    agrees = [changed = *changed](const GroupRosterStatement& statement) {
      return std::any_of(statement.members.begin(), statement.members.end(), [&](const GroupRosterStatementMember& m) {
        return m.identity == changed.member_identity && m.role == changed.role;
      });
    };
  } else {
    auto changed = GroupMembershipCodec::DecodeGroupPresetChangedFromMessage(message);
    if (!changed) {
      return changed.error();
    }
    group_id = changed->group_id;
    auto encoded = GroupMembershipCodec::EncodeGroupPresetChanged(*changed);
    if (!encoded) {
      return encoded.error();
    }
    detail_json = *encoded;
    display = changed->preset == GroupPreset::Managed ? "Group set to managed" : "Group set to open";
    agrees = [preset = changed->preset](const GroupRosterStatement& statement) { return statement.preset == preset; };
  }
  auto member = group_roster_.IsMember(group_id, local_account);
  if (!member || !*member) {
    return {}; // not our group (any more)
  }
  auto detail = group_roster_sync_codec::DetailOf(message);
  auto statement = detail ? group_roster_sync_codec::StatementIn(*detail) : std::nullopt;
  if (!statement || statement->group_id != group_id || statement->owner_identity != actor_identity) {
    return Error("group setting: not from the owner its roster names");
  }
  if (!agrees(*statement)) {
    return Error("group setting: the event does not match the roster it carries");
  }
  group_roster_sync::GroupViewApplied applied;
  auto kept = group_roster_sync::KeepAndCatchUp(group_roster_, *statement, account_cards_, util::NowUnixMs(), &applied);
  if (!kept) {
    return kept.error();
  }
  if (!*kept) {
    return {}; // an older roster than ours: nothing changes, no line
  }
  FinishView(group_id, applied, local_account, nullptr);
  auto thread = store_.FindGroupThread(group_id);
  if (thread && *thread) {
    if (auto sys =
            GroupMembershipCodec::BuildSystemMessage((*thread)->id, kind, display, detail_json, actor_identity)) {
      (void)store_.AppendMessage(*sys);
    }
  }
  return {};
}

Roe<void> RelayReceivePipeline::ApplyRosterStatement(const ThreadMessage& message,
                                                     const std::string& actor_identity,
                                                     const std::string& local_account,
                                                     RelayReceiveOutcome* outcome) const {
  if (outcome) {
    outcome->suppress_inbox_chrome = true;  // bookkeeping between members, not something to read
  }
  auto detail = group_roster_sync_codec::DetailOf(message);
  if (!detail) {
    return Error("roster statement: no detail");
  }
  auto statement = group_roster_sync_codec::DecodeStatement(*detail);
  if (!statement) {
    return statement.error();
  }
  auto member = group_roster_.IsMember(statement->group_id, local_account);
  if (!member || !*member || statement->owner_identity != actor_identity) {
    return {};  // not our group, or not sent by the owner it names
  }
  group_roster_sync::GroupViewApplied applied;
  if (auto kept =
          group_roster_sync::KeepAndCatchUp(group_roster_, *statement, account_cards_, util::NowUnixMs(), &applied);
      !kept) {
    log().warning << "Roster statement not kept group_id=" << statement->group_id << ": " << kept.error().message;
    return {};
  }
  FinishView(statement->group_id, applied, local_account, outcome);
  return {};
}

Roe<void> RelayReceivePipeline::ApplyGroupDissolved(const ThreadMessage& message, const std::string& actor_identity,
                                                    const std::string& local_account,
                                                    RelayReceiveOutcome* outcome) const {
  if (outcome) {
    outcome->suppress_inbox_chrome = true;  // the DM that carried it is bookkeeping; the group's chat gets the notice
  }
  auto detail = group_roster_sync_codec::DetailOf(message);
  if (!detail) {
    return Error("group_dissolved: no detail");
  }
  auto statement = group_roster_sync_codec::DecodeStatement(*detail);
  if (!statement) {
    return statement.error();
  }
  auto applied = ApplyGroupDissolvedToRoster(group_roster_, *statement, actor_identity, local_account);
  if (!applied) {
    return applied.error();
  }
  if (applied->applied) {
    log().info << "group dissolved group_id=" << statement->group_id;
  }
  // The line is written after the roster is: a copy redelivered after a crash in between finishes it. Only the
  // owner's own copy of a dissolve may do so, whatever else arrives for a group that is over.
  auto metadata = group_roster_.LoadMetadata(statement->group_id);
  if (metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Dissolved &&
      (*metadata)->owner_identity == actor_identity) {
    if (auto added = EnsureDissolvedLine(store_, statement->group_id, actor_identity, actor_identity);
        added && *added && outcome) {
      outcome->dissolved_group_thread_id = *added;
    }
  }
  if (outcome) {
    for (const PendingGroupInvite& invite : applied->voided_invites) {
      if (!invite.outbound) {
        outcome->voided_invite_cards.emplace_back(invite.inviter_identity, invite.invite_nonce);
      }
    }
  }
  return {};
}

void RelayReceivePipeline::MaybeSuppressInboxChrome(RelayReceiveOutcome& outcome, const ThreadMessage& message) const {
  if (controls_ && controls_->Quiet(message)) {
    outcome.suppress_inbox_chrome = true;
  }
  // A membership event arrives in the direct chat with its sender (the group thread has its own line): no unread
  // count, preview or notice for it there. Only the invitation is news in that chat.
  if (GroupMembershipCodec::IsHiddenInDirectThread(message)) {
    outcome.suppress_inbox_chrome = true;
  }
}

Roe<void> RelayReceivePipeline::ApplyInviteRefused(const ThreadMessage& message, const std::string& actor_identity,
                                                  const std::string& local_account,
                                                  RelayReceiveOutcome* outcome) const {
  auto refused = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
  if (!refused) {
    return refused.error();
  }
  // Only for an invite we received from this owner, for this group: anyone else's word drops nothing.
  auto pending = group_roster_.LoadPendingInvite(refused->invite_nonce);
  if (!pending || !*pending || (*pending)->outbound || (*pending)->inviter_identity != actor_identity ||
      (*pending)->group_id != refused->group_id) {
    return Error("group_invite_refused for no invite of ours from its sender");
  }
  (void)group_roster_.UpdateInviteStatus(refused->invite_nonce, InviteStatus::Expired);
  if (refused->reason == kInviteRefusedGroupDissolved) {
    // G016: the owner ended the group before our accept reached it. If we had already made the group on accept, it
    // ends here too — the chat stays, saying so; the invite's card ends either way.
    auto ended = EndGroupInRoster(group_roster_, refused->group_id, actor_identity, 0);
    if (ended && ended->applied) {
      if (auto added = EnsureDissolvedLine(store_, refused->group_id, actor_identity, actor_identity);
          added && *added && outcome) {
        outcome->dissolved_group_thread_id = *added;
      }
    }
    if (outcome) {
      outcome->voided_invite_cards.emplace_back(actor_identity, refused->invite_nonce);
    }
    return {};
  }
  // G014 (or G019: the inviter may no longer add us): we were never added — drop the group we made on accept; its
  // chat stays, read-only, saying why.
  if (!local_account.empty()) {
    (void)group_roster_.RemoveMember(refused->group_id, local_account);
  }
  (void)group_roster_.ClearGroupTarget(refused->group_id);
  if (auto metadata = group_roster_.LoadMetadata(refused->group_id); metadata && *metadata) {
    GroupMetadata removed = **metadata;
    removed.sync_state = GroupSyncState::Removed;
    (void)group_roster_.UpsertMetadata(removed);
  }
  if (auto thread = store_.FindGroupThread(refused->group_id); thread && *thread) {
    auto detail = GroupMembershipCodec::EncodeInviteRefused(refused->invite_nonce, refused->group_id,
                                                           refused->reason);
    if (detail) {
      const char* line = refused->reason == kInviteRefusedNotAllowed ? "You were not added to the group"
                                                                     : "The group is full: you were not added";
      if (auto sys = GroupMembershipCodec::BuildSystemMessage(
              (*thread)->id, GroupMembershipControlType::GroupInviteRefused, line, *detail, actor_identity)) {
        (void)store_.AppendMessage(*sys);
        if (outcome) {
          outcome->about_us = RelayReceiveOutcome::AboutUs::GroupFull;
          outcome->about_us_group_thread_id = (*thread)->id;
        }
      }
    }
  }
  log().info << "group invite refused group_id=" << refused->group_id << " reason=" << refused->reason;
  return {};
}

Roe<void> RelayReceivePipeline::ApplyInviteAccepted(const GroupMembershipCodec::InviteResponsePayload& response,
                                                    const std::string& actor_identity, const std::string& local_account,
                                                    RelayReceiveOutcome* outcome) const {
  // An invite this device issued is required: as the owner it adds them (then publishes member_joined); as a member
  // or admin it signs the add (G019).
  auto applied = ApplyInviteAcceptToRoster(group_roster_, response.invite_nonce, actor_identity, response.group_id,
                                           local_account, util::NowUnixMs());
  if (!applied) {
    const std::string& why = applied.error().message;
    const char* reason = why == kGroupFullError                           ? kInviteRefusedGroupFull
                         : why == group_roster_sync::kGroupDissolvedError ? kInviteRefusedGroupDissolved
                         : why == kGroupInviteNotAllowedError             ? kInviteRefusedNotAllowed
                                                                          : nullptr;
    if (!reason) {
      return applied.error();
    }
    if (outcome) {
      outcome->refuse_invite_group_id = response.group_id;
      outcome->refuse_invite_nonce = response.invite_nonce;
      outcome->refuse_invite_member = actor_identity;
      outcome->refuse_invite_reason = reason;
    }
    if (why == kGroupInviteNotAllowedError) {
      (void)group_roster_.UpdateInviteStatus(response.invite_nonce, InviteStatus::Expired);
    }
    return {};
  }
  if (!outcome || *applied == InviteAccepted::Already) {
    return {}; // already accepted: nothing new to apply or announce
  }
  if (*applied == InviteAccepted::SignAdd) {
    outcome->admit_group_id = response.group_id;
    outcome->admit_invite_nonce = response.invite_nonce;
    outcome->admit_member = actor_identity;
    return {};
  }
  if (auto metadata = group_roster_.LoadMetadata(response.group_id); metadata && *metadata) {
    outcome->publish_member_joined_group_id = response.group_id;
    outcome->publish_member_joined_member_identity = actor_identity;
    outcome->publish_member_joined_epoch = (*metadata)->roster_epoch;
    outcome->publish_member_joined_invite_nonce = response.invite_nonce;
  }
  return {};
}

Roe<void> RelayReceivePipeline::ApplyInboundMembershipMessage(ThreadMessage& message,
                                                              const std::string& actor_identity,
                                                              RelayReceiveOutcome* outcome) const {
  // Resolve group_id from any membership control we understand, then ignore events for groups
  // the local user has already left/dismissed (prevents transfer-to-leaver resurrection).
  auto local_id = local_keys_.account_id();
  const std::string local_account = local_id ? *local_id : std::string();

  // Each kind is decoded by its own branch; a decode error there is a real error.
  const std::optional<GroupMembershipControlType> kind = GroupMembershipCodec::ControlTypeFromMessage(message);
  // Whatever this event changes is applied when the function returns: the composer's `@` lines see a new version.
  struct BumpOnExit {
    bool active;
    ~BumpOnExit() {
      if (active) {
        GroupRosterVersion::Bump();
      }
    }
  } bump_on_exit{kind.has_value()};

  auto ignore_if_not_member = [this, &local_account](const std::string& group_id) -> Roe<bool> {
    if (local_account.empty() || group_id.empty()) {
      return false;
    }
    auto is_member = group_roster_.IsMember(group_id, local_account);
    if (!is_member) {
      return is_member.error();
    }
    return !*is_member; // true => ignore
  };

  if (kind == GroupMembershipControlType::GroupInviteRefused) {
    return ApplyInviteRefused(message, actor_identity, local_account, outcome);
  }
  if (kind == GroupMembershipControlType::GroupInviteAccept ||
      kind == GroupMembershipControlType::GroupInviteDecline) {
    auto response = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
    if (!response) {
      return response.error();
    }
    if (response->control_type == GroupMembershipControlType::GroupInviteAccept) {
      return ApplyInviteAccepted(*response, actor_identity, local_account, outcome);
    }
    if (auto declined = ApplyInviteDeclineToRoster(group_roster_, response->invite_nonce, actor_identity,
                                                   response->group_id);
        !declined) {
      return declined.error();
    }
    // The decline arrived in the direct chat, where it is not shown: the inviter's record of it is a line in the
    // group's thread.
    if (auto thread = store_.FindGroupThread(response->group_id); thread && *thread) {
      if (auto detail = GroupMembershipCodec::EncodeInviteResponse(response->invite_nonce, response->group_id)) {
        if (auto sys = GroupMembershipCodec::BuildSystemMessage((*thread)->id,
                                                                GroupMembershipControlType::GroupInviteDecline,
                                                                "Invitation declined", *detail, actor_identity)) {
          (void)store_.AppendMessage(*sys);
        }
      }
    }
    return {};
  }

  if (kind == GroupMembershipControlType::MemberJoined) {
    auto joined = GroupMembershipCodec::DecodeMemberJoinedFromMessage(message);
    if (!joined) {
      return joined.error();
    }
    if (auto ignore = ignore_if_not_member(joined->group_id); ignore && *ignore) {
      return {};
    }
    if (!local_account.empty() && joined->member_identity == local_account) {
      return ApplyJoinBundle(message, *joined, actor_identity, local_account, outcome); // G019: we joined
    }
    if (auto applied = ApplyMemberJoinedToRoster(group_roster_, *joined, actor_identity); !applied) {
      return applied.error();
    }
    KeepCarriedStatement(message, local_account, outcome);
    // What the owner says of the other members: kept for those on the roster now.
    KeepBundleCards(message, *joined, local_account, [this, &joined](const std::string& identity) {
      auto in = group_roster_.IsMember(joined->group_id, identity);
      return in && *in;
    });
    auto thread = store_.FindGroupThread(joined->group_id);
    // The newcomer wrote "You joined the group" itself when it accepted: this is the same join, not a second line.
    if (thread && *thread && joined->member_identity != local_account) {
      GroupMembershipCodec::MemberJoinedPayload line = *joined;
      line.members.clear();
      line.invite_nonce.clear();
      auto detail = GroupMembershipCodec::EncodeMemberJoined(line); // with who added them (G019 5)
      if (detail) {
        auto sys = GroupMembershipCodec::BuildSystemMessage(
            (*thread)->id, GroupMembershipControlType::MemberJoined, "Member joined the group", *detail,
            actor_identity);
        if (sys) {
          (void)store_.AppendMessage(*sys);
        }
      }
    }
    return {};
  }

  if (kind == GroupMembershipControlType::OwnerTransferred) {
    auto transferred = GroupMembershipCodec::DecodeOwnerTransferredFromMessage(message);
    if (!transferred) {
      return transferred.error();
    }
    if (auto ignore = ignore_if_not_member(transferred->group_id); ignore && *ignore) {
      return {};
    }
    if (auto applied = ApplyOwnerTransferredToRoster(group_roster_, *transferred, actor_identity); !applied) {
      return applied.error();
    }
    if (outcome && transferred->new_owner_identity == local_account) {
      if (auto detail = group_roster_sync_codec::DetailOf(message)) {
        if (auto handed = group_roster_sync_codec::TransferIn(*detail)) {
          outcome->took_ownership_group_id = transferred->group_id;
          outcome->took_ownership_transfer = std::move(*handed);
        }
      }
    }
    auto thread = store_.FindGroupThread(transferred->group_id);
    if (thread && *thread) {
      auto detail = GroupMembershipCodec::EncodeOwnerTransferred(
          transferred->group_id, transferred->new_owner_identity, transferred->roster_epoch,
          transferred->leave_previous);
      if (detail) {
        const std::string text = transferred->leave_previous ? "Ownership transferred; previous owner left"
                                                             : "Group ownership transferred";
        auto sys = GroupMembershipCodec::BuildSystemMessage((*thread)->id,
                                                            GroupMembershipControlType::OwnerTransferred, text,
                                                            *detail, actor_identity);
        if (sys) {
          (void)store_.AppendMessage(*sys);
        }
      }
    }
    return {};
  }

  if (kind == GroupMembershipControlType::GroupChange) {
    return ApplyGroupChange(message, actor_identity, local_account, outcome);
  }

  if (kind == GroupMembershipControlType::MemberRemoved) {
    auto removed = GroupMembershipCodec::DecodeMemberRemovedFromMessage(message);
    if (!removed) {
      return removed.error();
    }
    // If we are the removed member and already left, ignore; if still a member, drop self below.
    if (auto ignore = ignore_if_not_member(removed->group_id); ignore && *ignore) {
      return {};
    }
    if (auto applied = ApplyMemberRemovedToRoster(group_roster_, *removed, actor_identity); !applied) {
      return applied.error();
    }
    KeepCarriedStatement(message, local_account, outcome);
    if (!local_account.empty() && removed->member_identity == local_account) {
      MarkRemovedHere(removed->group_id); // G020 3: the chat stays, read-only
    }
    auto thread = store_.FindGroupThread(removed->group_id);
    if (thread && *thread) {
      auto detail = GroupMembershipCodec::EncodeMemberRemoved(removed->group_id, removed->member_identity,
                                                              removed->roster_epoch, actor_identity);
      if (detail) {
        auto sys = GroupMembershipCodec::BuildSystemMessage(
            (*thread)->id, GroupMembershipControlType::MemberRemoved, "Member removed", *detail, actor_identity);
        if (sys) {
          (void)store_.AppendMessage(*sys);
          if (outcome && !local_account.empty() && removed->member_identity == local_account) {
            outcome->about_us = RelayReceiveOutcome::AboutUs::Removed;
            outcome->about_us_group_thread_id = (*thread)->id;
          }
        }
      }
    }
    return {};
  }

  if (kind == GroupMembershipControlType::GroupRenamed) {
    auto renamed = GroupMembershipCodec::DecodeGroupRenamedFromMessage(message);
    if (!renamed) {
      return renamed.error();
    }
    if (auto ignore = ignore_if_not_member(renamed->group_id); ignore && *ignore) {
      return {};
    }
    auto applied = ApplyGroupRenamedToRoster(group_roster_, *renamed, actor_identity);
    if (!applied) {
      return applied.error();
    }
    if (!*applied) {
      return {};  // not newer than the title we hold
    }
    auto thread = store_.FindGroupThread(renamed->group_id);
    if (thread && *thread) {
      Thread row = **thread;
      row.title = renamed->title;
      row.updated_at = util::NowUnixMs();
      (void)store_.UpsertThread(row);
      auto detail =
          GroupMembershipCodec::EncodeGroupRenamed(renamed->group_id, renamed->title, renamed->roster_epoch,
                                                   renamed->title_version);
      if (detail) {
        auto sys = GroupMembershipCodec::BuildSystemMessage(row.id, GroupMembershipControlType::GroupRenamed,
                                                            "Group renamed to " + renamed->title, *detail,
                                                            actor_identity);
        if (sys) {
          (void)store_.AppendMessage(*sys);
        }
      }
    }
    return {};
  }

  if (kind == GroupMembershipControlType::GroupRosterStatement) {
    return ApplyRosterStatement(message, actor_identity, local_account, outcome);
  }

  if (kind == GroupMembershipControlType::MemberRoleChanged || kind == GroupMembershipControlType::GroupPresetChanged) {
    return ApplyGroupSettingChanged(message, *kind, actor_identity, local_account);
  }

  if (kind == GroupMembershipControlType::GroupDissolved) {
    return ApplyGroupDissolved(message, actor_identity, local_account, outcome);
  }

  if (!invite_gate_) {
    return {};
  }

  if (kind != GroupMembershipControlType::GroupInvite) {
    return {};
  }
  auto invite = GroupMembershipCodec::DecodeInviteFromMessage(message);
  if (!invite) {
    return invite.error();
  }
  // An invite speaks for its signed sender only, and only to us.
  if (invite->inviter_identity != actor_identity) {
    return Error("Invite rejected: inviter is not the sender");
  }
  if (local_account.empty() || invite->invitee_identity != local_account) {
    return Error("Invite rejected: not addressed to the local account");
  }
  // A group we already know: not one that ended, nor one we are in already. Members invite too (G019); the owner
  // the invite names is checked when the join bundle comes (its statement must verify for it).
  if (auto known = group_roster_.LoadMetadata(invite->group_id); !known) {
    return known.error();
  } else if (*known) {
    if ((*known)->sync_state == GroupSyncState::Dissolved) {
      return {};  // G016: the group is over; an invite to it is void
    }
    auto member = group_roster_.IsMember(invite->group_id, local_account);
    if (!member) {
      return member.error();
    }
    if (*member) {
      return Error("Invite rejected: already a member");
    }
  }
  auto allowed = invite_gate_->AllowsInboundInvite(*invite);
  if (!allowed) {
    return allowed.error();
  }
  if (!*allowed) {
    if (invite_gate_->RefusesUnknownInviter(invite->inviter_identity)) {
      return Error(kGroupInviteNeedsContactError);
    }
    return Error("Invite blocked by policy");
  }

  PendingGroupInvite pending;
  pending.invite_nonce = invite->invite_nonce;
  pending.group_id = invite->group_id;
  pending.group_title = invite->group_title;
  pending.inviter_identity = invite->inviter_identity;
  pending.invitee_identity = invite->invitee_identity;
  pending.roster_epoch = invite->roster_epoch == 0 ? 1 : invite->roster_epoch;
  pending.status = InviteStatus::Pending;
  pending.expires_at = invite->expires_at;
  pending.created_at = util::NowUnixMs();
  pending.outbound = false;
  pending.owner_identity = invite->Owner();
  if (auto recorded = group_roster_.UpsertPendingInvite(pending); !recorded) {
    return recorded.error();
  }
  invite_gate_->NoteInviteInCall(*invite); // G022: made during a call: that call shows it too
  message.chat_actions = GroupMembershipCodec::BuildInviteChatActions(*invite);
  return {};
}

ReplayWindow& RelayReceivePipeline::ReplayWindowFor(const std::string& thread_id, const uint32_t session_epoch,
                                                    const std::string& sender) {
  const ReplayKey key{thread_id, session_epoch, sender};
  return replay_windows_[key];
}

Roe<bool> RelayReceivePipeline::VerifySignature(const RelayEnvelope& envelope,
                                              const DirectChatTarget& target) const {
  const SenderSignatureCheck check = CheckSignature(envelope, target);
  switch (check.result) {
    case SenderSignatureCheck::Result::Verified:
      return true;
    case SenderSignatureCheck::Result::Mismatch:
      return false;
    case SenderSignatureCheck::Result::NoCard:
      return Error("No account card for the sender");
    case SenderSignatureCheck::Result::DeviceNotOnCard:
      return Error("The sending device is not on the sender's account card");
    case SenderSignatureCheck::Result::Invalid:
      break;
  }
  return Error(check.error);
}

SenderSignatureCheck RelayReceivePipeline::CheckSignature(const RelayEnvelope& envelope,
                                                          const DirectChatTarget& target) const {
  SenderSignatureCheck check;
  if (target.peer_identity_kind != ContactIdKindToString(mesh::ContactIdKind::Account) ||
      envelope.sender_device.empty()) {
    check.error = "Envelope names no sending device of an account";
    return check;
  }
  std::optional<AccountCard> kept = account_cards_ ? account_cards_(target.peer_identity_value) : std::nullopt;
  // The opening message of a session carries the sender's card: one that proves itself for this sender and
  // is newer than the kept one (a new device, a newer list) speaks for the sender, and is kept once the
  // envelope it came with has verified. An older one does not undo a newer list (a device taken out) — nor
  // one older than the newest this device knew before a restore (its floor).
  std::optional<AccountCard> carried;
  if (const auto& card_json = envelope.body.e2e.sender_card) {
    const int64_t floor = account_card_floor_ ? account_card_floor_(target.peer_identity_value) : 0;
    if (auto card = AccountCardFromJson(*card_json);
        card && card->keys.account_id == target.peer_identity_value && VerifyAccountCard(*card, util::NowUnixMs()) &&
        card->issued_at >= floor && (!kept || card->issued_at > kept->issued_at)) {
      carried = std::move(card.value());
    }
    check.carried_refused = !carried;
  }
  const AccountCard* card = carried ? &*carried : (kept ? &*kept : nullptr);
  if (card) {
    check.card_at = card->issued_at;
    check.card_carried = carried.has_value();
  }
  const AccountCardDevice* device = card ? FindCardDevice(*card, envelope.sender_device) : nullptr;
  if (!device) {
    // Nothing to check it with: ask the sending device for its card, which a later envelope then verifies with.
    if (on_unknown_sender_device_ && envelope.body.e2e.sender_card_at &&
        (!card || *envelope.body.e2e.sender_card_at > card->issued_at)) {
      on_unknown_sender_device_(target.peer_identity_value, envelope.sender_device, *envelope.body.e2e.sender_card_at);
    }
    check.result = card ? SenderSignatureCheck::Result::DeviceNotOnCard : SenderSignatureCheck::Result::NoCard;
    return check;
  }
  auto verified = EnvelopeSigner::Verify(envelope, device->signing_public_key_b64);
  if (!verified) {
    check.error = verified.error().message;
    return check;
  }
  if (!*verified) {
    check.result = SenderSignatureCheck::Result::Mismatch;
    return check;
  }
  check.result = SenderSignatureCheck::Result::Verified;
  if (carried && on_sender_card_) {
    on_sender_card_(*carried);
  }
  // M021 d6: the sender says its list is newer than the one we checked with — fetch it from the sending device.
  if (on_sender_card_version_ && envelope.body.e2e.sender_card_at &&
      *envelope.body.e2e.sender_card_at > card->issued_at) {
    on_sender_card_version_(target.peer_identity_value, envelope.sender_device, *envelope.body.e2e.sender_card_at);
  }
  return check;
}

RelayReceivePipeline::CopyTo RelayReceivePipeline::WhoseCopy(const RelayEnvelope& envelope) const {
  if (envelope.recipient_device.empty()) {
    return CopyTo::Account;
  }
  auto self = local_keys_.device_peer_id ? local_keys_.device_peer_id() : Roe<std::string>(Error("no device"));
  return self && *self == envelope.recipient_device ? CopyTo::ThisDevice : CopyTo::Sibling;
}

Roe<ByteVector> RelayReceivePipeline::CopyKemPrivateKey(const RelayEnvelope& envelope) const {
  if (envelope.recipient_device.empty()) {
    return local_keys_.kem_private_key();
  }
  if (!local_keys_.device_kem_private_key) {
    return Error("No device KEM key");
  }
  return local_keys_.device_kem_private_key();
}

Roe<void> RelayReceivePipeline::ValidateInboundPskRotate(const RelayEnvelope& envelope,
                                                        const ThreadMessage& message) const {
  if (!PskRotateCodec::IsPskRotateMessage(message)) {
    return {};
  }
  auto detail = PskRotateCodec::Decode(message);
  if (!detail) {
    return detail.error();
  }
  if (!envelope.body.e2e.key_init_b64 || envelope.body.e2e.key_init_b64->empty()) {
    return Error("psk_rotate missing key_init_b64");
  }
  auto hash = AutoKeyEstablishment::HashKeyInitB64(*envelope.body.e2e.key_init_b64);
  if (!hash) {
    return hash.error();
  }
  if (*hash != detail->key_init_hash) {
    return Error("psk_rotate key_init_hash mismatch");
  }
  return {};
}

Roe<void> RelayReceivePipeline::ApplyInboundPskRotate(const std::string& thread_id, const RelayEnvelope& envelope,
                                                     const ThreadMessage& message) {
  if (!PskRotateCodec::IsPskRotateMessage(message)) {
    return {};
  }
  auto kem = local_keys_.kem_private_key();
  if (!kem) {
    return kem.error();
  }
  auto account = local_keys_.account_id();
  if (!account) {
    return account.error();
  }
  auto applied = public_lock_.ApplyInbound(thread_id, envelope, message, *kem, *account,
                                          util::NowUnixMs());
  if (!applied) {
    return applied.error();
  }
  return {};
}

std::optional<std::string> RelayReceivePipeline::FindMessageIdAtSeq(const std::string& thread_id,
                                                                    const uint32_t session_epoch,
                                                                    const std::string& seq_owner_contact_id,
                                                                    const uint64_t sender_seq,
                                                                    const std::string& sender_device) const {
  SeqRangeQuery query;
  query.session_epoch = session_epoch;
  query.seq_owner_contact_id = seq_owner_contact_id;
  query.sender_device = sender_device;
  query.min_sender_seq = sender_seq;
  query.max_sender_seq = sender_seq;
  query.limit = 1;
  auto rows = store_.GetMessagesBySeqRange(thread_id, query);
  if (!rows || rows->empty()) {
    return std::nullopt;
  }
  return rows->front().id;
}

bool RelayReceivePipeline::SenderBlocked(const std::string& sender) const {
  if (!contact_trust_ || sender.empty()) {
    return false;
  }
  const auto blocked = contact_trust_->IsAccountBlocked(sender);
  return blocked && *blocked;
}

RelayReceiveOutcome RelayReceivePipeline::ProcessEnvelope(const RelayEnvelope& received,
                                                          const std::string& local_relay_user_id,
                                                          const bool authorized_older_backfill,
                                                          const MessageTransport transport) {
  // The copy this device reads (M022): its own, else the account copy. With copies for our other devices
  // only, the message is theirs (or one is missing for us).
  auto self = local_keys_.device_peer_id ? local_keys_.device_peer_id() : Roe<std::string>(Error("no device"));
  const std::optional<RelayEnvelope> selected = SelectRelayCopy(received, self ? *self : std::string());
  RelayReceiveOutcome outcome;
  if (!selected) {
    outcome.decision = IngestDecision::SilentDiscard;
    const bool chat = received.route.kind == kRelayRouteGroup || received.route.kind == kRelayRouteDirect;
    if (chat && self && !received.sender_contact_id.empty() && received.sender_contact_id != local_relay_user_id &&
        !received.sender_device.empty() && !received.message_id.empty()) {
      // A group record names no group (G018 item 7): its stream id, computed for the groups we share, tells.
      std::string group_id;
      if (received.route.kind == kRelayRouteGroup) {
        group_id = GroupOfStream(received, GroupsSharedWith(received.sender_contact_id, local_relay_user_id),
                                 local_relay_user_id)
                       .value_or("");
      }
      if (received.route.kind != kRelayRouteGroup || !group_id.empty()) {
        outcome.missing_copy = RelayReceiveOutcome::MissingCopy{.sender_account = received.sender_contact_id,
                                                                .sender_device = received.sender_device,
                                                                .message_id = received.message_id,
                                                                .group_id = group_id};
      }
    }
    if (received.route.kind == kRelayRouteControl && self) {
      NoteMissingControlCopy(received, local_relay_user_id, outcome);
    }
  } else if (selected->route.kind == kRelayRouteGroup) {
    outcome = ProcessGroupEnvelope(*selected, local_relay_user_id, authorized_older_backfill, transport);
  } else if (selected->route.kind == kRelayRouteControl) {
    outcome = ProcessControlEnvelope(*selected, local_relay_user_id);
  } else {
    outcome = ProcessDirectEnvelope(*selected, local_relay_user_id, authorized_older_backfill, transport);
  }
  // A sender that verified has a card kept now: what of its waited for one may be checked (#555).
  if (outcome.sender_verified && HoldsControlFrom(received.sender_contact_id)) {
    outcome.replay_held_from = received.sender_contact_id;
  }
  return outcome;
}

bool RelayReceivePipeline::HoldsControlFrom(const std::string& sender) const {
  std::lock_guard lock(control_hold_mutex_);
  return control_hold_.Holds(sender);
}

std::vector<RelayReceivePipeline::HeldControlReplay> RelayReceivePipeline::ReplayHeldControl(
    const std::string& sender, const std::string& local_relay_user_id) {
  const int64_t now_ms = util::NowUnixMs();
  std::vector<HeldControlEnvelope> held;
  ControlLaneHoldDrops drops;
  {
    std::lock_guard lock(control_hold_mutex_);
    held = control_hold_.Take(sender, now_ms, &drops);
  }
  if (drops.expired > 0) {
    log().info << "control lane: " << drops.expired << " held message(s) waited past " << kControlLaneHoldMaxMs
               << " ms for a card and were dropped";
  }
  std::vector<HeldControlReplay> replayed;
  replayed.reserve(held.size());
  for (HeldControlEnvelope& entry : held) {
    log().info << "control lane: checking again message_id=" << entry.envelope.message_id << " sender=" << sender
               << " held_ms=" << (now_ms - entry.held_since_ms);
    RelayReceiveOutcome outcome = ProcessControlEnvelope(entry.envelope, local_relay_user_id, entry.held_since_ms);
    replayed.push_back(HeldControlReplay{std::move(entry.envelope), std::move(outcome)});
  }
  return replayed;
}

bool ControlLaneFresh(const RelayEnvelope& envelope, const int64_t max_age_ms, const int64_t now_ms) {
  // The sender's signed time, against ours: bounds a replay whatever the transport claims (devices'
  // clocks may drift, so the bound is generous; the seen-store covers it).
  if (envelope.timestamp <= 0 || std::llabs(now_ms - envelope.timestamp) > kControlLaneSignedSkewMs) {
    return false;
  }
  // Through the relay: how long it waited there, on the relay's own clock (no skew between devices).
  if (envelope.relay_created_at_ms && envelope.relay_server_time_ms) {
    return *envelope.relay_server_time_ms - *envelope.relay_created_at_ms <= max_age_ms;
  }
  return true;  // direct (Amp): delivered as sent
}

RelayReceiveOutcome RelayReceivePipeline::ProcessControlEnvelope(const RelayEnvelope& received,
                                                                 const std::string& local_relay_user_id,
                                                                 const std::optional<int64_t> held_since_ms) {
  // Checked again after a wait for the sender's card: the wait counts in its age (ControlLaneHold).
  std::optional<RelayEnvelope> aged;
  if (held_since_ms) {
    aged = AgedByHold(received, *held_since_ms, util::NowUnixMs());
  }
  const RelayEnvelope& envelope = aged ? *aged : received;
  RelayReceiveOutcome outcome;
  outcome.decision = IngestDecision::HardReject;
  if (envelope.envelope_version != kRelayEnvelopeVersion ||
      DumpJson(RelayEnvelopeToJson(envelope)).size() > kMaxRelayEnvelopeBytes) {
    return outcome;
  }
  if (envelope.route.channel != ThreadChannel::E2ePublic || envelope.sender_seq != 0 ||
      envelope.message_id.empty() || envelope.sender_contact_id.empty()) {
    return outcome;
  }
  if (SenderBlocked(envelope.sender_contact_id)) {
    outcome.decision = IngestDecision::SilentDiscard;
    return outcome;
  }
  if (WhoseCopy(envelope) == CopyTo::Sibling) {
    outcome.decision = IngestDecision::SilentDiscard;  // another device's copy, in the shared mailbox (M013)
    return outcome;
  }
  const DirectChatTarget inbound_target = InboundTargetFromEnvelope(envelope);
  if (envelope.sender_contact_id == local_relay_user_id) {
    // A sibling's notice (M022): the account card it carries — the device list — is what counts, taken once
    // the envelope verifies with it (SetOnSenderCard). Nothing else to apply.
    outcome.sender_verified = CheckControlSender(received, inbound_target, held_since_ms, outcome);
    outcome.from_sibling = outcome.sender_verified;
    outcome.decision = IngestDecision::SilentDiscard;
    if (outcome.sender_verified) {
      ApplySiblingNotice(envelope, local_relay_user_id, inbound_target, outcome);
    }
    return outcome;
  }
  if (!CheckControlSender(received, inbound_target, held_since_ms, outcome)) {
    return outcome;
  }
  outcome.sender_verified = true;
  if (local_relay_user_id.empty()) {
    return outcome;
  }
  auto kem_private = CopyKemPrivateKey(envelope);
  if (!kem_private) {
    return outcome;
  }
  // A device's copy is read in our pair with the sending device (the chat stream's session); the account copy in
  // the account pair's (M024).
  const PeerSessionKey target_key = CopyTargetKey(envelope, inbound_target, CryptoChannel::E2ePublic);
  AutoKeyRead read;
  auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_relay_user_id, target_key, psk_store_,
                                                         *kem_private, &read);
  if (!decrypted) {
    ReportUnreadableControl(envelope, target_key, decrypted.error(), outcome);
    return outcome;
  }
  ThreadMessage message = std::move(*decrypted);
  const auto family = controls_ ? controls_->Find(message) : std::nullopt;
  if (message.content_type != ChatContentType::System || !family || !family->on_lane || !family->on_lane(message)) {
    // Only a bound family's lane signaling rides the lane; anything else is not ours to apply.
    outcome.decision = IngestDecision::SilentDiscard;
    return outcome;
  }
  const int64_t now_ms = util::NowUnixMs();
  if (!ControlLaneFresh(envelope, family->lane_max_age_ms, now_ms)) {
    log().info << "control lane: stale message dropped family=" << family->name << " message_id=" << envelope.message_id
               << " sent_ms=" << envelope.timestamp << " now_ms=" << now_ms;
    outcome.decision = IngestDecision::SilentDiscard;
    return outcome;
  }
  if (control_lane_seen_) {
    auto first = control_lane_seen_->FirstSight(envelope.sender_contact_id, envelope.message_id, now_ms);
    if (!first) {
      log().warning << "control lane: seen-check failed: " << first.error().message;
      return outcome;
    }
    if (!*first) {
      outcome.decision = IngestDecision::BenignDuplicate;
      return outcome;
    }
  }
  // Signed by the peer, read, fresh and first seen: what that proves about the pair's session is kept.
  if (auto kept = ReconcileE2ePublicSession(psk_store_, target_key, envelope.session_epoch, read,
                                            envelope.body.e2e.key_init_b64, envelope.timestamp);
      !kept) {
    log().warning << "control lane: session not updated: " << kept.error().message;
  }

  message.id = envelope.message_id;
  message.thread_id.clear();
  message.sender_contact_id = envelope.sender_contact_id;
  message.timestamp = envelope.timestamp;
  if (auto applied = ApplyInboundControl(message, envelope, &outcome); !applied) {
    if (control_lane_seen_) {
      control_lane_seen_->Forget(envelope.sender_contact_id, envelope.message_id);  // a redelivery may still apply
    }
    return outcome;
  }
  outcome.decision = IngestDecision::AcceptControl;
  return outcome;
}

namespace {

/** A lane copy's kind, for the diag lines. */
constexpr ShareWord kCopyAccount = "account";
constexpr ShareWord kCopyDevice = "device";

} // namespace

bool RelayReceivePipeline::CheckControlSender(const RelayEnvelope& envelope, const DirectChatTarget& target,
                                              const std::optional<int64_t> held_since_ms,
                                              RelayReceiveOutcome& outcome) {
  const SenderSignatureCheck check = CheckSignature(envelope, target);
  if (check.result == SenderSignatureCheck::Result::Verified) {
    return true;
  }
  // Only a want of the card waits for it (#555): it is asked for (SetOnUnknownSenderDevice), and the message is
  // checked again once a card is kept. A signature that does not verify will not verify later.
  const bool wants_card = check.result == SenderSignatureCheck::Result::NoCard ||
                          check.result == SenderSignatureCheck::Result::DeviceNotOnCard;
  outcome.held_for_card = wants_card && HoldForCard(envelope, held_since_ms);
  ReportUnverifiedControl(envelope, check, outcome.held_for_card);
  return false;
}

bool RelayReceivePipeline::HoldForCard(const RelayEnvelope& envelope, const std::optional<int64_t> held_since_ms) {
  const int64_t now_ms = util::NowUnixMs();
  ControlLaneHoldDrops drops;
  bool held = false;
  {
    std::lock_guard lock(control_hold_mutex_);
    held = control_hold_.Hold(envelope.sender_contact_id, envelope, held_since_ms.value_or(now_ms), now_ms, &drops);
  }
  if (drops.expired + drops.over_sender + drops.over_total > 0) {
    log().info << "control lane: held messages dropped expired=" << drops.expired
               << " over_sender=" << drops.over_sender << " over_total=" << drops.over_total;
  }
  return held;
}

namespace {

/** Why a lane message's sender did not check, for the diag line. */
ShareWord UnverifiedReasonWord(const SenderSignatureCheck::Result result) {
  switch (result) {
    case SenderSignatureCheck::Result::NoCard:
      return "no_card";
    case SenderSignatureCheck::Result::DeviceNotOnCard:
      return "device_not_on_card";
    case SenderSignatureCheck::Result::Mismatch:
      return "signature_mismatch";
    case SenderSignatureCheck::Result::Verified:
    case SenderSignatureCheck::Result::Invalid:
      break;
  }
  return "invalid";
}

/** Which card a check used, for the diag line: the one riding, the one kept, none. */
ShareWord CheckedCardWord(const SenderSignatureCheck& check) {
  if (!check.card_at) {
    return "none";
  }
  if (check.card_carried) {
    return "carried";
  }
  return "kept";
}

std::string OptionalMs(const std::optional<int64_t>& value) {
  return value ? std::to_string(*value) : std::string("-");
}

/** The start of a device id, for the developer log: enough to tell the sender's devices apart. */
std::string DevicePrefix(const std::string& device) {
  constexpr size_t kPrefix = 16;
  return device.size() > kPrefix ? device.substr(0, kPrefix) : device;
}

} // namespace

void RelayReceivePipeline::ReportUnverifiedControl(const RelayEnvelope& envelope, const SenderSignatureCheck& check,
                                                   const bool held) const {
  const ShareWord reason = UnverifiedReasonWord(check.result);
  const bool account_copy = envelope.recipient_device.empty();
  std::ostringstream text;
  text << "control lane: signature check failed reason=" << reason.view() << " message_id=" << envelope.message_id
       << " sender=" << envelope.sender_contact_id << " sender_device=" << DevicePrefix(envelope.sender_device)
       << " card=" << CheckedCardWord(check).view() << " card_issued_at=" << OptionalMs(check.card_at)
       << " sender_card_at=" << OptionalMs(envelope.body.e2e.sender_card_at)
       << " card_rode=" << (envelope.body.e2e.sender_card ? (check.carried_refused ? "refused" : "taken") : "no")
       << " route=" << envelope.route.kind << " channel=" << ThreadChannelToString(envelope.route.channel)
       << " copy=" << (account_copy ? "account" : "device") << " held_for_card=" << (held ? 1 : 0);
  if (check.result == SenderSignatureCheck::Result::Mismatch) {
    // The key of the sending device, from the card, does not verify it: a forgery, a record changed on its way — or
    // a sender on a build before sign v4 (2026-11-06), whose signed bytes differ. Not held: it will not verify later.
    text << " — the signature does not verify with that device's key (a sender on a build before sign v4, "
            "2026-11-06, shows so)";
  } else if (check.result == SenderSignatureCheck::Result::Invalid) {
    text << " err=" << check.error;
  }
  log().warning << text.str();
  DiagLine("control.unverified")
      .Add("reason", reason)
      .Add("card", CheckedCardWord(check))
      .Add("card_refused", check.carried_refused)
      .Add("copy", account_copy ? kCopyAccount : kCopyDevice)
      .Add("held", held)
      .Ref("sender", envelope.sender_contact_id)
      .Emit();
}

void RelayReceivePipeline::NoteMissingControlCopy(const RelayEnvelope& received, const std::string& local_account,
                                                  RelayReceiveOutcome& outcome) {
  if (received.sender_contact_id.empty() || received.sender_contact_id == local_account ||
      SenderBlocked(received.sender_contact_id)) {
    return; // our own account's notices are account copies; a blocked sender learns nothing of us
  }
  // Only a sender that proves itself is told our card: it lists our devices.
  auto verified = VerifySignature(received, InboundTargetFromEnvelope(received));
  if (!verified || !*verified) {
    return;
  }
  outcome.sender_verified = true;
  log().info << "control lane: no copy for this device message_id=" << received.message_id
             << " sender=" << received.sender_contact_id << " device=" << received.sender_device
             << " copies=" << received.body.e2e.copies.size();
  outcome.show_card_to = received.sender_contact_id;
}

void RelayReceivePipeline::ReportUnreadableControl(const RelayEnvelope& envelope, const PeerSessionKey& target_key,
                                                   const Error& error, RelayReceiveOutcome& outcome) const {
  const bool account_copy = envelope.recipient_device.empty();
  std::string held_session;
  uint32_t held_epoch = 0;
  if (auto record = psk_store_.Load(target_key); record && record->has_value()) {
    held_session = (*record)->auto_key_session_id.value_or("");
    held_epoch = (*record)->session_epoch;
  }
  const std::string offered_session = envelope.body.e2e.key_init_b64 && !envelope.body.e2e.key_init_b64->empty()
                                          ? AutoKeySessionId(*envelope.body.e2e.key_init_b64)
                                          : std::string();
  log().warning << "control lane: couldn't decrypt message_id=" << envelope.message_id
                << " sender=" << envelope.sender_contact_id << " sender_device=" << envelope.sender_device
                << " copy=" << (account_copy ? "account" : "device") << " epoch=" << envelope.session_epoch
                << " held_epoch=" << held_epoch << " held_session=" << (held_session.empty() ? "-" : held_session)
                << " offered_session=" << (offered_session.empty() ? "-" : offered_session) << " err=" << error.message;
  DiagLine line("control.unreadable");
  line.Add("copy", account_copy ? kCopyAccount : kCopyDevice)
      .Add("epoch", envelope.session_epoch)
      .Add("held_epoch", held_epoch);
  if (held_session.empty()) {
    line.Add("held", "-");
  } else {
    line.RandomId("held", held_session);
  }
  if (offered_session.empty()) {
    line.Add("offered", "-");
  } else {
    line.RandomId("offered", offered_session);
  }
  line.Ref("sender", envelope.sender_contact_id).Fail("error", error).Emit();
  if (account_copy) {
    // An account copy goes to a peer whose devices the sender does not know (or, for a while, one with a single
    // device): our card shows it this device, so its next message has a copy under our device pair (M024).
    outcome.show_card_to = envelope.sender_contact_id;
  }
}

void RelayReceivePipeline::ApplySiblingNotice(const RelayEnvelope& envelope, const std::string& local_account_id,
                                              const DirectChatTarget& inbound_target, RelayReceiveOutcome& outcome) {
  if (local_keys_.device_peer_id) {
    if (auto self = local_keys_.device_peer_id(); self && envelope.sender_device == *self) {
      return;  // our own notice, back from the shared mailbox
    }
  }
  auto kem_private = CopyKemPrivateKey(envelope);
  if (!kem_private) {
    return;
  }
  // A one-off opener to our account KEM (ControlLane::NotifySiblings): read, no session kept between siblings.
  const PeerSessionKey target_key = CopyTargetKey(envelope, inbound_target, CryptoChannel::E2ePublic);
  AutoKeyRead read;
  auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_account_id, target_key, psk_store_,
                                                         *kem_private, &read);
  if (!decrypted) {
    log().info << "sibling notice not read message_id=" << envelope.message_id << ": " << decrypted.error().message;
    return;
  }
  auto copy = sent_copy::Parse(*decrypted);
  if (!copy) {
    log().warning << "sibling's sent copy refused message_id=" << envelope.message_id << ": " << copy.error().message;
    return;
  }
  if (*copy) {
    outcome.from_sibling = false;  // a sent message, not news about the account's state (no locker sync)
    FileSentCopy(**copy, envelope, outcome);
    return;
  }
  // Another lane notice of ours (M021 d5: a call taken on a sibling): its family applies it, fresh and once,
  // as any lane message — the family knows it is from our own account (the context's sender).
  ThreadMessage message = std::move(*decrypted);
  const auto family = controls_ ? controls_->Find(message) : std::nullopt;
  if (!family || !family->on_lane || !family->on_lane(message)) {
    return;  // the device list notice: its card is what counts (taken above)
  }
  const int64_t now_ms = util::NowUnixMs();
  if (!ControlLaneFresh(envelope, family->lane_max_age_ms, now_ms)) {
    return;
  }
  if (control_lane_seen_) {
    auto first = control_lane_seen_->FirstSight(envelope.sender_contact_id, envelope.message_id, now_ms);
    if (!first || !*first) {
      return;
    }
  }
  outcome.from_sibling = false;  // not a nudge to sync the locker
  message.id = envelope.message_id;
  message.thread_id.clear();
  message.sender_contact_id = envelope.sender_contact_id;
  message.timestamp = envelope.timestamp;
  if (auto applied = ApplyInboundControl(message, envelope, &outcome); applied) {
    outcome.decision = IngestDecision::AcceptControl;
  }
}

void RelayReceivePipeline::FileSentCopy(const SentCopy& copy, const RelayEnvelope& envelope,
                                        RelayReceiveOutcome& outcome) {
  std::string thread_id;
  if (!copy.target.group_id.empty()) {
    auto thread = store_.FindGroupThread(copy.target.group_id);
    if (!thread || !*thread) {
      return;  // a group this device does not hold
    }
    if (auto metadata = group_roster_.LoadMetadata(copy.target.group_id);
        metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Dissolved) {
      return;  // G016: nothing more is filed in a group that ended
    }
    thread_id = (*thread)->id;
  } else {
    // Public chats only (private Secure stays on one device, M014). A chat this device has not opened yet is.
    const DirectChatTarget target{.peer_identity_kind = "account",
                                  .peer_identity_value = copy.target.peer_account,
                                  .channel = ThreadChannel::E2ePublic};
    const std::string title = ShortRelayId(copy.target.peer_account);
    auto thread = store_.FindOrCreateDirectThread(target, "", title.empty() ? copy.target.peer_account : title);
    if (!thread) {
      log().warning << "sibling's sent copy: no chat: " << thread.error().message;
      return;
    }
    thread_id = thread->id;
    outcome.thread_changed = true;
  }
  if (auto seen = store_.HasMessageId(thread_id, copy.message.id); !seen || *seen) {
    outcome.decision = IngestDecision::BenignDuplicate;
    return;
  }
  ThreadMessage message = copy.message;
  message.thread_id = thread_id;
  message.sender_contact_id = kLocalSelfContactId;  // ours, from another of our devices
  message.sender_device = envelope.sender_device;
  message.delivery = MessageDelivery::Relayed;
  message.transport = MessageTransport::Relay;
  message.relay_visible = true;
  // As a peer's message is taken: one we deleted before its copy came stays deleted; reactions are capped.
  TombstoneIfRetractedBeforeArrival(message, outcome);
  if (auto cap = RejectIfAnnotationCapExceeded(store_, message); !cap) {
    return;
  }
  auto stored = store_.AppendMessage(message);
  if (!stored) {
    log().warning << "sibling's sent copy not stored: " << stored.error().message;
    return;
  }
  ApplyInboundRetract(*stored, outcome);  // our delete for everyone, made on another device
  outcome.decision = IngestDecision::AcceptControl;
  outcome.persisted = true;
  outcome.thread_id = thread_id;
  outcome.persisted_message = *stored;
  outcome.suppress_inbox_chrome = true;  // what we sent ourselves: no unread count, no notice
  outcome.sibling_copy = true;
}

RelayReceiveOutcome RelayReceivePipeline::ProcessDirectEnvelope(const RelayEnvelope& envelope,
                                                                const std::string& local_relay_user_id,
                                                                const bool authorized_older_backfill,
                                                                const MessageTransport transport) {
  RelayReceiveOutcome outcome;

  const std::string serialized = DumpJson(RelayEnvelopeToJson(envelope));
  if (serialized.size() > kMaxRelayEnvelopeBytes) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  if (envelope.envelope_version != kRelayEnvelopeVersion) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  if (envelope.route.kind != "direct" || !ThreadChannelIsE2e(envelope.route.channel)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  // Early, on the claimed sender: a spoofed Blocked id only drops the spoofer's own message.
  if (SenderBlocked(envelope.sender_contact_id)) {
    log().info << "drop direct envelope from a Blocked sender message_id=" << envelope.message_id;
    outcome.decision = IngestDecision::SilentDiscard;
    return outcome;
  }
  // The shared mailbox (M013) holds every device's copies: another device's is not ours to read; our own
  // account's siblings do not write to us yet (M021 d3).
  if (WhoseCopy(envelope) == CopyTo::Sibling || envelope.sender_contact_id == local_relay_user_id) {
    outcome.decision = IngestDecision::SilentDiscard;
    return outcome;
  }
  const std::string numbering = CopyNumbering(envelope);

  const DirectChatTarget inbound_target = InboundTargetFromEnvelope(envelope);
  auto thread = store_.FindDirectThread(inbound_target);
  if (!thread) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::ThreadLookupFailed, thread.error().message);
    return outcome;
  }

  const bool auto_create = !*thread && envelope.route.channel == ThreadChannel::E2ePublic;
  if (!*thread && !auto_create) {
    // Private e2e with no local thread — expected for unknown/stale peers; silent.
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  if (!auto_create && !IsEnvelopeFromPeer(**thread, envelope)) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::SenderMismatch, "Envelope sender is not the thread peer",
                       (*thread)->id);
    return outcome;
  }

  std::string resolved_thread_id;
  if (!auto_create) {
    resolved_thread_id = (*thread)->id;
    auto has_message_id = store_.HasMessageId(resolved_thread_id, envelope.message_id);
    if (!has_message_id) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::StoreError, has_message_id.error().message,
                         resolved_thread_id);
      return outcome;
    }
    if (*has_message_id) {
      outcome.decision = IngestDecision::BenignDuplicate;
      // Message body may never have run call-control (history sync / prior drop). Decrypt and
      // apply idempotent CallMediaKey/Accept side effects before returning.
      auto verified_dup = VerifySignature(envelope, inbound_target);
      outcome.sender_verified = verified_dup && *verified_dup;
      if (verified_dup && *verified_dup && E2eRelayPayloadCodec::RequiresEncryption(envelope.route.channel)) {
        const PeerSessionKey target_key = CopyTargetKey(envelope, inbound_target,
                                                       E2eRelayPayloadCodec::ChannelFromThread(envelope.route.channel));
        std::optional<ByteVector> local_kem_private_key;
        if (envelope.route.channel == ThreadChannel::E2ePublic) {
          if (auto kem_private = CopyKemPrivateKey(envelope)) {
            local_kem_private_key = std::move(*kem_private);
          }
        }
        if (auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_relay_user_id, target_key,
                                                                   psk_store_, local_kem_private_key)) {
          if (AppliesOutOfOrder(*decrypted)) {
            ThreadMessage side = std::move(*decrypted);
            side.id = envelope.message_id;
            side.thread_id = resolved_thread_id;
            side.sender_contact_id = envelope.sender_contact_id;
            side.timestamp = envelope.timestamp;
            side.delivery = MessageDelivery::Relayed;
            side.relay_visible = true;
            side.transport = transport;
            side.sender_seq = envelope.sender_seq;
            side.session_epoch = envelope.session_epoch;
            side.sender_device = RowDevice(numbering);
            ApplyOutOfOrderControls(side, envelope, IngestDecision::BenignDuplicate, outcome);
          }
        }
      }
      return outcome;
    }
  }

  auto verified = VerifySignature(envelope, inbound_target);
  if (!verified) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::SignatureCheckFailed, verified.error().message,
                       resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
    return outcome;
  }
  if (!*verified) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::BadSignature, "Envelope signature did not verify",
                       resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
    return outcome;
  }
  outcome.sender_verified = true;

  // The pair's epoch as it stood before this envelope: reading it may move the session to a later epoch
  // the peer restarted at (ReconcileE2ePublicSession), and that is still an advance to classify.
  const bool device_copy = !envelope.recipient_device.empty();
  const PeerSessionKey copy_key =
      CopyTargetKey(envelope, inbound_target, E2eRelayPayloadCodec::ChannelFromThread(envelope.route.channel));
  std::optional<uint32_t> epoch_before;
  if (device_copy) {
    // A device pair's epoch is its session's (M021), not the thread's.
    if (auto loaded = psk_store_.Load(copy_key); loaded && loaded->has_value()) {
      epoch_before = (*loaded)->session_epoch;
    }
  } else if (!auto_create) {
    if (auto loaded = store_.GetChatTargetSessionEpoch(resolved_thread_id); loaded) {
      epoch_before = *loaded;
    }
  }

  ThreadMessage message;
  if (E2eRelayPayloadCodec::RequiresEncryption(envelope.route.channel)) {
    if (local_relay_user_id.empty()) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::MissingLocalIdentity,
                         "Local account identity unavailable for decrypt",
                         resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
      return outcome;
    }
    const std::string& local_aad_id = local_relay_user_id;

    const PeerSessionKey& target_key = copy_key;

    std::optional<ByteVector> local_kem_private_key;
    if (envelope.route.channel == ThreadChannel::E2ePublic) {
      auto kem_private = CopyKemPrivateKey(envelope);
      if (!kem_private) {
        outcome.decision = IngestDecision::HardReject;
        MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::MissingKey, kem_private.error().message,
                           resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
        return outcome;
      }
      local_kem_private_key = std::move(*kem_private);
    }

    AutoKeyRead read;
    auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_aad_id, target_key, psk_store_,
                                                           local_kem_private_key, &read);
    if (!decrypted) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::DecryptFailed, decrypted.error().message,
                         resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
      return outcome;
    }
    message = std::move(*decrypted);

    if (envelope.route.channel == ThreadChannel::E2ePublic && local_kem_private_key.has_value() &&
        !PskRotateCodec::IsPskRotateMessage(message)) {
      // Signed by the peer (checked above) and read: keep what that proves about the pair's session.
      if (auto kept = ReconcileE2ePublicSession(psk_store_, target_key, envelope.session_epoch, read,
                                                envelope.body.e2e.key_init_b64, envelope.timestamp);
          !kept) {
        log().warning << "e2e_public session not updated: " << kept.error().message;
      }
    }
  } else {
    auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64);
    if (!decoded) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::DecodeFailed, decoded.error().message,
                         resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
      return outcome;
    }
    if (decoded->content_type != ChatContentType::Text && decoded->content_type != ChatContentType::System &&
        decoded->content_type != ChatContentType::Annotation &&
        decoded->content_type != ChatContentType::ContactCard &&
        decoded->content_type != ChatContentType::CryptoTx &&
        decoded->content_type != ChatContentType::Attachment &&
        decoded->content_type != ChatContentType::Unsupported) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::Unsupported,
                         "Inbound payload content type not supported",
                         resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
      return outcome;
    }
    ChatPayloadValidator::SanitizeInboundFields(*decoded);
    message = std::move(*decoded);
  }

  if (message.content_type != ChatContentType::Text && message.content_type != ChatContentType::System &&
      message.content_type != ChatContentType::Annotation && message.content_type != ChatContentType::ContactCard &&
      message.content_type != ChatContentType::CryptoTx && message.content_type != ChatContentType::Attachment &&
      message.content_type != ChatContentType::Unsupported) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::Unsupported,
                       "Decoded message content type not supported",
                       resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
    return outcome;
  }

  if (controls_ && controls_->OnLane(message)) {
    // Lane-only signaling has no place in the chat stream: never applied, never stored here.
    log().info << "drop control-lane message sent in the chat stream message_id=" << envelope.message_id;
    outcome.decision = IngestDecision::SilentDiscard;
    return outcome;
  }

  if (auto_create) {
    const std::string fallback_title = ShortRelayId(envelope.sender_contact_id);
    auto created = store_.FindOrCreateDirectThread(
        inbound_target, "", fallback_title.empty() ? envelope.sender_contact_id : fallback_title);
    if (!created) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::OpenChatFailed, created.error().message);
      return outcome;
    }
    resolved_thread_id = created->id;
    outcome.thread_changed = true;
  }

  const std::string seq_owner = [&]() -> std::string {
    if (auto_create) {
      return envelope.sender_contact_id;
    }
    if (!(*thread)->participant_contact_ids.empty()) {
      const std::string& participant = (*thread)->participant_contact_ids.front();
      if (!participant.empty()) {
        return participant;
      }
    }
    return envelope.sender_contact_id;
  }();

  if (auto rotate = ValidateInboundPskRotate(envelope, message); !rotate) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::KeyUpdateFailed, rotate.error().message,
                       resolved_thread_id);
    return outcome;
  }

  PeerSyncState sync_state;
  uint32_t chat_target_epoch = envelope.session_epoch;
  if (auto_create) {
    sync_state = DefaultPeerSyncState();
  } else if (device_copy) {
    // The device pair's numbering (M021): its own receive state, from scratch for a pair new to us.
    chat_target_epoch = epoch_before.value_or(envelope.session_epoch);
    auto loaded_sync_state = store_.GetPeerSyncState(resolved_thread_id, envelope.session_epoch, numbering);
    sync_state = loaded_sync_state ? *loaded_sync_state : DefaultPeerSyncState();
  } else {
    auto loaded_epoch = store_.GetChatTargetSessionEpoch(resolved_thread_id);
    if (!loaded_epoch) {
      outcome.decision = IngestDecision::HardReject;
      log().info << "refuse message_id=" << envelope.message_id << ": chat target epoch unreadable";
      return outcome;
    }
    chat_target_epoch = epoch_before.value_or(*loaded_epoch);

    auto loaded_sync_state = store_.GetPeerSyncState(resolved_thread_id, envelope.session_epoch, numbering);
    if (loaded_sync_state) {
      sync_state = *loaded_sync_state;
    } else if (!numbering.empty()) {
      sync_state = DefaultPeerSyncState();  // a sending device's account copies, new to us (M021)
    } else if (envelope.session_epoch > chat_target_epoch) {
      sync_state = DefaultPeerSyncState();  // the peer moved the pair to an epoch nothing is held in yet
    } else {
      outcome.decision = IngestDecision::HardReject;
      log().info << "refuse message_id=" << envelope.message_id << " epoch=" << envelope.session_epoch
                 << ": no sync state (" << loaded_sync_state.error().message << ")";
      return outcome;
    }
  }

  IngestClassifierInput classifier_input;
  classifier_input.sender_seq = envelope.sender_seq;
  classifier_input.session_epoch = envelope.session_epoch;
  classifier_input.message_id = envelope.message_id;
  classifier_input.sync_state = sync_state;
  classifier_input.chat_target_epoch = chat_target_epoch;
  classifier_input.strict_mode = envelope.route.channel == ThreadChannel::E2e;
  classifier_input.authorized_older_backfill = authorized_older_backfill;
  classifier_input.has_message_id = false;
  classifier_input.existing_message_id_at_seq =
      FindMessageIdAtSeq(resolved_thread_id, envelope.session_epoch, seq_owner, envelope.sender_seq, numbering);

  const std::string replay_owner = envelope.sender_contact_id + "/" + numbering;
  auto& replay_window = ReplayWindowFor(resolved_thread_id, envelope.session_epoch, replay_owner);
  const IngestClassifierResult classified = E2eIngestClassifier::Classify(classifier_input, replay_window);
  outcome.decision = classified.decision;

  auto apply_out_of_order = [&](IngestDecision decision_label) {
    if (!AppliesOutOfOrder(message)) {
      return;
    }
    ThreadMessage side = message;
    side.id = envelope.message_id;
    side.thread_id = resolved_thread_id;
    side.sender_contact_id = seq_owner;
    side.timestamp = envelope.timestamp;
    side.delivery = MessageDelivery::Relayed;
    side.relay_visible = true;
    side.transport = transport;
    side.sender_seq = envelope.sender_seq;
    side.session_epoch = envelope.session_epoch;
    side.sender_device = RowDevice(numbering);
    ApplyOutOfOrderControls(side, envelope, decision_label, outcome);
  };

  if (classified.decision == IngestDecision::SilentDiscard || classified.decision == IngestDecision::BenignDuplicate) {
    if (classified.decision == IngestDecision::SilentDiscard) {
      log().info << "drop message_id=" << envelope.message_id << " sender=" << envelope.sender_contact_id
                 << " epoch=" << envelope.session_epoch << " seq=" << envelope.sender_seq << ": "
                 << classified.reason;
    }
    // Out-of-order controls (call media keys, accepts, membership) are idempotent — do not skip when
    // the classifier drops a resent seq/id.
    apply_out_of_order(classified.decision);
    return outcome;
  }
  const bool strict_channel = envelope.route.channel == ThreadChannel::E2e;
  if (classified.decision == IngestDecision::SoftCompromised || classified.decision == IngestDecision::HardReject) {
    log().info << "refuse message_id=" << envelope.message_id << " sender=" << envelope.sender_contact_id
               << " epoch=" << envelope.session_epoch << " seq=" << envelope.sender_seq << ": " << classified.reason;
    if (classified.decision == IngestDecision::SoftCompromised && strict_channel) {
      // D038: private e2e latches compromised until rotate/pause.
      PeerSyncState compromised_state = classified.sync_state;
      compromised_state.phase = PeerSyncPhase::Compromised;
      (void)store_.SetPeerSyncState(resolved_thread_id, envelope.session_epoch, compromised_state, numbering);
      // Defense-in-depth: membership DMs still apply on a frozen private stream.
      if (AppliesOutOfOrder(message)) {
        ThreadMessage persisted = message;
        persisted.id = envelope.message_id;
        persisted.thread_id = resolved_thread_id;
        persisted.sender_contact_id = seq_owner;
        persisted.timestamp = envelope.timestamp;
        persisted.delivery = MessageDelivery::Relayed;
        persisted.relay_visible = true;
        persisted.transport = transport;
        persisted.sender_seq = envelope.sender_seq;
        persisted.session_epoch = envelope.session_epoch;
        persisted.sender_device = RowDevice(numbering);
        if (auto membership = ApplyInboundMembershipMessage(persisted, envelope.sender_contact_id, &outcome);
            !membership) {
          MarkReceiveFailure(outcome, envelope.sender_contact_id, MembershipFailure(membership.error()),
                             membership.error().message, resolved_thread_id);
          return outcome;
        }
        if (auto control = ApplyInboundControl(persisted, envelope, &outcome, resolved_thread_id); !control) {
          return outcome;
        }
        MaybeSuppressInboxChrome(outcome, persisted);
        auto has_id = store_.HasMessageId(resolved_thread_id, envelope.message_id);
        if (has_id && !*has_id && store_.AppendMessage(persisted)) {
          outcome.persisted = true;
          outcome.persisted_message = persisted;
          outcome.thread_changed = true;
          outcome.thread_id = resolved_thread_id;
        } else if (has_id && *has_id) {
          outcome.thread_changed = true;
          outcome.thread_id = resolved_thread_id;
        }
      }
      outcome.decision = IngestDecision::SoftCompromised;
      return outcome;
    }
    // e2e_public SoftCompromised: membership still applies out of order.
    if (classified.decision == IngestDecision::SoftCompromised) {
      apply_out_of_order(classified.decision);
    }
    return outcome;
  }
  if (!classified.persist_message) {
    return outcome;
  }

  ThreadMessage persisted = message;
  persisted.id = envelope.message_id;
  persisted.thread_id = resolved_thread_id;
  persisted.sender_contact_id = seq_owner;
  persisted.timestamp = envelope.timestamp;
  persisted.delivery = MessageDelivery::Relayed;
  persisted.relay_visible = true;
  persisted.transport = transport;
  persisted.sender_seq = envelope.sender_seq;
  persisted.session_epoch = envelope.session_epoch;
  persisted.sender_device = RowDevice(numbering);

  if (auto membership = ApplyInboundMembershipMessage(persisted, envelope.sender_contact_id, &outcome);
      !membership) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, MembershipFailure(membership.error()),
                       membership.error().message, resolved_thread_id);
    return outcome;
  }
  if (auto control = ApplyInboundControl(persisted, envelope, &outcome, resolved_thread_id); !control) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  MaybeSuppressInboxChrome(outcome, persisted);

  TombstoneIfRetractedBeforeArrival(persisted, outcome);
  if (auto cap = RejectIfAnnotationCapExceeded(store_, persisted); !cap) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::ReactionRejected, cap.error().message,
                       resolved_thread_id);
    return outcome;
  }

  if (classified.decision == IngestDecision::AcceptEpochAdvance && !device_copy) {
    // The account-level pair moved on: the thread's epoch follows (a device pair's epoch is its session's,
    // moved by ReconcileE2ePublicSession).
    const uint32_t old_epoch = chat_target_epoch;
    if (!store_.AppendMessageWithPassiveEpochAdopt(persisted, old_epoch, envelope.session_epoch,
                                                   classified.sync_state)) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    replay_windows_.erase(ReplayKey{resolved_thread_id, old_epoch, replay_owner});
    outcome.persisted = true;
    outcome.persisted_message = persisted;
    outcome.thread_changed = true;
    outcome.thread_id = resolved_thread_id;
    ApplyInboundRetract(persisted, outcome);
    if (auto rotate = ApplyInboundPskRotate(resolved_thread_id, envelope, persisted); !rotate) {
      log().warning << "psk_rotate apply failed after persist: " << rotate.error().message;
    }
    return outcome;
  }

  if (!store_.AppendMessage(persisted)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  (void)store_.SetPeerSyncState(resolved_thread_id, envelope.session_epoch, classified.sync_state, numbering);
  if (classified.decision == IngestDecision::AcceptEpochAdvance) {
    replay_windows_.erase(ReplayKey{resolved_thread_id, chat_target_epoch, replay_owner});
  }
  outcome.persisted = true;
  outcome.persisted_message = persisted;
  outcome.thread_changed = true;
  outcome.thread_id = resolved_thread_id;
  ApplyInboundRetract(persisted, outcome);
  if (auto rotate = ApplyInboundPskRotate(resolved_thread_id, envelope, persisted); !rotate) {
    log().warning << "psk_rotate apply failed after persist: " << rotate.error().message;
  }
  return outcome;
}

std::vector<std::string> RelayReceivePipeline::GroupsSharedWith(const std::string& sender,
                                                                const std::string& local_account) const {
  std::vector<std::string> shared;
  auto groups = group_roster_.ListGroupIds();
  if (!groups || sender.empty() || local_account.empty()) {
    return shared;
  }
  for (const std::string& group_id : *groups) {
    auto local_member = group_roster_.IsMember(group_id, local_account);
    auto sender_member = group_roster_.IsMember(group_id, sender);
    if (local_member && *local_member && sender_member && *sender_member) {
      shared.push_back(group_id);
    }
  }
  return shared;
}

std::optional<std::string> RelayReceivePipeline::GroupOfStream(const RelayEnvelope& envelope,
                                                               const std::vector<std::string>& candidates,
                                                               const std::string& local_account) {
  for (const std::string& group_id : candidates) {
    if (!envelope.stream_key.empty() && BuildGroupRelayStreamKey(group_id, envelope.sender_contact_id, local_account,
                                                                 envelope.sender_device) == envelope.stream_key) {
      return group_id;
    }
  }
  return std::nullopt;
}

RelayReceiveOutcome RelayReceivePipeline::ProcessGroupEnvelope(const RelayEnvelope& envelope,
                                                               const std::string& local_relay_user_id,
                                                               const bool authorized_older_backfill,
                                                               const MessageTransport transport) {
  RelayReceiveOutcome outcome;
  if (local_relay_user_id.empty()) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  // The same envelope bounds as the direct path: known version, bounded size.
  if (envelope.envelope_version != kRelayEnvelopeVersion ||
      DumpJson(RelayEnvelopeToJson(envelope)).size() > kMaxRelayEnvelopeBytes) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  // The copy names its group only inside its ciphertext (G018 item 7). Unread, it is refused from a blocked
  // sender, or one in no group with us; once read, the group it names must be one of those (below).
  if (SenderBlocked(envelope.sender_contact_id)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  const std::vector<std::string> shared = GroupsSharedWith(envelope.sender_contact_id, local_relay_user_id);
  if (shared.empty()) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  // Another device's copy in the shared mailbox (M013); our own account's siblings do not write yet (M021 d3).
  if (WhoseCopy(envelope) == CopyTo::Sibling || envelope.sender_contact_id == local_relay_user_id) {
    outcome.decision = IngestDecision::SilentDiscard;
    return outcome;
  }
  const std::string numbering = CopyNumbering(envelope);

  DirectChatTarget inbound_target;
  inbound_target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  inbound_target.peer_identity_value = envelope.sender_contact_id;
  inbound_target.channel = ThreadChannel::E2ePublic;

  auto verified = VerifySignature(envelope, inbound_target);
  if (!verified) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::SignatureCheckFailed, verified.error().message);
    return outcome;
  }
  if (!*verified) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::BadSignature, "Group envelope signature did not verify");
    return outcome;
  }
  outcome.sender_verified = true;

  auto kem_private = CopyKemPrivateKey(envelope);
  if (!kem_private) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::MissingKey, kem_private.error().message);
    return outcome;
  }

  const PeerSessionKey pair_key = CopyTargetKey(envelope, inbound_target, CryptoChannel::E2ePublic);
  AutoKeyRead read;
  auto decrypted = GroupE2ePayloadCodec::DecryptForLocalMember(envelope, local_relay_user_id, pair_key, psk_store_,
                                                                 *kem_private, &read);
  if (!decrypted) {
    outcome.decision = IngestDecision::HardReject;
    std::optional<std::string> tid; // the chat to tell, when the stream id shows which group (a hint, unsigned)
    if (auto group_id = GroupOfStream(envelope, shared, local_relay_user_id)) {
      if (auto existing = store_.FindGroupThread(*group_id); existing && *existing) {
        tid = (**existing).id;
      }
    }
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::DecryptFailed, decrypted.error().message, tid);
    return outcome;
  }
  const std::string group_id = std::move(decrypted->group_id);
  // Right after reading it: both ends must be on this device's roster for the group it names — us (after leave /
  // dismiss / removal peers may still fan out), and the sender (a member, not blocked: checked above).
  if (std::find(shared.begin(), shared.end(), group_id) == shared.end()) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  NoteCopyState(group_id, decrypted->state, envelope.sender_contact_id, outcome); // G018 item 5
  // G013: while our check of a restored group goes unanswered, a member's message shows it is online.
  if (auto metadata = group_roster_.LoadMetadata(group_id);
      metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Checking &&
      util::NowUnixMs() - (*metadata)->sync_requested_at >= group_roster_sync::kCheckRetryMs) {
    outcome.restart_roster_check_group_id = group_id;
  }
  ThreadMessage message = std::move(decrypted->message);
  // Signed by the member (checked above) and read: what that proves about the pair's session is kept,
  // at the pair's epoch, as for a 1:1 message — so the next copy, without the key_init, is read too (G011).
  if (auto kept = ReconcileE2ePublicSession(psk_store_, pair_key, envelope.session_epoch, read,
                                            envelope.body.e2e.key_init_b64, envelope.timestamp);
      !kept) {
    log().warning << "group: pair session not updated: " << kept.error().message;
  }
  if (controls_ && controls_->OnLane(message)) {
    log().info << "drop control-lane message sent in a group stream message_id=" << envelope.message_id;
    outcome.decision = IngestDecision::SilentDiscard;
    return outcome;
  }

  auto thread = store_.FindGroupThread(group_id);
  if (!thread) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::ThreadLookupFailed, thread.error().message);
    return outcome;
  }
  if (!*thread) {
    // Expected: dismissed/deleted locally while older group envelopes remain on relay/peers.
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  const std::string resolved_thread_id = (**thread).id;

  auto has_message_id = store_.HasMessageId(resolved_thread_id, envelope.message_id);
  if (!has_message_id) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::StoreError, has_message_id.error().message,
                       resolved_thread_id);
    return outcome;
  }
  if (*has_message_id) {
    outcome.decision = IngestDecision::BenignDuplicate;
    return outcome;
  }

  PeerSyncState sync_state = DefaultPeerSyncState();
  if (auto loaded = store_.GetPeerSyncState(resolved_thread_id, envelope.session_epoch, numbering)) {
    sync_state = *loaded;
  }

  IngestClassifierInput classifier_input;
  classifier_input.sender_seq = envelope.sender_seq;
  classifier_input.session_epoch = envelope.session_epoch;
  classifier_input.message_id = envelope.message_id;
  classifier_input.sync_state = sync_state;
  classifier_input.chat_target_epoch = envelope.session_epoch;
  classifier_input.strict_mode = false;
  classifier_input.authorized_older_backfill = authorized_older_backfill;
  classifier_input.has_message_id = false;
  classifier_input.existing_message_id_at_seq = FindMessageIdAtSeq(
      resolved_thread_id, envelope.session_epoch, envelope.sender_contact_id, envelope.sender_seq, numbering);

  auto& replay_window =
      ReplayWindowFor(resolved_thread_id, envelope.session_epoch, envelope.sender_contact_id + "/" + numbering);
  const IngestClassifierResult classified = E2eIngestClassifier::Classify(classifier_input, replay_window);
  outcome.decision = classified.decision;
  if (classified.decision == IngestDecision::SilentDiscard || classified.decision == IngestDecision::BenignDuplicate) {
    return outcome;
  }
  if (classified.decision == IngestDecision::SoftCompromised || classified.decision == IngestDecision::HardReject) {
    // Compromised/reject from seq checks — silent here (banner / gap UX elsewhere).
    return outcome;
  }
  if (!classified.persist_message) {
    return outcome;
  }

  ThreadMessage persisted = message;
  persisted.id = envelope.message_id;
  persisted.thread_id = resolved_thread_id;
  persisted.sender_contact_id = envelope.sender_contact_id;
  persisted.timestamp = envelope.timestamp;
  persisted.delivery = MessageDelivery::Relayed;
  persisted.relay_visible = true;
  persisted.transport = transport;
  persisted.sender_seq = envelope.sender_seq;
  persisted.session_epoch = envelope.session_epoch;
  persisted.sender_device = RowDevice(numbering);

  if (auto membership = ApplyInboundMembershipMessage(persisted, envelope.sender_contact_id, &outcome);
      !membership) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, MembershipFailure(membership.error()),
                       membership.error().message, resolved_thread_id);
    return outcome;
  }

  MaybeSuppressInboxChrome(outcome, persisted);  // a family's quiet line (a group call summary)
  TombstoneIfRetractedBeforeArrival(persisted, outcome);
  if (auto cap = RejectIfAnnotationCapExceeded(store_, persisted); !cap) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::ReactionRejected, cap.error().message,
                       resolved_thread_id);
    return outcome;
  }

  if (!store_.AppendMessage(persisted)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  (void)store_.SetPeerSyncState(resolved_thread_id, envelope.session_epoch, classified.sync_state, numbering);
  outcome.persisted = true;
  outcome.persisted_message = persisted;
  outcome.thread_changed = true;
  outcome.thread_id = resolved_thread_id;
  ApplyInboundRetract(persisted, outcome);
  return outcome;
}

namespace {

/** `annotation` withdraws `target`: a retract, aimed at it, from the target's own sender. */
bool IsRetractOf(const ThreadMessage& annotation, const ThreadMessage& target) {
  if (annotation.content_type != ChatContentType::Annotation || annotation.target_message_id != target.id ||
      annotation.sender_contact_id != target.sender_contact_id) {
    return false;
  }
  auto fields = ChatPayloadCodec::DecodeAnnotationJson(annotation.payload_json);
  return fields && fields->annotation_type == kAnnotationTypeRetract;
}

} // namespace

void RelayReceivePipeline::ApplyInboundRetract(const ThreadMessage& persisted, RelayReceiveOutcome& outcome) {
  if (persisted.content_type != ChatContentType::Annotation || !persisted.target_message_id) {
    return;
  }
  auto fields = ChatPayloadCodec::DecodeAnnotationJson(persisted.payload_json);
  if (!fields || fields->annotation_type != kAnnotationTypeRetract) {
    return;
  }
  outcome.deleted_by_sender = true;
  outcome.suppress_inbox_chrome = true;
  if (*persisted.target_message_id == persisted.id) {
    // History placeholder for a message its sender deleted (ChatHistoryResponder): it fills the seq, and
    // shows as "This message was deleted".
    (void)store_.DeleteMessageContent(persisted.thread_id, persisted.id);
    return;
  }
  auto found = store_.GetMessage(persisted.thread_id, *persisted.target_message_id);
  if (!found || !found->has_value()) {
    return; // target not here yet: the stored retract tombstones it on arrival
  }
  const ThreadMessage target = **found;
  if (target.content_type == ChatContentType::Deleted || target.content_type == ChatContentType::Annotation ||
      target.content_type == ChatContentType::System) {
    return; // only a message can be withdrawn, not a reaction or a system line
  }
  if (!IsRetractOf(persisted, target)) {
    log().warning << "retract ignored: not from the message's sender";
    return;
  }
  auto deleted = store_.DeleteMessageContent(persisted.thread_id, target.id);
  if (deleted && *deleted && on_message_retracted_) {
    on_message_retracted_(persisted.thread_id, target);
  }
}

void RelayReceivePipeline::TombstoneIfRetractedBeforeArrival(ThreadMessage& persisted, RelayReceiveOutcome& outcome) {
  if (persisted.content_type == ChatContentType::Annotation || persisted.content_type == ChatContentType::System ||
      persisted.content_type == ChatContentType::Deleted) {
    return;
  }
  auto count = store_.CountAnnotationsForTarget(persisted.thread_id, persisted.id); // cheap, no decrypt
  if (!count || *count == 0) {
    return;
  }
  auto annotations = store_.GetAnnotationsForTarget(persisted.thread_id, persisted.id);
  if (!annotations) {
    return;
  }
  for (const ThreadMessage& annotation : *annotations) {
    if (IsRetractOf(annotation, persisted)) {
      persisted.content_type = ChatContentType::Deleted;
      persisted.text.clear();
      persisted.payload_json.clear();
      persisted.content_rml.reset();
      outcome.deleted_by_sender = true;
      outcome.suppress_inbox_chrome = true;
      return;
    }
  }
}

} // namespace pbr
