#include "base/net/ServiceClientsImpl.h"

#include "common/Utilities.h"
#include "base/crypto/CryptoUtil.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/net/HttpClient.h"
#include "base/people/Ed25519Signer.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include <curl/curl.h>
#include <sstream>

namespace pbr {

Roe<std::vector<DirectoryHit>> MockDirectoryClient::SearchPeople(const std::string& query) {
  DirectoryHit alice;
  alice.hit_id = "hit_alice";
  alice.display_name = "Alice Example";
  alice.nickname = "alice";
  alice.ids = {{ContactIdKind::RelayUser, "relay:alice123", true}};

  DirectoryHit bob;
  bob.hit_id = "hit_bob";
  bob.display_name = "Bob Builder";
  bob.nickname = "bob";
  bob.ids = {{ContactIdKind::RelayUser, "relay:bob456", true}};

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
  reply.session_epoch = envelope.session_epoch;
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

Roe<RelayPollResult> MockRelayClient::PollInbox(const std::string& /*cursor*/) {
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
  if (envelope.session_epoch != request.session_epoch) {
    return false;
  }
  if (envelope.route.channel != request.channel) {
    return false;
  }
  if (request.min_sender_seq && envelope.sender_seq < *request.min_sender_seq) {
    return false;
  }
  if (request.max_sender_seq && envelope.sender_seq > *request.max_sender_seq) {
    return false;
  }
  return true;
}

std::string UrlEncode(const std::string& value) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return value;
  }
  char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
  const std::string out = encoded ? encoded : value;
  if (encoded) {
    curl_free(encoded);
  }
  curl_easy_cleanup(curl);
  return out;
}

} // namespace

Roe<ChatHistoryResponse> MockRelayClient::FetchChatHistory(const ChatHistoryRequest& request) {
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

Roe<RegistrationResult> MockRegistrationClient::Register(const std::string& public_key_b64,
                                                         const std::string& nickname, const std::string& /*signature*/,
                                                         int64_t /*timestamp*/) {
  return RegistrationResult{.success = true,
                            .relay_user_id = "relay:" + public_key_b64.substr(0, 12),
                            .message = "Registered as " + nickname};
}

Roe<RegistrationResult> MockRegistrationClient::UpdateNickname(const std::string& new_nickname,
                                                                 const std::string& /*signature*/,
                                                                 int64_t /*timestamp*/) {
  return RegistrationResult{.success = true, .message = "Nickname updated to " + new_nickname};
}

HttpRelayClient::HttpRelayClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<void> HttpRelayClient::Send(const RelayEnvelope& envelope) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  const std::string url = base_url_ + "/v1/messages";
  const auto response = HttpClient::Post(url, RelayEnvelopeToJson(envelope).dump(),
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

Roe<RelayPollResult> HttpRelayClient::PollInbox(const std::string& cursor) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  std::string url = base_url_ + "/v1/inbox";
  if (!cursor.empty()) {
    url += "?cursor=" + cursor;
  }
  const auto response = HttpClient::Get(url);
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
      auto envelope = ParseRelayEnvelope(item);
      if (envelope) {
        result.messages.push_back(*envelope);
      }
    }
  }
  return result;
}

Roe<ChatHistoryResponse> HttpRelayClient::FetchChatHistory(const ChatHistoryRequest& request) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  const nlohmann::json json = ChatHistoryRequestToJson(request);
  std::ostringstream url;
  url << base_url_ << "/v1/chat-targets/messages";
  bool first = true;
  auto append = [&](const char* key, const std::string& value) {
    url << (first ? '?' : '&') << key << '=' << UrlEncode(value);
    first = false;
  };
  append("requester_identity_kind", json["requester_identity_kind"].get<std::string>());
  append("requester_identity_value", json["requester_identity_value"].get<std::string>());
  append("peer_identity_kind", json["peer_identity_kind"].get<std::string>());
  append("peer_identity_value", json["peer_identity_value"].get<std::string>());
  append("channel", json["channel"].get<std::string>());
  append("session_epoch", std::to_string(json["session_epoch"].get<uint32_t>()));
  append("limit", std::to_string(json["limit"].get<size_t>()));
  append("order", json["order"].get<std::string>());
  if (json.contains("min_sender_seq")) {
    append("min_sender_seq", std::to_string(json["min_sender_seq"].get<uint64_t>()));
  }
  if (json.contains("max_sender_seq")) {
    append("max_sender_seq", std::to_string(json["max_sender_seq"].get<uint64_t>()));
  }

  const auto response = HttpClient::Get(url.str());
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
  return ChatHistoryResponseFromJson(root);
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

HttpRegistrationClient::HttpRegistrationClient(std::string base_url) : base_url_(std::move(base_url)) {}

Roe<RegistrationResult> HttpRegistrationClient::Register(const std::string& public_key_b64,
                                                         const std::string& nickname, const std::string& signature,
                                                         int64_t timestamp) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  const nlohmann::json body = {{"public_key", public_key_b64},
                               {"nickname", nickname},
                               {"timestamp", timestamp},
                               {"signature", signature}};
  const auto response =
      HttpClient::Post(base_url_ + "/v1/register", body.dump(), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Registration failed with status " + std::to_string(response.value().status_code));
  }

  const nlohmann::json root = nlohmann::json::parse(response.value().body, nullptr, false);
  RegistrationResult result{.success = true};
  if (!root.is_discarded()) {
    if (root.contains("relay_user_id") && root["relay_user_id"].is_string()) {
      result.relay_user_id = root["relay_user_id"].get<std::string>();
    }
    if (root.contains("message") && root["message"].is_string()) {
      result.message = root["message"].get<std::string>();
    }
  }
  return result;
}

Roe<RegistrationResult> HttpRegistrationClient::UpdateNickname(const std::string& new_nickname,
                                                             const std::string& signature, int64_t timestamp) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  const nlohmann::json body = {{"nickname", new_nickname}, {"timestamp", timestamp}, {"signature", signature}};
  const auto response =
      HttpClient::Post(base_url_ + "/v1/nickname", body.dump(), {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Nickname update failed with status " + std::to_string(response.value().status_code));
  }
  return RegistrationResult{.success = true, .message = "Nickname updated"};
}

} // namespace pbr
