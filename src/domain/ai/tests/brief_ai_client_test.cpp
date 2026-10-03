#include "domain/ai/BriefAiClient.h"

#include "common/ValueJson.h"
#include "domain/ai/LlmClient.h"
#include "domain/ai/tests/sse_test_server.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>

namespace {

using pbr::BriefAiEvent;
using pbr::BriefAiOutcome;
using namespace std::chrono_literals;

pbr::BriefAiClient ClientFor(const SseTestServer& server, std::string api_key = {}) {
  pbr::LlmConfig config;
  config.base_url = server.Url();
  config.api_key = std::move(api_key);
  return pbr::BriefAiClient(config);
}

pbr::Object ParsedRequest(const pbr::BriefAiRequest& request) {
  auto parsed = pbr::TryParseObject(pbr::BriefAiClient::BuildRequestJson(request));
  EXPECT_TRUE(parsed.has_value());
  return parsed.value_or(pbr::Object{});
}

std::string Data(const std::string& json) {
  return "data: " + json + "\n\n";
}

const std::string kMeta = Data(R"({"type":"meta","route":"web"})");
const std::string kDone = Data(R"({"type":"done","response":"ab","route":"web","finish":"stop","sources":[]})");
const std::string kDoneLine = "data: [DONE]\n\n";

struct StreamRun {
  std::vector<BriefAiEvent> events;
  pbr::Roe<BriefAiOutcome> result = BriefAiOutcome::Done;
};

StreamRun RunStream(const SseTestServer& server, const std::string& key = {}) {
  StreamRun run;
  std::atomic<bool> cancel{false};
  run.result = ClientFor(server, key).Stream(
      pbr::BriefAiRequest{"hi"}, [&](const BriefAiEvent& e) { run.events.push_back(e); }, cancel);
  return run;
}

} // namespace

TEST(BriefAiClientTest, RequestJsonFullRequest) {
  pbr::BriefAiRequest request;
  request.message = "question";
  request.intent = "factcheck";
  request.history = {{"user", "q1"}, {"assistant", "a1"}};
  request.summary = "sum";
  request.app_version = "1.4.0";
  request.platform = "ios";
  request.lang = "zh-Hans";
  request.capabilities = {"add_contact", "set_language"};
  const pbr::Object json = ParsedRequest(request);

  EXPECT_EQ(json.getString("message").value_or(""), "question");
  EXPECT_EQ(json.getString("intent").value_or(""), "factcheck");
  ASSERT_TRUE(json.contains("image"));
  EXPECT_TRUE(pbr::isNullValue(json.fields().tryGet("image")->get()));
  EXPECT_EQ(json.fields().size(), 5u);

  const pbr::Object* context = json.getObject("context");
  ASSERT_NE(context, nullptr);
  EXPECT_EQ(context->getString("summary").value_or(""), "sum");
  const pbr::Array* history = context->getArray("history");
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->elements.size(), 2u);
  EXPECT_EQ(pbr::asObject(history->elements[1])->getString("role").value_or(""), "assistant");
  EXPECT_EQ(pbr::asObject(history->elements[1])->getString("content").value_or(""), "a1");

  const pbr::Object* client = json.getObject("client");
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client->getString("app").value_or(""), "pp");
  EXPECT_EQ(client->getString("version").value_or(""), "1.4.0");
  EXPECT_EQ(client->getString("platform").value_or(""), "ios");
  EXPECT_EQ(client->getString("lang").value_or(""), "zh-Hans");
  const pbr::Array* capabilities = client->getArray("capabilities");
  ASSERT_NE(capabilities, nullptr);
  EXPECT_EQ(capabilities->elements.size(), 2u);
}

TEST(BriefAiClientTest, RequestJsonMinimalRequest) {
  pbr::BriefAiRequest request;
  request.message = "hello";
  const pbr::Object json = ParsedRequest(request);
  EXPECT_FALSE(json.contains("intent"));
  EXPECT_FALSE(json.contains("context"));
  EXPECT_TRUE(json.contains("image"));
  const pbr::Object* client = json.getObject("client");
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client->getString("app").value_or(""), "pp");
  EXPECT_EQ(client->fields().size(), 1u); // no empty optional fields
}

TEST(BriefAiClientTest, RequestJsonWithoutImageHasNullImage) {
  pbr::BriefAiRequest request;
  request.message = "hello";
  EXPECT_NE(pbr::BriefAiClient::BuildRequestJson(request).find(R"("image":null)"), std::string::npos);
}

TEST(BriefAiClientTest, RequestJsonImageRoundTripsBase64) {
  pbr::BriefAiRequest request;
  request.message = "what is this";
  std::vector<uint8_t> bytes;
  for (int i = 0; i < 1001; ++i) { // length not a multiple of 3: exercises padding
    bytes.push_back(static_cast<uint8_t>(i * 7));
  }
  request.image = pbr::BriefAiImage{"image/jpeg", bytes};
  const pbr::Object json = ParsedRequest(request);
  const pbr::Object* image = json.getObject("image");
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->getString("mime").value_or(""), "image/jpeg");
  const std::string data = image->getString("data").value_or("");
  EXPECT_EQ(data.find('\n'), std::string::npos);
  EXPECT_EQ(data.back(), '=');
  auto decoded = pbr::Base64Decode(data);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(*decoded, bytes);
}

TEST(BriefAiClientTest, RequestJsonImageOmitsCapabilities) {
  pbr::BriefAiRequest request;
  request.message = "x";
  request.capabilities = {"add_contact"};
  request.image = pbr::BriefAiImage{"image/png", {1, 2, 3}};
  const pbr::Object json = ParsedRequest(request);
  const pbr::Object* client = json.getObject("client");
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client->getArray("capabilities"), nullptr);
}

TEST(BriefAiClientTest, StreamRejectsBadImageWithoutSending) {
  SseTestServer server({{kSseHead}, {kMeta + kDone + kDoneLine}}, false);
  std::atomic<bool> cancel{false};
  auto client = ClientFor(server);
  auto run = [&](pbr::BriefAiImage image) {
    pbr::BriefAiRequest request;
    request.message = "x";
    request.image = std::move(image);
    return client.Stream(request, [](const BriefAiEvent&) {}, cancel);
  };
  EXPECT_FALSE(static_cast<bool>(run({"image/bmp", {1, 2, 3}})));
  EXPECT_FALSE(static_cast<bool>(run({"image/png", {}})));
  EXPECT_FALSE(static_cast<bool>(
      run({"image/png", std::vector<uint8_t>(pbr::BriefAiClient::kMaxImageBytes + 1, 0)})));
  EXPECT_TRUE(server.Request().empty()); // nothing reached the server
}

TEST(BriefAiClientTest, ValidateImageAcceptsLimitAndSupportedMimes) {
  for (const char* mime : {"image/png", "image/jpeg", "image/gif", "image/webp"}) {
    pbr::BriefAiRequest request;
    request.image = pbr::BriefAiImage{mime, std::vector<uint8_t>(pbr::BriefAiClient::kMaxImageBytes, 1)};
    EXPECT_TRUE(static_cast<bool>(pbr::BriefAiClient::ValidateImage(request))) << mime;
  }
}

TEST(BriefAiClientTest, RequestJsonContextOnlyWhenNeeded) {
  pbr::BriefAiRequest request;
  request.message = "m";
  request.summary = "only summary";
  pbr::Object json = ParsedRequest(request);
  const pbr::Object* context = json.getObject("context");
  ASSERT_NE(context, nullptr);
  EXPECT_FALSE(context->contains("history"));
  EXPECT_TRUE(context->contains("summary"));

  request.summary.clear();
  request.history = {{"user", "q"}};
  json = ParsedRequest(request);
  context = json.getObject("context");
  ASSERT_NE(context, nullptr);
  EXPECT_FALSE(context->contains("summary"));
  EXPECT_TRUE(context->contains("history"));
}

TEST(BriefAiClientTest, RequestJsonKeepsLastSixHistoryTurnsInOrder) {
  pbr::BriefAiRequest request;
  request.message = "m";
  for (int i = 0; i < 9; ++i) {
    request.history.push_back({i % 2 == 0 ? "user" : "assistant", "t" + std::to_string(i)});
  }
  const pbr::Object json = ParsedRequest(request);
  const pbr::Array* history = json.getObject("context")->getArray("history");
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->elements.size(), 6u);
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(pbr::asObject(history->elements[i])->getString("content").value_or(""), "t" + std::to_string(i + 3));
  }
}

TEST(BriefAiClientTest, ParseMeta) {
  auto e = pbr::BriefAiClient::ParseEvent(R"({"type":"meta","route":"web"})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->type, BriefAiEvent::Type::Meta);
  EXPECT_EQ(e->route, "web");
}

TEST(BriefAiClientTest, ParseStatus) {
  auto e = pbr::BriefAiClient::ParseEvent(
      R"({"type":"status","phase":"tool","tool":"search_web","query":"CNN 白宫 记者 禁入 诉讼"})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->type, BriefAiEvent::Type::Status);
  EXPECT_EQ(e->phase, "tool");
  EXPECT_EQ(e->tool, "search_web");
  EXPECT_EQ(e->query, "CNN 白宫 记者 禁入 诉讼");
  e = pbr::BriefAiClient::ParseEvent(R"({"type":"status","phase":"answer"})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->phase, "answer");
  EXPECT_TRUE(e->tool.empty());
}

TEST(BriefAiClientTest, ParseToken) {
  auto e = pbr::BriefAiClient::ParseEvent(R"({"type":"token","delta":"CNN、Politico 和 MS NOW "})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->type, BriefAiEvent::Type::Token);
  EXPECT_EQ(e->delta, "CNN、Politico 和 MS NOW ");
}

TEST(BriefAiClientTest, ParseDone) {
  auto e = pbr::BriefAiClient::ParseEvent(
      R"({"type":"done","response":"full","route":"web","finish":"stop","sources":[{"title":"White House bars CNN reporters","url":"https://example.com/a","kind":"web"}]})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->type, BriefAiEvent::Type::Done);
  EXPECT_EQ(e->response, "full");
  EXPECT_EQ(e->route, "web");
  EXPECT_EQ(e->finish, "stop");
  ASSERT_EQ(e->sources.size(), 1u);
  EXPECT_EQ(e->sources[0].title, "White House bars CNN reporters");
  EXPECT_EQ(e->sources[0].url, "https://example.com/a");
  EXPECT_EQ(e->sources[0].kind, "web");
}

TEST(BriefAiClientTest, ParseHandoff) {
  auto e = pbr::BriefAiClient::ParseEvent(R"({"type":"handoff","reason":"app_action","capability":"add_contact"})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->type, BriefAiEvent::Type::Handoff);
  EXPECT_EQ(e->capability, "add_contact");
  e = pbr::BriefAiClient::ParseEvent(R"({"type":"handoff","reason":"app_action","capability":null})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->type, BriefAiEvent::Type::Handoff);
  EXPECT_TRUE(e->capability.empty());
}

TEST(BriefAiClientTest, ParseError) {
  auto e = pbr::BriefAiClient::ParseEvent(
      R"({"type":"error","code":"provider_unavailable","message":"模型暂时不可用，请稍后重试","retryable":true})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->type, BriefAiEvent::Type::Error);
  EXPECT_EQ(e->code, "provider_unavailable");
  EXPECT_EQ(e->message, "模型暂时不可用，请稍后重试");
  EXPECT_TRUE(e->retryable);
  e = pbr::BriefAiClient::ParseEvent(R"({"type":"error"})");
  ASSERT_TRUE(e.has_value());
  EXPECT_FALSE(e->retryable);
  EXPECT_TRUE(e->code.empty());
}

TEST(BriefAiClientTest, ParseIgnoresDoneUnknownTypeUnknownFieldsAndGarbage) {
  EXPECT_FALSE(pbr::BriefAiClient::ParseEvent("[DONE]").has_value());
  EXPECT_FALSE(pbr::BriefAiClient::ParseEvent(R"({"type":"thinking","delta":"x"})").has_value());
  EXPECT_FALSE(pbr::BriefAiClient::ParseEvent(R"({"delta":"no type"})").has_value());
  EXPECT_FALSE(pbr::BriefAiClient::ParseEvent("{not json").has_value());
  EXPECT_FALSE(pbr::BriefAiClient::ParseEvent("").has_value());
  auto e = pbr::BriefAiClient::ParseEvent(R"({"type":"token","delta":"x","future":{"a":[1,2]}})");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->delta, "x");
}

TEST(BriefAiClientTest, ParseFiltersSources) {
  std::string json = R"({"type":"done","response":"r","route":"web","finish":"stop","sources":[)";
  json += R"({"title":"plain http","url":"http://example.com/x","kind":"web"},)";
  json += R"({"title":"empty","url":"","kind":"web"},)";
  json += R"({"title":"no url","kind":"web"},)";
  json += R"({"title":"scheme only","url":"https://","kind":"web"},)";
  json += R"x({"title":"js","url":"javascript:alert(1)","kind":"web"})x";
  for (int i = 0; i < 12; ++i) {
    json += R"(,{"title":"ok)" + std::to_string(i) + R"(","url":"https://example.com/)" + std::to_string(i) +
            R"(","kind":"web"})";
  }
  json += "]}";
  auto e = pbr::BriefAiClient::ParseEvent(json);
  ASSERT_TRUE(e.has_value());
  ASSERT_EQ(e->sources.size(), 10u);
  EXPECT_EQ(e->sources[0].title, "ok0");
  EXPECT_EQ(e->sources[9].title, "ok9");
}

TEST(BriefAiClientTest, StreamFullAnswerWithPingsAndSplitToken) {
  SseTestServer server({{kSseHead},
                        {": ping\n\n" + kMeta},
                        {Data(R"({"type":"status","phase":"answer"})") + ": ping\n"},
                        {"\n" + Data(R"({"type":"token","delta":"a"})") + "data: {\"type\":\"tok", 20},
                        {"en\",\"delta\":\"b\"}\n\n", 20},
                        {kDone + kDoneLine}},
                       false);
  StreamRun run = RunStream(server);
  ASSERT_TRUE(static_cast<bool>(run.result));
  EXPECT_EQ(*run.result, BriefAiOutcome::Done);
  ASSERT_EQ(run.events.size(), 5u);
  EXPECT_EQ(run.events[0].type, BriefAiEvent::Type::Meta);
  EXPECT_EQ(run.events[1].type, BriefAiEvent::Type::Status);
  EXPECT_EQ(run.events[2].type, BriefAiEvent::Type::Token);
  EXPECT_EQ(run.events[3].type, BriefAiEvent::Type::Token);
  EXPECT_EQ(run.events[4].type, BriefAiEvent::Type::Done);
  EXPECT_EQ(run.events[2].delta + run.events[3].delta, run.events[4].response);
}

TEST(BriefAiClientTest, StreamHandoffWithoutDone) {
  SseTestServer server({{kSseHead},
                        {Data(R"({"type":"meta","route":"app_action"})")},
                        {Data(R"({"type":"handoff","reason":"app_action","capability":"add_contact"})") + kDoneLine}},
                       false);
  StreamRun run = RunStream(server);
  ASSERT_TRUE(static_cast<bool>(run.result));
  EXPECT_EQ(*run.result, BriefAiOutcome::Handoff);
  ASSERT_EQ(run.events.size(), 2u);
  EXPECT_EQ(run.events[1].capability, "add_contact");
}

TEST(BriefAiClientTest, StreamErrorAfterTokens) {
  SseTestServer server(
      {{kSseHead},
       {kMeta + Data(R"({"type":"token","delta":"a"})") + Data(R"({"type":"token","delta":"b"})")},
       {Data(R"({"type":"error","code":"provider_unavailable","message":"retry later","retryable":true})") +
        kDoneLine}},
      false);
  StreamRun run = RunStream(server);
  ASSERT_TRUE(static_cast<bool>(run.result));
  EXPECT_EQ(*run.result, BriefAiOutcome::Error);
  ASSERT_EQ(run.events.size(), 4u);
  EXPECT_EQ(run.events[1].delta, "a");
  EXPECT_EQ(run.events[2].delta, "b");
  EXPECT_EQ(run.events[3].type, BriefAiEvent::Type::Error);
  EXPECT_EQ(run.events[3].code, "provider_unavailable");
  EXPECT_TRUE(run.events[3].retryable);
}

TEST(BriefAiClientTest, EventsAfterTerminalAreIgnored) {
  SseTestServer server({{kSseHead}, {kMeta + kDone + Data(R"({"type":"token","delta":"late"})") + kDoneLine}}, false);
  StreamRun run = RunStream(server);
  ASSERT_TRUE(static_cast<bool>(run.result));
  EXPECT_EQ(*run.result, BriefAiOutcome::Done);
  EXPECT_EQ(run.events.size(), 2u);
}

TEST(BriefAiClientTest, StreamClosedWithoutTerminalIsInterrupted) {
  SseTestServer server({{kSseHead}, {kMeta + Data(R"({"type":"token","delta":"a"})")}}, false);
  StreamRun run = RunStream(server);
  ASSERT_FALSE(static_cast<bool>(run.result));
  EXPECT_EQ(run.result.error().category, static_cast<int32_t>(pbr::ErrorCategory::Network));
  EXPECT_EQ(run.result.error().code, static_cast<int32_t>(pbr::Err::Network::HttpError));
  EXPECT_NE(run.result.error().message.find("answer interrupted"), std::string::npos);
  EXPECT_EQ(run.events.size(), 2u); // tokens already delivered stay delivered
}

TEST(BriefAiClientTest, CancelMidStream) {
  SseTestServer server({{kSseHead}, {kMeta + Data(R"({"type":"token","delta":"a"})")}}, true);
  std::atomic<bool> cancel{false};
  std::atomic<bool> got_token{false};
  auto client = ClientFor(server);
  auto worker = std::async(std::launch::async, [&] {
    return client.Stream(
        pbr::BriefAiRequest{"hi"},
        [&](const BriefAiEvent& e) { got_token = got_token || e.type == BriefAiEvent::Type::Token; }, cancel);
  });
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!got_token && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(got_token);
  cancel = true;
  ASSERT_EQ(worker.wait_for(10s), std::future_status::ready);
  auto result = worker.get();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, BriefAiOutcome::Cancelled);
}

TEST(BriefAiClientTest, RateLimitMapsLikeLlmClient) {
  const std::string body = R"({"error":{"message":"slow down"}})";
  SseTestServer server({{HttpError(429, "Too Many Requests", body)}}, false);
  StreamRun run = RunStream(server);
  ASSERT_FALSE(static_cast<bool>(run.result));
  const pbr::Error expected = pbr::LlmClient::MapHttpError(429, body);
  EXPECT_EQ(run.result.error().category, expected.category);
  EXPECT_EQ(run.result.error().code, expected.code);
  EXPECT_TRUE(run.events.empty());
}

TEST(BriefAiClientTest, RequestOnTheWireCarriesBearerPathAndBody) {
  SseTestServer server({{kSseHead}, {kMeta + kDone + kDoneLine}}, false);
  StreamRun run = RunStream(server, "secret-key");
  ASSERT_TRUE(static_cast<bool>(run.result));
  ASSERT_TRUE(server.WaitForRequest());
  const std::string seen = server.Request();
  EXPECT_NE(seen.find("POST /pp/chat/stream"), std::string::npos);
  EXPECT_NE(seen.find("Authorization: Bearer secret-key"), std::string::npos);
  EXPECT_NE(seen.find(R"("app":"pp")"), std::string::npos);
  EXPECT_NE(seen.find(R"("message":"hi")"), std::string::npos);
}
