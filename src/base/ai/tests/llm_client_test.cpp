#include "base/ai/LlmClient.h"

#include <gtest/gtest.h>

#include <string>

TEST(LlmClientTest, ParsesToolCallsAndContentResponses) {
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
  ASSERT_TRUE(static_cast<bool>(tool_result));
  EXPECT_EQ(tool_result->finish_reason, "tool_calls");
  ASSERT_EQ(tool_result->tool_calls.size(), 1u);
  EXPECT_EQ(tool_result->tool_calls[0].name, "web_search");
  EXPECT_EQ(tool_result->tool_calls[0].arguments["query"], "latest news");

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
  ASSERT_TRUE(static_cast<bool>(content_result));
  EXPECT_TRUE(content_result->content.has_value());
  EXPECT_TRUE(content_result->tool_calls.empty());
  EXPECT_EQ(content_result->finish_reason, "stop");
}

TEST(LlmClientTest, ParsesTruncatedResponses) {
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
  ASSERT_TRUE(static_cast<bool>(truncated_result));
  EXPECT_EQ(truncated_result->finish_reason, "length");
}
