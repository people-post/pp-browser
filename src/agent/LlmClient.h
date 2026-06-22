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
  std::string base_url = "https://api.openai.com/v1";
  std::string model = "gpt-4o-mini";
  std::string preset;
  bool require_api_key = true;
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

private:
  LlmConfig config_;
};

} // namespace pbr
