#include "base/ai/LlmClient.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

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

TEST(LlmClientTest, CoalescesLeadingSystemMessages) {
  std::vector<pbr::ChatMessage> messages = {
      {.role = "system", .content = "prompt"},
      {.role = "system", .content = "Conversation summary:\nprefs"},
      {.role = "user", .content = "hi"},
      {.role = "system", .content = "late instruction"},
  };

  const auto normalized = pbr::LlmClient::CoalesceLeadingSystemMessages(std::move(messages));
  ASSERT_EQ(normalized.size(), 3u);
  EXPECT_EQ(normalized[0].role, "system");
  EXPECT_EQ(normalized[0].content, "prompt\n\nConversation summary:\nprefs");
  EXPECT_EQ(normalized[1].role, "user");
  EXPECT_EQ(normalized[1].content, "hi");
  EXPECT_EQ(normalized[2].role, "system");
  EXPECT_EQ(normalized[2].content, "late instruction");
}

TEST(LlmClientTest, RequiresApiKeyWhenConfigured) {
  pbr::LlmConfig config;
  config.require_api_key = true;
  config.api_key.clear();
  config.base_url = "http://127.0.0.1:9/v1";
  pbr::LlmClient client(config);
  auto result = client.Complete("sys", "user");
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("API key"), std::string::npos);
}
