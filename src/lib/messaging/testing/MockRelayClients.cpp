#include "lib/messaging/testing/MockRelayClients.h"

#include "common/Utilities.h"
#include "common/chat/RelayStreamKey.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <algorithm>
#include <mutex>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

Roe<void> MockRelayClient::Send(const RelayEnvelope& envelope) {
  std::lock_guard lock(mutex_);
  pending_.push_back(envelope);

  // Echo inbound payload so mock replies stay valid ChatPayload without messaging codecs.
  RelayEnvelope reply;
  reply.envelope_version = kRelayEnvelopeVersion;
  reply.message_id = util::GenerateUuid();
  reply.sender_relay_id = next_reply_sender_id_.empty() ? "relay:mock-peer" : next_reply_sender_id_;
  reply.sender_contact_id = reply.sender_relay_id;
  reply.route.kind = "direct";
  reply.route.channel = envelope.route.channel;
  reply.body.e2e.payload_b64 = envelope.body.e2e.payload_b64;
  reply.sender_seq = envelope.sender_seq + 1;
  reply.order_key = reply.sender_seq;
  reply.session_epoch = envelope.session_epoch;
  reply.stream_key = envelope.stream_key;
  reply.timestamp = util::NowUnixMs();

  if (!reply_signing_private_key_.empty()) {
    if (!build_sign_bytes_) {
      return Error("Mock reply signing requires SetBuildSignBytesFn");
    }
    auto sign_bytes = build_sign_bytes_(reply);
    if (sign_bytes) {
      auto signature = MlDsa::Sign(reply_signing_private_key_, *sign_bytes);
      if (signature) {
        reply.signature = Base64Encode(*signature);
      }
    }
  }

  delivered_.push_back(std::move(reply));
  next_reply_sender_id_.clear();
  return {};
}

Roe<RelayPollResult> MockRelayClient::PollInbox(const std::string& /*requester_contact_id*/,
                                              const std::string& /*cursor*/) {
  std::lock_guard lock(mutex_);
  RelayPollResult result;
  if (poll_index_ < delivered_.size()) {
    result.messages.push_back(delivered_[poll_index_++]);
    result.next_cursor = std::to_string(poll_index_);
  }
  return result;
}

Roe<RelayDeleteResult> MockRelayClient::AckInbox(const std::string& /*requester_contact_id*/,
                                                 const std::string& cursor) {
  std::lock_guard lock(mutex_);
  RelayDeleteResult result;
  if (cursor.empty()) {
    return result;
  }
  // Soft-ack (M013): validate cursor shape only; do not erase shared mock mailbox.
  try {
    (void)std::stoull(cursor);
  } catch (...) {
    return Error("Invalid mock inbox cursor");
  }
  result.deleted = 0;
  return result;
}

Roe<RelayDeleteResult> MockRelayClient::ClearInbox(const std::string& /*requester_contact_id*/,
                                                   const std::string& /*before_created_at*/) {
  std::lock_guard lock(mutex_);
  RelayDeleteResult result;
  result.deleted = static_cast<int64_t>(delivered_.size());
  delivered_.clear();
  poll_index_ = 0;
  return result;
}

namespace {

bool EnvelopeMatchesHistoryRequest(const RelayEnvelope& envelope, const ChatHistoryRequest& request) {
  const std::string expected_stream =
      BuildCanonicalRelayStreamKey(request.requester_identity_value, request.peer_identity_value, request.channel,
                                   request.session_epoch);
  if (!envelope.stream_key.empty()) {
    if (envelope.stream_key != expected_stream) {
      return false;
    }
  } else if (envelope.sender_contact_id != request.peer_identity_value &&
             envelope.sender_relay_id != request.peer_identity_value) {
    // Communicating identity may be Account ID while history peer is the Brief relay route (M010).
    return false;
  }
  const uint64_t order = envelope.order_key != 0 ? envelope.order_key : envelope.sender_seq;
  if (request.min_sender_seq && order < *request.min_sender_seq) {
    return false;
  }
  if (request.max_sender_seq && order > *request.max_sender_seq) {
    return false;
  }
  return true;
}

} // namespace

Roe<ChatHistoryResponse> MockRelayClient::FetchChatHistory(const ChatHistoryRequest& request) {
  std::lock_guard lock(mutex_);
  if (!fetch_history_error_.empty()) {
    return Error(fetch_history_error_);
  }
  ChatHistoryResponse response;
  response.peer_identity_kind = request.peer_identity_kind;
  response.peer_identity_value = request.peer_identity_value;
  response.channel = request.channel;
  response.session_epoch = request.session_epoch;
  for (const RelayEnvelope& envelope : delivered_) {
    if (EnvelopeMatchesHistoryRequest(envelope, request)) {
      response.messages.push_back(envelope);
    }
  }
  if (request.order == "desc") {
    std::sort(response.messages.begin(), response.messages.end(),
              [](const RelayEnvelope& a, const RelayEnvelope& b) { return a.sender_seq > b.sender_seq; });
  } else {
    std::sort(response.messages.begin(), response.messages.end(),
              [](const RelayEnvelope& a, const RelayEnvelope& b) { return a.sender_seq < b.sender_seq; });
  }
  if (response.messages.size() > request.limit) {
    response.messages.resize(request.limit);
    response.has_more = true;
  }
  return response;
}

bool MockChatHistoryPeerClient::IsPeerReachable(const std::string& peer_identity_value) const {
  std::lock_guard lock(mutex_);
  const auto it = reachable_peers_.find(peer_identity_value);
  return it != reachable_peers_.end() && it->second;
}

Roe<ChatHistoryResponse> MockChatHistoryPeerClient::FetchChatHistory(const ChatHistoryRequest& request) {
  if (!IsPeerReachable(request.peer_identity_value)) {
    return Error("Peer-direct history unavailable");
  }
  std::lock_guard lock(mutex_);
  ChatHistoryResponse response;
  response.peer_identity_kind = request.peer_identity_kind;
  response.peer_identity_value = request.peer_identity_value;
  response.channel = request.channel;
  response.session_epoch = request.session_epoch;
  for (const RelayEnvelope& envelope : delivered_) {
    if (EnvelopeMatchesHistoryRequest(envelope, request)) {
      response.messages.push_back(envelope);
    }
  }
  if (request.order == "desc") {
    std::sort(response.messages.begin(), response.messages.end(),
              [](const RelayEnvelope& a, const RelayEnvelope& b) { return a.sender_seq > b.sender_seq; });
  } else {
    std::sort(response.messages.begin(), response.messages.end(),
              [](const RelayEnvelope& a, const RelayEnvelope& b) { return a.sender_seq < b.sender_seq; });
  }
  if (response.messages.size() > request.limit) {
    response.messages.resize(request.limit);
    response.has_more = true;
  }
  return response;
}

} // namespace pbr
