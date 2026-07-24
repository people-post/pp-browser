#include "feature/messaging/RelayReceivePipeline.h"

#include "base/messaging/AutoKeyEnvelopeResolver.h"
#include "base/crypto/CryptoUtil.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/GroupMembershipApply.h"
#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/GroupE2ePayloadCodec.h"
#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/messaging/SyncStateCodec.h"
#include "base/people/ContactTypes.h"
#include "base/people/PeerDisplayLabel.h"
#include "feature/messaging/GroupInviteGate.h"

#include "common/Utilities.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

bool IsEnvelopeFromPeer(const Thread& thread, const RelayEnvelope& envelope) {
  if (!thread.peer_identity_value.empty()) {
    return envelope.sender_contact_id == thread.peer_identity_value ||
           envelope.sender_relay_id == thread.peer_identity_value;
  }
  return false;
}

DirectChatTarget InboundTargetFromEnvelope(const RelayEnvelope& envelope) {
  DirectChatTarget target;
  target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  target.peer_identity_value = envelope.sender_contact_id;
  target.channel = envelope.route.channel;
  return target;
}

} // namespace

RelayReceivePipeline::RelayReceivePipeline(IThreadStore& store, IPeerSigningKeyResolver& signing_keys,
                                           IPskSessionStore& psk_store, IdentityStore& identity,
                                           GroupRosterStore& group_roster, GroupInviteGate* invite_gate)
    : store_(store), signing_keys_(signing_keys), psk_store_(psk_store), identity_(identity),
      group_roster_(group_roster), invite_gate_(invite_gate) {}

Roe<void> RelayReceivePipeline::ApplyInboundMembershipMessage(ThreadMessage& message,
                                                              const std::string& actor_identity) const {
  // Resolve group_id from any membership control we understand, then ignore events for groups
  // the local user has already left/dismissed (prevents transfer-to-leaver resurrection).
  auto local_id = identity_.Get();
  const std::string local_relay =
      (local_id && !local_id->relay_user_id.empty()) ? local_id->relay_user_id : std::string();

  auto ignore_if_not_member = [this, &local_relay](const std::string& group_id) -> Roe<bool> {
    if (local_relay.empty() || group_id.empty()) {
      return false;
    }
    auto is_member = group_roster_.IsMember(group_id, local_relay);
    if (!is_member) {
      return is_member.error();
    }
    return !*is_member; // true => ignore
  };

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
    if (!local_relay.empty() && removed->member_identity == local_relay) {
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

  auto response = GroupMembershipCodec::DecodeInviteResponseFromMessage(message);
  if (response) {
    if (response->control_type == GroupMembershipControlType::GroupInviteAccept) {
      if (auto applied = ApplyInviteAcceptToRoster(group_roster_, response->invite_nonce, actor_identity);
          !applied) {
        return applied.error();
      }
      auto metadata = group_roster_.LoadMetadata(response->group_id);
      auto thread = store_.FindGroupThread(response->group_id);
      if (thread && *thread && metadata && *metadata) {
        auto detail = GroupMembershipCodec::EncodeMemberJoined(response->group_id, actor_identity, MemberRole::Member,
                                                               (*metadata)->roster_epoch);
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
    if (auto declined = ApplyInviteDeclineToRoster(group_roster_, response->invite_nonce, actor_identity);
        !declined) {
      return declined.error();
    }
    return {};
  }
  if (response.error().message.find("not a group invite response") == std::string::npos) {
    return response.error();
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

  const nlohmann::json wire_json = RelayEnvelopeToJson(envelope);
  const std::string serialized = wire_json.dump();
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
    return outcome;
  }

  const bool auto_create = !*thread && envelope.route.channel == ThreadChannel::E2ePublic;
  if (!*thread && !auto_create) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  if (!auto_create && !IsEnvelopeFromPeer(**thread, envelope)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  std::string resolved_thread_id;
  if (!auto_create) {
    resolved_thread_id = (*thread)->id;
    auto has_message_id = store_.HasMessageId(resolved_thread_id, envelope.message_id);
    if (!has_message_id) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    if (*has_message_id) {
      outcome.decision = IngestDecision::BenignDuplicate;
      return outcome;
    }
  }

  auto verified = VerifySignature(envelope, inbound_target);
  if (!verified || !*verified) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  ThreadMessage message;
  if (E2eRelayPayloadCodec::RequiresEncryption(envelope.route.channel)) {
    if (local_relay_user_id.empty()) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }

    ChatTargetKey target_key;
    target_key.peer_identity_kind = inbound_target.peer_identity_kind;
    target_key.peer_identity_value = inbound_target.peer_identity_value;
    target_key.channel = E2eRelayPayloadCodec::ChannelFromThread(envelope.route.channel);

    std::optional<ByteVector> local_kem_private_key;
    if (envelope.route.channel == ThreadChannel::E2ePublic) {
      auto kem_private = identity_.GetOrCreateHybridKemPrivateKey();
      if (!kem_private) {
        outcome.decision = IngestDecision::HardReject;
        return outcome;
      }
      local_kem_private_key = std::move(*kem_private);
    }

    auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_relay_user_id, target_key, psk_store_,
                                                           local_kem_private_key);
    if (!decrypted) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    message = std::move(*decrypted);

    if (envelope.route.channel == ThreadChannel::E2ePublic && local_kem_private_key.has_value()) {
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
      return outcome;
    }
    if (decoded->content_type != ChatContentType::Text && decoded->content_type != ChatContentType::System &&
        decoded->content_type != ChatContentType::Annotation &&
        decoded->content_type != ChatContentType::ContactCard &&
        decoded->content_type != ChatContentType::CryptoTx) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    ChatPayloadValidator::SanitizeInboundFields(*decoded);
    message = std::move(*decoded);
  }

  if (message.content_type != ChatContentType::Text && message.content_type != ChatContentType::System &&
      message.content_type != ChatContentType::Annotation && message.content_type != ChatContentType::ContactCard &&
      message.content_type != ChatContentType::CryptoTx) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  if (auto_create) {
    const std::string fallback_title = ShortRelayId(envelope.sender_contact_id);
    auto created = store_.FindOrCreateDirectThread(
        inbound_target, "", fallback_title.empty() ? envelope.sender_contact_id : fallback_title);
    if (!created) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    resolved_thread_id = created->id;
    outcome.thread_changed = true;
  }

  const std::string seq_owner =
      auto_create ? envelope.sender_contact_id
                  : ((*thread)->participant_contact_ids.empty() ? envelope.sender_contact_id
                                                                : (*thread)->participant_contact_ids.front());

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

  if (classified.decision == IngestDecision::SilentDiscard || classified.decision == IngestDecision::BenignDuplicate) {
    return outcome;
  }
  if (classified.decision == IngestDecision::SoftCompromised || classified.decision == IngestDecision::HardReject) {
    if (classified.decision == IngestDecision::SoftCompromised) {
      PeerSyncState compromised_state = classified.sync_state;
      compromised_state.phase = PeerSyncPhase::Compromised;
      (void)store_.SetPeerSyncState(resolved_thread_id, envelope.session_epoch, compromised_state);
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

  if (auto membership = ApplyInboundMembershipMessage(persisted, envelope.sender_contact_id); !membership) {
    outcome.decision = IngestDecision::HardReject;
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
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  DirectChatTarget inbound_target;
  inbound_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  inbound_target.peer_identity_value = envelope.sender_contact_id;
  inbound_target.channel = ThreadChannel::E2ePublic;

  auto verified = VerifySignature(envelope, inbound_target);
  if (!verified || !*verified) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  auto kem_private = identity_.GetOrCreateHybridKemPrivateKey();
  if (!kem_private) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  auto decrypted = GroupE2ePayloadCodec::DecryptForLocalMember(envelope, local_relay_user_id, psk_store_,
                                                                 *kem_private);
  if (!decrypted) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  ThreadMessage message = std::move(*decrypted);

  auto thread = store_.FindGroupThread(group_id);
  if (!thread) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  if (!*thread) {
    // Never resurrect a closed group session from inbound traffic. AcceptInvite / CreateGroup
    // create the local thread explicitly; leave/dismiss clears it on purpose.
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  const std::string resolved_thread_id = (**thread).id;

  auto has_message_id = store_.HasMessageId(resolved_thread_id, envelope.message_id);
  if (!has_message_id) {
    outcome.decision = IngestDecision::HardReject;
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

  if (auto membership = ApplyInboundMembershipMessage(persisted, envelope.sender_contact_id); !membership) {
    outcome.decision = IngestDecision::HardReject;
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
