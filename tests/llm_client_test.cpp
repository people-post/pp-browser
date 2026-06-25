#include "base/ai/LlmClient.h"

#include <stdexcept>
#include <string>

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

int main() {
  const std::string tool_response = R"({
    "choices": [{
      "finish_reason": "tool_calls",
      "message": {
        "role": "assistant",
        "content": null,
        "tool_calls": [{
          "id": "call_1",
          "type": "function",
          "function": {
            "name": "web_search",
            "arguments": "{\"query\":\"latest news\"}"
          }
        }]
      }
    }]
  })";

  auto tool_result = pbr::LlmClient::ParseChatCompletionResponse(tool_response);
  Expect(static_cast<bool>(tool_result), "tool response must parse");
  Expect(tool_result->finish_reason == "tool_calls", "finish_reason");
  Expect(tool_result->tool_calls.size() == 1, "one tool call");
  Expect(tool_result->tool_calls[0].name == "web_search", "tool name");
  Expect(tool_result->tool_calls[0].arguments["query"] == "latest news", "tool args");

  const std::string content_response = R"({
    "choices": [{
      "finish_reason": "stop",
      "message": {
        "role": "assistant",
        "content": "```json\n{\"blocks\":[]}\n```"
      }
    }]
  })";

  auto content_result = pbr::LlmClient::ParseChatCompletionResponse(content_response);
  Expect(static_cast<bool>(content_result), "content response must parse");
  Expect(content_result->content.has_value(), "content present");
  Expect(content_result->tool_calls.empty(), "no tool calls");
  Expect(content_result->finish_reason == "stop", "stop finish_reason");

  const std::string truncated_response = R"({
    "choices": [{
      "finish_reason": "length",
      "message": {
        "role": "assistant",
        "content": "```json\n{\"blocks\":[{\"type\":\"paragraph\",\"text\":\"truncated"
      }
    }]
  })";

  auto truncated_result = pbr::LlmClient::ParseChatCompletionResponse(truncated_response);
  Expect(static_cast<bool>(truncated_result), "truncated response must parse");
  Expect(truncated_result->finish_reason == "length", "length finish_reason");

  return 0;
}
