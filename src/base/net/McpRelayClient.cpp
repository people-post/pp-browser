#include "base/net/McpRelayClient.h"

#include "base/messaging/MessagingJson.h"
#include "base/net/McpInfraBridge.h"

namespace pbr {

McpRelayClient::McpRelayClient(McpClient* client) : client_(client) {}

void McpRelayClient::SetClient(McpClient* client) {
  client_ = client;
}

Roe<void> McpRelayClient::Send(const RelayEnvelope& envelope) {
  if (!client_) {
    return Error("MCP client not available");
  }
  auto result = CallMcpToolJson(*client_, "relay_send", RelayEnvelopeToJson(envelope));
  if (!result) {
    return result.error();
  }
  if (result->contains("success") && (*result)["success"].is_boolean() && !(*result)["success"].get<bool>()) {
    return Error(result->value("message", "relay_send failed"));
  }
  return {};
}

Roe<RelayPollResult> McpRelayClient::PollInbox(const std::string& cursor) {
  nlohmann::json arguments = nlohmann::json::object();
  if (!cursor.empty()) {
    arguments["cursor"] = cursor;
  }

  if (!client_) {
    return Error("MCP client not available");
  }
  auto result = CallMcpToolJson(*client_, "relay_poll_inbox", arguments);
  if (!result) {
    return result.error();
  }

  RelayPollResult poll;
  if (result->contains("next_cursor") && (*result)["next_cursor"].is_string()) {
    poll.next_cursor = (*result)["next_cursor"].get<std::string>();
  }
  if (result->contains("messages") && (*result)["messages"].is_array()) {
    for (const auto& item : (*result)["messages"]) {
      auto envelope = ParseRelayEnvelope(item);
      if (envelope) {
        poll.messages.push_back(*envelope);
      }
    }
  }
  return poll;
}

} // namespace pbr
