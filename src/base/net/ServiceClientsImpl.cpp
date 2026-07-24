#include "base/net/ServiceClientsImpl.h"

#include "common/Utilities.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/HybridKem.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayStreamKey.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/net/HttpClient.h"
#include "base/net/RelayApiSignPayload.h"
#include "base/people/ContactJson.h"
#include "base/people/Ed25519Signer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>

namespace pbr {

namespace {

std::optional<std::string> MockPeerKemPublicKeyB64() {
  static std::once_flag once;
  static std::string value;
  std::call_once(once, []() {
    auto keys = HybridKem::GenerateKeyPair();
    if (!keys) {
      return;
    }
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

Roe<std::vector<DirectoryHit>> MockDirectoryClient::SearchPeople(const std::string& query) {
  DirectoryHit alice;
  alice.hit_id = "hit_alice";
  alice.display_name = "Alice Example";
  alice.nickname = "alice";
  alice.ids = {{ContactIdKind::RelayUser, "relay:alice123", true}};
  alice.signing_public_key_b64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
  if (auto kem = MockPeerKemPublicKeyB64()) {
    alice.kem_public_key_b64 = *kem;
  } else if (!default_kem_public_key_b64_.empty()) {
    alice.kem_public_key_b64 = default_kem_public_key_b64_;
  }

  DirectoryHit bob;
  bob.hit_id = "hit_bob";
  bob.display_name = "Bob Builder";
  bob.nickname = "bob";
  bob.ids = {{ContactIdKind::RelayUser, "relay:bob456", true}};
  bob.signing_public_key_b64 = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
  if (auto kem = MockPeerKemPublicKeyB64()) {
    bob.kem_public_key_b64 = *kem;
  } else if (!default_kem_public_key_b64_.empty()) {
    bob.kem_public_key_b64 = default_kem_public_key_b64_;
  }

  if (query.empty()) {
    return std::vector<DirectoryHit>{alice, bob};
  }

  std::vector<DirectoryHit> out;
  for (const DirectoryHit& hit : {alice, bob}) {
    if (hit.display_name.find(query) != std::string::npos || hit.nickname.find(query) != std::string::npos) {
      out.push_back(hit);
    }
  }
  return out;
}

Roe<DirectoryHit> MockDirectoryClient::LookupRelayUser(const std::string& relay_user_id) {
  auto hits = SearchPeople("");
  if (!hits) {
    return hits.error();
  }
  for (const DirectoryHit& hit : *hits) {
    for (const ContactId& id : hit.ids) {
      if (id.kind == ContactIdKind::RelayUser && id.value == relay_user_id) {
        return hit;
      }
    }
  }
  return Error("Relay user not found");
}

Roe<void> MockRelayClient::Send(const RelayEnvelope& envelope) {
  std::lock_guard lock(mutex_);
  pending_.push_back(envelope);

  auto reply_text = RelayWirePayload::EncodePlaintextText("Mock reply");
  if (!reply_text) {
    return reply_text.error();
  }

  RelayEnvelope reply;
  reply.envelope_version = kRelayEnvelopeVersion;
  reply.message_id = util::GenerateUuid();
  reply.sender_relay_id = next_reply_sender_id_.empty() ? "relay:mock-peer" : next_reply_sender_id_;
  reply.sender_contact_id = reply.sender_relay_id;
  reply.route.kind = "direct";
  reply.route.channel = envelope.route.channel;
  reply.body.e2e.payload_b64 = *reply_text;
  reply.sender_seq = envelope.sender_seq + 1;
  reply.order_key = reply.sender_seq;
  reply.session_epoch = envelope.session_epoch;
  reply.stream_key = envelope.stream_key;
  reply.timestamp = util::NowUnixMs();

  if (!reply_signing_private_key_.empty()) {
    auto sign_bytes = EnvelopeSigner::BuildSignBytes(reply);
    if (sign_bytes) {
      auto signature = Ed25519Signer::Sign(std::string(sign_bytes->begin(), sign_bytes->end()),
                                           reply_signing_private_key_);
      if (signature) {
        reply.signature = *signature;
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

namespace {

bool EnvelopeMatchesHistoryRequest(const RelayEnvelope& envelope, const ChatHistoryRequest& request) {
  if (envelope.sender_contact_id != request.peer_identity_value) {
    return false;
  }
  const std::string expected_stream =
      BuildCanonicalRelayStreamKey(request.requester_identity_value, request.peer_identity_value, request.channel,
                                 request.session_epoch);
  if (!envelope.stream_key.empty() && envelope.stream_key != expected_stream) {
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
                                                                       const std::string& /*kem_public_key_b64*/) {
  return RegistrationStartResult{.challenge = "mock-challenge", .signature_alg = signature_alg,
                                 .expires_at = "2099-01-01T00:00:00.000Z"};
}

Roe<RegistrationResult> MockRegistrationClient::FinishRegistration(const std::string& /*challenge*/,
                                                                   const std::string& public_key_b64,
                                                                   const std::string& nickname,
                                                                   const std::string& /*signature*/,
                                                                   int64_t /*timestamp*/,
                                                                   const std::string& /*signature_alg*/,
                                                                   const std::string& /*kem_public_key_b64*/) {
  return RegistrationResult{.success = true,
                            .relay_user_id = "relay:" + public_key_b64.substr(0, 12),
                            .message = "Registered as " + nickname,
                            .llm_api_key = "brf_llm_mock_key",
                            .expires_at = "2099-01-01T00:00:00.000Z"};
}

Roe<RegistrationResult> MockRegistrationClient::UpdateNickname(const std::string& new_nickname,
                                                                 const std::string& /*signature*/,
                                                                 int64_t /*timestamp*/,
                                                                 const std::string& /*relay_user_id*/) {
  return RegistrationResult{.success = true, .message = "Nickname updated to " + new_nickname};
}

Roe<std::string> HttpRelayClient::SignRelayApiBytes(const std::vector<uint8_t>& sign_bytes) const {
  if (!auth_signer_) {
    return Error("Relay auth signer not configured");
  }
  if (sign_bytes.empty()) {
    return Error("Empty relay API sign bytes");
  }
  return auth_signer_(sign_bytes);
}

HttpRelayClient::HttpRelayClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<void> HttpRelayClient::Send(const RelayEnvelope& envelope) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  auto record = RelayWireSendRecordFromEnvelope(envelope);
  if (!record) {
    return record.error();
  }
  record->timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiSendSignBytes(*record, record->timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build relay send sign bytes");
  }
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }
  record->signature = *signature;

  const std::string url = base_url_ + "/v1/messages";
  const auto response = HttpClient::Post(url, RelayWireSendRecordToJson(*record).dump(),
                                         {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code == 429) {
    return Error("Relay rate limit exceeded");
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Relay send failed with status " + std::to_string(response.value().status_code));
  }
  return {};
}

Roe<RelayPollResult> HttpRelayClient::PollInbox(const std::string& requester_contact_id,
                                              const std::string& cursor) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiPollInboxSignBytes(requester_contact_id, cursor, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build relay poll sign bytes");
  }
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  const nlohmann::json body = {{"requester_contact_id", requester_contact_id},
                               {"cursor", cursor},
                               {"timestamp", timestamp},
                               {"signature", *signature}};
  const std::string url = base_url_ + "/v1/inbox/poll";
  const auto response = HttpClient::Post(url, body.dump(), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Relay poll failed with status " + std::to_string(response.value().status_code));
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  if (root.is_discarded()) {
    return Error("Invalid relay inbox JSON");
  }

  RelayPollResult result;
  if (root.contains("next_cursor") && root["next_cursor"].is_string()) {
    result.next_cursor = root["next_cursor"].get<std::string>();
  }
  if (root.contains("messages") && root["messages"].is_array()) {
    for (const auto& item : root["messages"]) {
      if (item.contains("blob_b64")) {
        auto inbound = ParseRelayInboundRecord(item);
        if (!inbound) {
          continue;
        }
        auto envelope = RelayEnvelopeFromInboundRecord(*inbound);
        if (envelope) {
          result.messages.push_back(*envelope);
        }
      } else {
        auto envelope = ParseRelayEnvelope(item);
        if (envelope) {
          result.messages.push_back(*envelope);
        }
      }
    }
  }
  return result;
}

Roe<ChatHistoryResponse> HttpRelayClient::FetchChatHistory(const ChatHistoryRequest& request) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  const std::string stream_id =
      BuildCanonicalRelayStreamKey(request.requester_identity_value, request.peer_identity_value, request.channel,
                                 request.session_epoch);
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiStreamHistorySignBytes(request, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build relay history sign bytes");
  }
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  nlohmann::json body = {{"requester_contact_id", request.requester_identity_value},
                         {"sender_contact_id", request.peer_identity_value},
                         {"stream_id", stream_id},
                         {"limit", request.limit},
                         {"order", request.order},
                         {"timestamp", timestamp},
                         {"signature", *signature}};
  if (request.min_sender_seq) {
    body["min_index_key"] = *request.min_sender_seq;
  }
  if (request.max_sender_seq) {
    body["max_index_key"] = *request.max_sender_seq;
  }

  const std::string url = base_url_ + "/v1/streams/messages/query";
  const auto response = HttpClient::Post(url, body.dump(), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Relay history fetch failed with status " + std::to_string(response.value().status_code));
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  if (root.is_discarded()) {
    return Error("Invalid relay history JSON");
  }
  auto parsed = ChatHistoryResponseFromJson(root);
  if (!parsed) {
    return parsed.error();
  }
  if (parsed->peer_identity_kind.empty()) {
    parsed->peer_identity_kind = request.peer_identity_kind;
    parsed->peer_identity_value = request.peer_identity_value;
    parsed->channel = request.channel;
    parsed->session_epoch = request.session_epoch;
  }
  return *parsed;
}

HttpPushDeviceClient::HttpPushDeviceClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<std::string> HttpPushDeviceClient::SignRelayApiBytes(const std::vector<uint8_t>& sign_bytes) const {
  if (!auth_signer_) {
    return Error("Relay auth signer not configured");
  }
  if (sign_bytes.empty()) {
    return Error("Empty relay API sign bytes");
  }
  return auth_signer_(sign_bytes);
}

Roe<void> HttpPushDeviceClient::PostDevice(const char* path, const RelayApiOp op,
                                           const PushDeviceRegistration& registration) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRelayApiDeviceSignBytes(op, registration.relay_user_id, registration.platform,
                                                       registration.device_id, registration.push_token, timestamp);
  auto signature = SignRelayApiBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }
  const nlohmann::json body = {{"relay_user_id", registration.relay_user_id},
                               {"platform", registration.platform},
                               {"device_id", registration.device_id},
                               {"push_token", registration.push_token},
                               {"timestamp", timestamp},
                               {"signature", *signature}};
  const std::string url = base_url_ + path;
  const auto response = HttpClient::Post(url, body.dump(), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error(std::string("Device API failed with status ") + std::to_string(response.value().status_code));
  }
  return {};
}

Roe<void> HttpPushDeviceClient::RegisterDevice(const PushDeviceRegistration& registration) {
  return PostDevice("/v1/devices/register", RelayApiOp::DeviceRegister, registration);
}

Roe<void> HttpPushDeviceClient::UnregisterDevice(const PushDeviceRegistration& registration) {
  return PostDevice("/v1/devices/unregister", RelayApiOp::DeviceUnregister, registration);
}

HttpDirectoryClient::HttpDirectoryClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<std::vector<DirectoryHit>> HttpDirectoryClient::SearchPeople(const std::string& query) {
  if (base_url_.empty()) {
    return Error("Directory base_url not configured");
  }
  const std::string url = base_url_ + "/v1/search?q=" + query;
  const auto response = HttpClient::Get(url);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Directory search failed with status " + std::to_string(response.value().status_code));
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  if (root.is_discarded() || !root.contains("hits") || !root["hits"].is_array()) {
    return Error("Invalid directory search JSON");
  }

  std::vector<DirectoryHit> hits;
  for (const auto& item : root["hits"]) {
    hits.push_back(DirectoryHitFromJson(item));
  }
  return hits;
}

Roe<DirectoryHit> HttpDirectoryClient::LookupRelayUser(const std::string& relay_user_id) {
  if (base_url_.empty()) {
    return Error("Directory base_url not configured");
  }
  const std::string url = base_url_ + "/v1/users/" + relay_user_id;
  const auto response = HttpClient::Get(url);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code == 404) {
    return Error("Relay user not found");
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Directory lookup failed with status " + std::to_string(response.value().status_code));
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  if (root.is_discarded() || !root.contains("relay_user_id")) {
    return Error("Invalid relay user lookup JSON");
  }
  return DirectoryHitFromJson(root);
}

HttpRegistrationClient::HttpRegistrationClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<RegistrationStartResult> HttpRegistrationClient::StartRegistration(const std::string& public_key_b64,
                                                                     const std::string& nickname,
                                                                     const std::string& signature_alg,
                                                                     const std::string& kem_public_key_b64) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  nlohmann::json body = {{"public_key", public_key_b64}, {"nickname", nickname},
                         {"signature_alg", signature_alg}, {"kem_public_key_b64", kem_public_key_b64}};
  if (kem_public_key_b64.empty()) {
    return Error("kem_public_key_b64 is required");
  }
  const auto response = HttpClient::Post(base_url_ + "/v1/register/start", body.dump(),
                                         {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Registration start failed with status " + std::to_string(response.value().status_code));
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  if (root.is_discarded() || !root.contains("challenge") || !root["challenge"].is_string()) {
    return Error("Invalid registration start JSON");
  }

  RegistrationStartResult result;
  result.challenge = root["challenge"].get<std::string>();
  if (root.contains("signature_alg") && root["signature_alg"].is_string()) {
    result.signature_alg = root["signature_alg"].get<std::string>();
  } else {
    result.signature_alg = signature_alg;
  }
  if (root.contains("expires_at") && root["expires_at"].is_string()) {
    result.expires_at = root["expires_at"].get<std::string>();
  }
  return result;
}

Roe<RegistrationResult> HttpRegistrationClient::FinishRegistration(const std::string& challenge,
                                                                   const std::string& public_key_b64,
                                                                   const std::string& nickname,
                                                                   const std::string& signature,
                                                                   int64_t timestamp,
                                                                   const std::string& signature_alg,
                                                                   const std::string& kem_public_key_b64) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  nlohmann::json body = {{"challenge", challenge},
                         {"public_key", public_key_b64},
                         {"nickname", nickname},
                         {"signature", signature},
                         {"timestamp", timestamp},
                         {"signature_alg", signature_alg},
                         {"kem_public_key_b64", kem_public_key_b64}};
  if (kem_public_key_b64.empty()) {
    return Error("kem_public_key_b64 is required");
  }
  const auto response = HttpClient::Post(base_url_ + "/v1/register/finish", body.dump(),
                                         {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Registration finish failed with status " + std::to_string(response.value().status_code));
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  RegistrationResult result{.success = true};
  if (!root.is_discarded()) {
    if (root.contains("success") && root["success"].is_boolean()) {
      result.success = root["success"].get<bool>();
    }
    if (root.contains("relay_user_id") && root["relay_user_id"].is_string()) {
      result.relay_user_id = root["relay_user_id"].get<std::string>();
    }
    if (root.contains("message") && root["message"].is_string()) {
      result.message = root["message"].get<std::string>();
    }
    if (root.contains("llm_api_key") && root["llm_api_key"].is_string()) {
      result.llm_api_key = root["llm_api_key"].get<std::string>();
    }
    if (root.contains("expires_at") && root["expires_at"].is_string()) {
      result.expires_at = root["expires_at"].get<std::string>();
    }
  }
  return result;
}

Roe<RegistrationResult> HttpRegistrationClient::UpdateNickname(const std::string& new_nickname,
                                                             const std::string& signature, int64_t timestamp,
                                                             const std::string& relay_user_id) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  const nlohmann::json body = {{"relay_user_id", relay_user_id},
                               {"nickname", new_nickname},
                               {"timestamp", timestamp},
                               {"signature", signature}};
  const auto response =
      HttpClient::Post(base_url_ + "/v1/profile/nickname", body.dump(), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Nickname update failed with status " + std::to_string(response.value().status_code));
  }
  return RegistrationResult{.success = true, .message = "Nickname updated"};
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

HttpClientCompatClient::HttpClientCompatClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<ClientCompatDocument> HttpClientCompatClient::Fetch() {
  if (base_url_.empty()) {
    return Error("client-compat base_url not configured");
  }
  const auto response = HttpClient::Get(base_url_ + "/v1/client-compat");
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("client-compat fetch failed with status " + std::to_string(response.value().status_code));
  }
  return ParseClientCompatDocument(response.value().body);
}

} // namespace pbr
