#include "feature/ai/AgentSession.h"

#include "lib/ai/conversation/Conversation.h"
#include "foundation/data/Config.h"
#include "foundation/error/AppError.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/thread/IThreadStore.h"
#include "lib/ai/ToolRegistry.h"
#include "lib/ai/mcp/McpClient.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "common/PbrCompat.h"

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

// Keeps messages in memory; everything the AI thread path does not touch is a stub.
class MemoryThreadStore : public IThreadStore {
public:
  std::vector<ThreadMessage> messages;

  void Flush() override {}
  Roe<std::vector<Thread>> ListThreads() const override { return std::vector<Thread>{}; }
  Roe<std::optional<Thread>> GetThread(const std::string&) const override { return std::optional<Thread>{}; }
  Roe<Thread> UpsertThread(const Thread& thread) override { return thread; }
  Roe<bool> DeleteThread(const std::string&) override { return true; }
  Roe<std::optional<Thread>> FindDirectThread(const DirectChatTarget&) const override { return std::optional<Thread>{}; }
  Roe<Thread> FindOrCreateDirectThread(const DirectChatTarget&, const std::string&, const std::string&) override { return Thread{}; }
  Roe<std::optional<Thread>> FindGroupThread(const std::string&) const override { return std::optional<Thread>{}; }
  Roe<Thread> FindOrCreateGroupThread(const std::string&, const std::string&, const std::vector<std::string>&) override {
    return Thread{};
  }
  Roe<std::vector<ThreadMessage>> GetMessages(const std::string&) const override { return messages; }
  Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string&, std::optional<int64_t>, size_t) const override {
    return messages;
  }
  Roe<std::vector<ThreadMessage>> GetMessagesForContext(const std::string&, const ContextBudget&) const override {
    return messages;
  }
  Roe<int64_t> CountContextEligibleMessagesAfter(const std::string&, int64_t) const override { return int64_t{0}; }
  Roe<int64_t> CountAnnotationsForTarget(const std::string&, const std::string&) const override { return int64_t{0}; }
  Roe<std::vector<ThreadMessage>> GetContextEligibleMessagesAfter(const std::string&, int64_t) const override {
    return std::vector<ThreadMessage>{};
  }
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) override {
    ThreadMessage stored = message;
    stored.display_order = static_cast<int64_t>(messages.size()) + 1;
    messages.push_back(stored);
    return stored;
  }
  Roe<bool> UpdateMessage(const ThreadMessage&) override { return true; }
  Roe<bool> HasMessageId(const std::string&, const std::string&) const override { return false; }
  Roe<void> ClearMessages(const std::string&, const ClearMessagesOptions&) override { return {}; }
  Roe<std::vector<ThreadMessage>> ExportMessagesUpTo(const std::string&, const std::optional<std::string>&) const override {
    return messages;
  }
  Roe<std::optional<ConversationSummary>> GetThreadMemory(const std::string&) const override {
    return std::optional<ConversationSummary>{};
  }
  Roe<void> SetThreadMemory(const std::string&, const ConversationSummary&) override { return {}; }
  Roe<void> ClearThreadMemory(const std::string&) override { return {}; }
  Roe<uint64_t> AllocateSenderSeq(const std::string&) override { return uint64_t{0}; }
  Roe<uint32_t> GetChatTargetSessionEpoch(const std::string&) const override { return uint32_t{0}; }
  Roe<std::vector<ThreadMessage>> GetMessagesBySeqRange(const std::string&, const SeqRangeQuery&) const override {
    return std::vector<ThreadMessage>{};
  }
  Roe<PeerSyncState> GetPeerSyncState(const std::string&, uint32_t, const std::string&) const override {
    return PeerSyncState{};
  }
  Roe<void> SetPeerSyncState(const std::string&, uint32_t, const PeerSyncState&, const std::string&) override {
    return {};
  }
  Roe<void> CancelOldEpochPending(const std::string&, uint32_t) override { return {}; }
  Roe<void> AdoptChatTargetEpoch(const std::string&, uint32_t) override { return {}; }
  Roe<ThreadMessage> AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message, uint32_t, uint32_t,
                                                        const PeerSyncState&) override {
    return message;
  }
  Roe<uint32_t> BumpLocalChatTargetEpoch(const std::string&) override { return uint32_t{0}; }
  Roe<void> ReconcileOutbox() override { return {}; }
  Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const override {
    return std::vector<std::pair<std::string, std::string>>{};
  }
};

AgentImageTurn TestImage(const std::string& message_id = {}) {
  return AgentImageTurn{.image = BriefAiImage{.mime = "image/jpeg", .data = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3}},
                        .message_id = message_id,
                        .ai_image_json = message_id.empty() ? std::string() : R"({"content_hash":"ab"})"};
}

class AgentSessionBriefStreamTest : public ::testing::Test {
protected:
  void SetUp() override { AppRuntime::Initialize(); }
  void TearDown() override {
    session_.reset();
    AppRuntime::Shutdown();
  }

  void Start(const std::string& preset = "brief") { StartWith(MakeConfig(preset)); }

  void StartWith(const AppConfig& config) {
    session_ = std::make_unique<AgentSession>();
    session_->Configure(config);
    session_->WaitForConfigureIdle(); // Configure() counts the run before posting it, so this is enough
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
  EXPECT_TRUE(Requests().empty()); // the fake stream was never called
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
  EXPECT_EQ(store.messages[0].id, "msg-1"); // the caller's id, so the GUI can pair its thumbnail
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
  session_->SetBriefAiStream([this](const BriefAiRequest& request, const std::function<void(const BriefAiEvent&)>& on_event,
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

  EXPECT_TRUE(Requests().empty()); // no stream
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty()); // no completions call (it would fail on the closed port)
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

// --- The promoted MCP server was unreachable when the session was configured (the phone's network not up yet
// at launch): a feed turn re-lists its tools for a bounded time instead of failing at once.

struct FakeMcpServer {
  std::atomic<int> list_calls{0};
  std::atomic<int> failing_lists{0}; // tools/list fails this many times, then answers; < 0: always fails
  std::atomic<long> last_list_timeout_s{-1};
  std::string tool_name = "blog_articles"; // what tools/list offers
  std::atomic<int> list_status{200};        // non-200: the server is up but answers tools/list with this status
  std::atomic<bool> hold_list{false};       // tools/list waits (up to 2 s) until this is cleared
};

HttpPostFn FakeMcpPost(std::shared_ptr<FakeMcpServer> server) {
  return [server](const std::string&, const std::string& body, const std::map<std::string, std::string>&,
                  const long timeout_s) -> Roe<HttpResponse> {
    if (body.find("\"tools/list\"") != std::string::npos) {
      server->last_list_timeout_s = timeout_s;
      const int call = server->list_calls.fetch_add(1);
      const auto held = std::chrono::steady_clock::now();
      while (server->hold_list && std::chrono::steady_clock::now() - held < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (server->list_status != 200) {
        return HttpResponse{.status_code = server->list_status, .body = "Service Unavailable"};
      }
      if (server->failing_lists < 0 || call < server->failing_lists) {
        return Error("connection refused");
      }
      return HttpResponse{
          .status_code = 200,
          .body = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":")" + server->tool_name +
                  R"(","description":"articles","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}}]}})"};
    }
    if (body.find("\"tools/call\"") != std::string::npos) {
      return HttpResponse{
          .status_code = 200,
          .body = R"({"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":)"
                  R"("{\"articles\":[{\"id\":\"a1\",\"title\":\"First\",\"content\":\"First brief.\",)"
                  R"(\"link_to\":\"https://apnews.com/x\",\"created_at\":1759000000}]}"}]}})"};
    }
    return HttpResponse{.status_code = 200, .body = R"({"jsonrpc":"2.0","id":0,"result":{}})"};
  };
}

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
    AgentSessionBriefStreamTest::TearDown();
    McpClient::SetHttpPost(nullptr);
  }

  void SubmitArticles() {
    session_->SubmitToThread("t1", "Show me recent articles",
                             std::string(R"({"tool":"blog_articles","brf_domain":"en","brf_language":"en","size":2})"));
  }

  MemoryThreadStore store_;
  std::shared_ptr<FakeMcpServer> server_ = std::make_shared<FakeMcpServer>();
};

TEST_F(AgentSessionFeedRelistTest, FeedToolListedShortlyAfterLaunchStillShowsTheFeed) {
  StartWithPromotedMcp(true, 2);
  SubmitArticles();
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].response_goal, ResponseGoal::DisplayFeed);
  EXPECT_NE(ready[0].text.find("apnews.com"), std::string::npos);
  EXPECT_EQ(server_->list_calls.load(), 3);
  EXPECT_EQ(server_->last_list_timeout_s.load(), 3); // a short bound, not the transport's 30 s
}

// The server answers but does not offer the tool: no point in polling it for the rest of the wait.
TEST_F(AgentSessionFeedRelistTest, ServerListingWithoutTheToolFailsAtOnce) {
  server_->tool_name = "other_tool";
  StartWithPromotedMcp(true, 0, std::chrono::milliseconds(5000));
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  const auto events = WaitForTurn();

  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(1000));
  EXPECT_EQ(server_->list_calls.load(), 1);
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

// Stop during the wait ends the turn quietly: no error, and the wait stops.
TEST_F(AgentSessionFeedRelistTest, CancelDuringTheWaitEndsTheTurnWithoutAnError) {
  StartWithPromotedMcp(true, -1, std::chrono::milliseconds(5000));
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  while (server_->list_calls.load() < 1 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GE(server_->list_calls.load(), 1);
  session_->Cancel();
  const auto events = WaitForTurn(std::chrono::milliseconds(1000));

  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const int calls = server_->list_calls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(server_->list_calls.load(), calls);
}

// Stop and a new question right away: the stopped wait must not end (or fail) the new turn.
TEST_F(AgentSessionFeedRelistTest, CancelThenNewTurnLeavesTheNewTurnAlone) {
  StartWithPromotedMcp(true, -1, std::chrono::milliseconds(5000));
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  while (server_->list_calls.load() < 1 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GE(server_->list_calls.load(), 1);
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

TEST_F(AgentSessionFeedRelistTest, FeedToolNeverListedEndsWithUnavailableAfterTheWait) {
  StartWithPromotedMcp(true, -1);
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  const auto events = WaitForTurn();

  EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(300)); // it waited (last poll may be skipped)
  EXPECT_GE(server_->list_calls.load(), 2);
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
  EXPECT_EQ(server_->list_calls.load(), 1);
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].message, "Briefs are unavailable right now.");
}

// Stop while tools/list is in flight: the listing that comes back afterwards is not written into the registry
// (the next turn may already be reading it), so the next feed turn still finds the tool missing.
TEST_F(AgentSessionFeedRelistTest, CancelDuringTheListingRegistersNothing) {
  StartWithPromotedMcp(true, 0, std::chrono::milliseconds(300));
  server_->hold_list = true;
  const auto started = std::chrono::steady_clock::now();
  SubmitArticles();
  while (server_->list_calls.load() < 1 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(server_->list_calls.load(), 1);
  session_->Cancel();
  server_->hold_list = false; // the listing (with the tool) now returns to a stopped turn
  const auto stopped = WaitForTurn();
  EXPECT_TRUE(Of(stopped, AgentEventType::AssistantReady).empty());

  server_->failing_lists = -1; // from here on the server is unreachable
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
// A hit goes straight to the local pipeline (its completions endpoint is a closed port, so it ends with a
// non-retryable Error); a miss, or a turn with an image, still streams.

class AgentSessionLocalAppActionTest : public AgentSessionBriefStreamTest {
protected:
  void StartWithMessagingTool() {
    session_ = std::make_unique<AgentSession>();
    session_->SetToolRegistrationHook([](ToolRegistry& registry) {
      registry.Register(MakeTool(ToolDefinition{"add_contact", "add a contact", Object{}}, ToolMeta{.provider = "messaging"},
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

TEST_F(AgentSessionLocalAppActionTest, HitRunsTheLocalPipelineWithoutStreaming) {
  StartWithMessagingTool();
  session_->SubmitToThread("t1", "Add Tom as a friend");
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_TRUE(Of(events, AgentEventType::AssistantDelta).empty());
  const auto errors = Of(events, AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u); // the local pipeline ran and failed on the closed port
  EXPECT_FALSE(errors[0].retryable);
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

TEST_F(AgentSessionLocalAppActionTest, HitInTheHomeComposerAlsoSkipsTheStream) {
  StartWithMessagingTool();
  session_->Submit("加张三为好友");
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  EXPECT_EQ(Of(events, AgentEventType::Error).size(), 1u);
}

} // namespace
