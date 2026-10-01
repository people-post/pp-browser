#include "common/PlatformLimits.h"
#include "domain/ai/LlmClient.h"
#include "domain/ai/SseClient.h"
#include "domain/ai/tests/sse_test_server.h"
#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

pbr::SseRequest RequestFor(const SseTestServer& server) {
  pbr::SseRequest request;
  request.url = server.Url() + "/v1/stream";
  request.json_body = R"({"stream":true})";
  return request;
}

void ExpectSameErrorAsLlmClient(int status, const std::string& reason, const std::string& body) {
  SseTestServer server({{HttpError(status, reason, body)}}, false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  auto result = client.Post(RequestFor(server), [](const pbr::SseEvent&) {}, cancel);
  ASSERT_FALSE(static_cast<bool>(result));
  const pbr::Error expected = pbr::LlmClient::MapHttpError(status, body);
  EXPECT_EQ(result.error().category, expected.category);
  EXPECT_EQ(result.error().code, expected.code);
}

} // namespace

TEST(SseClientTest, DeliversEventsInOrderAcrossDelayedChunks) {
  SseTestServer server({{kSseHead},
                        {"data: one\n\nevent: delta\nda", 30},
                        {"ta: two\n\n", 30},
                        {"data: \xE4\xBD", 30}, // "你" split mid-character
                        {"\xA0\n\n", 30}},
                       false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  std::vector<pbr::SseEvent> events;
  auto result = client.Post(RequestFor(server), [&](const pbr::SseEvent& e) { events.push_back(e); }, cancel);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, pbr::SseOutcome::Completed);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "one");
  EXPECT_EQ(events[1].event, "delta");
  EXPECT_EQ(events[1].data, "two");
  EXPECT_EQ(events[2].data, "你");
}

TEST(SseClientTest, SendsBearerAcceptAndBody) {
  SseTestServer server({{kSseHead}, {"data: x\n\n"}}, false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  pbr::SseRequest request = RequestFor(server);
  request.bearer_token = "secret-token";
  ASSERT_TRUE(static_cast<bool>(client.Post(request, [](const pbr::SseEvent&) {}, cancel)));
  ASSERT_TRUE(server.WaitForRequest());
  const std::string seen = server.Request();
  EXPECT_NE(seen.find("POST /v1/stream"), std::string::npos);
  EXPECT_NE(seen.find("Authorization: Bearer secret-token"), std::string::npos);
  EXPECT_NE(seen.find("Accept: text/event-stream"), std::string::npos);
  EXPECT_NE(seen.find("Content-Type: application/json"), std::string::npos);
  EXPECT_NE(seen.find(R"({"stream":true})"), std::string::npos);
}

TEST(SseClientTest, NoAuthorizationHeaderWithoutToken) {
  SseTestServer server({{kSseHead}}, false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  ASSERT_TRUE(static_cast<bool>(client.Post(RequestFor(server), [](const pbr::SseEvent&) {}, cancel)));
  ASSERT_TRUE(server.WaitForRequest());
  EXPECT_EQ(server.Request().find("Authorization"), std::string::npos);
}

TEST(SseClientTest, CancelWhileServerIsSilentReturnsCancelledQuickly) {
  SseTestServer server({{kSseHead}, {"data: first\n\n"}}, true);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  std::atomic<bool> got_first{false};
  std::atomic<std::chrono::steady_clock::rep> finished_at{0};

  auto worker = std::async(std::launch::async, [&] {
    auto result = client.Post(RequestFor(server), [&](const pbr::SseEvent&) { got_first = true; }, cancel);
    finished_at = std::chrono::steady_clock::now().time_since_epoch().count();
    return result;
  });

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!got_first && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(got_first);
  const auto cancelled_at = std::chrono::steady_clock::now();
  cancel = true;

  ASSERT_EQ(worker.wait_for(10s), std::future_status::ready);
  auto result = worker.get();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, pbr::SseOutcome::Cancelled);
  const auto elapsed = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(finished_at.load())) - cancelled_at;
  EXPECT_LT(elapsed, 1s);
  EXPECT_TRUE(server.WaitForDisconnect());
}

TEST(SseClientTest, CancelBeforeAnyResponseReturnsCancelled) {
  SseTestServer server({}, true); // accepts and reads the request, never answers
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  auto worker = std::async(std::launch::async, [&] { return client.Post(RequestFor(server), [](const pbr::SseEvent&) {}, cancel); });
  ASSERT_TRUE(server.WaitForRequest());
  cancel = true;
  ASSERT_EQ(worker.wait_for(10s), std::future_status::ready);
  auto result = worker.get();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, pbr::SseOutcome::Cancelled);
  EXPECT_TRUE(server.WaitForDisconnect());
}

TEST(SseClientTest, IdleTimeoutReturnsTimeoutError) {
  SseTestServer server({{kSseHead}, {"data: only\n\n"}}, true);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  pbr::SseRequest request = RequestFor(server);
  request.idle_timeout_s = 1;
  std::vector<pbr::SseEvent> events;
  auto result = client.Post(request, [&](const pbr::SseEvent& e) { events.push_back(e); }, cancel);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().category, static_cast<int32_t>(pbr::ErrorCategory::Network));
  EXPECT_EQ(result.error().code, static_cast<int32_t>(pbr::Err::Network::Timeout));
  EXPECT_EQ(events.size(), 1u); // events delivered before the stall stay delivered
}

TEST(SseClientTest, RateLimitMapsLikeLlmClient) {
  ExpectSameErrorAsLlmClient(429, "Too Many Requests", R"({"error":{"message":"slow down"}})");
}

TEST(SseClientTest, UnauthorizedMapsLikeLlmClient) {
  ExpectSameErrorAsLlmClient(401, "Unauthorized", R"({"detail":{"error":{"message":"bad key","code":"auth_error"}}})");
}

TEST(SseClientTest, SuccessThatIsNotAnEventStreamIsAnError) {
  const std::string body = R"({"choices":[{"message":{"content":"not streamed"}}]})";
  SseTestServer server({{"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body}},
                       false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  std::vector<pbr::SseEvent> events;
  auto result = client.Post(RequestFor(server), [&](const pbr::SseEvent& e) { events.push_back(e); }, cancel);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().category, static_cast<int32_t>(pbr::ErrorCategory::Network));
  EXPECT_EQ(result.error().code, static_cast<int32_t>(pbr::Err::Network::HttpError));
  EXPECT_TRUE(events.empty());
}

TEST(SseClientTest, HeadersCountAsActivity) {
  // Headers at 1.2 s and the first event 1.2 s later: each gap is inside the 2 s idle limit,
  // although the first body byte arrives 2.4 s after the request.
  SseTestServer server({{kSseHead, 1200}, {"data: late\n\n", 1200}}, false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  pbr::SseRequest request = RequestFor(server);
  request.idle_timeout_s = 2;
  std::vector<pbr::SseEvent> events;
  auto result = client.Post(request, [&](const pbr::SseEvent& e) { events.push_back(e); }, cancel);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(events.size(), 1u);
}

TEST(SseClientTest, ThrowingHandlerIsAnErrorNotACrash) {
  SseTestServer server({{kSseHead}, {"data: x\n\n"}}, false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  auto result = client.Post(RequestFor(server), [](const pbr::SseEvent&) { throw std::runtime_error("boom"); }, cancel);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().category, static_cast<int32_t>(pbr::ErrorCategory::Internal));
}

TEST(SseClientTest, ConnectionRefusedIsUnreachable) {
  std::string url;
  {
    SseTestServer server({}, false);
    url = server.Url();
  } // server gone: nothing listens on that port any more
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  pbr::SseRequest request;
  request.url = url;
  request.json_body = "{}";
  auto result = client.Post(request, [](const pbr::SseEvent&) {}, cancel);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().category, static_cast<int32_t>(pbr::ErrorCategory::Network));
  EXPECT_EQ(result.error().code, static_cast<int32_t>(pbr::Err::Network::Unreachable));
}
