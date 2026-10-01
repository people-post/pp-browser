#include "feature/ai/AgentSession.h"

#include "domain/ai/conversation/Conversation.h"
#include "foundation/data/Config.h"
#include "foundation/error/AppError.h"
#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace pbr;

BriefAiEvent Meta() {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Meta;
  event.route = "chat";
  return event;
}

BriefAiEvent Status(const std::string& tool, const std::string& phase) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Status;
  event.tool = tool;
  event.phase = phase;
  return event;
}

BriefAiEvent Token(const std::string& delta) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Token;
  event.delta = delta;
  return event;
}

BriefAiEvent Done(const std::string& response) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Done;
  event.response = response;
  event.finish = "stop";
  event.sources.push_back(BriefAiSource{.title = "T", .url = "https://example.com/a", .kind = "web"});
  return event;
}

BriefAiEvent ErrorEvent(const std::string& message, const bool retryable) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Error;
  event.code = "provider_unavailable";
  event.message = message;
  event.retryable = retryable;
  return event;
}

// Network-free config: no MCP (the promoted entry is present but disabled so the default Brief MCP URL is not used),
// the local pipeline's LLM points at a closed port so a handoff fails fast.
AppConfig MakeConfig(const std::string& preset) {
  AppConfig config;
  config.llm.preset = preset;
  config.llm.api_key = "dummy-key";
  config.llm.base_url = "http://127.0.0.1:1";
  config.llm.model = preset == "brief" ? "xai" : "llama3.2";
  config.llm.require_api_key = true;
  config.promoted_mcp.id = "off";
  config.promoted_mcp.url = "http://127.0.0.1:1/mcp";
  config.promoted_mcp.enabled = false;
  config.mcp_servers.clear();
  return config;
}

class AgentSessionBriefStreamTest : public ::testing::Test {
protected:
  void SetUp() override { AppRuntime::Initialize(); }
  void TearDown() override {
    session_.reset();
    AppRuntime::Shutdown();
  }

  void Start(const std::string& preset = "brief") {
    session_ = std::make_unique<AgentSession>();
    session_->Configure(MakeConfig(preset));
    // WaitForConfigureIdle() alone can return before the posted ConfigureOnIO has started (the in-flight counter is
    // bumped on the worker), so wait for IsConfigured() first.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!session_->IsConfigured() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    session_->WaitForConfigureIdle();
    ASSERT_TRUE(session_->IsConfigured());
  }

  // Installs a fake stream replaying `events` and returning `outcome`.
  void Script(std::vector<BriefAiEvent> events, Roe<BriefAiOutcome> outcome) {
    session_->SetBriefAiStream([this, events = std::move(events), outcome = std::move(outcome)](
                                   const BriefAiRequest& request,
                                   const std::function<void(const BriefAiEvent&)>& on_event,
                                   const std::atomic<bool>&) -> Roe<BriefAiOutcome> {
      Record(request);
      for (const BriefAiEvent& event : events) {
        on_event(event);
      }
      return outcome;
    });
  }

  void Record(const BriefAiRequest& request) {
    std::lock_guard lock(mu_);
    requests_.push_back(request);
  }

  std::vector<BriefAiRequest> Requests() {
    std::lock_guard lock(mu_);
    return requests_;
  }

  // Polls until a LoadingChanged(false) arrives (or `stop_when` returns true); fails on timeout.
  std::vector<AgentEvent> WaitForTurn(const std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    std::vector<AgentEvent> all;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<AgentEvent> batch;
      AppRuntime::RunUITasks(); // events are posted to the UI mailbox
      session_->PollEvents(batch);
      for (AgentEvent& event : batch) {
        all.push_back(std::move(event));
      }
      for (const AgentEvent& event : all) {
        if (event.type == AgentEventType::LoadingChanged && !event.loading) {
          return all;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ADD_FAILURE() << "turn did not finish within the bound";
    return all;
  }

  static std::vector<AgentEvent> Of(const std::vector<AgentEvent>& events, const AgentEventType type) {
    std::vector<AgentEvent> out;
    for (const AgentEvent& event : events) {
      if (event.type == type) {
        out.push_back(event);
      }
    }
    return out;
  }

  std::unique_ptr<AgentSession> session_;
  std::mutex mu_;
  std::vector<BriefAiRequest> requests_;
};

TEST_F(AgentSessionBriefStreamTest, DoneStreamsDeltasThenReady) {
  Start();
  Script({Meta(), Status("search_web", "tool"), Status("", "answer"), Token("Hel"), Token("lo"), Done("Hello")},
         BriefAiOutcome::Done);
  session_->Submit("hi there");
  const auto events = WaitForTurn();

  ASSERT_GE(events.size(), 7u);
  EXPECT_EQ(events.front().type, AgentEventType::LoadingChanged);
  EXPECT_TRUE(events.front().loading);
  EXPECT_EQ(events.back().type, AgentEventType::LoadingChanged);
  EXPECT_FALSE(events.back().loading);

  const auto activity = Of(events, AgentEventType::ToolActivity);
  ASSERT_FALSE(activity.empty());
  EXPECT_EQ(activity.front().tool_name, "search_web");

  const auto deltas = Of(events, AgentEventType::AssistantDelta);
  ASSERT_EQ(deltas.size(), 2u);
  EXPECT_EQ(deltas[0].text, "Hel");
  EXPECT_EQ(deltas[1].text, "Hello");
  EXPECT_FALSE(deltas[0].entry_id.empty());
  EXPECT_EQ(deltas[0].entry_id, deltas[1].entry_id);

  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "Hello");
  EXPECT_EQ(ready[0].render_mode, RenderMode::Markdown);
  EXPECT_EQ(ready[0].finish_reason, "stop");
  ASSERT_EQ(ready[0].sources.size(), 1u);
  EXPECT_EQ(ready[0].sources[0].url, "https://example.com/a");
  EXPECT_EQ(ready[0].entry_id, deltas[0].entry_id);
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());

  // Order: loading(true), activity..., deltas, ready, loading(false).
  size_t first_delta = 0, ready_at = 0, last_activity = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].type == AgentEventType::AssistantDelta && first_delta == 0) first_delta = i;
    if (events[i].type == AgentEventType::AssistantReady) ready_at = i;
    if (events[i].type == AgentEventType::ToolActivity) last_activity = i;
  }
  EXPECT_LT(last_activity, first_delta);
  EXPECT_LT(first_delta, ready_at);
  EXPECT_LT(ready_at, events.size() - 1);

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].message, "hi there");
  // Configure registers only the web-search provider (no messaging/settings tools without a registration hook).
  EXPECT_TRUE(requests[0].capabilities.empty());

  const auto& entries = session_->conversation().Entries();
  ASSERT_FALSE(entries.empty());
  EXPECT_EQ(entries.back().user_text, "hi there");
  ASSERT_TRUE(entries.back().assistant_raw.has_value());
  EXPECT_EQ(*entries.back().assistant_raw, "Hello");
}

TEST_F(AgentSessionBriefStreamTest, DoneWithEmptyResponseUsesTokens) {
  Start();
  Script({Meta(), Token("a"), Token("b"), Done("")}, BriefAiOutcome::Done);
  session_->Submit("q");
  const auto events = WaitForTurn();
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "ab");
}

TEST_F(AgentSessionBriefStreamTest, ErrorAfterTokensKeepsPartial) {
  Start();
  Script({Meta(), Token("par"), Token("tial"), ErrorEvent("模型暂时不可用", true)}, BriefAiOutcome::Error);
  session_->Submit("q");
  const auto events = WaitForTurn();

  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "partial");
  EXPECT_EQ(ready[0].finish_reason, "error");
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "模型暂时不可用");
  EXPECT_TRUE(errors[0].retryable);

  size_t ready_at = 0, error_at = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].type == AgentEventType::AssistantReady) ready_at = i;
    if (events[i].type == AgentEventType::Error) error_at = i;
  }
  EXPECT_LT(ready_at, error_at);
  EXPECT_LT(error_at, events.size() - 1);
  EXPECT_EQ(events.back().type, AgentEventType::LoadingChanged);
  EXPECT_FALSE(events.back().loading);
}

TEST_F(AgentSessionBriefStreamTest, InterruptedKeepsPartialAndIsRetryable) {
  Start();
  Script({Meta(), Token("par"), Token("tial")},
         Roe<BriefAiOutcome>(AppError::Network(Err::Network::HttpError, "answer interrupted")));
  session_->Submit("q");
  const auto events = WaitForTurn();

  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "partial");
  EXPECT_EQ(ready[0].finish_reason, "error");
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_FALSE(errors[0].message.empty());
  EXPECT_TRUE(errors[0].retryable);
  EXPECT_FALSE(events.back().loading);
}

TEST_F(AgentSessionBriefStreamTest, CancelKeepsPartialWithoutError) {
  Start();
  std::atomic<bool> sent_token{false};
  session_->SetBriefAiStream([this, &sent_token](const BriefAiRequest& request,
                                                 const std::function<void(const BriefAiEvent&)>& on_event,
                                                 const std::atomic<bool>& cancel) -> Roe<BriefAiOutcome> {
    Record(request);
    on_event(Meta());
    on_event(Token("part"));
    sent_token = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!cancel.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return BriefAiOutcome::Cancelled;
  });
  session_->Submit("q");

  std::vector<AgentEvent> events;
  bool cancelled = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    std::vector<AgentEvent> batch;
    AppRuntime::RunUITasks();
    session_->PollEvents(batch);
    for (AgentEvent& event : batch) {
      events.push_back(std::move(event));
    }
    if (!cancelled && !Of(events, AgentEventType::AssistantDelta).empty()) {
      session_->Cancel();
      cancelled = true;
    }
    if (!events.empty() && events.back().type == AgentEventType::LoadingChanged && !events.back().loading) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(cancelled);

  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "part");
  EXPECT_EQ(ready[0].finish_reason, "cancelled");
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().type, AgentEventType::LoadingChanged);
  EXPECT_FALSE(events.back().loading);
}

TEST_F(AgentSessionBriefStreamTest, HandoffRunsLocalPipeline) {
  Start();
  BriefAiEvent handoff;
  handoff.type = BriefAiEvent::Type::Handoff;
  handoff.capability = "messaging";
  Script({Meta(), handoff}, BriefAiOutcome::Handoff);
  session_->Submit("send a message");
  const auto events = WaitForTurn();

  EXPECT_EQ(Requests().size(), 1u);
  EXPECT_TRUE(Of(events, AgentEventType::AssistantDelta).empty());
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().type, AgentEventType::LoadingChanged);
  EXPECT_FALSE(events.back().loading);
}

TEST_F(AgentSessionBriefStreamTest, PayloadSubmitSkipsStream) {
  Start();
  Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  session_->Submit("text", std::string("{\"some\":\"payload\"}"));
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::AssistantDelta).empty());
  EXPECT_FALSE(events.back().loading);
}

TEST_F(AgentSessionBriefStreamTest, OtherPresetSkipsStream) {
  Start("ollama");
  Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  session_->Submit("hello");
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::AssistantDelta).empty());
  EXPECT_FALSE(events.back().loading);
}

} // namespace
