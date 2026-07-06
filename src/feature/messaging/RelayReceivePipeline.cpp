#include "feature/messaging/RelayReceivePipeline.h"

#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/people/ContactTypes.h"

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
                                           IPskSessionStore& psk_store)
    : store_(store), signing_keys_(signing_keys), psk_store_(psk_store) {}

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
  if (!thread || !*thread) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  if (!IsEnvelopeFromPeer(**thread, envelope)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  const std::string& resolved_thread_id = (*thread)->id;
  auto has_message_id = store_.HasMessageId(resolved_thread_id, envelope.message_id);
  if (!has_message_id) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }
  if (*has_message_id) {
    outcome.decision = IngestDecision::BenignDuplicate;
    return outcome;
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
    const ChatTargetKey target_key = E2eRelayPayloadCodec::ChatTargetFromThread(**thread);
    auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, local_relay_user_id, target_key, psk_store_);
    if (!decrypted) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    message = std::move(*decrypted);
  } else {
    auto decoded = RelayWirePayload::DecodeInboundPayload(envelope.body.e2e.payload_b64);
    if (!decoded) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    if (decoded->content_type != ChatContentType::Text) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    ChatPayloadValidator::SanitizeInboundFields(*decoded);
    message = std::move(*decoded);
  }

  if (message.content_type != ChatContentType::Text) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  const std::string seq_owner =
      (*thread)->participant_contact_ids.empty() ? envelope.sender_contact_id : (*thread)->participant_contact_ids.front();

  auto sync_state = store_.GetPeerSyncState(resolved_thread_id, envelope.session_epoch);
  if (!sync_state) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  auto chat_target_epoch = store_.GetChatTargetSessionEpoch(resolved_thread_id);
  if (!chat_target_epoch) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  IngestClassifierInput classifier_input;
  classifier_input.sender_seq = envelope.sender_seq;
  classifier_input.session_epoch = envelope.session_epoch;
  classifier_input.message_id = envelope.message_id;
  classifier_input.sync_state = *sync_state;
  classifier_input.chat_target_epoch = *chat_target_epoch;
  classifier_input.strict_mode = (*thread)->channel == ThreadChannel::E2e;
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

  if (classified.decision == IngestDecision::AcceptEpochAdvance) {
    const uint32_t old_epoch = *chat_target_epoch;
    if (!store_.AppendMessageWithPassiveEpochAdopt(persisted, old_epoch, envelope.session_epoch,
                                                   classified.sync_state)) {
      outcome.decision = IngestDecision::HardReject;
      return outcome;
    }
    replay_windows_.erase(ReplayKey{resolved_thread_id, old_epoch});
    outcome.persisted = true;
    outcome.thread_changed = true;
    return outcome;
  }

  if (!store_.AppendMessage(persisted)) {
    outcome.decision = IngestDecision::HardReject;
    return outcome;
  }

  (void)store_.SetPeerSyncState(resolved_thread_id, envelope.session_epoch, classified.sync_state);
  outcome.persisted = true;
  outcome.thread_changed = true;
  return outcome;
}

} // namespace pbr
