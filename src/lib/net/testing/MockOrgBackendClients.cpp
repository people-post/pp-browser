#include "lib/net/testing/MockOrgBackendClients.h"

#include "common/Utilities.h"
#include "common/chat/RelayStreamKey.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"

#include <algorithm>
#include <mutex>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

std::optional<std::string> MockPeerKemPublicKeyB64() {
  static std::once_flag once;
  static std::string value;
  std::call_once(once, []() {
    auto keys = HybridKem::GenerateKeyPair();
    if (keys) {
      value = Base64Encode(keys->public_key);
    }
  });
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

} // namespace

Roe<BlobPresignResult> MockBlobClient::Presign(const std::string& /*relay_user_id*/,
                                                 const std::string& /*content_type*/, const uint64_t byte_length,
                                                 const BlobPurpose /*purpose*/) {
  ++presign_call_count_;
  if (presign_error_) {
    return presign_error_.value();
  }
  BlobPresignResult result;
  result.blob_id = "mock-blob-" + std::to_string(next_blob_id_++);
  result.upload_url = "https://mock.example/upload/" + result.blob_id;
  result.public_url = "https://mock.example/public/" + result.blob_id;
  result.tier = byte_length <= 4u * 1024u * 1024u ? BlobTier::Small : BlobTier::Large;
  result.pending_expires_at = "2099-01-01T00:00:00.000Z";
  return result;
}

Roe<void> MockBlobClient::Retain(const std::string& /*relay_user_id*/, const std::string& blob_id) {
  retained_blob_ids_.push_back(blob_id);
  return Roe<void>{};
}

Roe<void> MockBlobClient::Delete(const std::string& /*relay_user_id*/, const std::string& blob_id) {
  deleted_blob_ids_.push_back(blob_id);
  return Roe<void>{};
}

Roe<BlobListResult> MockBlobClient::List(const std::string& /*relay_user_id*/, const std::string& /*status_filter*/) {
  if (list_result_) {
    return *list_result_;
  }
  return BlobListResult{};
}

Roe<void> MockBlobClient::SetProfileIcon(const std::string& /*relay_user_id*/, const std::string& /*url*/,
                                         const std::string& /*blob_id*/, const std::string& /*kind*/) {
  return Roe<void>{};
}

Roe<void> MockBlobClient::PutUpload(const std::string& /*upload_url*/, const std::string& /*content_type*/,
                                    const std::string& body) {
  uploaded_bodies_.push_back(body);
  return Roe<void>{};
}

Roe<std::vector<mesh::DirectoryHit>> MockDirectoryClient::SearchPeople(const std::string& query) {
  mesh::DirectoryHit alice;
  alice.hit_id = "hit_alice";
  alice.display_name = "Alice Example";
  alice.nickname = "alice";
  alice.account_id = "account:alice_acct";
  alice.ids = {{mesh::ContactIdKind::Account, "account:alice_acct", true},
               {mesh::ContactIdKind::RelayUser, "relay:alice123", false}};
  alice.signing_public_key_b64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
  if (auto kem = MockPeerKemPublicKeyB64()) {
    alice.kem_public_key_b64 = *kem;
  } else if (!default_kem_public_key_b64_.empty()) {
    alice.kem_public_key_b64 = default_kem_public_key_b64_;
  }

  mesh::DirectoryHit bob;
  bob.hit_id = "hit_bob";
  bob.display_name = "Bob Builder";
  bob.nickname = "bob";
  bob.account_id = "account:bob_acct";
  bob.ids = {{mesh::ContactIdKind::Account, "account:bob_acct", true},
             {mesh::ContactIdKind::RelayUser, "relay:bob456", false}};
  bob.signing_public_key_b64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
  if (auto kem = MockPeerKemPublicKeyB64()) {
    bob.kem_public_key_b64 = *kem;
  } else if (!default_kem_public_key_b64_.empty()) {
    bob.kem_public_key_b64 = default_kem_public_key_b64_;
  }

  if (query.empty()) {
    return std::vector<mesh::DirectoryHit>{alice, bob};
  }

  std::vector<mesh::DirectoryHit> out;
  for (const mesh::DirectoryHit& hit : {alice, bob}) {
    if (hit.display_name.find(query) != std::string::npos || hit.nickname.find(query) != std::string::npos) {
      out.push_back(hit);
      continue;
    }
    if (hit.account_id && hit.account_id->find(query) != std::string::npos) {
      out.push_back(hit);
      continue;
    }
    for (const mesh::ContactId& id : hit.ids) {
      if (id.value.find(query) != std::string::npos) {
        out.push_back(hit);
        break;
      }
    }
  }
  return out;
}

Roe<mesh::DirectoryHit> MockDirectoryClient::LookupRelayUser(const std::string& relay_user_id) {
  auto hits = SearchPeople("");
  if (!hits) {
    return hits.error();
  }
  for (const mesh::DirectoryHit& hit : *hits) {
    for (const mesh::ContactId& id : hit.ids) {
      if (id.kind == mesh::ContactIdKind::RelayUser && id.value == relay_user_id) {
        return hit;
      }
    }
  }
  return Error("Relay user not found");
}

Roe<mesh::DirectoryHit> MockDirectoryClient::LookupByAccount(const std::string& account_id) {
  auto hits = SearchPeople("");
  if (!hits) {
    return hits.error();
  }
  for (const mesh::DirectoryHit& hit : *hits) {
    if (hit.account_id && *hit.account_id == account_id) {
      return hit;
    }
    for (const mesh::ContactId& id : hit.ids) {
      if (id.kind == mesh::ContactIdKind::Account && id.value == account_id) {
        return hit;
      }
    }
  }
  return Error("Account not found");
}

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

Roe<RegistrationStartResult> MockRegistrationClient::StartRegistration(const std::string& /*public_key_b64*/,
                                                                       const std::string& /*nickname*/,
                                                                       const std::string& signature_alg,
                                                                       const std::string& /*kem_public_key_b64*/,
                                                                       const std::string& /*peer_id*/,
                                                                       const std::vector<std::string>& /*multiaddrs*/,
                                                                       const RegistrationPublishOpts& /*publish*/) {
  return RegistrationStartResult{.challenge = "mock-challenge", .signature_alg = signature_alg,
                                 .expires_at = "2099-01-01T00:00:00.000Z"};
}

Roe<RegistrationResult> MockRegistrationClient::FinishRegistration(const std::string& /*challenge*/,
                                                                   const std::string& public_key_b64,
                                                                   const std::string& nickname,
                                                                   const std::string& /*signature*/,
                                                                   int64_t /*timestamp*/,
                                                                   const std::string& /*signature_alg*/,
                                                                   const std::string& /*kem_public_key_b64*/,
                                                                   const std::string& /*kem_binding_signature_b64*/,
                                                                   const std::string& /*peer_id*/,
                                                                   const std::vector<std::string>& /*multiaddrs*/,
                                                                   int64_t initiation_floor,
                                                                   const RegistrationPublishOpts& /*publish*/) {
  return RegistrationResult{.success = true,
                            .relay_user_id = "relay:" + public_key_b64.substr(0, 12),
                            .message = "Registered as " + nickname,
                            .llm_api_key = "brf_llm_mock_key",
                            .expires_at = "2099-01-01T00:00:00.000Z",
                            .initiation_floor = initiation_floor,
                            .initiation_floor_present = true,
                            .kem_bound = true};
}

Roe<RegistrationResult> MockRegistrationClient::UpdateNickname(const std::string& new_nickname,
                                                                 const std::string& /*signature*/,
                                                                 int64_t /*timestamp*/,
                                                                 const std::string& /*relay_user_id*/) {
  return RegistrationResult{.success = true, .message = "Nickname updated to " + new_nickname};
}

Roe<void> MockRegistrationClient::RevokeRegistration(const std::string& /*relay_user_id*/,
                                                     const std::string& /*signature*/, const int64_t /*timestamp*/) {
  return {};
}

Roe<ClientCompatDocument> MockClientCompatClient::Fetch() {
  if (!error_.empty()) {
    return Error(error_);
  }
  if (!has_document_) {
    return Error("Mock client-compat document not set");
  }
  return document_;
}
} // namespace pbr
