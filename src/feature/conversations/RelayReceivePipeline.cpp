#include "feature/conversations/RelayReceivePipeline.h"

#include "domain/messaging/AutoKeyEnvelopeResolver.h"
#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/chat/ChatPayloadTypes.h"
#include "domain/messaging/ChatPayloadValidator.h"
#include "domain/messaging/E2eRelayPayloadCodec.h"
#include "domain/messaging/EnvelopeSigner.h"
#include "domain/messaging/GroupMembershipApply.h"
#include "domain/messaging/GroupMembershipCodec.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/InitiationBillingCodec.h"
#include "domain/messaging/InitiationBillingStore.h"
#include "domain/messaging/InitiationPricing.h"
#include "common/chat/MessagingJson.h"
#include "common/directory/DirectoryJson.h"
#include "common/chat/MessagingLimits.h"
#include "domain/messaging/GroupE2ePayloadCodec.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/PskRotateCodec.h"
#include "domain/messaging/RelayWirePayload.h"
#include "domain/messaging/SyncStateCodec.h"
#include "domain/people/ContactTypes.h"
#include "domain/people/PeerDisplayLabel.h"
#include "feature/conversations/calls/CallSessionManager.h"
#include "feature/conversations/GroupInviteGate.h"
#include "domain/messaging/PublicPskLockCoordinator.h"

#include "common/Logger.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

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
  target.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  target.peer_identity_value = envelope.sender_contact_id;
  target.channel = envelope.route.channel;
  return target;
}

void MarkReceiveFailure(RelayReceiveOutcome& outcome, const std::string& sender, const std::string& reason_short,
                        const std::string& detail, const std::optional<std::string>& thread_id = std::nullopt) {
  const std::string who = sender.empty() ? std::string("a peer") : ShortRelayId(sender);
  outcome.receive_failure_notice = "Couldn't read a message from " + who + " (" + reason_short + ").";
  outcome.receive_failure_detail = detail.empty() ? reason_short : detail;
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

bool IsCallControlMessage(const ThreadMessage& message) {
  return CallControlCodec::IsCallControlMessage(message);
}

} // namespace

RelayReceivePipeline::RelayReceivePipeline(IThreadStore& store, IPeerSigningKeyResolver& signing_keys,
                                           IPskSessionStore& psk_store, IdentityStore& identity,
                                           GroupRosterStore& group_roster, GroupInviteGate* invite_gate)
    : store_(store), signing_keys_(signing_keys), psk_store_(psk_store), identity_(identity),
      group_roster_(group_roster), invite_gate_(invite_gate), public_lock_(store, psk_store) {
  redirectLogger("RelayReceivePipeline");
}

Roe<void> RelayReceivePipeline::ApplyInboundCallMessage(ThreadMessage& message,
                                                        const std::string& actor_identity,
                                                        const std::optional<int64_t> relay_created_at_ms,
                                                        const std::optional<int64_t> relay_server_time_ms) const {
  if (!call_sessions_ || !IsCallControlMessage(message)) {
    return {};
  }
  return call_sessions_->ApplyInboundControl(message, actor_identity, relay_created_at_ms, relay_server_time_ms);
}

Roe<void> RelayReceivePipeline::ApplyInboundBillingMessage(ThreadMessage& message,
                                                           const std::string& actor_identity) const {
  if (!initiation_billing_) {
    return {};
  }
  const auto type = InitiationBillingCodec::ControlTypeFromMessage(message);
  if (!type) {
    return {};
  }
  auto payload = TryParseObject(message.payload_json);
  auto detail_text = payload ? payload->getString("detail") : std::nullopt;
  if (!detail_text) {
    return Error("Initiation billing control missing detail");
  }
  const std::string detail_json = *detail_text;
  const std::string peer = actor_identity.empty() ? std::string() : actor_identity;

  switch (*type) {
  case InitiationBillingControlType::ChargeRequired: {
    auto charge = InitiationBillingCodec::DecodeChargeRequired(detail_json);
    if (!charge) {
      return charge.error();
    }
    const std::string key = charge->peer_identity.empty() ? peer : charge->peer_identity;
    if (key.empty()) {
      return {};
    }
    (void)initiation_billing_->SetFloor(key, charge->floor_minor);
    (void)initiation_billing_->MarkClosed(key);
    log().info << "charge_required from " << key << " floor=" << charge->floor_minor;
    return {};
  }
  case InitiationBillingControlType::InitiationOffer: {
    auto offer = InitiationBillingCodec::DecodeInitiationOffer(detail_json);
    if (!offer) {
      return offer.error();
    }
    const std::string key = offer->peer_identity.empty() ? peer : offer->peer_identity;
    int64_t local_floor = 0;
    if (auto id = identity_.Get()) {
      local_floor = id->initiation_floor;
    }
    if (local_floor > 0) {
      if (auto ok = InitiationPricing::CheckOfferAgainstFloor(offer->offer_minor, local_floor); !ok) {
        log().info << "initiation_offer rejected offer_too_low peer=" << key
                   << " offer=" << offer->offer_minor << " floor=" << local_floor;
        return {}; // soft drop; sender sees no accept
      }
    }
    (void)initiation_billing_->MarkOffered(key, offer->offer_minor, local_floor > 0 ? local_floor : offer->floor_minor);
    return {};
  }
  case InitiationBillingControlType::InitiationAccept: {
    auto accept = InitiationBillingCodec::DecodeInitiationAccept(detail_json);
    if (!accept) {
      return accept.error();
    }
    const std::string key = accept->peer_identity.empty() ? peer : accept->peer_identity;
    // waive or take_all both open the relationship; settlement deferred.
    (void)initiation_billing_->MarkOpen(key);
    return {};
  }
  }
  return {};
}

Roe<void> RelayReceivePipeline::ApplyInboundMembershipMessage(ThreadMessage& message,
                                                              const std::string& actor_identity,
                                                              RelayReceiveOutcome* outcome) const {
  // Resolve group_id from any membership control we understand, then ignore events for groups
  // the local user has already left/dismissed (prevents transfer-to-leaver resurrection).
  auto local_id = identity_.Get();
  const std::string local_account =
      (local_id && !local_id->account_id.empty()) ? local_id->account_id : std::string();

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

  auto response = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
  if (response) {
    if (response->control_type == GroupMembershipControlType::GroupInviteAccept) {
      // Pending invite is required — no fallback seed. Owner then publishes member_joined.
      if (auto applied = ApplyInviteAcceptToRoster(group_roster_, response->invite_nonce, actor_identity);
          !applied) {
        return applied.error();
      }
      auto metadata = group_roster_.LoadMetadata(response->group_id);
      if (outcome && metadata && *metadata) {
        outcome->publish_member_joined_group_id = response->group_id;
        outcome->publish_member_joined_member_identity = actor_identity;
        outcome->publish_member_joined_epoch = (*metadata)->roster_epoch;
      }
      return {};
    }
    if (auto declined = ApplyInviteDeclineToRoster(group_roster_, response->invite_nonce, actor_identity);
        !declined) {
      return declined.error();
    }
    return {};
  }
  if (response.error().message.find("not a group invite response") == std::string::npos) {
    return response.error();
  }

  auto joined = GroupMembershipCodec::DecodeMemberJoinedFromMessage(message);
  if (joined) {
    if (auto ignore = ignore_if_not_member(joined->group_id); ignore && *ignore) {
      return {};
    }
    if (auto applied = ApplyMemberJoinedToRoster(group_roster_, *joined, actor_identity); !applied) {
      return applied.error();
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
  if (joined.error().message.find("not a member_joined") == std::string::npos) {
    return joined.error();
  }

  auto transferred = GroupMembershipCodec::DecodeOwnerTransferredFromMessage(message);
  if (transferred) {
    if (auto ignore = ignore_if_not_member(transferred->group_id); ignore && *ignore) {
      return {};
    }
    if (auto applied = ApplyOwnerTransferredToRoster(group_roster_, *transferred, actor_identity); !applied) {
      return applied.error();
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
  if (transferred.error().message.find("not an owner_transferred") == std::string::npos) {
    return transferred.error();
  }

  auto left = GroupMembershipCodec::DecodeMemberLeftFromMessage(message);
  if (left) {
    if (auto ignore = ignore_if_not_member(left->group_id); ignore && *ignore) {
      return {};
    }
    if (auto applied = ApplyMemberLeftToRoster(group_roster_, *left, actor_identity); !applied) {
      return applied.error();
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
  if (left.error().message.find("not a member_left") == std::string::npos) {
    return left.error();
  }

  auto removed = GroupMembershipCodec::DecodeMemberRemovedFromMessage(message);
  if (removed) {
    // If we are the removed member and already left, ignore; if still a member, drop self below.
    if (auto ignore = ignore_if_not_member(removed->group_id); ignore && *ignore) {
      return {};
    }
    if (auto applied = ApplyMemberRemovedToRoster(group_roster_, *removed, actor_identity); !applied) {
      return applied.error();
    }
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
  if (removed.error().message.find("not a member_removed") == std::string::npos) {
    return removed.error();
  }

  auto renamed = GroupMembershipCodec::DecodeGroupRenamedFromMessage(message);
  if (renamed) {
    if (auto ignore = ignore_if_not_member(renamed->group_id); ignore && *ignore) {
      return {};
    }
    auto metadata = group_roster_.LoadMetadata(renamed->group_id);
    if (!metadata) {
      return metadata.error();
    }
    if (!*metadata) {
      return Error("Unknown group for rename");
    }
    if ((*metadata)->owner_identity != actor_identity) {
      return Error("Rename rejected: actor is not the group owner");
    }
    GroupMetadata updated = **metadata;
    updated.title = renamed->title;
    if (renamed->roster_epoch > updated.roster_epoch) {
      updated.roster_epoch = renamed->roster_epoch;
    }
    if (auto saved = group_roster_.UpsertMetadata(updated); !saved) {
      return saved.error();
    }
    auto thread = store_.FindGroupThread(renamed->group_id);
    if (thread && *thread) {
      Thread row = **thread;
      row.title = renamed->title;
      row.updated_at = util::NowUnixMs();
      (void)store_.UpsertThread(row);
      auto detail =
          GroupMembershipCodec::EncodeGroupRenamed(renamed->group_id, renamed->title, updated.roster_epoch);
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
  if (renamed.error().message.find("not a group rename") == std::string::npos) {
    return renamed.error();
  }

  if (!invite_gate_) {
    return {};
  }

  auto invite = GroupMembershipCodec::DecodeInviteFromMessage(message);
  if (!invite) {
    if (invite.error().message.find("not a group invite") != std::string::npos) {
      return {};
    }
    return invite.error();
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
  if (auto recorded = group_roster_.UpsertPendingInvite(pending); !recorded) {
    return recorded.error();
  }
  message.chat_actions = GroupMembershipCodec::BuildInviteChatActions(*invite);
  return {};
}

ReplayWindow& RelayReceivePipeline::ReplayWindowFor(const std::string& thread_id, const uint32_t session_epoch) {
  const ReplayKey key{thread_id, session_epoch};
  return replay_windows_[key];
}

Roe<bool> RelayReceivePipeline::VerifySignature(const RelayEnvelope& envelope,
                                              const DirectChatTarget& target) const {
  auto key = signing_keys_.Resolve(target.peer_identity_kind, target.peer_identity_value);
  if (!key) {
    return key.error();
  }
  return EnvelopeSigner::Verify(envelope, key->signing_public_key_b64);
}

Roe<void> RelayReceivePipeline::PersistDerivedAutoKeyPsk(const RelayEnvelope& envelope,
                                                        const ChatTargetKey& target_key,
                                                        const ByteVector& master_psk) const {
  auto existing = psk_store_.Load(target_key);
  if (!existing) {
    return existing.error();
  }
  PskSessionRecord record;
  if (*existing) {
    record = **existing;
  } else {
    record.key = target_key;
    record.session_epoch = envelope.session_epoch;
  }
  if (!record.master_psk_b64.has_value()) {
    record.master_psk_b64 = Base64Encode(master_psk);
    record.session_epoch = envelope.session_epoch;
    return psk_store_.Save(record);
  }
  return {};
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
  auto kem = identity_.GetOrCreateHybridKemPrivateKey();
  if (!kem) {
    return kem.error();
  }
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  auto applied = public_lock_.ApplyInbound(thread_id, envelope, message, *kem, identity->account_id,
                                          util::NowUnixMs());
  if (!applied) {
    return applied.error();
  }
  return {};
}

std::optional<std::string> RelayReceivePipeline::FindMessageIdAtSeq(const std::string& thread_id,
                                                                    const uint32_t session_epoch,
                                                                    const std::string& seq_owner_contact_id,
                                                                    const uint64_t sender_seq) const {
  SeqRangeQuery query;
  query.session_epoch = session_epoch;
  query.seq_owner_contact_id = seq_owner_contact_id;
  query.min_sender_seq = sender_seq;
  query.max_sender_seq = sender_seq;
  query.limit = 1;
  auto rows = store_.GetMessagesBySeqRange(thread_id, query);
  if (!rows || rows->empty()) {
    return std::nullopt;
  }
  return rows->front().id;
}

RelayReceiveOutcome RelayReceivePipeline::ProcessEnvelope(const RelayEnvelope& envelope,
                                                          const std::string& local_relay_user_id,
                                                          const bool authorized_older_backfill,
                                                          const MessageTransport transport) {
  if (envelope.route.kind == "group") {
    return ProcessGroupEnvelope(envelope, local_relay_user_id, authorized_older_backfill, transport);
  }
  return ProcessDirectEnvelope(envelope, local_relay_user_id, authorized_older_backfill, transport);
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

  const DirectChatTarget inbound_target = InboundTargetFromEnvelope(envelope);
  auto thread = store_.FindDirectThread(inbound_target);
  if (!thread) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "thread lookup failed", thread.error().message);
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
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "sender mismatch", "Envelope sender is not the thread peer",
                       (*thread)->id);
    return outcome;
  }

  std::string resolved_thread_id;
  if (!auto_create) {
    resolved_thread_id = (*thread)->id;
    auto has_message_id = store_.HasMessageId(resolved_thread_id, envelope.message_id);
    if (!has_message_id) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, "store error", has_message_id.error().message,
                         resolved_thread_id);
      return outcome;
    }
    if (*has_message_id) {
      outcome.decision = IngestDecision::BenignDuplicate;
      // Message body may never have run call-control (history sync / prior drop). Decrypt and
      // apply idempotent CallMediaKey/Accept side effects before returning.
      auto verified_dup = VerifySignature(envelope, inbound_target);
      if (verified_dup && *verified_dup && E2eRelayPayloadCodec::RequiresEncryption(envelope.route.channel)) {
        ChatTargetKey target_key;
        target_key.peer_identity_kind = inbound_target.peer_identity_kind;
        target_key.peer_identity_value = inbound_target.peer_identity_value;
        target_key.channel = E2eRelayPayloadCodec::ChannelFromThread(envelope.route.channel);
        std::optional<ByteVector> local_kem_private_key;
        if (envelope.route.channel == ThreadChannel::E2ePublic) {
          // Account KEM secret (shared across linked devices after import — M015).
          if (auto kem_private = identity_.GetOrCreateHybridKemPrivateKey()) {
            local_kem_private_key = std::move(*kem_private);
          }
        }
        if (auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_relay_user_id, target_key,
                                                                   psk_store_, local_kem_private_key)) {
          if (IsCallControlMessage(*decrypted)) {
            log().warning
                << "Apply call-control on BenignDuplicate message_id=" << envelope.message_id;
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
            (void)ApplyInboundCallMessage(side, envelope.sender_contact_id, envelope.relay_created_at_ms,
                                          envelope.relay_server_time_ms);
          }
        }
      }
      return outcome;
    }
  }

  auto verified = VerifySignature(envelope, inbound_target);
  if (!verified) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "signature check failed", verified.error().message,
                       resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
    return outcome;
  }
  if (!*verified) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "bad signature", "Envelope signature did not verify",
                       resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
    return outcome;
  }

  ThreadMessage message;
  if (E2eRelayPayloadCodec::RequiresEncryption(envelope.route.channel)) {
    if (local_relay_user_id.empty()) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, "missing local identity",
                         "Local account identity unavailable for decrypt",
                         resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
      return outcome;
    }
    const std::string& local_aad_id = local_relay_user_id;

    ChatTargetKey target_key;
    target_key.peer_identity_kind = inbound_target.peer_identity_kind;
    target_key.peer_identity_value = inbound_target.peer_identity_value;
    target_key.channel = E2eRelayPayloadCodec::ChannelFromThread(envelope.route.channel);

    std::optional<ByteVector> local_kem_private_key;
    if (envelope.route.channel == ThreadChannel::E2ePublic) {
      auto kem_private = identity_.GetOrCreateHybridKemPrivateKey();
      if (!kem_private) {
        outcome.decision = IngestDecision::HardReject;
        MarkReceiveFailure(outcome, envelope.sender_contact_id, "missing decryption key", kem_private.error().message,
                           resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
        return outcome;
      }
      local_kem_private_key = std::move(*kem_private);
    }

    auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_aad_id, target_key, psk_store_,
                                                           local_kem_private_key);
    if (!decrypted) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't decrypt", decrypted.error().message,
                         resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
      return outcome;
    }
    message = std::move(*decrypted);

    if (envelope.route.channel == ThreadChannel::E2ePublic && local_kem_private_key.has_value() &&
        !PskRotateCodec::IsPskRotateMessage(message)) {
      auto master_psk = ResolveOrDeriveMasterPsk(envelope, target_key, psk_store_,
                                                                       *local_kem_private_key);
      if (master_psk) {
        (void)PersistDerivedAutoKeyPsk(envelope, target_key, *master_psk);
      }
    }
  } else {
    auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64);
    if (!decoded) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't decode", decoded.error().message,
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
      MarkReceiveFailure(outcome, envelope.sender_contact_id, "unsupported content",
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
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "unsupported content",
                       "Decoded message content type not supported",
                       resolved_thread_id.empty() ? std::nullopt : std::optional(resolved_thread_id));
    return outcome;
  }

  if (auto_create) {
    const std::string fallback_title = ShortRelayId(envelope.sender_contact_id);
    auto created = store_.FindOrCreateDirectThread(
        inbound_target, "", fallback_title.empty() ? envelope.sender_contact_id : fallback_title);
    if (!created) {
      outcome.decision = IngestDecision::HardReject;
      MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't open chat", created.error().message);
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
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't apply key update", rotate.error().message,
                       resolved_thread_id);
    return outcome;
  }

  PeerSyncState sync_state;
  uint32_t chat_target_epoch = envelope.session_epoch;
  if (auto_create) {
    sync_state = DefaultPeerSyncState();
  } else {
    auto loaded_sync_state = store_.GetPeerSyncState(resolved_thread_id, envelope.session_epoch);
    if (!loaded_sync_state) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    sync_state = *loaded_sync_state;

    auto loaded_epoch = store_.GetChatTargetSessionEpoch(resolved_thread_id);
    if (!loaded_epoch) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    chat_target_epoch = *loaded_epoch;
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
      FindMessageIdAtSeq(resolved_thread_id, envelope.session_epoch, seq_owner, envelope.sender_seq);

  auto& replay_window = ReplayWindowFor(resolved_thread_id, envelope.session_epoch);
  const IngestClassifierResult classified = E2eIngestClassifier::Classify(classifier_input, replay_window);
  outcome.decision = classified.decision;

  auto apply_call_control_side_effects = [&](IngestDecision decision_label) {
    if (!IsCallControlMessage(message) && !IsGroupMembershipControlMessage(message)) {
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
    if (IsGroupMembershipControlMessage(side)) {
      (void)ApplyInboundMembershipMessage(side, envelope.sender_contact_id, &outcome);
    }
    if (IsCallControlMessage(side)) {
      log().warning
          << "Apply call-control on " << static_cast<int>(decision_label)
          << " message_id=" << envelope.message_id;
      if (auto call = ApplyInboundCallMessage(side, envelope.sender_contact_id, envelope.relay_created_at_ms,
                                              envelope.relay_server_time_ms);
          !call) {
        log().warning
            << "Call-control side-effect failed: " << call.error().message;
      }
    }
    (void)ApplyInboundBillingMessage(side, envelope.sender_contact_id);
  };

  if (classified.decision == IngestDecision::SilentDiscard || classified.decision == IngestDecision::BenignDuplicate) {
    // MediaKey/Accept are idempotent — do not skip when classifier drops a resent seq/id.
    apply_call_control_side_effects(classified.decision);
    return outcome;
  }
  const bool strict_channel = envelope.route.channel == ThreadChannel::E2e;
  if (classified.decision == IngestDecision::SoftCompromised || classified.decision == IngestDecision::HardReject) {
    if (classified.decision == IngestDecision::SoftCompromised && strict_channel) {
      // D038: private e2e latches compromised until rotate/pause.
      PeerSyncState compromised_state = classified.sync_state;
      compromised_state.phase = PeerSyncPhase::Compromised;
      (void)store_.SetPeerSyncState(resolved_thread_id, envelope.session_epoch, compromised_state);
      // Defense-in-depth: membership DMs still apply on a frozen private stream.
      if (IsGroupMembershipControlMessage(message) || IsCallControlMessage(message)) {
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
        if (auto membership = ApplyInboundMembershipMessage(persisted, envelope.sender_contact_id, &outcome);
            !membership) {
          MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't apply membership update",
                             membership.error().message, resolved_thread_id);
          return outcome;
        }
        if (auto call = ApplyInboundCallMessage(persisted, envelope.sender_contact_id, envelope.relay_created_at_ms,
                                                envelope.relay_server_time_ms);
            !call) {
          MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't apply call update", call.error().message,
                             resolved_thread_id);
          return outcome;
        }
        auto has_id = store_.HasMessageId(resolved_thread_id, envelope.message_id);
        if (has_id && !*has_id && store_.AppendMessage(persisted)) {
          outcome.persisted = true;
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
    // e2e_public SoftCompromised/HardReject: still apply call-control (MediaKey) when decoded.
    if (classified.decision == IngestDecision::SoftCompromised || IsCallControlMessage(message)) {
      apply_call_control_side_effects(classified.decision);
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

  if (auto membership = ApplyInboundMembershipMessage(persisted, envelope.sender_contact_id, &outcome);
      !membership) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't apply membership update",
                       membership.error().message, resolved_thread_id);
    return outcome;
  }
  if (auto call = ApplyInboundCallMessage(persisted, envelope.sender_contact_id, envelope.relay_created_at_ms,
                                          envelope.relay_server_time_ms);
      !call) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't apply call update", call.error().message,
                       resolved_thread_id);
    return outcome;
  }
  if (auto billing = ApplyInboundBillingMessage(persisted, envelope.sender_contact_id); !billing) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't apply billing update",
                       billing.error().message, resolved_thread_id);
    return outcome;
  }

  if (auto cap = RejectIfAnnotationCapExceeded(store_, persisted); !cap) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't accept reaction", cap.error().message,
                       resolved_thread_id);
    return outcome;
  }

  if (classified.decision == IngestDecision::AcceptEpochAdvance) {
    const uint32_t old_epoch = chat_target_epoch;
    if (!store_.AppendMessageWithPassiveEpochAdopt(persisted, old_epoch, envelope.session_epoch,
                                                   classified.sync_state)) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    replay_windows_.erase(ReplayKey{resolved_thread_id, old_epoch});
    outcome.persisted = true;
    outcome.thread_changed = true;
    outcome.thread_id = resolved_thread_id;
    if (auto rotate = ApplyInboundPskRotate(resolved_thread_id, envelope, persisted); !rotate) {
      log().warning << "psk_rotate apply failed after persist: " << rotate.error().message;
    }
    return outcome;
  }

  if (!store_.AppendMessage(persisted)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  (void)store_.SetPeerSyncState(resolved_thread_id, envelope.session_epoch, classified.sync_state);
  outcome.persisted = true;
  outcome.thread_changed = true;
  outcome.thread_id = resolved_thread_id;
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
  const std::string& group_id = *envelope.route.group_id;
  if (!group_roster_.IsMember(group_id, local_relay_user_id)) {
    // Expected after leave/dismiss while peers still fan out — silent.
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  DirectChatTarget inbound_target;
  inbound_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  inbound_target.peer_identity_value = envelope.sender_contact_id;
  inbound_target.channel = ThreadChannel::E2ePublic;

  auto verified = VerifySignature(envelope, inbound_target);
  if (!verified) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "signature check failed", verified.error().message);
    return outcome;
  }
  if (!*verified) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "bad signature", "Group envelope signature did not verify");
    return outcome;
  }

  auto kem_private = identity_.GetOrCreateHybridKemPrivateKey();
  if (!kem_private) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "missing decryption key", kem_private.error().message);
    return outcome;
  }

  auto decrypted = GroupE2ePayloadCodec::DecryptForLocalMember(envelope, local_relay_user_id, psk_store_,
                                                                 *kem_private);
  if (!decrypted) {
    outcome.decision = IngestDecision::HardReject;
    std::optional<std::string> tid;
    if (auto existing = store_.FindGroupThread(group_id); existing && *existing) {
      tid = (**existing).id;
    }
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't decrypt", decrypted.error().message, tid);
    return outcome;
  }
  ThreadMessage message = std::move(*decrypted);

  auto thread = store_.FindGroupThread(group_id);
  if (!thread) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "thread lookup failed", thread.error().message);
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
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "store error", has_message_id.error().message,
                       resolved_thread_id);
    return outcome;
  }
  if (*has_message_id) {
    outcome.decision = IngestDecision::BenignDuplicate;
    return outcome;
  }

  PeerSyncState sync_state = DefaultPeerSyncState();
  if (auto loaded = store_.GetPeerSyncState(resolved_thread_id, envelope.session_epoch)) {
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
      resolved_thread_id, envelope.session_epoch, envelope.sender_contact_id, envelope.sender_seq);

  auto& replay_window = ReplayWindowFor(resolved_thread_id, envelope.session_epoch);
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

  if (auto membership = ApplyInboundMembershipMessage(persisted, envelope.sender_contact_id, &outcome);
      !membership) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't apply membership update",
                       membership.error().message, resolved_thread_id);
    return outcome;
  }

  if (auto cap = RejectIfAnnotationCapExceeded(store_, persisted); !cap) {
    outcome.decision = IngestDecision::HardReject;
    MarkReceiveFailure(outcome, envelope.sender_contact_id, "couldn't accept reaction", cap.error().message,
                       resolved_thread_id);
    return outcome;
  }

  if (!store_.AppendMessage(persisted)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  (void)store_.SetPeerSyncState(resolved_thread_id, envelope.session_epoch, classified.sync_state);
  outcome.persisted = true;
  outcome.thread_changed = true;
  outcome.thread_id = resolved_thread_id;
  return outcome;
}

} // namespace pbr
