#pragma once

#include "foundation/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct ChatMessage {
  std::string role;
  std::string content;
  std::optional<std::string> tool_call_id;
  std::optional<Value> tool_calls;
};

struct ToolDefinition {
  std::string name;
  std::string description;
  Object parameters;
};

struct ChatCompletionRequest {
  std::vector<ChatMessage> messages;
  std::vector<ToolDefinition> tools;
};

struct ToolCall {
  std::string id;
  std::string name;
  Object arguments;
};

struct ChatCompletionResponse {
  std::optional<std::string> content;
  std::vector<ToolCall> tool_calls;
  std::string finish_reason;
};

class LlmClient : public Module {
public:
  explicit LlmClient(LlmConfig config);
  LlmClient(const LlmClient& other);

  Roe<std::string> Complete(const std::string& system_prompt, const std::string& user_prompt) const;
  Roe<ChatCompletionResponse> Complete(const ChatCompletionRequest& request) const;

  static Roe<ChatCompletionResponse> ParseChatCompletionResponse(const std::string& response);

  // Merge leading consecutive system messages into one. Safe for strict chat templates (e.g. Qwen).
  static std::vector<ChatMessage> CoalesceLeadingSystemMessages(std::vector<ChatMessage> messages);

private:
  LlmConfig config_;
};

} // namespace pbr
