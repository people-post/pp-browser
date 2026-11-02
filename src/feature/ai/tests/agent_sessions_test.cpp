// AgentSessions (DEV/PLAN-ai-concurrent-turns.md, step 2): one session for the Home composer, one per thread, the cap
// on thread turns running at once, idle sessions closed and rebuilt. Real AgentSessions with a fake brief_AI stream.
// The product cap is kMaxConcurrentTurns (1 for now); tests about several turns at once build it with 3.

#include "feature/ai/AgentSessions.h"
#include "feature/ai/tests/agent_session_test_support.h"

#include "foundation/i18n/LocalizationService.h"

#include <algorithm>
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

class AgentSessionsTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(ProductRuntimeConfig());
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  }
  void TearDown() override {
    release_ = true;
    block_configure_ = false;
    if (sessions_) {
      sessions_->CancelAll();
      sessions_->WaitForConfigureIdle();
    }
    sessions_.reset();
    AppRuntime::Shutdown();
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  /**
   * Sessions with the fake stream and the fake app tools. A question starting with "hold" streams one token and waits
   * for `release_` (or a cancel); any other answers at once. `configure`: configure and wait for it.
   */
  void Make(const size_t cap = kMaxConcurrentTurns, const bool configure = true) {
    sessions_ = std::make_unique<AgentSessions>(cap, [this] { return now_; });
    sessions_->SetThreadStore(&store_);
    sessions_->SetToolRegistrationHook([this](ToolRegistry& registry) { RegisterFakes(registry); });
    sessions_->SetBriefAiStream([this](const BriefAiRequest& request,
                                       const std::function<void(const BriefAiEvent&)>& on_event,
                                       const std::atomic<bool>& cancel) -> Roe<BriefAiOutcome> {
      {
        std::lock_guard lock(mu_);
        requests_.push_back(request);
      }
      on_event(Meta());
      if (request.message.rfind("hold", 0) == 0) {
        on_event(Token("part"));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!cancel.load() && !release_.load() && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (cancel.load()) {
          return BriefAiOutcome::Cancelled;
        }
      }
      on_event(Done("x", RoleFor(request.message)));
      return BriefAiOutcome::Done;
    });
    if (configure) {
      sessions_->Configure(MakeConfig("brief"));
      sessions_->WaitForConfigureIdle();
      ASSERT_TRUE(sessions_->IsConfigured());
    }
  }

  void RegisterFakes(ToolRegistry& registry) {
    ++configures_;
    if (block_configure_) {
      in_configure_ = true;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (block_configure_.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    registry.Register(MakeTool(ToolDefinition{"search_people", "search_people", Object{}},
                               ToolMeta{.provider = "messaging", .domain = "people", .risk = "read"},
                               [this](const Object& args) -> Roe<std::string> {
                                 const int now_running = ++tools_running_;
                                 int seen = max_tools_running_.load();
                                 while (now_running > seen && !max_tools_running_.compare_exchange_weak(seen, now_running)) {
                                 }
                                 std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                 {
                                   std::lock_guard lock(mu_);
                                   tool_calls_.push_back(args.getString("query").value_or(""));
                                 }
                                 --tools_running_;
                                 return std::string("[]");
                               }));
    registry.Register(MakeTool(ToolDefinition{"set_notifications", "set_notifications", Object{}},
                               ToolMeta{.provider = "settings", .domain = "settings", .risk = "write", .mutating = true},
                               [this](const Object&) -> Roe<std::string> {
                                 std::lock_guard lock(mu_);
                                 tool_calls_.push_back("set_notifications");
                                 return std::string(R"({"show_notifications":false})");
                               }));
  }

  std::string RoleFor(const std::string& message) {
    std::lock_guard lock(mu_);
    const auto it = roles_.find(message);
    return it == roles_.end() ? std::string() : it->second;
  }

  /** Pumps the UI mailbox and polls the sessions until `done` holds for every event so far; fails on timeout. */
  void PumpUntil(const std::function<bool()>& done, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      Poll();
      if (done()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ADD_FAILURE() << "timed out waiting for " << what;
  }

  void Poll() {
    AppRuntime::RunUITasks();
    sessions_->PollEvents(events_);
  }

  /** LoadingChanged(false) events heard for `thread` (empty: the Home composer). */
  size_t Ended(const std::string& thread) const {
    return static_cast<size_t>(std::count_if(events_.begin(), events_.end(), [&](const AgentEvent& event) {
      return event.type == AgentEventType::LoadingChanged && !event.loading && event.thread_id == thread;
    }));
  }

  void WaitEnded(const std::string& thread, const size_t count = 1) {
    PumpUntil([&] { return Ended(thread) >= count; }, thread.empty() ? "the Home turn to end" : thread.c_str());
  }

  void WaitRequests(const size_t count) {
    PumpUntil([&] { return Requests().size() >= count; }, "the stream requests");
  }

  std::vector<BriefAiRequest> Requests() {
    std::lock_guard lock(mu_);
    return requests_;
  }

  std::vector<std::string> ToolCalls() {
    std::lock_guard lock(mu_);
    return tool_calls_;
  }

  std::vector<AgentEvent> OfType(const AgentEventType type) const {
    std::vector<AgentEvent> out;
    for (const AgentEvent& event : events_) {
      if (event.type == type) {
        out.push_back(event);
      }
    }
    return out;
  }

  void Advance(const std::chrono::steady_clock::duration by) { now_ += by; }

  static std::string ApprovalId(const std::string& blocks) {
    const std::string key = R"("approval_id":")";
    const size_t at = blocks.find(key);
    if (at == std::string::npos) {
      return {};
    }
    const size_t begin = at + key.size();
    return blocks.substr(begin, blocks.find('"', begin) - begin);
  }

  static Object Query(const std::string& query) {
    Object args;
    args.set("query", query);
    return args;
  }

  LockedThreadStore store_;
  std::unique_ptr<AgentSessions> sessions_;
  std::chrono::steady_clock::time_point now_ = std::chrono::steady_clock::now();
  std::vector<AgentEvent> events_;
  std::atomic<bool> release_{false};
  std::atomic<bool> block_configure_{false};
  std::atomic<bool> in_configure_{false};
  std::atomic<int> configures_{0};
  std::atomic<int> tools_running_{0};
  std::atomic<int> max_tools_running_{0};
  std::mutex mu_;
  std::vector<BriefAiRequest> requests_;
  std::vector<std::string> tool_calls_;
  std::map<std::string, std::string> roles_;
};

// --- several turns at once (cap 3; the product runs with kMaxConcurrentTurns) -----------------------

TEST_F(AgentSessionsTest, TwoThreadsRunAtOnceAndEventsKeepTheirThread) {
  Make(3);
  EXPECT_EQ(sessions_->SubmitToThread("t1", "hold one"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitToThread("t2", "hold two"), SubmitOutcome::Started);
  WaitRequests(2); // both streams are open at the same time
  EXPECT_TRUE(sessions_->IsBusy("t1"));
  EXPECT_TRUE(sessions_->IsBusy("t2"));

  release_ = true;
  WaitEnded("t1");
  WaitEnded("t2");
  std::set<std::string> started;
  for (const AgentEvent& event : OfType(AgentEventType::LoadingChanged)) {
    if (event.loading) {
      started.insert(event.thread_id);
    }
  }
  EXPECT_EQ(started, (std::set<std::string>{"t1", "t2"}));
  std::set<std::string> answered;
  for (const AgentEvent& event : OfType(AgentEventType::AssistantReady)) {
    answered.insert(event.thread_id);
  }
  EXPECT_EQ(answered, (std::set<std::string>{"t1", "t2"}));
  EXPECT_FALSE(sessions_->AnyBusy());
}

TEST_F(AgentSessionsTest, FourthThreadIsReportedFullNotDropped) {
  Make(3);
  for (const char* thread : {"t1", "t2", "t3"}) {
    EXPECT_EQ(sessions_->SubmitToThread(thread, "hold"), SubmitOutcome::Started) << thread;
  }
  EXPECT_EQ(sessions_->CanStart("t4"), TurnStart::Full);
  EXPECT_EQ(sessions_->SubmitToThread("t4", "fourth"), SubmitOutcome::Full);
  EXPECT_FALSE(sessions_->HasSession("t4")) << "a question that was not taken opens no session";
  EXPECT_TRUE(sessions_->ThreadTurnsFull());
  WaitRequests(3);
  EXPECT_EQ(Requests().size(), 3u);

  release_ = true;
  WaitEnded("t1");
  WaitEnded("t2");
  WaitEnded("t3");
  EXPECT_EQ(sessions_->SubmitToThread("t4", "fourth"), SubmitOutcome::Started) << "free again: the UI hands it over now";
  WaitEnded("t4");
  EXPECT_EQ(Requests().size(), 4u);
}

TEST_F(AgentSessionsTest, HomeTurnDoesNotCountTowardsTheThreadCap) {
  Make(3);
  EXPECT_EQ(sessions_->Submit("hold home"), SubmitOutcome::Started);
  for (const char* thread : {"t1", "t2", "t3"}) {
    EXPECT_EQ(sessions_->SubmitToThread(thread, "hold"), SubmitOutcome::Started) << thread;
  }
  EXPECT_TRUE(sessions_->IsBusy(""));
  EXPECT_EQ(sessions_->CanStart("t4"), TurnStart::Full);
  EXPECT_EQ(sessions_->CanStart(""), TurnStart::BusyHere);
  EXPECT_EQ(sessions_->Submit("again"), SubmitOutcome::BusyHere);

  release_ = true;
  WaitEnded("");
  WaitEnded("t1");
  WaitEnded("t2");
  WaitEnded("t3");
  EXPECT_FALSE(sessions_->AnyBusy());
}

TEST_F(AgentSessionsTest, HomeTurnDoesNotCountAtTheProductCapEither) {
  Make();
  EXPECT_EQ(sessions_->Submit("hold home"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitToThread("t1", "hold"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitToThread("t2", "second"), SubmitOutcome::Full) << "one thread turn at a time";
  release_ = true;
  WaitEnded("");
  WaitEnded("t1");
}

// --- stop, cards, configure ---------------------------------------------------------------------------

TEST_F(AgentSessionsTest, CancelOneThreadLeavesTheOthers) {
  Make();
  EXPECT_EQ(sessions_->Submit("hold home"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitToThread("t1", "hold thread"), SubmitOutcome::Started);
  WaitRequests(2);

  sessions_->Cancel("t1");
  WaitEnded("t1");
  EXPECT_FALSE(sessions_->IsBusy("t1"));
  EXPECT_TRUE(sessions_->IsBusy("")) << "the Home turn goes on";
  EXPECT_EQ(Ended(""), 0u);

  sessions_->Cancel();
  WaitEnded("");
  EXPECT_FALSE(sessions_->AnyBusy());
  EXPECT_TRUE(OfType(AgentEventType::Error).empty());
}

TEST_F(AgentSessionsTest, ParkedApprovalKeepsTheInstanceButFreesTheSlot) {
  Make(3);
  EXPECT_EQ(sessions_->SubmitToThread("a", "关掉通知"), SubmitOutcome::Started);
  WaitEnded("a");
  const auto ready = OfType(AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  const std::string approval = ApprovalId(ready.front().text);
  ASSERT_FALSE(approval.empty()) << ready.front().text;
  EXPECT_FALSE(sessions_->IsBusy("a")) << "a card waiting is not a running turn";

  for (const char* thread : {"b", "c", "d"}) {
    EXPECT_EQ(sessions_->SubmitToThread(thread, "hold"), SubmitOutcome::Started) << thread;
  }
  Advance(kSessionIdleTimeout + std::chrono::minutes(1));
  Poll();
  EXPECT_TRUE(sessions_->HasSession("a")) << "the card keeps its session however long it waits";
  EXPECT_FALSE(sessions_->ResumeToolPermission(approval, "allow_once")) << "resuming is a turn: full now";

  release_ = true;
  for (const char* thread : {"b", "c", "d"}) {
    WaitEnded(thread);
  }
  ASSERT_TRUE(sessions_->ResumeToolPermission(approval, "allow_once"));
  WaitEnded("a", 2);
  EXPECT_EQ(ToolCalls(), std::vector<std::string>{"set_notifications"});
}

TEST_F(AgentSessionsTest, SubmitDuringConfigureWaitsForThatInstance) {
  block_configure_ = true;
  Make(kMaxConcurrentTurns, /*configure=*/false);
  sessions_->Configure(MakeConfig("brief"));
  EXPECT_EQ(sessions_->SubmitToThread("t1", "question"), SubmitOutcome::Started) << "held by the session, not refused";
  EXPECT_TRUE(sessions_->IsBusy("t1"));
  PumpUntil([&] { return in_configure_.load(); }, "a configure to start");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Poll();
  EXPECT_TRUE(Requests().empty()) << "nothing goes out before the session is configured";

  block_configure_ = false;
  WaitEnded("t1");
  EXPECT_EQ(Requests().size(), 1u);
  EXPECT_TRUE(OfType(AgentEventType::Error).empty()) << "no MissingKey: the configured session ran it";
  EXPECT_EQ(OfType(AgentEventType::AssistantReady).size(), 1u);
}

TEST_F(AgentSessionsTest, ConfigureReachesEveryLiveInstance) {
  Make(3);
  EXPECT_EQ(configures_.load(), 1) << "Home";
  sessions_->SubmitToThread("t1", "q");
  sessions_->SubmitToThread("t2", "q");
  WaitEnded("t1");
  WaitEnded("t2");
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(configures_.load(), 3);

  sessions_->Configure(MakeConfig("brief"));
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(configures_.load(), 6) << "Home, t1 and t2 again";

  sessions_->SubmitToThread("t3", "q");
  WaitEnded("t3");
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(configures_.load(), 7) << "a later session gets the current configuration";
}

// --- idle sessions -----------------------------------------------------------------------------------

TEST_F(AgentSessionsTest, LastRoleSurvivesInstanceRebuild) {
  Make();
  roles_["first"] = "news";
  roles_["second"] = "chat";
  sessions_->SubmitToThread("t1", "first");
  WaitEnded("t1");

  Advance(kSessionIdleTimeout + std::chrono::seconds(1));
  Poll();
  ASSERT_FALSE(sessions_->HasSession("t1")) << "idle for the timeout: closed";

  sessions_->SubmitToThread("t1", "second");
  WaitEnded("t1", 2);
  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[1].last_role, "news");

  sessions_->ForgetThread("t1");
  sessions_->SubmitToThread("t1", "third");
  WaitEnded("t1", 3);
  EXPECT_EQ(Requests().back().last_role, "") << "a cleared thread starts without one";
}

TEST_F(AgentSessionsTest, RebuiltInstanceAnswersTheFirstQuestionLikeTheSecond) {
  Make();
  // Right after a turn ended: the same session answers.
  sessions_->SubmitToThread("t1", "one");
  WaitEnded("t1");
  sessions_->SubmitToThread("t1", "two");
  WaitEnded("t1", 2);
  EXPECT_TRUE(sessions_->HasSession("t1"));

  // Closed when idle, then asked while the rebuilt session is still configuring.
  Advance(kSessionIdleTimeout);
  Poll();
  ASSERT_FALSE(sessions_->HasSession("t1"));
  block_configure_ = true;
  EXPECT_EQ(sessions_->SubmitToThread("t1", "three"), SubmitOutcome::Started);
  PumpUntil([&] { return in_configure_.load(); }, "the rebuilt session to configure");
  block_configure_ = false;
  WaitEnded("t1", 3);

  const auto ready = OfType(AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 3u);
  for (const AgentEvent& event : ready) {
    EXPECT_EQ(event.thread_id, "t1");
    EXPECT_EQ(event.text, ready.front().text);
  }
  EXPECT_TRUE(OfType(AgentEventType::Error).empty());
  EXPECT_EQ(Requests().size(), 3u);
}

TEST_F(AgentSessionsTest, SoftAliveCapEvictsIdleNotRunning) {
  Make(10);
  for (const char* thread : {"t1", "t2", "t3", "t4", "t5"}) {
    sessions_->SubmitToThread(thread, "q");
    WaitEnded(thread);
    Advance(std::chrono::seconds(1));
  }
  ASSERT_EQ(sessions_->LiveSessions(), kSoftMaxLiveSessions);

  sessions_->SubmitToThread("t6", "q");
  WaitEnded("t6");
  EXPECT_FALSE(sessions_->HasSession("t1")) << "the least recently used idle session made room";
  EXPECT_TRUE(sessions_->HasSession("t2"));
  EXPECT_EQ(sessions_->LiveSessions(), kSoftMaxLiveSessions);

  // Every session runs a turn: a new thread still gets one.
  EXPECT_EQ(sessions_->Submit("hold"), SubmitOutcome::Started);
  for (const char* thread : {"t2", "t3", "t4", "t5", "t6"}) {
    EXPECT_EQ(sessions_->SubmitToThread(thread, "hold"), SubmitOutcome::Started) << thread;
  }
  EXPECT_EQ(sessions_->SubmitToThread("t7", "hold"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->LiveSessions(), kSoftMaxLiveSessions + 1) << "a soft cap: nothing waits for it";

  release_ = true;
  PumpUntil([&] { return !sessions_->AnyBusy(); }, "every turn to end");
}

// --- app actions ---------------------------------------------------------------------------------------

// Step 0: app-action tools run on the turn's worker thread, not on the UI thread, so two threads' actions are kept
// apart only by the cap. At kMaxConcurrentTurns (1) the second is Full until the first has ended; the UI hands it
// over then, and the events reach the UI in that order.
TEST_F(AgentSessionsTest, TwoLocalActionsFromTwoThreadsRunInOrderOnUi) {
  Make();
  EXPECT_EQ(sessions_->SubmitLocalAction("ta", "find a", "search_people", Query("a")), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitLocalAction("tb", "find b", "search_people", Query("b")), SubmitOutcome::Full);
  WaitEnded("ta");
  EXPECT_EQ(sessions_->SubmitLocalAction("tb", "find b", "search_people", Query("b")), SubmitOutcome::Started);
  WaitEnded("tb");

  EXPECT_EQ(ToolCalls(), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(max_tools_running_.load(), 1);
  const auto ready = OfType(AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 2u);
  EXPECT_EQ(ready[0].thread_id, "ta");
  EXPECT_EQ(ready[1].thread_id, "tb");
  EXPECT_TRUE(Requests().empty()) << "app actions never reach brief_AI";
}

} // namespace
