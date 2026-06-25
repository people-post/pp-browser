#include "net/ServiceClientsImpl.h"

#include "common/Utilities.h"
#include "messaging/MessagingJson.h"
#include "net/HttpClient.h"

#include <nlohmann/json.hpp>

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

  RelayEnvelope reply;
  reply.thread_id = envelope.thread_id;
  reply.message_id = util::GenerateUuid();
  reply.sender_relay_id = "relay:mock-peer";
  reply.body.text = "Mock reply to: " + envelope.body.text;
  reply.timestamp = util::NowUnixMs();
  delivered_.push_back(std::move(reply));
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
      result.messages.push_back(RelayEnvelopeFromJson(item));
    }
  }
  return result;
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
