#include "domain/ai/LlmClient.h"

#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

class LlmClientLimitsTest : public ::testing::Test {
protected:
  void SetUp() override {
    response_dir_ = std::filesystem::temp_directory_path() /
                    ("pbr_llm_client_test_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(response_dir_ / "chat");
  }

  void TearDown() override { std::filesystem::remove_all(response_dir_); }

  pbr::LlmClient Client() const {
    pbr::LlmConfig config;
    config.base_url = "file://" + response_dir_.string();
    config.model = "test-model";
    config.require_api_key = false;
    config.num_predict = 0;
    return pbr::LlmClient(config);
  }

  void WriteResponse(const std::string& response) const {
    std::ofstream out(response_dir_ / "chat" / "completions", std::ios::binary);
    out.write(response.data(), static_cast<std::streamsize>(response.size()));
  }

  std::filesystem::path response_dir_;
};

pbr::ChatCompletionRequest RequestWithPayloadSize(const size_t payload_size) {
  pbr::Object message;
  message.set("role", "user");
  message.set("content", "");
  pbr::Object body;
  body.set("model", "test-model");
  body.set("messages", pbr::ArrayValue({pbr::ObjectValue(message)}));

  const size_t overhead = pbr::DumpJson(body).size();
  pbr::ChatCompletionRequest request;
  request.messages = {{.role = "user", .content = std::string(payload_size - overhead, 'x')}};
  return request;
}

std::string CompletionResponseOfSize(const size_t response_size) {
  std::string response = R"({"choices":[{"finish_reason":"stop","message":{"content":"ok"}}]})";
  response.append(response_size - response.size(), ' ');
  return response;
}

} // namespace

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
  EXPECT_EQ(tool_result->tool_calls[0].arguments.getString("query"), std::optional<std::string>("latest news"));

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

TEST_F(LlmClientLimitsTest, AcceptsRequestAtConfiguredLimit) {
  WriteResponse(CompletionResponseOfSize(128));
  const auto request = RequestWithPayloadSize(pbr::kMaxLlmRequestBytes);

  const auto result = Client().Complete(request);

  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;
  ASSERT_TRUE(result->content);
  EXPECT_EQ(*result->content, "ok");
}

TEST_F(LlmClientLimitsTest, RejectsRequestOverConfiguredLimitBeforeCurl) {
  const auto request = RequestWithPayloadSize(pbr::kMaxLlmRequestBytes + 1);

  const auto result = Client().Complete(request);

  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("request body exceeds limit of 2097152 bytes"), std::string::npos);
}

TEST_F(LlmClientLimitsTest, AcceptsResponseAtConfiguredLimit) {
  WriteResponse(CompletionResponseOfSize(pbr::kMaxLlmResponseBytes));

  const auto result = Client().Complete("system", "user");

  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;
  ASSERT_TRUE(result->content);
  EXPECT_EQ(*result->content, "ok");
}

TEST_F(LlmClientLimitsTest, RejectsResponseOverConfiguredLimit) {
  WriteResponse(CompletionResponseOfSize(pbr::kMaxLlmResponseBytes + 1));

  const auto result = Client().Complete("system", "user");

  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("response body exceeds limit of 8388608 bytes"), std::string::npos);
}
