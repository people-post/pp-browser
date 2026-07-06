#include "base/messaging/ChatHistoryResponder.h"

#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/RelayStreamKey.h"
#include "base/messaging/RelayWirePayload.h"

#include <algorithm>

namespace pbr {

namespace {

Roe<RelayEnvelope> OutboundMessageToEnvelope(const ThreadMessage& message, const Thread& thread,
                                             const std::string& local_relay_user_id,
                                             const std::string& peer_relay_user_id, IdentityStore& identity) {
  if (!message.sender_seq || !message.session_epoch) {
    return Error("History row missing seq fields");
  }

  auto payload_b64 = RelayWirePayload::EncodePlaintextText(message.text);
  if (!payload_b64) {
    return payload_b64.error();
  }

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = message.id;
  envelope.sender_relay_id = local_relay_user_id;
  envelope.sender_contact_id = local_relay_user_id;
  envelope.route.kind = "direct";
  envelope.route.channel = thread.channel;
  envelope.body.e2e.payload_b64 = *payload_b64;
  envelope.sender_seq = *message.sender_seq;
  envelope.order_key = envelope.sender_seq;
  envelope.session_epoch = *message.session_epoch;
  envelope.stream_key =
      BuildCanonicalRelayStreamKey(local_relay_user_id, peer_relay_user_id, thread.channel, *message.session_epoch);
  envelope.recipient_contact_id = peer_relay_user_id;
  envelope.timestamp = message.timestamp;

  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  if (!sign_bytes) {
    return sign_bytes.error();
  }
  auto signature = identity.SignBytes(*sign_bytes);
  if (!signature) {
    return signature.error();
  }
  envelope.signature = *signature;
  return envelope;
}

} // namespace

Roe<ChatHistoryResponse> ChatHistoryResponder::Serve(IThreadStore& store, IdentityStore& identity,
                                                     const ChatHistoryRequest& request,
                                                     const std::string& local_relay_user_id) {
  if (local_relay_user_id.empty()) {
    return Error("Local relay identity missing");
  }
  if (request.peer_identity_value != local_relay_user_id) {
    return Error("History request targets a different peer stream");
  }
  if (request.requester_identity_value.empty()) {
    return Error("History request missing requester identity");
  }

  DirectChatTarget target;
  target.peer_identity_kind = request.peer_identity_kind;
  target.peer_identity_value = request.requester_identity_value;
  target.channel = request.channel;

  auto thread = store.FindDirectThread(target);
  if (!thread || !*thread) {
    return Error("Requester is not a chat participant");
  }
  if (!ThreadChannelIsE2e((*thread)->channel)) {
    return Error("History fetch requires E2E direct thread");
  }

  size_t limit = request.limit == 0 ? kDefaultTailSyncLimit : request.limit;
  limit = std::min(limit, kMaxPollBatchMessages);

  SeqRangeQuery query;
  query.session_epoch = request.session_epoch;
  query.seq_owner_contact_id = kLocalSelfContactId;
  query.min_sender_seq = request.min_sender_seq;
  query.max_sender_seq = request.max_sender_seq;
  query.limit = limit + 1;
  query.ascending = request.order != "desc";

  auto rows = store.GetMessagesBySeqRange((*thread)->id, query);
  if (!rows) {
    return rows.error();
  }

  ChatHistoryResponse response;
  response.peer_identity_kind = request.peer_identity_kind;
  response.peer_identity_value = request.peer_identity_value;
  response.channel = request.channel;
  response.session_epoch = request.session_epoch;

  if (rows->size() > limit) {
    response.has_more = true;
    rows->resize(limit);
  }

  for (const ThreadMessage& message : *rows) {
    auto envelope =
        OutboundMessageToEnvelope(message, **thread, local_relay_user_id, request.requester_identity_value, identity);
    if (!envelope) {
      return envelope.error();
    }
    response.messages.push_back(std::move(*envelope));
  }

  if (!response.messages.empty()) {
    if (query.ascending) {
      response.cursor.next_min_sender_seq = response.messages.back().sender_seq + 1;
    } else {
      if (response.messages.back().sender_seq > 0) {
        response.cursor.next_max_sender_seq = response.messages.back().sender_seq - 1;
      }
    }
  }

  return response;
}

} // namespace pbr
