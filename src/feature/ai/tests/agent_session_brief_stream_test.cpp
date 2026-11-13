#include "feature/ai/tests/agent_session_test_support.h"

#include <algorithm>

namespace {

using namespace pbr;
using namespace pbr::agent_test;

// The default brief config streams through the real client (here: a closed port), so the turn ends
// with a retryable error and never reaches the local pipeline or a persisted answer.
TEST_F(AgentSessionBriefStreamTest, DefaultBriefConfigStreamsThroughTheRealClient) {
  Start("brief");
  session_->Submit("hi there");
  const auto events = WaitForTurn();
  bool delta = false, ready = false, ended = false, error = false;
  for (const AgentEvent& event : events) {
    delta |= event.type == AgentEventType::AssistantDelta;
    ready |= event.type == AgentEventType::AssistantReady;
    ended |= event.type == AgentEventType::LoadingChanged && !event.loading;
    error |= event.type == AgentEventType::Error && event.retryable;
  }
  EXPECT_FALSE(delta);
  EXPECT_FALSE(ready);
  EXPECT_TRUE(error);
  EXPECT_TRUE(ended);
  // (No fake is installed here, so Requests() says nothing; the retryable error is what shows the
  // stream path ran: PushError(Error) from the local pipeline is never retryable.)
}

TEST_F(AgentSessionBriefStreamTest, MissingKeyIsASettingsErrorNotARetry) {
  AppConfig config = MakeConfig("brief");
  config.llm.api_key.clear();
  StartWith(config);
  Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  session_->Submit("hi there");
  const auto events = WaitForTurn();
  bool error = false, retryable = false, ready = false;
  for (const AgentEvent& event : events) {
    if (event.type == AgentEventType::Error) {
      error = true;
      retryable |= event.retryable;
      EXPECT_NE(event.message.find("key"), std::string::npos) << event.message;
    }
    ready |= event.type == AgentEventType::AssistantReady;
  }
  EXPECT_TRUE(error);
  EXPECT_FALSE(retryable);
  EXPECT_FALSE(ready);
  EXPECT_TRUE(Requests().empty());    // the fake stream was never called
  EXPECT_TRUE(LlmRequests().empty()); // nor the completions route (B99)
}

TEST_F(AgentSessionBriefStreamTest, DoneStreamsDeltasThenReady) {
  Start();
  Script({Meta(), Status("search_web", "tool"), Status("", "answer"), Token("Hel"), Token("lo"), Done("Hello")},
         BriefAiOutcome::Done);
  session_->Submit("hi there");
  const auto events = WaitForTurn();

  ASSERT_GE(events.size(), 6u);
  EXPECT_EQ(events.front().type, AgentEventType::LoadingChanged);
  EXPECT_TRUE(events.front().loading);
  EXPECT_EQ(events.back().type, AgentEventType::LoadingChanged);
  EXPECT_FALSE(events.back().loading);

  const auto activity = Of(events, AgentEventType::ToolActivity);
  ASSERT_FALSE(activity.empty());
  EXPECT_EQ(activity.front().tool_name, "search_web");

  // Deltas are rate-limited (20/s) and carry the text so far; the final text always comes with
  // AssistantReady, so a scripted burst may collapse to the first delta only.
  const auto deltas = Of(events, AgentEventType::AssistantDelta);
  ASSERT_GE(deltas.size(), 1u);
  EXPECT_EQ(deltas[0].text, "Hel");
  for (const AgentEvent& delta : deltas) {
    EXPECT_EQ(std::string("Hello").rfind(delta.text, 0), 0u) << delta.text; // each delta is a prefix
    EXPECT_EQ(delta.entry_id, deltas[0].entry_id);
  }
  EXPECT_FALSE(deltas[0].entry_id.empty());

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
    if (events[i].type == AgentEventType::AssistantDelta && first_delta == 0)
      first_delta = i;
    if (events[i].type == AgentEventType::AssistantReady)
      ready_at = i;
    if (events[i].type == AgentEventType::ToolActivity)
      last_activity = i;
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
  EXPECT_TRUE(LlmRequests().empty()); // a failed stream is reported as is, never retried through completions

  size_t ready_at = 0, error_at = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].type == AgentEventType::AssistantReady)
      ready_at = i;
    if (events[i].type == AgentEventType::Error)
      error_at = i;
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

// A handoff is answered on the device; a capability the app does not declare cannot be done (B99: never the
// completions route, never the stream again).
TEST_F(AgentSessionBriefStreamTest, HandoffForAnUndeclaredCapabilityIsAnsweredOnTheDevice) {
  Start();
  BriefAiEvent handoff;
  handoff.type = BriefAiEvent::Type::Handoff;
  handoff.capability = "messaging";
  Script({Meta(), handoff}, BriefAiOutcome::Handoff);
  session_->Submit("send a message");
  const auto events = WaitForTurn();

  EXPECT_EQ(Requests().size(), 1u);
  EXPECT_TRUE(LlmRequests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::AssistantDelta).empty());
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_FALSE(ready[0].from_model);
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
  EXPECT_TRUE(LlmRequests().empty()); // a payload without a plan is not handed to the model planner (B99)
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

// --- Image turns (thread mode) ---

TEST_F(AgentSessionBriefStreamTest, ImageTurnSendsImageWithoutCapabilitiesAndStreams) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("It is "), Token("a cat"), Done("It is a cat")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "What is this?", std::nullopt, TestImage("msg-1"));
  const auto events = WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_TRUE(LlmRequests().empty());
  EXPECT_EQ(requests[0].message, "What is this?"); // the question only, no marker
  ASSERT_TRUE(requests[0].image.has_value());
  EXPECT_EQ(requests[0].image->mime, "image/jpeg");
  EXPECT_EQ(requests[0].image->data, TestImage().image.data);
  EXPECT_TRUE(requests[0].capabilities.empty());

  EXPECT_FALSE(Of(events, AgentEventType::AssistantDelta).empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "It is a cat");
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_FALSE(events.back().loading);

  // The stored user message is text only, with the marker.
  ASSERT_GE(store.messages.size(), 2u);
  EXPECT_EQ(store.messages[0].text, std::string(kAiImageTurnMarker) + "What is this?");
  EXPECT_EQ(store.messages[0].id, "msg-1");                 // the caller's id, so the GUI can pair its thumbnail
  ASSERT_TRUE(store.messages[0].ai_image_json.has_value()); // the GUI's reference to the stored image
  EXPECT_EQ(*store.messages[0].ai_image_json, R"({"content_hash":"ab"})");
  EXPECT_FALSE(store.messages[1].ai_image_json.has_value()); // the answer carries none
}

// "@ai" in a chat goes to brief_AI too: the question only, no history, never handed back.
TEST_F(AgentSessionBriefStreamTest, LocalAtAiInAChatStreamsTheQuestionOnly) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("It is "), Token("true"), Done("It is true")}, BriefAiOutcome::Done);
  session_->SubmitScopedAssist("t1", "Is this true?\n\n> the quoted message", std::nullopt, AtAiMode::Local);
  const auto events = WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].message, "Is this true?\n\n> the quoted message");
  EXPECT_TRUE(requests[0].history.empty());
  EXPECT_TRUE(requests[0].summary.empty());
  EXPECT_TRUE(requests[0].capabilities.empty());
  EXPECT_FALSE(requests[0].image.has_value());

  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "It is true");
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());

  // The answer stays on this device: stored, not relay-visible.
  ASSERT_EQ(store.messages.size(), 1u);
  EXPECT_EQ(store.messages[0].sender_contact_id, kAiAssistantContactId);
  EXPECT_FALSE(store.messages[0].relay_visible);
}

// A shared "@ai+" / "@ai++" streams through brief_AI like "@ai" (the brief gateway has no completions
// route: B99). Only the question goes out; the answer is not persisted here (the chat UI renders and
// relays it) and the ready event carries the mode so the UI knows to relay.
TEST_F(AgentSessionBriefStreamTest, AtAiInAChatSendsThatChatsPreviousRole) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("a"), Done("A", "pp_news_analyst")}, BriefAiOutcome::Done);
  session_->SubmitScopedAssist("p1", "What happened in Iran?", std::nullopt, AtAiMode::Local);
  WaitForTurn();
  session_->SubmitScopedAssist("p1", "And their reaction?", std::nullopt, AtAiMode::Local);
  WaitForTurn();
  session_->SubmitScopedAssist("p2", "Weather tomorrow?", std::nullopt, AtAiMode::Local);
  WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_TRUE(requests[0].last_role.empty());
  EXPECT_EQ(requests[1].last_role, "pp_news_analyst"); // same chat
  EXPECT_TRUE(requests[2].last_role.empty());          // another chat
}

TEST_F(AgentSessionBriefStreamTest, SharedReplyAtAiStreamsThroughBriefAndLeavesRelayToTheUi) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("A joke"), Done("A joke")}, BriefAiOutcome::Done);
  session_->SubmitScopedAssist("t1", "Tell a joke", std::nullopt, AtAiMode::SharedReply);
  const auto events = WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].message, "Tell a joke");
  EXPECT_TRUE(requests[0].history.empty());
  EXPECT_TRUE(requests[0].capabilities.empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "A joke");
  EXPECT_EQ(ready[0].shared_ai_mode, AtAiMode::SharedReply);
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_TRUE(store.messages.empty()) << "the chat UI persists a shared reply after rendering";
}

TEST_F(AgentSessionBriefStreamTest, SharedFullAtAiStreamsThroughBriefAndLeavesRelayToTheUi) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("A joke"), Done("A joke")}, BriefAiOutcome::Done);
  session_->SubmitScopedAssist("t1", "Tell a joke", std::nullopt, AtAiMode::SharedFull);
  const auto events = WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].message, "Tell a joke");
  EXPECT_TRUE(requests[0].history.empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].shared_ai_mode, AtAiMode::SharedFull);
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_TRUE(store.messages.empty());
}

TEST_F(AgentSessionBriefStreamTest, ImageTurnCancelKeepsPartial) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  session_->SetBriefAiStream([this](const BriefAiRequest& request,
                                    const std::function<void(const BriefAiEvent&)>& on_event,
                                    const std::atomic<bool>& cancel) -> Roe<BriefAiOutcome> {
    Record(request);
    on_event(Meta());
    on_event(Token("part"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!cancel.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return BriefAiOutcome::Cancelled;
  });
  session_->SubmitToThread("t1", "q", std::nullopt, TestImage());

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
}

TEST_F(AgentSessionBriefStreamTest, ImageTurnWithOtherPresetFailsWithoutDroppingSilently) {
  Start("ollama");
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "q", std::nullopt, TestImage());
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].message.find("Brief"), std::string::npos) << errors[0].message;
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  EXPECT_TRUE(store.messages.empty()); // nothing was recorded for the failed turn
  EXPECT_FALSE(events.back().loading);
}

TEST_F(AgentSessionBriefStreamTest, ImageTurnIsNeverHandedBack) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  BriefAiEvent handoff;
  handoff.type = BriefAiEvent::Type::Handoff;
  handoff.capability = "messaging";
  Script({Meta(), handoff}, BriefAiOutcome::Handoff);
  session_->SubmitToThread("t1", "q", std::nullopt, TestImage());
  const auto events = WaitForTurn();

  EXPECT_EQ(Of(events, AgentEventType::Error).size(), 1u);
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  EXPECT_FALSE(events.back().loading);
}

TEST_F(AgentSessionBriefStreamTest, ImageTurnHttpStatusIsMarkedForTheGui) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({}, Roe<BriefAiOutcome>(AppError::Network(Err::Network::HttpError, "LLM HTTP 413: too big")));
  session_->SubmitToThread("t1", "q", std::nullopt, TestImage());
  auto errors = Of(WaitForTurn(), AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].error_kind, "image_too_large");

  Script({}, Roe<BriefAiOutcome>(AppError::Network(Err::Network::HttpError, "LLM HTTP 400: bad")));
  session_->SubmitToThread("t1", "q", std::nullopt, TestImage());
  errors = Of(WaitForTurn(), AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].error_kind, "image_unsupported");

  // A turn without an image keeps the server's own wording.
  session_->SubmitToThread("t1", "q");
  errors = Of(WaitForTurn(), AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_TRUE(errors[0].error_kind.empty());
}

// --- The role that answered the previous turn travels back (context.last_role) ---

TEST_F(AgentSessionBriefStreamTest, NextTurnInTheSameThreadSendsThePreviousRole) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("a"), Done("A", "pp_news_analyst")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "first");
  WaitForTurn();
  Script({Meta(), Token("b"), Done("B", "pp_assistant")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "second");
  WaitForTurn();
  session_->SubmitToThread("t1", "third");
  WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_TRUE(requests[0].last_role.empty());
  EXPECT_EQ(requests[1].last_role, "pp_news_analyst");
  EXPECT_EQ(requests[2].last_role, "pp_assistant"); // the newest answer wins
}

TEST_F(AgentSessionBriefStreamTest, AnotherThreadDoesNotGetThePreviousRole) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("a"), Done("A", "pp_news_analyst")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "first");
  WaitForTurn();
  session_->SubmitToThread("t2", "elsewhere");
  WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_TRUE(requests[1].last_role.empty());
}

TEST_F(AgentSessionBriefStreamTest, ForgettingTheThreadDropsItsRole) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("a"), Done("A", "pp_news_analyst")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "first");
  WaitForTurn();
  session_->ForgetThread("t1");
  session_->SubmitToThread("t1", "after clear");
  WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_TRUE(requests[1].last_role.empty());
}

TEST_F(AgentSessionBriefStreamTest, ErrorAndHandoffKeepThePreviousRole) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("a"), Done("A", "pp_news_analyst")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "first");
  WaitForTurn();

  Script({Meta(), ErrorEvent("boom", true)}, BriefAiOutcome::Error);
  session_->SubmitToThread("t1", "fails");
  WaitForTurn();

  BriefAiEvent handoff;
  handoff.type = BriefAiEvent::Type::Handoff;
  handoff.capability = "not_declared";
  Script({Meta(), handoff}, BriefAiOutcome::Handoff);
  session_->SubmitToThread("t1", "handed back");
  WaitForTurn();

  Script({Meta(), Token("c"), Done("C")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "fourth");
  WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 4u);
  EXPECT_EQ(requests[1].last_role, "pp_news_analyst");
  EXPECT_EQ(requests[2].last_role, "pp_news_analyst");
  EXPECT_EQ(requests[3].last_role, "pp_news_analyst");
}

TEST_F(AgentSessionBriefStreamTest, DoneWithoutRoleClearsTheRemembered) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("a"), Done("A", "pp_news_analyst")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "first");
  WaitForTurn();
  Script({Meta(), Token("b"), Done("B")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "second");
  WaitForTurn();
  session_->SubmitToThread("t1", "third");
  WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[1].last_role, "pp_news_analyst");
  EXPECT_TRUE(requests[2].last_role.empty());
}

TEST_F(AgentSessionBriefStreamTest, FollowUpHistoryHasMarkerAndNoImage) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("a"), Done("A cat.")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "What is this?", std::nullopt, TestImage());
  WaitForTurn();
  session_->SubmitToThread("t1", "And its colour?");
  WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_FALSE(requests[1].image.has_value());
  EXPECT_EQ(requests[1].message, "And its colour?");
  ASSERT_GE(requests[1].history.size(), 2u);
  EXPECT_EQ(requests[1].history[0].role, "user");
  EXPECT_EQ(requests[1].history[0].content, "[Image] What is this?");
  EXPECT_EQ(requests[1].history[1].role, "assistant");
  EXPECT_EQ(requests[1].history[1].content, "A cat.");
}

// The ask-AI panel's one-off question (settings "?", DEV/PLAN-ui-batch5-ask-ai.md 5a) runs in the Home session after
// StartNewConversation: it goes without the earlier Home turns, and its own follow-up keeps it.
TEST_F(AgentSessionBriefStreamTest, ANewHomeConversationSendsNoEarlierHistory) {
  Start();
  Script({Meta(), Token("a"), Done("Earlier answer.")}, BriefAiOutcome::Done);
  session_->Submit("earlier question");
  WaitForTurn();
  ASSERT_TRUE(session_->StartNewConversation());
  EXPECT_TRUE(session_->conversation().Entries().empty());
  session_->Submit("What does Send crash reports do?");
  WaitForTurn();
  session_->Submit("And is it on by default?");
  WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_TRUE(requests[1].history.empty()) << "the one-off question carries nothing from before";
  ASSERT_EQ(requests[2].history.size(), 2u) << "its follow-up stays with it";
  EXPECT_EQ(requests[2].history[0].content, "What does Send crash reports do?");
}

TEST_F(AgentSessionBriefStreamTest, ANewHomeConversationWaitsForTheRunningTurn) {
  Start();
  Script({Meta(), Token("a"), Done("A")}, BriefAiOutcome::Done);
  session_->Submit("question");
  EXPECT_FALSE(session_->StartNewConversation()) << "a running turn keeps its conversation";
  WaitForTurn();
  EXPECT_TRUE(session_->StartNewConversation());
}

// --- Home chips: tool payload turns run on the device, no stream and no completions call ---
// The config's completions endpoint is a closed port, so a model call would surface as an Error event.

class AgentSessionPayloadTurnTest : public AgentSessionBriefStreamTest {
protected:
  void StartWithTools(const bool with_articles) {
    session_ = std::make_unique<AgentSession>();
    session_->SetToolRegistrationHook([this, with_articles](ToolRegistry& registry) {
      registry.Register(MakeTool(ToolDefinition{"search_people", "search", Object{}}, ToolMeta{},
                                 [this](const Object& args) -> Roe<std::string> {
                                   std::lock_guard lock(mu_);
                                   calls_.push_back("search_people " + DumpJson(args));
                                   return std::string("[]");
                                 }));
      if (with_articles) {
        registry.Register(MakeTool(
            ToolDefinition{"blog_articles", "articles", Object{}}, ToolMeta{},
            [this](const Object& args) -> Roe<std::string> {
              std::lock_guard lock(mu_);
              calls_.push_back("blog_articles " + DumpJson(args));
              return std::string(
                  R"({"articles":[{"id":"a1","title":"","content":"First brief.","link_to":"https://apnews.com/x","created_at":1759000000},)"
                  R"({"id":"a2","title":"Second","content":"Body.","link_to":"http://plain.example/y","created_at":1759000000000}]})");
            }));
      }
    });
    session_->Configure(MakeConfig("brief"));
    session_->WaitForConfigureIdle();
    ASSERT_TRUE(session_->IsConfigured());
    session_->SetThreadStore(&store_);
    Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  }

  std::vector<std::string> Calls() {
    std::lock_guard lock(mu_);
    return calls_;
  }

  MemoryThreadStore store_;
  std::vector<std::string> calls_;
};

TEST_F(AgentSessionPayloadTurnTest, ArticlesChipShowsTheFeedWithoutAnyModelCall) {
  StartWithTools(true);
  session_->SubmitToThread("t1", "Show me recent articles",
                           std::string(R"({"tool":"blog_articles","brf_domain":"en","brf_language":"en","size":2})"));
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());    // no stream
  EXPECT_TRUE(LlmRequests().empty()); // no completions call
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].response_goal, ResponseGoal::DisplayFeed);
  EXPECT_NE(ready[0].text.find("long_list"), std::string::npos);
  EXPECT_NE(ready[0].text.find("apnews.com"), std::string::npos);
  EXPECT_NE(ready[0].text.find("open_url"), std::string::npos);
  EXPECT_NE(ready[0].text.find("before_id"), std::string::npos); // a full page offers "load more"
  const auto calls = Calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_NE(calls[0].find(R"("brf_domain":"en")"), std::string::npos);
}

TEST_F(AgentSessionPayloadTurnTest, ArticlesChipWithoutTheToolSaysSoAndAsksNoModel) {
  StartWithTools(false);
  session_->SubmitToThread("t1", "Show me recent articles",
                           std::string(R"({"tool":"blog_articles","brf_domain":"en","brf_language":"en","size":10})"));
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

// --- Stop on a turn that does not stream, then a new question: the stopped turn's late result is dropped ---

TEST_F(AgentSessionBriefStreamTest, AStoppedTurnsLateResultNeverLandsInTheNextTurn) {
  // Each feed call blocks until its turn's release; it answers with that turn's own link.
  struct Gate {
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::atomic<bool> returned{false};
  };
  auto one = std::make_shared<Gate>();
  auto two = std::make_shared<Gate>();
  session_ = std::make_unique<AgentSession>();
  session_->SetToolRegistrationHook([one, two](ToolRegistry& registry) {
    registry.Register(MakeTool(ToolDefinition{"blog_articles", "articles", Object{}}, ToolMeta{},
                               [one, two](const Object& args) -> Roe<std::string> {
                                 const std::string which = args.getString("brf_domain").value_or("");
                                 Gate& gate = which == "one" ? *one : *two;
                                 gate.started = true;
                                 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                                 while (!gate.release && std::chrono::steady_clock::now() < deadline) {
                                   std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                 }
                                 gate.returned = true;
                                 return R"({"articles":[{"id":")" + which + R"(","title":"T","content":"Body )" +
                                        which + R"(","link_to":"https://)" + which +
                                        R"(.example/a","created_at":1759000000}]})";
                               }));
  });
  session_->Configure(MakeConfig("brief"));
  session_->WaitForConfigureIdle();
  ASSERT_TRUE(session_->IsConfigured());
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  const auto wait_for = [](const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return flag.load();
  };

  ASSERT_EQ(session_->SubmitToThread("t1", "first", std::string(R"({"tool":"blog_articles","brf_domain":"one"})")),
            SubmitOutcome::Started);
  ASSERT_TRUE(wait_for(one->started));
  session_->Cancel(); // Stop: the session is free at once, the first feed call still runs
  ASSERT_EQ(session_->SubmitToThread("t1", "second", std::string(R"({"tool":"blog_articles","brf_domain":"two"})")),
            SubmitOutcome::Started);
  ASSERT_TRUE(wait_for(two->started));

  one->release = true; // the stopped turn's result arrives while the new turn runs
  ASSERT_TRUE(wait_for(one->returned));
  std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let the stopped turn's step run to its end
  EXPECT_TRUE(session_->IsBusy()) << "the stopped turn ended the new one";
  two->release = true;
  const auto events = WaitForTurn();

  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_NE(ready[0].text.find("two.example"), std::string::npos);
  EXPECT_EQ(ready[0].text.find("one.example"), std::string::npos);
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  for (const ThreadMessage& message : store.messages) {
    EXPECT_EQ(message.text.find("one.example"), std::string::npos) << "the stopped turn's result was stored";
  }
  session_.reset(); // before `store`
}

// --- A feed turn whose tool the promoted MCP server did not list: it connects (B210), and when the server answered
// without the tool it re-lists for a bounded time; an unreachable server fails it at once, without that wait.

class AgentSessionFeedRelistTest : public AgentSessionBriefStreamTest {
protected:
  // The promoted MCP points at a closed port, so configure lists no tools; then the fake server takes over.
  void StartWithPromotedMcp(const bool enabled, const int failing_lists,
                            const std::chrono::milliseconds wait = std::chrono::milliseconds(400)) {
    AppConfig config = MakeConfig("brief");
    config.promoted_mcp.id = "brief";
    config.promoted_mcp.enabled = enabled;
    session_ = std::make_unique<AgentSession>();
    session_->SetMissingToolWait(wait, std::chrono::milliseconds(20));
    session_->Configure(config);
    session_->WaitForConfigureIdle();
    ASSERT_TRUE(session_->IsConfigured());
    session_->SetThreadStore(&store_);
    Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
    server_->failing_lists = failing_lists;
    McpClient::SetHttpPost(FakeMcpPost(server_));
  }

  void TearDown() override {
    server_->hold_list = false;
    AgentSessionBriefStreamTest::TearDown();
    WaitForMcpConnects();
    McpClient::SetHttpPost(nullptr);
  }

  /** The connect lists another tool (the server answers), then every re-list fails: the turn waits and re-lists. */
  void ListOtherToolThenStopAnswering() {
    server_->first_list_tool = "other_tool";
    server_->fail_from = 1;
  }

  void SubmitArticles() {
    session_->SubmitToThread("t1", "Show me recent articles",
                             std::string(R"({"tool":"blog_articles","brf_domain":"en","brf_language":"en","size":2})"));
  }

  MemoryThreadStore store_;
  std::shared_ptr<FakeMcpServer> server_ = std::make_shared<FakeMcpServer>();
};

// The connect gets no answer (offline): the feed fails at once, no re-list wait on top of the connect.
TEST_F(AgentSessionFeedRelistTest, AnUnreachableServerFailsTheFeedAtOnce) {
  // The re-list wait is a minute: a turn that waited it out would not end within WaitForTurn's 20 s (no wall-clock
  // limit of its own: that is far from any scheduling jitter), and list_calls == 1 shows no re-list was made.
  StartWithPromotedMcp(true, -1, std::chrono::seconds(60));
  SubmitArticles();
  const auto events = WaitForTurn(std::chrono::seconds(20));

  EXPECT_EQ(server_->list_calls.load(), 1); // the connect's only
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

// Offline: the next question within the cooldown dials nothing and fails the same way; a new configuration dials
// again.
TEST_F(AgentSessionFeedRelistTest, AnUnreachableServerIsNotDialedAgainDuringTheCooldown) {
  StartWithPromotedMcp(true, -1);
  SubmitArticles();
  ASSERT_EQ(Of(WaitForTurn(), AgentEventType::Error).size(), 1u);
  WaitForMcpConnects();
  ASSERT_EQ(server_->init_calls.load(), 1);
  ASSERT_EQ(server_->list_calls.load(), 1);

  // "Dials nothing" is shown by the call counts below, not by a time limit.
  SubmitArticles();
  const auto second = WaitForTurn();
  const auto errors = Of(second, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
  EXPECT_EQ(server_->init_calls.load(), 1);
  EXPECT_EQ(server_->list_calls.load(), 1);

  AppConfig config = MakeConfig("brief");
  config.promoted_mcp.id = "brief";
  config.promoted_mcp.enabled = true;
  session_->Configure(config);
  session_->WaitForConfigureIdle();
  server_->failing_lists = 0;
  SubmitArticles();
  EXPECT_EQ(Of(WaitForTurn(), AgentEventType::AssistantReady).size(), 1u);
  EXPECT_EQ(server_->init_calls.load(), 2);
}

// The server answers but does not offer the tool: no point in polling it for the rest of the wait.
TEST_F(AgentSessionFeedRelistTest, ServerListingWithoutTheToolFailsAtOnce) {
  server_->tool_name = "other_tool";
  // As above: the minute-long wait is excluded by the turn ending within 20 s and by one re-list only.
  StartWithPromotedMcp(true, 0, std::chrono::seconds(60));
  SubmitArticles();
  const auto events = WaitForTurn(std::chrono::seconds(20));

  EXPECT_EQ(server_->list_calls.load(), 2); // the turn's connect, then one re-list
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

// Stop during the wait ends the turn quietly: no error, and the wait stops.
TEST_F(AgentSessionFeedRelistTest, CancelDuringTheWaitEndsTheTurnWithoutAnError) {
  StartWithPromotedMcp(true, 0, std::chrono::milliseconds(5000));
  ListOtherToolThenStopAnswering();
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  while (server_->list_calls.load() < 2 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GE(server_->list_calls.load(), 2); // the connect, then the re-list wait
  session_->Cancel();
  const auto events = WaitForTurn(std::chrono::milliseconds(1000));

  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const int calls = server_->list_calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(server_->list_calls.load(), calls);
}

// Stop and a new question right away: the stopped wait must not end (or fail) the new turn.
TEST_F(AgentSessionFeedRelistTest, CancelThenNewTurnLeavesTheNewTurnAlone) {
  StartWithPromotedMcp(true, 0, std::chrono::milliseconds(5000));
  ListOtherToolThenStopAnswering();
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  while (server_->list_calls.load() < 2 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GE(server_->list_calls.load(), 2);
  session_->Cancel();
  session_->SubmitToThread("t1", "What is in the news today?");
  auto events = WaitForTurn();
  const int calls = server_->list_calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(150)); // the old wait would poll ~7 more times
  AppRuntime::RunUITasks();
  std::vector<AgentEvent> late;
  session_->PollEvents(late);
  events.insert(events.end(), late.begin(), late.end());

  EXPECT_LE(server_->list_calls.load(), calls + 1);
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
  int ends = 0;
  for (const AgentEvent& event : events) {
    ends += event.type == AgentEventType::LoadingChanged && !event.loading;
  }
  EXPECT_EQ(ends, 1); // only the new turn ended
}

// The server answered the connect without the tool and then stops answering: the turn re-lists for the wait.
TEST_F(AgentSessionFeedRelistTest, AReListThatNeverAnswersEndsWithUnavailableAfterTheWait) {
  StartWithPromotedMcp(true, 0);
  ListOtherToolThenStopAnswering();
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  const auto events = WaitForTurn();

  EXPECT_GE(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(300)); // it waited (last poll may be skipped)
  EXPECT_GE(server_->list_calls.load(), 3);
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

TEST_F(AgentSessionFeedRelistTest, DisabledPromotedMcpFailsAtOnceWithoutListing) {
  StartWithPromotedMcp(false, 0);
  SubmitArticles();
  const auto events = WaitForTurn();

  EXPECT_EQ(server_->list_calls.load(), 0);
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

// At launch Configure runs again once unlock applies the Brief key, so a chip can be sent mid-configure. That
// send must run once the configure is done and leave the session free for the next one; a feed turn that
// re-lists takes configure_work_mutex itself, so it must not be started while ConfigureOnIO still holds it.
TEST_F(AgentSessionFeedRelistTest, SubmitDuringAConfigureRunsOnceItIsDone) {
  StartWithPromotedMcp(true, -1);
  auto in_configure = std::make_shared<std::atomic<bool>>(false);
  auto release = std::make_shared<std::atomic<bool>>(false);
  session_->SetToolRegistrationHook([this, in_configure, release](ToolRegistry&) {
    // ConfigureOnIO installs the real HTTP post before this hook runs: put the fake server back, now listing.
    server_->failing_lists = 0;
    McpClient::SetHttpPost(FakeMcpPost(server_));
    *in_configure = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!*release && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  AppConfig config = MakeConfig("brief");
  config.promoted_mcp.id = "brief";
  config.promoted_mcp.enabled = true;
  session_->Configure(config);
  const auto started = std::chrono::steady_clock::now();
  // Readiness wait, not a timing assertion: the first configure dials a closed port for real, which a
  // Windows CI runner can take several seconds to refuse.
  while (!*in_configure && std::chrono::steady_clock::now() - started < std::chrono::seconds(30)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(*in_configure);

  SubmitArticles();
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the submit reaches a worker mid-configure
  *release = true;
  const auto events = WaitForTurn();
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u); // the queued feed turn ran, re-listed the fake server and showed the feed
  EXPECT_EQ(ready[0].response_goal, ResponseGoal::DisplayFeed);
  EXPECT_GT(server_->list_calls.load(), 0); // the fake server, not the real transport, answered

  session_->SubmitToThread("t1", "What is in the news today?"); // not dropped as "busy"
  const auto next = WaitForTurn();
  EXPECT_EQ(Of(next, AgentEventType::AssistantReady).size(), 1u);
}

// The server is up but answers tools/list with an error status (a deploy's 503): it answered, so waiting longer
// cannot help — fail at once instead of polling for the whole wait.
TEST_F(AgentSessionFeedRelistTest, ServerErrorStatusFailsAtOnce) {
  StartWithPromotedMcp(true, 0, std::chrono::milliseconds(5000));
  server_->list_status = 503;
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  const auto events = WaitForTurn();

  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(1000));
  EXPECT_EQ(server_->list_calls.load(), 2); // the turn's connect, then one re-list
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

// Stop while tools/list is in flight: the listing that comes back afterwards is not written into the registry
// (the next turn may already be reading it), so the next feed turn still finds the tool missing.
TEST_F(AgentSessionFeedRelistTest, CancelDuringTheListingRegistersNothing) {
  // The connect lists another tool (call 0); the re-list (call 1) is held, then lists the feed tool.
  StartWithPromotedMcp(true, 0, std::chrono::milliseconds(300));
  server_->first_list_tool = "other_tool";
  server_->hold_from = 1;
  server_->hold_list = true;
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  while (server_->list_calls.load() < 2 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(server_->list_calls.load(), 2);
  session_->Cancel();
  server_->hold_list = false; // the listing (with the tool) now returns to a stopped turn
  const auto stopped = WaitForTurn();
  EXPECT_TRUE(Of(stopped, AgentEventType::AssistantReady).empty());

  server_->fail_from = 2; // from here on tools/list gets no answer (the kept connection is reused)
  SubmitArticles();
  const auto events = WaitForTurn();
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

// A Configure that fails while a question waits for it ends that question (error + loading off) instead of
// leaving the session busy: the next question, after a good Configure, runs.
TEST_F(AgentSessionBriefStreamTest, SubmitDuringAFailingConfigureEndsTheTurn) {
  Start();
  Script({Meta(), Token("ok"), Done("ok")}, BriefAiOutcome::Done);
  auto in_configure = std::make_shared<std::atomic<bool>>(false);
  auto release = std::make_shared<std::atomic<bool>>(false);
  auto fail = std::make_shared<std::atomic<bool>>(true);
  session_->SetToolRegistrationHook([in_configure, release, fail](ToolRegistry&) {
    if (!*fail) {
      return;
    }
    *in_configure = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!*release && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("registry broke");
  });
  session_->Configure(MakeConfig("brief"));
  const auto started = std::chrono::steady_clock::now();
  // Readiness wait, not a timing assertion: the first configure dials a closed port for real, which a
  // Windows CI runner can take several seconds to refuse.
  while (!*in_configure && std::chrono::steady_clock::now() - started < std::chrono::seconds(30)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(*in_configure);
  session_->Submit("hi there");
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the submit is queued behind the configure
  *release = true;
  const auto failed = WaitForTurn();
  const auto errors = Of(failed, AgentEventType::Error);
  ASSERT_FALSE(errors.empty()); // the configure's own error, and the queued question's
  for (const AgentEvent& error : errors) {
    EXPECT_NE(error.message.find("Agent configure failed"), std::string::npos) << error.message;
  }
  EXPECT_TRUE(Of(failed, AgentEventType::AssistantReady).empty());

  *fail = false;
  session_->Configure(MakeConfig("brief"));
  session_->WaitForConfigureIdle();
  ASSERT_TRUE(session_->IsConfigured());
  session_->Submit("hi again"); // not dropped as "busy"
  const auto next = WaitForTurn();
  ASSERT_EQ(Of(next, AgentEventType::AssistantReady).size(), 1u);
  EXPECT_EQ(Requests().back().message, "hi again");
}

TEST_F(AgentSessionPayloadTurnTest, FindSomeoneChipListsThePeopleWithoutAnyModelCall) {
  StartWithTools(true);
  session_->SubmitToThread("t1", "Find someone on the network", std::string(R"({"tool":"search_people","query":""})"));
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].render_mode, RenderMode::PeopleList);
  EXPECT_FALSE(ready[0].from_model); // host-built blocks, the only text that may keep its button payloads
  const auto calls = Calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_NE(calls[0].find(R"("query":"")"), std::string::npos);
}

// PB-007: provenance is carried by the event, never guessed from the text. A reply that is a bare blocks document
// is still model output unless the host built it, so the default must be "model".
TEST(AgentEventTest, ReplyIsModelOutputUnlessTheHostBuiltIt) {
  AgentEvent event{.type = AgentEventType::AssistantReady, .text = R"({"blocks":[]})"};
  EXPECT_TRUE(event.from_model);
}

// --- Typed sentences judged on the device (AppActionClassifier) ---
// A hit is answered on the device without the stream or the completions route (add_contact has no tool of its
// own: "cannot do that yet"); a miss, or a turn with an image, still streams.

class AgentSessionLocalAppActionTest : public AgentSessionBriefStreamTest {
protected:
  void StartWithMessagingTool() {
    session_ = std::make_unique<AgentSession>();
    session_->SetToolRegistrationHook([](ToolRegistry& registry) {
      registry.Register(MakeTool(ToolDefinition{"add_contact", "add a contact", Object{}},
                                 ToolMeta{.provider = "messaging"},
                                 [](const Object&) -> Roe<std::string> { return std::string("{}"); }));
    });
    session_->Configure(MakeConfig("brief"));
    session_->WaitForConfigureIdle();
    ASSERT_TRUE(session_->IsConfigured());
    session_->SetThreadStore(&store_);
    Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  }

  MemoryThreadStore store_;
};

TEST_F(AgentSessionLocalAppActionTest, HitIsAnsweredOnTheDeviceWithoutStreaming) {
  StartWithMessagingTool();
  session_->SubmitToThread("t1", "Add Tom as a friend");
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_TRUE(LlmRequests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::AssistantDelta).empty());
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_FALSE(ready[0].from_model);
  EXPECT_FALSE(events.back().loading);
}

TEST_F(AgentSessionLocalAppActionTest, MissStillStreams) {
  StartWithMessagingTool();
  session_->SubmitToThread("t1", "What is in the news today?");
  const auto events = WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].capabilities, std::vector<std::string>{"add_contact"}); // the server fallback keeps its list
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
}

TEST_F(AgentSessionLocalAppActionTest, HitWithAnImageStillStreams) {
  StartWithMessagingTool();
  session_->SubmitToThread("t1", "Add Tom as a friend", std::nullopt, TestImage());
  const auto events = WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_TRUE(requests[0].image.has_value());
  EXPECT_TRUE(requests[0].capabilities.empty());
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
}

// The ask-AI panel's one-off (settings "?") explains in words: the same sentence is not run as an app action, nothing
// is offered to brief_AI to hand back, and no permission card is left waiting.
TEST_F(AgentSessionLocalAppActionTest, AnExplanationIsNeverAnAppAction) {
  StartWithMessagingTool();
  ASSERT_EQ(session_->SubmitExplanation("加张三为好友"), SubmitOutcome::Started);
  const auto events = WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u) << "answered by brief_AI, not run on the device";
  EXPECT_TRUE(requests[0].capabilities.empty()) << "nothing offered to hand back";
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
  EXPECT_TRUE(session_->CanClose()) << "no permission card or question asked back left behind";

  // The next ordinary Home question is an app action again.
  session_->Submit("加张三为好友");
  (void)WaitForTurn();
  EXPECT_EQ(Requests().size(), 1u);
}

TEST_F(AgentSessionLocalAppActionTest, HitInTheHomeComposerAlsoSkipsTheStream) {
  StartWithMessagingTool();
  session_->Submit("加张三为好友");
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_TRUE(LlmRequests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
}

// --- HTTP 401: the gateway no longer knows the Bearer (B183) ---

class AgentSessionBriefKeyRenewTest : public AgentSessionBriefStreamTest {
protected:
  // The fake stream answers with `outcomes` in order (the last one repeats) and counts its calls.
  void ScriptOutcomes(std::vector<Roe<BriefAiOutcome>> outcomes) {
    session_->SetBriefAiStream(
        [this, outcomes = std::move(outcomes)](const BriefAiRequest& request,
                                               const std::function<void(const BriefAiEvent&)>& on_event,
                                               const std::atomic<bool>&) -> Roe<BriefAiOutcome> {
          Record(request);
          const size_t call = stream_calls_.fetch_add(1);
          const Roe<BriefAiOutcome>& outcome = outcomes[std::min(call, outcomes.size() - 1)];
          if (outcome && *outcome == BriefAiOutcome::Done) {
            on_event(Token("ok"));
            on_event(Done("ok"));
          }
          return outcome;
        });
  }

  // The key ports: `renew` answers `key` (or an error when empty), `stored_key` answers stored_ (under mu_); each call
  // is counted, and the refused key and base URL kept.
  void Renewer(const std::string& key) {
    BriefKeyPorts ports;
    ports.renew = [this, key](const std::string& rejected, const std::string& base_url,
                              const std::atomic<bool>&) -> Roe<BriefKeyRenewal> {
      ++renew_calls_;
      {
        std::lock_guard lock(mu_);
        rejected_ = rejected;
        base_url_ = base_url;
      }
      if (key.empty()) {
        return AppError::Network(Err::Network::HttpError, "Guest LLM mint failed (HTTP 503)");
      }
      return BriefKeyRenewal{.key = key};
    };
    ports.commit = [this](const std::string& rejected) {
      ++commit_calls_;
      std::lock_guard lock(mu_);
      committed_ = rejected;
    };
    ports.stored_key = [this]() {
      std::lock_guard lock(mu_);
      return stored_;
    };
    session_->SetBriefKeyPorts(std::move(ports));
  }

  void SetStored(const std::string& key) {
    std::lock_guard lock(mu_);
    stored_ = key;
  }

  static Roe<BriefAiOutcome> Http(const long status) {
    return Roe<BriefAiOutcome>(LlmClient::MapHttpError(status, R"({"error":{"message":"Invalid Brief LLM API key"}})"));
  }

  std::atomic<size_t> stream_calls_{0};
  std::atomic<int> renew_calls_{0};
  std::atomic<int> commit_calls_{0};
  std::string rejected_;
  std::string base_url_;
  std::string committed_;
  std::string stored_ = "dummy-key"; // what the identity holds: the key the session sends
};

TEST_F(AgentSessionBriefKeyRenewTest, A401RenewsTheKeyOnceAndAsksTheSameQuestionAgain) {
  Start();
  ScriptOutcomes({Http(401), BriefAiOutcome::Done});
  Renewer("brf_guest_new");
  session_->Submit("hi there");
  const auto events = WaitForTurn();

  EXPECT_EQ(renew_calls_.load(), 1);
  EXPECT_EQ(commit_calls_.load(), 1); // the retry was answered: the refused key goes now
  {
    std::lock_guard lock(mu_);
    EXPECT_EQ(rejected_, "dummy-key"); // the Bearer the session was configured with
    EXPECT_EQ(committed_, "dummy-key");
    EXPECT_EQ(base_url_, "http://127.0.0.1:1"); // the session's own gateway, not the compiled default
  }
  ASSERT_EQ(stream_calls_.load(), 2u);
  const auto requests = Requests();
  EXPECT_EQ(requests[0].message, "hi there");
  EXPECT_EQ(requests[1].message, "hi there");
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].text, "ok");
}

TEST_F(AgentSessionBriefKeyRenewTest, A401AfterTheRenewalEndsInTheErrorWithoutAnotherRenewal) {
  Start();
  ScriptOutcomes({Http(401)});
  Renewer("brf_guest_new");
  session_->Submit("q");
  auto events = WaitForTurn();
  EXPECT_EQ(renew_calls_.load(), 1);
  EXPECT_EQ(stream_calls_.load(), 2u); // the question, asked once more with the new key
  auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_FALSE(errors[0].retryable);
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  EXPECT_EQ(commit_calls_.load(), 0); // the refused key (a registered one) is kept

  // No turn answered since and the stored key is still the refused one: the next 401 is the error at once.
  session_->Submit("q2");
  events = WaitForTurn();
  EXPECT_EQ(renew_calls_.load(), 1);
  EXPECT_EQ(stream_calls_.load(), 3u);
  EXPECT_EQ(Of(events, AgentEventType::Error).size(), 1u);
}

// Stop during a 401 renewal (the turn waited for the unlock's mint, or minted): the renewal did not finish, so the
// next turn's 401 renews again instead of "already renewed since the last answer".
TEST_F(AgentSessionBriefKeyRenewTest, AStopDuringTheRenewalLeavesTheNextTurnFreeToRenew) {
  Start();
  ScriptOutcomes({Http(401), Http(401), BriefAiOutcome::Done});
  std::atomic<bool> in_renew{false};
  std::atomic<bool> first_returned{false};
  BriefKeyPorts ports;
  ports.renew = [&](const std::string&, const std::string&,
                    const std::atomic<bool>& cancelled) -> Roe<BriefKeyRenewal> {
    if (++renew_calls_ == 1) {
      in_renew = true;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      first_returned = true;
      return AppError::Network(Err::Network::HttpError, "Brief key renewal stopped"); // as the app's wait gives up
    }
    return BriefKeyRenewal{.key = "brf_guest_new"};
  };
  ports.commit = [this](const std::string&) { ++commit_calls_; };
  ports.stored_key = [this]() {
    std::lock_guard lock(mu_);
    return stored_;
  };
  session_->SetBriefKeyPorts(std::move(ports));

  session_->Submit("q");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!in_renew && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(in_renew.load());
  session_->Cancel(); // Stop while the renewal waits
  while (!first_returned && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(first_returned.load());
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the stopped turn runs to its end
  (void)WaitForTurn(std::chrono::milliseconds(1000));

  session_->Submit("q2");
  const auto events = WaitForTurn();
  EXPECT_EQ(renew_calls_.load(), 2) << "the stopped renewal did not count as one";
  EXPECT_EQ(stream_calls_.load(), 3u); // q (401), q2 (401), q2 again with the renewed key
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
  EXPECT_EQ(commit_calls_.load(), 1);
}

TEST_F(AgentSessionBriefKeyRenewTest, OtherStatusesNeverRenew) {
  Start();
  ScriptOutcomes({Http(500)});
  Renewer("brf_guest_new");
  session_->Submit("q");
  auto events = WaitForTurn();
  EXPECT_EQ(Of(events, AgentEventType::Error).size(), 1u);

  ScriptOutcomes({Http(403)});
  session_->Submit("q2");
  events = WaitForTurn();
  EXPECT_EQ(Of(events, AgentEventType::Error).size(), 1u);
  EXPECT_EQ(renew_calls_.load(), 0);
  EXPECT_EQ(stream_calls_.load(), 2u);
}

TEST_F(AgentSessionBriefKeyRenewTest, AFailedMintEndsInTheErrorAndIsNotTriedAgain) {
  Start();
  ScriptOutcomes({Http(401)});
  Renewer(""); // the mint fails
  session_->Submit("q");
  auto events = WaitForTurn();
  EXPECT_EQ(renew_calls_.load(), 1);
  EXPECT_EQ(stream_calls_.load(), 1u); // not asked again without a new key
  auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_TRUE(errors[0].retryable); // a network failure of the mint
  EXPECT_EQ(commit_calls_.load(), 0);

  session_->Submit("q2");
  events = WaitForTurn();
  EXPECT_EQ(renew_calls_.load(), 1);
  EXPECT_EQ(stream_calls_.load(), 2u);
  EXPECT_EQ(Of(events, AgentEventType::Error).size(), 1u);
}

TEST_F(AgentSessionBriefKeyRenewTest, AnAnsweredTurnAllowsTheNextRenewal) {
  Start();
  ScriptOutcomes({Http(401), BriefAiOutcome::Done, Http(401), BriefAiOutcome::Done});
  Renewer("brf_guest_new");
  session_->Submit("q");
  auto events = WaitForTurn();
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);

  session_->Submit("q2");
  events = WaitForTurn();
  EXPECT_EQ(renew_calls_.load(), 2);
  EXPECT_EQ(commit_calls_.load(), 2);
  EXPECT_EQ(stream_calls_.load(), 4u);
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
}

TEST_F(AgentSessionBriefKeyRenewTest, AfterARenewalAKeyStoredMeanwhileIsTriedWithoutAMint) {
  Start();
  ScriptOutcomes({Http(401), Http(401), Http(401), BriefAiOutcome::Done});
  Renewer("brf_guest_new");
  session_->Submit("q");
  (void)WaitForTurn(); // 401, renewed, 401 again: the guard is set
  ASSERT_EQ(renew_calls_.load(), 1);

  // Another turn stored a key this session was not configured with yet: asked once with it, no mint.
  SetStored("brf_guest_other");
  session_->Submit("q2");
  const auto events = WaitForTurn();
  EXPECT_EQ(renew_calls_.load(), 1);
  EXPECT_EQ(stream_calls_.load(), 4u);
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
  EXPECT_EQ(commit_calls_.load(), 1);
}

TEST_F(AgentSessionBriefKeyRenewTest, WithoutARenewerA401IsTheTurnsError) {
  Start();
  ScriptOutcomes({Http(401), BriefAiOutcome::Done});
  session_->Submit("q");
  const auto events = WaitForTurn();
  EXPECT_EQ(stream_calls_.load(), 1u);
  EXPECT_EQ(Of(events, AgentEventType::Error).size(), 1u);
}

} // namespace
