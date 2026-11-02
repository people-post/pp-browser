// AgentSessions (DEV/PLAN-ai-concurrent-turns.md, step 2): one session for the Home composer, one per thread, the cap
// on thread turns running at once, idle sessions closed and rebuilt. Real AgentSessions with a fake brief_AI stream.
// Without a fixed cap the sessions take theirs from the configured key: kMaxConcurrentTurns (3), a guest 1.

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
  /** `cap`: a fixed cap; none: the configured key's (RegisteredConfig: kMaxConcurrentTurns). */
  void Make(const std::optional<size_t> cap = std::nullopt, const bool configure = true) {
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
      sessions_->Configure(RegisteredConfig());
      sessions_->WaitForConfigureIdle();
      ASSERT_TRUE(sessions_->IsConfigured());
    }
  }

  /** MakeConfig with a registered user's brief_AI key: kMaxConcurrentTurns. */
  static AppConfig RegisteredConfig() {
    AppConfig config = MakeConfig("brief");
    config.llm.api_key = "brf_llm_test";
    return config;
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
    // Stands in for an app-state tool (the real search_people is network-only; local actions only take this one).
    registry.Register(MakeTool(ToolDefinition{"search_people", "search_people", Object{}},
                               ToolMeta{.provider = "messaging", .domain = "people", .risk = "read", .app_state = true},
                               [this](const Object& args) -> Roe<std::string> {
                                 const int now_running = ++tools_running_;
                                 int seen = max_tools_running_.load();
                                 while (now_running > seen && !max_tools_running_.compare_exchange_weak(seen, now_running)) {
                                 }
                                 if (gate_tools_) {
                                   // Held until both turns have asked for their tool; then a second one running at
                                   // the same time would be inside here with this one.
                                   const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                                   while (!both_requested_.load() && std::chrono::steady_clock::now() < until) {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                   }
                                   const auto overlap = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                                   while (tools_running_.load() < 2 && std::chrono::steady_clock::now() < overlap) {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                   }
                                   int both = max_tools_running_.load();
                                   const int running = tools_running_.load();
                                   while (running > both && !max_tools_running_.compare_exchange_weak(both, running)) {
                                   }
                                 }
                                 {
                                   std::lock_guard lock(mu_);
                                   tool_calls_.push_back(args.getString("query").value_or(""));
                                 }
                                 --tools_running_;
                                 return std::string("[]");
                               }));
    ToolMeta on_ui{.provider = "messaging", .domain = "communications", .risk = "read", .app_state = true,
                   .ui_thread = true};
    registry.Register(MakeTool(ToolDefinition{"open_conversation", "open_conversation", Object{}}, on_ui,
                               [this](const Object& args) -> Roe<std::string> {
                                 const int now_running = ++tools_running_;
                                 int seen = max_tools_running_.load();
                                 while (now_running > seen && !max_tools_running_.compare_exchange_weak(seen, now_running)) {
                                 }
                                 std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                 {
                                   std::lock_guard lock(mu_);
                                   tool_calls_.push_back(args.getString("thread_id").value_or(""));
                                   on_ui_.push_back(AppRuntime::CurrentlyOnUI());
                                 }
                                 --tools_running_;
                                 return std::string(R"({"id":"x","title":"x"})");
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

  static Object OpenArgs(const std::string& thread_id) {
    Object args;
    args.set("thread_id", thread_id);
    return args;
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
  std::atomic<bool> gate_tools_{false};     // search_people waits for both_requested_
  std::atomic<bool> both_requested_{false}; // both turns reported their tool "running"
  std::mutex mu_;
  std::vector<BriefAiRequest> requests_;
  std::vector<std::string> tool_calls_;
  std::vector<bool> on_ui_; // open_conversation: ran on the UI thread
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

TEST_F(AgentSessionsTest, HomeTurnDoesNotCountAtCapOneEither) {
  Make(1);
  EXPECT_EQ(sessions_->Submit("hold home"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitToThread("t1", "hold"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitToThread("t2", "second"), SubmitOutcome::Full) << "one thread turn at a time";
  release_ = true;
  WaitEnded("");
  WaitEnded("t1");
}

// --- the cap follows the key ----------------------------------------------------------------------------

TEST_F(AgentSessionsTest, ARegisteredKeyAnswersThreeChatsAtOnce) {
  Make();
  EXPECT_EQ(sessions_->MaxConcurrentTurns(), kMaxConcurrentTurns);
  for (const char* thread : {"t1", "t2", "t3"}) {
    EXPECT_EQ(sessions_->SubmitToThread(thread, "hold"), SubmitOutcome::Started) << thread;
  }
  EXPECT_EQ(sessions_->SubmitToThread("t4", "hold"), SubmitOutcome::Full);
  release_ = true;
  PumpUntil([&] { return !sessions_->AnyBusy(); }, "every turn to end");
}

TEST_F(AgentSessionsTest, AGuestKeyAnswersOneChatAtATime) {
  Make(std::nullopt, /*configure=*/false);
  AppConfig guest = MakeConfig("brief");
  guest.llm.api_key = "brf_guest_0123";
  sessions_->Configure(guest);
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(sessions_->MaxConcurrentTurns(), kMaxConcurrentGuestTurns);
  EXPECT_EQ(sessions_->Submit("hold home"), SubmitOutcome::Started) << "Home is not counted";
  EXPECT_EQ(sessions_->SubmitToThread("t1", "hold"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->CanStart("t2"), TurnStart::Full);
  EXPECT_EQ(sessions_->SubmitToThread("t2", "hold"), SubmitOutcome::Full);
  release_ = true;
  PumpUntil([&] { return !sessions_->AnyBusy(); }, "every turn to end");

  AppConfig registered = MakeConfig("brief");
  registered.llm.api_key = "brf_llm_0123";
  sessions_->Configure(registered); // the user signed up
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(sessions_->MaxConcurrentTurns(), kMaxConcurrentTurns);
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

  sessions_->Cancel("");
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

TEST_F(AgentSessionsTest, AResumedCardCountsFromTheTap) {
  Make(1);
  sessions_->SubmitToThread("a", "关掉通知");
  WaitEnded("a");
  const std::string approval = ApprovalId(OfType(AgentEventType::AssistantReady).front().text);
  ASSERT_FALSE(approval.empty());
  EXPECT_EQ(sessions_->CanStart("b"), TurnStart::Yes) << "a card waiting takes no slot";

  ASSERT_TRUE(sessions_->ResumeToolPermission(approval, "allow_once"));
  EXPECT_EQ(sessions_->CanStart("b"), TurnStart::Full) << "the resumed turn counts before its worker runs";
  EXPECT_EQ(sessions_->SubmitToThread("b", "q"), SubmitOutcome::Full);
  WaitEnded("a", 2);
  EXPECT_EQ(sessions_->SubmitToThread("b", "q"), SubmitOutcome::Started);
  WaitEnded("b");
}

TEST_F(AgentSessionsTest, AForgottenThreadDropsItsCardAndCanClose) {
  Make();
  sessions_->SubmitToThread("a", "关掉通知");
  WaitEnded("a");
  const std::string approval = ApprovalId(OfType(AgentEventType::AssistantReady).front().text);
  ASSERT_FALSE(approval.empty());

  sessions_->ForgetThread("a");
  PumpUntil([&] { return !OfType(AgentEventType::ApprovalClosed).empty(); }, "the card to close");
  Advance(kSessionIdleTimeout);
  Poll();
  EXPECT_FALSE(sessions_->HasSession("a")) << "nothing keeps the session of a cleared thread";
  EXPECT_FALSE(sessions_->ResumeToolPermission(approval, "allow_once"));
}

TEST_F(AgentSessionsTest, SubmitDuringConfigureWaitsForThatInstance) {
  block_configure_ = true;
  Make(kMaxConcurrentTurns, /*configure=*/false);
  sessions_->Configure(RegisteredConfig());
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
  EXPECT_EQ(configures_.load(), 2) << "Home and the spare";
  sessions_->SubmitToThread("t1", "q");
  sessions_->SubmitToThread("t2", "q");
  WaitEnded("t1");
  WaitEnded("t2");
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(configures_.load(), 4) << "each new thread took the spare and a new one was warmed";

  sessions_->Configure(RegisteredConfig());
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(configures_.load(), 8) << "Home, the spare, t1 and t2 again";

  sessions_->SubmitToThread("t3", "q");
  WaitEnded("t3");
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(configures_.load(), 9) << "the next spare gets the current configuration";
}

TEST_F(AgentSessionsTest, FirstQuestionInAThreadTakesTheWarmSpare) {
  Make();
  EXPECT_TRUE(sessions_->HasSpare());
  EXPECT_EQ(configures_.load(), 2);
  EXPECT_EQ(sessions_->SubmitToThread("t1", "q"), SubmitOutcome::Started);
  WaitEnded("t1");
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(configures_.load(), 3) << "t1 was configured before it was asked; only the next spare configured";
  EXPECT_TRUE(sessions_->HasSpare());
  EXPECT_EQ(sessions_->LiveSessions(), 3u) << "Home, t1 and one spare, never more";
}

TEST_F(AgentSessionsTest, IgnoredQuestionsOpenNoSession) {
  Make();
  EXPECT_EQ(sessions_->SubmitToThread("t1", ""), SubmitOutcome::Ignored);
  EXPECT_EQ(sessions_->SubmitScopedAssist("t2", ""), SubmitOutcome::Ignored);
  EXPECT_EQ(sessions_->SubmitLocalAction("t3", "go", "delete_everything", Object{}), SubmitOutcome::Ignored);
  EXPECT_FALSE(sessions_->HasSession("t1"));
  EXPECT_FALSE(sessions_->HasSession("t2"));
  EXPECT_FALSE(sessions_->HasSession("t3"));
  EXPECT_FALSE(sessions_->AnyBusy());
}

TEST_F(AgentSessionsTest, PauseKeepsTheToolHook) {
  Make();
  sessions_->SubmitToThread("t1", "q");
  WaitEnded("t1");
  sessions_->WaitForConfigureIdle();
  const int before = configures_.load(); // Home, t1 (the first spare), the next spare
  sessions_->CancelAll();
  sessions_->Configure(RegisteredConfig());
  sessions_->WaitForConfigureIdle();
  EXPECT_EQ(configures_.load(), before + 3) << "every session still registers the app's tools after a pause";
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

  // Closed when idle, then asked while the session it gets (the spare) is still configuring.
  Advance(kSessionIdleTimeout);
  Poll();
  ASSERT_FALSE(sessions_->HasSession("t1"));
  block_configure_ = true;
  sessions_->Configure(RegisteredConfig()); // the spare reconfigures and is held in it
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
  for (const char* thread : {"t1", "t2", "t3", "t4"}) {
    sessions_->SubmitToThread(thread, "q");
    WaitEnded(thread);
    Advance(std::chrono::seconds(1));
  }
  ASSERT_EQ(sessions_->LiveSessions(), kSoftMaxLiveSessions) << "Home, four threads and the spare";

  sessions_->SubmitToThread("t5", "q");
  WaitEnded("t5");
  EXPECT_FALSE(sessions_->HasSession("t1")) << "the least recently used idle session made room";
  EXPECT_TRUE(sessions_->HasSession("t2"));
  EXPECT_EQ(sessions_->LiveSessions(), kSoftMaxLiveSessions);

  // Every session runs a turn: a new thread still gets one.
  EXPECT_EQ(sessions_->Submit("hold"), SubmitOutcome::Started);
  for (const char* thread : {"t2", "t3", "t4", "t5"}) {
    EXPECT_EQ(sessions_->SubmitToThread(thread, "hold"), SubmitOutcome::Started) << thread;
  }
  EXPECT_EQ(sessions_->SubmitToThread("t6", "hold"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->LiveSessions(), kSoftMaxLiveSessions + 1) << "a soft cap: nothing waits for it";

  release_ = true;
  PumpUntil([&] { return !sessions_->AnyBusy(); }, "every turn to end");
}

// --- app actions ---------------------------------------------------------------------------------------

// At cap 1 the second thread's app action is Full until the first has ended; the UI hands it over then, and the
// events reach the UI in that order.
TEST_F(AgentSessionsTest, SecondThreadsLocalActionIsFullAtCapOneThenRunsInOrder) {
  Make(1);
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

// Step 2b: app actions of different threads run one at a time, and those that change the screens on the UI thread.
TEST_F(AgentSessionsTest, TwoThreadsAppActionsRunOneAtATime) {
  Make(3);
  gate_tools_ = true;
  EXPECT_EQ(sessions_->SubmitLocalAction("ta", "find a", "search_people", Query("a")), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitLocalAction("tb", "find b", "search_people", Query("b")), SubmitOutcome::Started);
  // Both turns have reached their tool (it reports "running" right before it runs); the first holds until now.
  PumpUntil(
      [&] {
        std::set<std::string> running;
        for (const AgentEvent& event : OfType(AgentEventType::ToolActivity)) {
          if (event.status == "running") {
            running.insert(event.thread_id);
          }
        }
        return running.size() == 2;
      },
      "both turns to ask for their tool");
  both_requested_ = true;
  WaitEnded("ta");
  WaitEnded("tb");
  auto calls = ToolCalls();
  std::sort(calls.begin(), calls.end());
  EXPECT_EQ(calls, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(max_tools_running_.load(), 1) << "two turns ran at once, their app actions did not";
}

TEST_F(AgentSessionsTest, TwoThreadsUiToolsRunOneAtATimeOnTheUiThread) {
  Make(3);
  sessions_->SetLocalContactsSource([] {
    return std::vector<LocalPlanContact>{{.id = "ca", .thread_id = "a", .remark = "A"},
                                         {.id = "cb", .thread_id = "b", .remark = "B"}};
  });
  EXPECT_EQ(sessions_->SubmitLocalAction("ta", "open a", "open_conversation", OpenArgs("a")), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitLocalAction("tb", "open b", "open_conversation", OpenArgs("b")), SubmitOutcome::Started);
  WaitEnded("ta");
  WaitEnded("tb");
  auto calls = ToolCalls();
  std::sort(calls.begin(), calls.end());
  EXPECT_EQ(calls, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(max_tools_running_.load(), 1);
  std::lock_guard lock(mu_);
  EXPECT_EQ(on_ui_, (std::vector<bool>{true, true})) << "the turns' workers handed them to the UI thread";
}

// Step 2b: streamed turns hold their thread for the whole answer; they run on the sessions' own threads, so the app's
// worker pool stays free for everything else.
TEST_F(AgentSessionsTest, FourTurnsLeaveTheWorkerPoolFree) {
  Make(3);
  EXPECT_EQ(sessions_->Submit("hold home"), SubmitOutcome::Started);
  for (const char* thread : {"t1", "t2", "t3"}) {
    EXPECT_EQ(sessions_->SubmitToThread(thread, "hold"), SubmitOutcome::Started) << thread;
  }
  WaitRequests(4); // four streams open at once

  std::atomic<bool> ran{false};
  const auto posted = std::chrono::steady_clock::now();
  AppRuntime::PostWorkerNormal([&ran] { ran = true; });
  while (!ran.load() && std::chrono::steady_clock::now() - posted < std::chrono::seconds(1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(ran.load()) << "a plain worker task waited for the AI answers";

  release_ = true;
  PumpUntil([&] { return !sessions_->AnyBusy(); }, "every turn to end");
}

TEST_F(AgentSessionsTest, StopTurnsAndWaitEndsEveryRunningTurn) {
  Make(3);
  EXPECT_EQ(sessions_->Submit("hold home"), SubmitOutcome::Started);
  EXPECT_EQ(sessions_->SubmitToThread("t1", "hold"), SubmitOutcome::Started);
  WaitRequests(2);
  EXPECT_TRUE(sessions_->StopTurnsAndWait(std::chrono::seconds(2)));
  EXPECT_FALSE(sessions_->AnyBusy());
}

TEST_F(AgentSessionsTest, NoBriefKeyYetAnswersOneChatAtATime) {
  AppConfig none = MakeConfig("brief");
  none.llm.api_key.clear(); // the guest key is minted after launch
  EXPECT_EQ(MaxConcurrentTurnsFor(none), kMaxConcurrentGuestTurns);
  AppConfig unknown = MakeConfig("brief");
  unknown.llm.api_key = "something-else";
  EXPECT_EQ(MaxConcurrentTurnsFor(unknown), kMaxConcurrentGuestTurns);
  EXPECT_EQ(MaxConcurrentTurnsFor(RegisteredConfig()), kMaxConcurrentTurns);
  AppConfig other = MakeConfig("ollama");
  EXPECT_EQ(MaxConcurrentTurnsFor(other), kMaxConcurrentTurns) << "another provider's own key";
}

TEST_F(AgentSessionsTest, EveryTurnThreadTakenIsFullNotAStuckTurn) {
  Make(10); // the cap is not what stops it here
  EXPECT_EQ(sessions_->Submit("hold home"), SubmitOutcome::Started);
  for (const char* thread : {"t1", "t2", "t3", "t4", "t5"}) {
    EXPECT_EQ(sessions_->SubmitToThread(thread, "hold"), SubmitOutcome::Started) << thread;
  }
  WaitRequests(kTurnPoolThreads);
  EXPECT_EQ(sessions_->CanStart("t6"), TurnStart::Full) << "no thread left for it";
  EXPECT_EQ(sessions_->SubmitToThread("t6", "q"), SubmitOutcome::Full);
  release_ = true;
  PumpUntil([&] { return !sessions_->AnyBusy(); }, "every turn to end");
  EXPECT_EQ(sessions_->SubmitToThread("t6", "q"), SubmitOutcome::Started);
  WaitEnded("t6");
}

} // namespace
