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

private:
  mutable std::mutex mu_;
};

constexpr const char* kFeedPayload = R"({"tool":"blog_articles","brf_domain":"en","brf_language":"en","size":2})";

class McpConnectOnFirstUseTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(AppRuntimeConfig{});
    McpClient::SetHttpPost(FakeMcpPost(server_)); // before any configure: the sessions keep it (SetHttpPostIfUnset)
  }
  void TearDown() override {
    server_->hold_list = false;
    if (sessions_) {
      sessions_->CancelAll();
      sessions_->WaitForConfigureIdle();
    }
    sessions_.reset();
    AppRuntime::Shutdown();
    McpClient::SetHttpPost(nullptr);
  }

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
    return static_cast<size_t>(std::count_if(events.begin(), events.end(),
                                             [type](const AgentEvent& event) { return event.type == type; }));
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
  const auto started = std::chrono::steady_clock::now();
  while (server_->list_calls.load() < 1 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the second turn reaches the connect meanwhile
  server_->hold_list = false;
  auto events = WaitForTurns({"t1", "t2"});

  EXPECT_TRUE(ShowedTheFeed(events["t1"]));
  EXPECT_TRUE(ShowedTheFeed(events["t2"]));
  EXPECT_EQ(server_->init_calls.load(), 1);
  EXPECT_EQ(server_->list_calls.load(), 1);
}

// The server answers tools/list with an error (a deploy's 503): the question ends with the feed's failure at once,
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
  ASSERT_EQ(sessions_->SubmitToThread("t1", "hello again"), SubmitOutcome::Started);
  WaitForTurns({"t1"});
  EXPECT_EQ(server_->init_calls.load(), 1);
}

} // namespace
