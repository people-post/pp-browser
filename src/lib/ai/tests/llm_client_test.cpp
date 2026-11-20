#include "lib/ai/LlmClient.h"

#include "common/PlatformLimits.h"
#include "common/ValueJson.h"
#include "common/tests/LocalHttpServer.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace {

TEST(LlmLimitKindTest, ClassifiesTheGatewayMessages) {
  using pbr::LlmClient;
  using pbr::LlmLimitKind;
  EXPECT_EQ(LlmClient::ClassifyLimit("Guest Brief LLM daily limit exceeded"), LlmLimitKind::Daily);
  EXPECT_EQ(LlmClient::ClassifyLimit("Guest Brief LLM rate limit exceeded"), LlmLimitKind::PerMinute);
  EXPECT_EQ(LlmClient::ClassifyLimit("Per-user Brief LLM rate limit exceeded"), LlmLimitKind::PerMinute);
  EXPECT_EQ(LlmClient::ClassifyLimit("Global Brief LLM rate limit exceeded"), LlmLimitKind::Other);
  EXPECT_EQ(LlmClient::ClassifyLimit("Guest LLM mint rate limit exceeded"), LlmLimitKind::Other);
  EXPECT_EQ(LlmClient::ClassifyLimit(""), LlmLimitKind::Other);
}

TEST(LlmLimitKindTest, ClassifiesTheNeutralGatewayMessages) {
  using pbr::LlmClient;
  using pbr::LlmLimitKind;
  EXPECT_EQ(LlmClient::ClassifyLimit("Per-user LLM rate limit exceeded"), LlmLimitKind::PerMinute);
  EXPECT_EQ(LlmClient::ClassifyLimit("Per-user LLM daily limit exceeded"), LlmLimitKind::Daily);
  EXPECT_EQ(LlmClient::ClassifyLimit("Guest LLM rate limit exceeded"), LlmLimitKind::PerMinute);
  EXPECT_EQ(LlmClient::ClassifyLimit("Global LLM rate limit exceeded"), LlmLimitKind::Other);
  EXPECT_EQ(LlmClient::ClassifyLimit("Global LLM guest rate limit exceeded"), LlmLimitKind::Other);
  for (const char* text :
       {"Per-user LLM rate limit exceeded", "Per-user LLM daily limit exceeded", "Guest LLM rate limit exceeded",
        "Global LLM rate limit exceeded", "Global LLM guest rate limit exceeded"}) {
    EXPECT_TRUE(LlmClient::IsBriefGatewayLimit(text)) << text;
  }
}

// RC018 item 2: the relay's gateway names each limit in `code` (SPECS § llm-gateway); its words come from the code.
TEST(LlmLimitKindTest, ClassifiesTheRelaysCodes) {
  using pbr::LlmClient;
  using pbr::LlmLimitKind;
  EXPECT_EQ(LlmClient::ClassifyLimitCode("user_rate_limit"), LlmLimitKind::PerMinute);
  EXPECT_EQ(LlmClient::ClassifyLimitCode("guest_rate_limit"), LlmLimitKind::PerMinute);
  EXPECT_EQ(LlmClient::ClassifyLimitCode("user_daily_limit"), LlmLimitKind::Daily);
  EXPECT_EQ(LlmClient::ClassifyLimitCode("guest_daily_limit"), LlmLimitKind::Daily);
  EXPECT_EQ(LlmClient::ClassifyLimitCode("global_rate_limit"), LlmLimitKind::Other);
  EXPECT_EQ(LlmClient::ClassifyLimitCode("guest_global_rate_limit"), LlmLimitKind::Other);
  EXPECT_EQ(LlmClient::ClassifyLimitCode("guest_mint_rate_limit"), LlmLimitKind::Other);
  EXPECT_EQ(LlmClient::ClassifyLimitCode("_daily_limit"), LlmLimitKind::Other);
  EXPECT_EQ(LlmClient::ClassifyLimitCode(""), LlmLimitKind::Other);
}

TEST(LlmLimitKindTest, TheRelaysCodeDecidesNotItsText) {
  using pbr::LlmClient;
  // The code wins over a text that would read otherwise (the text is the relay's to change).
  const pbr::Error daily =
      LlmClient::MapHttpError(429, R"({"error":"Per-user LLM rate limit exceeded","code":"user_daily_limit"})");
  EXPECT_EQ(daily.code, static_cast<int32_t>(pbr::Err::Auth::RateLimited));
  EXPECT_EQ(daily.user, pbr::Tr("errors.ai.limit_daily"));
  const pbr::Error minute = LlmClient::MapHttpError(429, R"({"error":"slow down","code":"user_rate_limit"})");
  EXPECT_EQ(minute.user, pbr::Tr("errors.ai.limit_minute"));
  EXPECT_EQ(LlmClient::MapHttpError(429, R"({"error":"x","code":"guest_rate_limit"})").user,
            pbr::Tr("errors.ai.limit_minute"));
  const pbr::Error global = LlmClient::MapHttpError(429, R"({"error":"Some new words","code":"global_rate_limit"})");
  EXPECT_EQ(global.code, static_cast<int32_t>(pbr::Err::Auth::RateLimited));
  EXPECT_NE(global.user, "Some new words") << "a coded gateway limit never shows the relay's English text";
  EXPECT_EQ(LlmClient::MapHttpError(429, R"({"error":"busy","code":"server_busy"})").code,
            static_cast<int32_t>(pbr::Err::Auth::ServerBusy));
}

TEST(LlmLimitKindTest, AnExpiredTokenIsExpiredNotForbidden) {
  using pbr::LlmClient;
  const pbr::Error expired = LlmClient::MapHttpError(401, R"({"error":"Invalid LLM API key","code":"token_expired"})");
  EXPECT_EQ(expired.code, static_cast<int32_t>(pbr::Err::Auth::Expired));
  EXPECT_EQ(expired.message.rfind("LLM HTTP 401", 0), 0u) << "still a refused Bearer to the 401 heal";
  EXPECT_EQ(LlmClient::MapHttpError(401, R"({"error":"x","code":"legacy_key_retired"})").code,
            static_cast<int32_t>(pbr::Err::Auth::Expired));
  EXPECT_EQ(LlmClient::MapHttpError(401, R"({"error":"Invalid LLM API key"})").code,
            static_cast<int32_t>(pbr::Err::Auth::Forbidden));
}

TEST(LlmLimitKindTest, OtherProvidersKeepTheirOwnText) {
  const pbr::Error gateway = pbr::LlmClient::MapHttpError(
      429, R"({"error":{"message":"Global Brief LLM rate limit exceeded","code":"rate_limit_error"}})");
  EXPECT_NE(gateway.user, "Global Brief LLM rate limit exceeded");
  const pbr::Error openai = pbr::LlmClient::MapHttpError(
      429, R"({"error":{"message":"You exceeded your current quota","code":"insufficient_quota"}})");
  EXPECT_EQ(openai.user, "You exceeded your current quota");
}

class LlmClientLimitsTest : public ::testing::Test {
protected:
  pbr::LlmClient Client() const {
    pbr::LlmConfig config;
    config.base_url = server_.Url() + "/v1";
    config.model = "test-model";
    config.require_api_key = false;
    config.num_predict = 0;
    return pbr::LlmClient(config);
  }

  void WriteResponse(std::string response) { server_.SetResponse(std::move(response)); }

  pbr::test::LocalHttpServer server_;
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

// PB-035: the user's key travels only over https or to the local machine. A custom base_url on plain
// http gets a clear configuration error before any request is made.
TEST(LlmClientTest, KeyIsNotSentOverPlainHttp) {
  pbr::LlmConfig config;
  config.api_key = "sk-secret";
  config.base_url = "http://203.0.113.5/v1";
  pbr::LlmClient client(config);
  auto result = client.Complete("sys", "user");
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("https"), std::string::npos) << result.error().message;
  EXPECT_EQ(result.error().message.find("sk-secret"), std::string::npos) << "the key itself must not be echoed";
}

TEST(LlmClientTest, KeyMayGoToLoopbackOverPlainHttp) {
  pbr::LlmConfig config;
  config.api_key = "sk-secret";
  config.base_url = "http://127.0.0.1:9/v1"; // nothing listens: a transport error, not a config error
  pbr::LlmClient client(config);
  auto result = client.Complete("sys", "user");
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().message.find("https"), std::string::npos) << result.error().message;
}

TEST_F(LlmClientLimitsTest, AcceptsRequestAtConfiguredLimit) {
  WriteResponse(CompletionResponseOfSize(128));
  const auto request = RequestWithPayloadSize(pbr::kMaxLlmRequestBytes);

  auto result = Client().Complete(request);

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

  auto result = Client().Complete("system", "user");

  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;
  EXPECT_EQ(*result, "ok");
}

TEST_F(LlmClientLimitsTest, RejectsResponseOverConfiguredLimit) {
  WriteResponse(CompletionResponseOfSize(pbr::kMaxLlmResponseBytes + 1));

  const auto result = Client().Complete("system", "user");

  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("response body exceeds limit of 8388608 bytes"), std::string::npos);
}

TEST(LlmClientTest, EmptyBaseUrlFailsWithoutRequest) {
  pbr::LlmConfig config;
  config.require_api_key = false;
  ASSERT_TRUE(config.base_url.empty());
  auto result = pbr::LlmClient(config).Complete("sys", "user");
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("not configured"), std::string::npos);
}
