#include "lib/messaging/RelayReceivePipeline.h"

#include "lib/messaging/SentCopy.h"

#include "lib/messaging/AccountCard.h"

#include "lib/messaging/DeliveryNotice.h"
#include "lib/messaging/GroupRosterSync.h"

#include <cstdlib>

#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/ReactionTypes.h"

#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/chat/ChatPayloadTypes.h"
#include "lib/messaging/ChatPayloadValidator.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "lib/messaging/EnvelopeSigner.h"
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "common/directory/ShortRelayId.h"
#include "common/chat/MessagingLimits.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/PskRotateCodec.h"
#include "lib/messaging/RelayWirePayload.h"
#include "lib/messaging/SyncStateCodec.h"
#include "lib/messaging/GroupInviteGate.h"
#include "lib/messaging/PublicPskLockCoordinator.h"

#include "common/Logger.h"
#include "common/Utilities.h"
#include "lib/messaging/ControlLaneSeenStore.h"
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
ChatTargetKey CopyTargetKey(const RelayEnvelope& envelope, const DirectChatTarget& sender, const CryptoChannel channel) {
  ChatTargetKey key;
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

/** Membership DMs must apply even when the 1:1 seq stream is SoftCompromised. */
bool IsGroupMembershipControlMessage(const ThreadMessage& message) {
  return GroupMembershipCodec::ControlTypeFromMessage(message).has_value();
}

} // namespace

RelayReceivePipeline::RelayReceivePipeline(IThreadStore& store, AccountCardLookup account_cards,
                                           IPskSessionStore& psk_store, LocalKeys local_keys,
                                           GroupRosterStore& group_roster, GroupInviteGate* invite_gate)
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

void RelayReceivePipeline::KeepCarriedStatement(const ThreadMessage& message) const {
  auto detail = group_roster_sync_codec::DetailOf(message);
  auto statement = detail ? group_roster_sync_codec::StatementIn(*detail) : std::nullopt;
  if (!statement) {
    return;  // an event from an app before G012
  }
  if (auto kept = group_roster_sync::KeepStatement(group_roster_, *statement); !kept) {
    log().warning << "Roster statement not kept group_id=" << statement->group_id << ": " << kept.error().message;
  }
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
  if (auto kept = group_roster_sync::KeepAndCatchUp(group_roster_, *statement); !kept) {
    log().warning << "Roster statement not kept group_id=" << statement->group_id << ": " << kept.error().message;
  }
  return {};
}

void RelayReceivePipeline::MaybeSuppressInboxChrome(RelayReceiveOutcome& outcome, const ThreadMessage& message) const {
  if (controls_ && controls_->Quiet(message)) {
    outcome.suppress_inbox_chrome = true;
  }
}

Roe<void> RelayReceivePipeline::ApplyInviteRefused(const ThreadMessage& message, const std::string& actor_identity,
                                                  const std::string& local_account) const {
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
  // G014: we were never added — drop the group we made on accept; its chat stays, read-only, saying why.
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
      if (auto sys = GroupMembershipCodec::BuildSystemMessage((*thread)->id, GroupMembershipControlType::GroupInviteRefused,
                                                              "The group is full: you were not added", *detail,
                                                              actor_identity)) {
        (void)store_.AppendMessage(*sys);
      }
    }
  }
  log().info << "group invite refused group_id=" << refused->group_id << " reason=" << refused->reason;
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
    return ApplyInviteRefused(message, actor_identity, local_account);
  }
  if (kind == GroupMembershipControlType::GroupInviteAccept ||
      kind == GroupMembershipControlType::GroupInviteDecline) {
    auto response = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
    if (!response) {
      return response.error();
    }
    if (response->control_type == GroupMembershipControlType::GroupInviteAccept) {
      // An invite this device issued for a group it owns is required. Owner then publishes member_joined.
      auto applied = ApplyInviteAcceptToRoster(group_roster_, response->invite_nonce, actor_identity,
                                               response->group_id, local_account, util::NowUnixMs());
      if (!applied && applied.error().message == kGroupFullError) {
        if (outcome) {
          outcome->refuse_invite_group_id = response->group_id;
          outcome->refuse_invite_nonce = response->invite_nonce;
          outcome->refuse_invite_member = actor_identity;
        }
        return {};
      }
      if (!applied) {
        return applied.error();
      }
      if (!*applied) {
        return {};  // already accepted: nothing new to apply or announce
      }
      auto metadata = group_roster_.LoadMetadata(response->group_id);
      if (outcome && metadata && *metadata) {
        outcome->publish_member_joined_group_id = response->group_id;
        outcome->publish_member_joined_member_identity = actor_identity;
        outcome->publish_member_joined_epoch = (*metadata)->roster_epoch;
      }
      return {};
    }
    if (auto declined = ApplyInviteDeclineToRoster(group_roster_, response->invite_nonce, actor_identity,
                                                   response->group_id);
        !declined) {
      return declined.error();
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
    if (auto applied = ApplyMemberJoinedToRoster(group_roster_, *joined, actor_identity); !applied) {
      return applied.error();
    }
    KeepCarriedStatement(message);
    // What the owner says of the other members: kept; the cards are then left out of what is stored.
    bool carried_cards = false;
    for (const GroupMembershipCodec::MemberJoinedEntry& entry : joined->members) {
      carried_cards = carried_cards || entry.card.has_value();
      if (entry.member_identity != local_account && (entry.card || !entry.relay_user_id.empty()) &&
          on_roster_member_) {
        on_roster_member_(entry);
      }
    }
    if (carried_cards) {
      if (auto stripped = GroupMembershipCodec::WithoutCards(*joined); stripped) {
        Object payload;
        payload.set("control_type", GroupMembershipControlTypeToWire(GroupMembershipControlType::MemberJoined));
        payload.set("detail", *stripped);
        message.payload_json = DumpJson(payload);
      }
    }
    auto thread = store_.FindGroupThread(joined->group_id);
    if (thread && *thread) {
      auto detail = GroupMembershipCodec::EncodeMemberJoined(joined->group_id, joined->member_identity, joined->role,
                                                             joined->roster_epoch);
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

  if (kind == GroupMembershipControlType::MemberLeft) {
    auto left = GroupMembershipCodec::DecodeMemberLeftFromMessage(message);
    if (!left) {
      return left.error();
    }
    if (auto ignore = ignore_if_not_member(left->group_id); ignore && *ignore) {
      return {};
    }
    if (auto applied = ApplyMemberLeftToRoster(group_roster_, *left, actor_identity); !applied) {
      return applied.error();
    }
    if (outcome) {
      auto metadata = group_roster_.LoadMetadata(left->group_id);
      if (metadata && *metadata && (*metadata)->owner_identity == local_account) {
        outcome->resign_roster_group_id = left->group_id;
      }
    }
    auto thread = store_.FindGroupThread(left->group_id);
    if (thread && *thread) {
      auto detail =
          GroupMembershipCodec::EncodeMemberLeft(left->group_id, left->member_identity, left->roster_epoch);
      if (detail) {
        auto sys = GroupMembershipCodec::BuildSystemMessage((*thread)->id, GroupMembershipControlType::MemberLeft,
                                                            "Member left", *detail, actor_identity);
        if (sys) {
          (void)store_.AppendMessage(*sys);
        }
      }
    }
    return {};
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
    KeepCarriedStatement(message);
    if (!local_account.empty() && removed->member_identity == local_account) {
      (void)group_roster_.ClearGroupTarget(removed->group_id);
    }
    auto thread = store_.FindGroupThread(removed->group_id);
    if (thread && *thread) {
      auto detail = GroupMembershipCodec::EncodeMemberRemoved(removed->group_id, removed->member_identity,
                                                             removed->roster_epoch);
      if (detail) {
        auto sys = GroupMembershipCodec::BuildSystemMessage(
            (*thread)->id, GroupMembershipControlType::MemberRemoved, "Member removed", *detail, actor_identity);
        if (sys) {
          (void)store_.AppendMessage(*sys);
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
  // A group we already know is the owner's to invite to, and not one we are in already.
  if (auto known = group_roster_.LoadMetadata(invite->group_id); !known) {
    return known.error();
  } else if (*known) {
    if ((*known)->owner_identity != actor_identity) {
      return Error("Invite rejected: sender does not own the group");
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
  if (auto recorded = group_roster_.UpsertPendingInvite(pending); !recorded) {
    return recorded.error();
  }
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
  if (target.peer_identity_kind != ContactIdKindToString(mesh::ContactIdKind::Account) ||
      envelope.sender_device.empty()) {
    return Error("Envelope names no sending device of an account");
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
  }
  const AccountCard* card = carried ? &*carried : (kept ? &*kept : nullptr);
  if (!card) {
    return Error("No account card for the sender");
  }
  const AccountCardDevice* device = FindCardDevice(*card, envelope.sender_device);
  if (!device) {
    return Error("The sending device is not on the sender's account card");
  }
  auto verified = EnvelopeSigner::Verify(envelope, device->signing_public_key_b64);
  if (verified && *verified && carried && on_sender_card_) {
    on_sender_card_(*carried);
  }
  return verified;
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
  if (!selected) {
    RelayReceiveOutcome outcome;
    outcome.decision = IngestDecision::SilentDiscard;
    const bool chat = received.route.kind == kRelayRouteGroup || received.route.kind == kRelayRouteDirect;
    if (chat && self && !received.sender_contact_id.empty() && received.sender_contact_id != local_relay_user_id &&
        !received.sender_device.empty() && !received.message_id.empty()) {
      outcome.missing_copy = RelayReceiveOutcome::MissingCopy{.sender_account = received.sender_contact_id,
                                                              .sender_device = received.sender_device,
                                                              .message_id = received.message_id,
                                                              .group_id = received.route.group_id.value_or("")};
    }
    return outcome;
  }
  const RelayEnvelope& envelope = *selected;
  if (envelope.route.kind == kRelayRouteGroup) {
    return ProcessGroupEnvelope(envelope, local_relay_user_id, authorized_older_backfill, transport);
  }
  if (envelope.route.kind == kRelayRouteControl) {
    return ProcessControlEnvelope(envelope, local_relay_user_id);
  }
  return ProcessDirectEnvelope(envelope, local_relay_user_id, authorized_older_backfill, transport);
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

RelayReceiveOutcome RelayReceivePipeline::ProcessControlEnvelope(const RelayEnvelope& envelope,
                                                                 const std::string& local_relay_user_id) {
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
    auto verified = VerifySignature(envelope, inbound_target);
    outcome.sender_verified = verified && *verified;
    outcome.from_sibling = outcome.sender_verified;
    outcome.decision = IngestDecision::SilentDiscard;
    if (outcome.sender_verified) {
      ApplySiblingNotice(envelope, local_relay_user_id, inbound_target, outcome);
    }
    return outcome;
  }
  auto verified = VerifySignature(envelope, inbound_target);
  if (!verified || !*verified) {
    log().warning << "control lane: signature check failed message_id=" << envelope.message_id
                  << " sender=" << envelope.sender_contact_id
                  << (verified ? std::string() : " err=" + verified.error().message);
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
  const ChatTargetKey target_key = CopyTargetKey(envelope, inbound_target, CryptoChannel::E2ePublic);
  AutoKeyRead read;
  auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_relay_user_id, target_key, psk_store_,
                                                         *kem_private, &read);
  if (!decrypted) {
    log().warning << "control lane: couldn't decrypt message_id=" << envelope.message_id
                  << " err=" << decrypted.error().message;
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
  const ChatTargetKey target_key = CopyTargetKey(envelope, inbound_target, CryptoChannel::E2ePublic);
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
        const ChatTargetKey target_key = CopyTargetKey(envelope, inbound_target,
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
  const ChatTargetKey copy_key =
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

    const ChatTargetKey& target_key = copy_key;

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
          MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::MembershipUpdateFailed,
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
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::MembershipUpdateFailed,
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

RelayReceiveOutcome RelayReceivePipeline::ProcessGroupEnvelope(const RelayEnvelope& envelope,
                                                               const std::string& local_relay_user_id,
                                                               const bool authorized_older_backfill,
                                                               const MessageTransport transport) {
  RelayReceiveOutcome outcome;
  if (!envelope.route.group_id || local_relay_user_id.empty()) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  // The same envelope bounds as the direct path: known version, bounded size.
  if (envelope.envelope_version != kRelayEnvelopeVersion ||
      DumpJson(RelayEnvelopeToJson(envelope)).size() > kMaxRelayEnvelopeBytes) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  const std::string& group_id = *envelope.route.group_id;
  // Both ends must be on this device's roster for the group: us (after leave / dismiss / removal
  // peers may still fan out — silent), and the sender (a member, not blocked).
  auto local_member = group_roster_.IsMember(group_id, local_relay_user_id);
  if (!local_member || !*local_member) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  if (SenderBlocked(envelope.sender_contact_id)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  auto sender_member = group_roster_.IsMember(group_id, envelope.sender_contact_id);
  if (!sender_member || !*sender_member) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  // G013: while our check of a restored group goes unanswered, a member's message shows it is online.
  if (auto metadata = group_roster_.LoadMetadata(group_id);
      metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Checking &&
      util::NowUnixMs() - (*metadata)->sync_requested_at >= group_roster_sync::kCheckRetryMs) {
    outcome.restart_roster_check_group_id = group_id;
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

  const ChatTargetKey pair_key = CopyTargetKey(envelope, inbound_target, CryptoChannel::E2ePublic);
  AutoKeyRead read;
  auto decrypted = GroupE2ePayloadCodec::DecryptForLocalMember(envelope, local_relay_user_id, pair_key, psk_store_,
                                                                 *kem_private, &read);
  if (!decrypted) {
    outcome.decision = IngestDecision::HardReject;
    std::optional<std::string> tid;
    if (auto existing = store_.FindGroupThread(group_id); existing && *existing) {
      tid = (**existing).id;
    }
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::DecryptFailed, decrypted.error().message, tid);
    return outcome;
  }
  ThreadMessage message = std::move(*decrypted);
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
    MarkReceiveFailure(outcome, envelope.sender_contact_id, ReceiveFailure::MembershipUpdateFailed,
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
