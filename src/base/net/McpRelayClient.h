#pragma once

#include "base/ai/mcp/McpClient.h"
#include "base/net/ServiceClients.h"

#include <string>

namespace pbr {

class McpRelayClient : public IRelayClient {
public:
  explicit McpRelayClient(McpClient* client = nullptr);

  void SetClient(McpClient* client);
  Roe<void> Send(const RelayEnvelope& envelope) override;
  Roe<RelayPollResult> PollInbox(const std::string& cursor) override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  McpClient* client_ = nullptr;
};

} // namespace pbr
