// B210: no AI session connects to the MCP server when it is configured (launch, unlock, a key change, opening a chat);
// the first turn that needs MCP tools connects once for every session of an AgentSessions, later turns reuse that
// connection, a failed one is tried again by the next turn, and a new configuration connects again.

#include "feature/ai/AgentSessions.h"
#include "feature/ai/tests/agent_session_test_support.h"

#include <map>
#include <set>

namespace {

using namespace pbr;
using namespace pbr::agent_test;

/** MemoryThreadStore with a lock: several sessions write to it at once here. */
class LockedThreadStore : public MemoryThreadStore {
public:
  Roe<std::vector<ThreadMessage>> GetMessagesForContext(const std::string& thread_id,
                                                        const ContextBudget& budget) const override {
    std::lock_guard lock(mu_);
    return MemoryThreadStore::GetMessagesForContext(thread_id, budget);
  }
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) override {
    std::lock_guard lock(mu_);
    return MemoryThreadStore::AppendMessage(message);
  }
  Roe<bool> UpdateMessage(const ThreadMessage& message) override {
    std::lock_guard lock(mu_);
    return MemoryThreadStore::UpdateMessage(message);
  }
  bool HasText(const std::string& text) const {
    std::lock_guard lock(mu_);
    return std::any_of(messages.begin(), messages.end(),
                       [&text](const ThreadMessage& message) { return message.text == text; });
  }

private:
  mutable std::mutex mu_;
};

/** Waits (up to 2 s) until `n` callers wait in SharedMcpTools::Acquire for a connect. */
void WaitForConnectWaiters(const size_t n) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (SharedMcpTools::WaitersForTests() < n && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_GE(SharedMcpTools::WaitersForTests(), n);
}

constexpr const char* kFeedPayload = R"({"tool":"blog_articles","brf_domain":"en","brf_language":"en","size":2})";

class McpConnectOnFirstUseTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(AppRuntimeConfig{});
    McpClient::SetHttpPost(FakeMcpPost(server_)); // before any configure: the sessions keep it (SetHttpPostIfUnset)
    LlmClient::SetRequestObserverForTests([this](const std::string& body) {
      std::lock_guard lock(mu_);
      llm_bodies_.push_back(body);
    });
  }
  void TearDown() override {
    server_->hold_list = false;
    if (sessions_) {
      sessions_->CancelAll();
      sessions_->WaitForConfigureIdle();
    }
    sessions_.reset();
    WaitForMcpConnects();
    AppRuntime::Shutdown();
    McpClient::SetHttpPost(nullptr);
    LlmClient::SetRequestObserverForTests(nullptr);
  }

  static void WaitForWaiters(const size_t n) { WaitForConnectWaiters(n); }

  /** The brief preset with the promoted MCP server on (the fake answers it). */
  static AppConfig McpConfig(const std::string& preset = "brief") {
    AppConfig config = MakeConfig(preset);
    config.llm.api_key = "brf_llm_test"; // a registered key: three thread turns at once
    config.promoted_mcp.id = "brief";
    config.promoted_mcp.enabled = true;
    return config;
  }

  void Make(const AppConfig& config = McpConfig()) {
    sessions_ = std::make_unique<AgentSessions>();
    sessions_->SetThreadStore(&store_);
    sessions_->SetBriefAiStream([](const BriefAiRequest&, const std::function<void(const BriefAiEvent&)>& on_event,
                                   const std::atomic<bool>&) -> Roe<BriefAiOutcome> {
      on_event(Meta());
      on_event(Token("x"));
      on_event(Done("x"));
      return BriefAiOutcome::Done;
    });
    sessions_->Configure(config);
    sessions_->WaitForConfigureIdle();
    ASSERT_TRUE(sessions_->IsConfigured());
  }

  /** Polls until every thread in `threads` ended its turn (LoadingChanged false); events by thread. */
  std::map<std::string, std::vector<AgentEvent>> WaitForTurns(const std::set<std::string>& threads) {
    std::map<std::string, std::vector<AgentEvent>> by_thread;
    std::set<std::string> ended;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ended.size() < threads.size() && std::chrono::steady_clock::now() < deadline) {
      AppRuntime::RunUITasks();
      std::vector<AgentEvent> batch;
      sessions_->PollEvents(batch);
      for (AgentEvent& event : batch) {
        if (event.type == AgentEventType::LoadingChanged && !event.loading) {
          ended.insert(event.thread_id);
        }
        by_thread[event.thread_id].push_back(std::move(event));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(ended.size(), threads.size()) << "a turn did not finish within the bound";
    return by_thread;
  }

  std::vector<AgentEvent> AskFeed(const std::string& thread_id) {
    EXPECT_EQ(sessions_->SubmitToThread(thread_id, "Show me recent articles", std::string(kFeedPayload)),
              SubmitOutcome::Started);
    return WaitForTurns({thread_id})[thread_id];
  }

  static size_t Count(const std::vector<AgentEvent>& events, const AgentEventType type) {
    return static_cast<size_t>(
        std::count_if(events.begin(), events.end(), [type](const AgentEvent& event) { return event.type == type; }));
  }

  static bool ShowedTheFeed(const std::vector<AgentEvent>& events) {
    for (const AgentEvent& event : events) {
      if (event.type == AgentEventType::AssistantReady && event.response_goal == ResponseGoal::DisplayFeed &&
          event.text.find("apnews.com") != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  LockedThreadStore store_;
  std::shared_ptr<FakeMcpServer> server_ = std::make_shared<FakeMcpServer>();
  std::unique_ptr<AgentSessions> sessions_;
  std::mutex mu_;
  std::vector<std::string> llm_bodies_;
};

// Launch, then unlock applying the key (a second configure), then a new configuration: Home and the spare are
// configured each time, and nothing reaches the MCP server.
TEST_F(McpConnectOnFirstUseTest, ConfiguringConnectsNothing) {
  Make();
  AppConfig keyed = McpConfig();
  keyed.llm.api_key = "brf_llm_other";
  sessions_->Configure(keyed);
  sessions_->WaitForConfigureIdle();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(sessions_->IsConfigured());
  EXPECT_TRUE(sessions_->HasSpare());
  EXPECT_EQ(server_->init_calls.load(), 0);
  EXPECT_EQ(server_->list_calls.load(), 0);
}

// Streamed brief questions in two chats (switching between them, each opening its session), "@ai" in a person chat,
// a Home explanation and an app-action button: none needs MCP tools, so none connects.
TEST_F(McpConnectOnFirstUseTest, QuestionsWithoutMcpToolsConnectNothing) {
  Make();
  ASSERT_EQ(sessions_->SubmitToThread("t1", "hello"), SubmitOutcome::Started);
  ASSERT_EQ(sessions_->SubmitToThread("t2", "hello again"), SubmitOutcome::Started);
  ASSERT_EQ(sessions_->SubmitScopedAssist("peer", "what is a VPN?"), SubmitOutcome::Started);
  auto events = WaitForTurns({"t1", "t2", "peer"});
  EXPECT_EQ(Count(events["t1"], AgentEventType::AssistantReady), 1u);
  EXPECT_EQ(Count(events["t2"], AgentEventType::AssistantReady), 1u);
  ASSERT_EQ(sessions_->SubmitHomeExplanation("what does this setting do?"), SubmitOutcome::Started);
  WaitForTurns({""});
  ASSERT_EQ(sessions_->SubmitLocalAction("t1", "Search", "search_people", Object{}), SubmitOutcome::Started);
  WaitForTurns({"t1"});

  EXPECT_EQ(server_->init_calls.load(), 0);
  EXPECT_EQ(server_->list_calls.load(), 0);
}

TEST_F(McpConnectOnFirstUseTest, FirstFeedQuestionConnectsOnceAndTheNextReusesIt) {
  Make();
  const auto first = AskFeed("t1");
  EXPECT_TRUE(ShowedTheFeed(first));
  EXPECT_EQ(Count(first, AgentEventType::Error), 0u);
  EXPECT_EQ(server_->init_calls.load(), 1);
  EXPECT_EQ(server_->list_calls.load(), 1);

  const auto second = AskFeed("t1");
  EXPECT_TRUE(ShowedTheFeed(second));
  EXPECT_EQ(server_->init_calls.load(), 1);
  EXPECT_EQ(server_->list_calls.load(), 1);
}

// Another chat runs in another session (the spare): it attaches the same connection.
TEST_F(McpConnectOnFirstUseTest, TwoSessionsShareOneConnection) {
  Make();
  EXPECT_TRUE(ShowedTheFeed(AskFeed("t1")));
  EXPECT_TRUE(ShowedTheFeed(AskFeed("t2")));
  ASSERT_EQ(sessions_->Submit("Show me recent articles", std::string(kFeedPayload)), SubmitOutcome::Started);
  EXPECT_TRUE(ShowedTheFeed(WaitForTurns({""})[""])); // Home's session too

  EXPECT_TRUE(sessions_->HasSession("t1"));
  EXPECT_TRUE(sessions_->HasSession("t2"));
  EXPECT_EQ(server_->init_calls.load(), 1);
  EXPECT_EQ(server_->list_calls.load(), 1);
}

// Two chats ask their first feed question at once: the second waits for the first one's connection.
TEST_F(McpConnectOnFirstUseTest, ConcurrentFirstQuestionsConnectOnce) {
  Make();
  server_->hold_list = true;
  ASSERT_EQ(sessions_->SubmitToThread("t1", "Show me recent articles", std::string(kFeedPayload)),
            SubmitOutcome::Started);
  ASSERT_EQ(sessions_->SubmitToThread("t2", "Show me recent articles", std::string(kFeedPayload)),
            SubmitOutcome::Started);
  WaitForWaiters(2); // both turns wait for the one connect
  server_->hold_list = false;
  auto events = WaitForTurns({"t1", "t2"});

  EXPECT_TRUE(ShowedTheFeed(events["t1"]));
  EXPECT_TRUE(ShowedTheFeed(events["t2"]));
  EXPECT_EQ(server_->init_calls.load(), 1);
  EXPECT_EQ(server_->list_calls.load(), 1);
}

// The server answers tools/list with an error (a deploy's 503) — it answered, so no cooldown: the question ends with the feed's failure at once,
// the failed connection is not kept, and the next question connects again and shows the feed.
TEST_F(McpConnectOnFirstUseTest, FailedConnectFailsTheQuestionAndTheNextConnectsAgain) {
  Make();
  server_->list_status = 503;
  const auto failed = AskFeed("t1");
  EXPECT_FALSE(ShowedTheFeed(failed));
  ASSERT_EQ(Count(failed, AgentEventType::Error), 1u);
  for (const AgentEvent& event : failed) {
    if (event.type == AgentEventType::Error) {
      EXPECT_EQ(event.message, "Briefs are unavailable right now.");
    }
  }
  EXPECT_EQ(server_->init_calls.load(), 1);

  server_->list_status = 200;
  EXPECT_TRUE(ShowedTheFeed(AskFeed("t1")));
  EXPECT_EQ(server_->init_calls.load(), 2);
}

// A new configuration (a preset, key or backend change) drops the connection: the next feed question connects again,
// once, for every session.
TEST_F(McpConnectOnFirstUseTest, ConfigureDropsTheConnection) {
  Make();
  EXPECT_TRUE(ShowedTheFeed(AskFeed("t1")));
  ASSERT_EQ(server_->init_calls.load(), 1);

  AppConfig other = McpConfig();
  other.llm.api_key = "brf_llm_other";
  sessions_->Configure(other);
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(server_->init_calls.load(), 1); // the configure itself connects nothing

  EXPECT_TRUE(ShowedTheFeed(AskFeed("t1")));
  EXPECT_TRUE(ShowedTheFeed(AskFeed("t2")));
  EXPECT_EQ(server_->init_calls.load(), 2);
}

// Another preset than brief offers the model its tools on every question: the first question connects (before the
// planner, whose model is unreachable here), the second does not.
TEST_F(McpConnectOnFirstUseTest, AModelPresetConnectsOnItsFirstQuestionOnly) {
  Make(McpConfig("ollama"));
  ASSERT_EQ(sessions_->SubmitToThread("t1", "hello"), SubmitOutcome::Started);
  WaitForTurns({"t1"});
  EXPECT_EQ(server_->init_calls.load(), 1);
  {
    std::lock_guard lock(mu_);
    ASSERT_FALSE(llm_bodies_.empty()); // the planner's request (its server is unreachable here)
    EXPECT_NE(llm_bodies_.front().find("blog_articles"), std::string::npos) << "the model was not offered the MCP tool";
  }
  ASSERT_EQ(sessions_->SubmitToThread("t1", "hello again"), SubmitOutcome::Started);
  WaitForTurns({"t1"});
  EXPECT_EQ(server_->init_calls.load(), 1);
}

// Stop while the connect hangs: the turn ends at once (no error, "thinking" off), the connect finishes in the
// background, and the next question uses it without connecting again.
TEST_F(McpConnectOnFirstUseTest, StopWhileConnectingEndsTheTurnAtOnceAndTheNextReusesTheConnection) {
  Make();
  server_->hold_list = true;
  ASSERT_EQ(sessions_->SubmitToThread("t1", "Show me recent articles", std::string(kFeedPayload)),
            SubmitOutcome::Started);
  WaitForWaiters(1);
  sessions_->Cancel("t1");
  const auto stopped = WaitForTurns({"t1"})["t1"];
  // The turn ended while the fake server still holds the listing: it did not wait for the connect to finish or time
  // out (an event-order proof, no wall-clock limit).
  EXPECT_TRUE(server_->hold_list);
  EXPECT_EQ(SharedMcpTools::RunningConnectsForTests(), 1u);
  EXPECT_EQ(Count(stopped, AgentEventType::Error), 0u);
  EXPECT_FALSE(sessions_->IsBusy("t1"));

  server_->hold_list = false;
  WaitForMcpConnects();
  EXPECT_TRUE(ShowedTheFeed(AskFeed("t1")));
  EXPECT_EQ(server_->init_calls.load(), 1);
  EXPECT_EQ(server_->list_calls.load(), 1);
}

// A new configuration while a connect hangs: that connect's result is not kept, the next question connects again.
TEST_F(McpConnectOnFirstUseTest, AConnectOverTakenByANewConfigurationIsNotKept) {
  Make();
  server_->hold_list = true;
  ASSERT_EQ(sessions_->SubmitToThread("t1", "Show me recent articles", std::string(kFeedPayload)),
            SubmitOutcome::Started);
  WaitForWaiters(1);
  AppConfig other = McpConfig();
  other.llm.api_key = "brf_llm_other";
  sessions_->Configure(other);
  server_->hold_list = false;
  WaitForTurns({"t1"});
  sessions_->WaitForConfigureIdle();
  WaitForMcpConnects();
  ASSERT_EQ(server_->init_calls.load(), 1);

  EXPECT_TRUE(ShowedTheFeed(AskFeed("t1")));
  EXPECT_EQ(server_->init_calls.load(), 2);
}

// Offline: the first question's connect gets no answer; the second, within the cooldown, sends nothing at all and
// fails at once with the same words.
TEST_F(McpConnectOnFirstUseTest, OfflineTheSecondQuestionDialsNothingDuringTheCooldown) {
  Make();
  server_->failing_lists = -1;
  ASSERT_EQ(Count(AskFeed("t1"), AgentEventType::Error), 1u);
  WaitForMcpConnects();
  const int inits = server_->init_calls.load();
  const int lists = server_->list_calls.load();
  ASSERT_EQ(inits, 1);

  // "Dials nothing" is shown by the init / list counts below, not by a time limit.
  const auto second = AskFeed("t2"); // another session: the failure is shared too
  ASSERT_EQ(Count(second, AgentEventType::Error), 1u);
  for (const AgentEvent& event : second) {
    if (event.type == AgentEventType::Error) {
      EXPECT_EQ(event.message, "Briefs are unavailable right now.");
    }
  }
  EXPECT_EQ(server_->init_calls.load(), inits);
  EXPECT_EQ(server_->list_calls.load(), lists);
}

// Quitting (or a profile reset) while the connect hangs: the sessions go at once, the connect thread ends later on
// its own without touching them.
TEST_F(McpConnectOnFirstUseTest, DestroyingTheSessionsWhileConnectingReturnsAtOnce) {
  Make();
  server_->hold_list = true;
  ASSERT_EQ(sessions_->SubmitToThread("t1", "Show me recent articles", std::string(kFeedPayload)),
            SubmitOutcome::Started);
  WaitForWaiters(1);
  sessions_.reset();
  // It returned while the connect thread is still held in the fake network: it did not wait for the connect.
  EXPECT_EQ(SharedMcpTools::RunningConnectsForTests(), 1u);
  server_->hold_list = false;
  WaitForMcpConnects();
}

// The question shows in the chat before the turn waits for the connect.
TEST_F(McpConnectOnFirstUseTest, TheQuestionIsStoredBeforeTheConnectWait) {
  Make();
  server_->hold_list = true;
  ASSERT_EQ(sessions_->SubmitToThread("t1", "Show me recent articles", std::string(kFeedPayload)),
            SubmitOutcome::Started);
  WaitForWaiters(1);
  EXPECT_TRUE(store_.HasText("Show me recent articles"));
  server_->hold_list = false;
  EXPECT_TRUE(ShowedTheFeed(WaitForTurns({"t1"})["t1"]));
}

// --- SharedMcpTools on its own.

class SharedMcpToolsTest : public ::testing::Test {
protected:
  void SetUp() override { McpClient::SetHttpPost(FakeMcpPost(server_)); }
  void TearDown() override {
    server_->hold_list = false;
    WaitForMcpConnects();
    McpClient::SetHttpPost(nullptr);
  }

  static AppConfig Config() {
    AppConfig config = MakeConfig("brief");
    config.promoted_mcp.id = "brief";
    config.promoted_mcp.enabled = true;
    return config;
  }

  std::shared_ptr<FakeMcpServer> server_ = std::make_shared<FakeMcpServer>();
};

// The connect throws (a transport bug): every waiter gets an empty connection, nobody hangs, and the next Acquire
// connects again (no cooldown: the server never said it was down).
TEST_F(SharedMcpToolsTest, AThrowingConnectReleasesEveryWaiter) {
  SharedMcpTools tools;
  server_->throw_on_initialize = true;
  server_->hold_init = true;
  std::shared_ptr<McpConnection> first;
  std::shared_ptr<McpConnection> other;
  std::thread starter([&] { first = tools.Acquire(Config()); });
  WaitForConnectWaiters(1);
  std::thread waiter([&] { other = tools.Acquire(Config()); });
  WaitForConnectWaiters(2);
  server_->hold_init = false;
  starter.join();
  waiter.join();
  ASSERT_TRUE(first);
  ASSERT_TRUE(other);
  EXPECT_EQ(first, other); // one connect for both
  EXPECT_FALSE(first->complete);
  EXPECT_TRUE(first->tools.Tools().empty());
  EXPECT_EQ(server_->init_calls.load(), 1);

  server_->throw_on_initialize = false;
  EXPECT_TRUE(tools.Acquire(Config())->complete);
  EXPECT_EQ(server_->init_calls.load(), 2);
}

TEST_F(SharedMcpToolsTest, AnUnreachableServerIsDialedAgainAfterTheCooldown) {
  SharedMcpTools tools;
  tools.SetRetryCooldownForTests(std::chrono::milliseconds(150));
  server_->failing_lists = -1;
  const auto failed = tools.Acquire(Config());
  EXPECT_TRUE(failed->unreachable);
  EXPECT_EQ(tools.Acquire(Config()), failed); // cooling down: the same answer, nothing sent
  EXPECT_EQ(server_->init_calls.load(), 1);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  server_->failing_lists = 0;
  EXPECT_TRUE(tools.Acquire(Config())->complete);
  EXPECT_EQ(server_->init_calls.load(), 2);
}

TEST_F(SharedMcpToolsTest, InvalidateEndsTheCooldown) {
  SharedMcpTools tools;
  server_->failing_lists = -1;
  EXPECT_TRUE(tools.Acquire(Config())->unreachable);
  tools.Invalidate();
  server_->failing_lists = 0;
  EXPECT_TRUE(tools.Acquire(Config())->complete);
  EXPECT_EQ(server_->init_calls.load(), 2);
}

} // namespace
