#pragma once

#include "common/Error.h"
#include "common/Module.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct LlmConfig {
  std::string api_key;
  std::string base_url = "https://www.brief.global/api/llm/v1";
  std::string model = "grok-4-1-fast-reasoning";
  std::string preset = "brief";
  bool require_api_key = false;
  int num_predict = 8192;
};

struct ChatMessage {
  std::string role;
  std::string content;
  std::optional<std::string> tool_call_id;
  std::optional<nlohmann::json> tool_calls;
};

struct ToolDefinition {
  std::string name;
  std::string description;
  nlohmann::json parameters;
};

struct ChatCompletionRequest {
  std::vector<ChatMessage> messages;
  std::vector<ToolDefinition> tools;
};

struct ToolCall {
  std::string id;
  std::string name;
  nlohmann::json arguments;
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
