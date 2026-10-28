// On-device app actions end to end (DEV/PLAN-local-app-actions.md, step 3): the brief preset plans, asks back,
// executes and answers "operate the app" requests on the device. No completions request ever leaves the session
// (LlmRequests() counts them), the brief_AI stream is asked at most once per message, and contact remarks never
// reach either.

#include "feature/ai/tests/agent_session_test_support.h"

#include "common/thread/ThreadTypes.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ai/LocalActionRender.h"

namespace {

using namespace pbr;
using namespace pbr::agent_test;

constexpr const char* kRemark = "欠钱的老王";

BriefAiEvent Handoff(const std::string& capability) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Handoff;
  event.capability = capability;
  return event;
}

Object Args(std::initializer_list<std::pair<const char*, const char*>> fields) {
  Object args;
  for (const auto& [key, value] : fields) {
    args.set(key, std::string(value));
  }
  return args;
}

class AgentSessionLocalActionTest : public AgentSessionBriefStreamTest {
protected:
  void SetUp() override {
    AgentSessionBriefStreamTest::SetUp();
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    contacts_ = {
        {.id = "c1", .thread_id = "d1", .remark = kRemark, .nickname = "Wang", .last_contact_ms = 1759000000000},
        {.id = "c2", .thread_id = "d2", .remark = "", .nickname = "Tom", .last_contact_ms = 1759000000000},
        {.id = "c3", .thread_id = "d3", .remark = "", .nickname = "Tom", .last_contact_ms = 1760000000000},
    };
  }
  void TearDown() override {
    AgentSessionBriefStreamTest::TearDown();
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  void StartLocal(ToolPermissionsPrefs permissions = {}) {
    session_ = std::make_unique<AgentSession>();
    session_->SetToolRegistrationHook([this](ToolRegistry& registry) { RegisterFakes(registry); });
    session_->SetToolPermissions(std::move(permissions));
    session_->SetLocalContactsSource([this]() {
      std::lock_guard lock(contacts_mu_);
      return contacts_;
    });
    session_->SetThreadStore(&store_);
    session_->Configure(MakeConfig("brief"));
    session_->WaitForConfigureIdle();
    ASSERT_TRUE(session_->IsConfigured());
    Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  }

  static ToolPermissionsPrefs Allow(const std::string& tool) {
    ToolPermissionsPrefs prefs;
    prefs.by_tool[tool] = ToolPermissionEntry{.decision = "allow"};
    return prefs;
  }

  void RegisterFakes(ToolRegistry& registry) {
    const auto add = [&](const std::string& name, ToolMeta meta, std::function<std::string(const Object&)> result) {
      registry.Register(MakeTool(ToolDefinition{name, name, Object{}}, std::move(meta),
                                 [this, name, result](const Object& args) -> Roe<std::string> {
                                   {
                                     std::lock_guard lock(mu_);
                                     calls_.push_back(name + " " + DumpJson(args));
                                   }
                                   return result(args);
                                 }));
    };
    add("get_preferences", ToolMeta{.provider = "settings", .domain = "settings", .risk = "read"},
        [](const Object&) { return std::string(R"({"appearance":"dark"})"); });
    add("search_people", ToolMeta{.provider = "messaging", .domain = "people", .risk = "read"},
        [](const Object&) { return std::string("[]"); });
    add("open_conversation", ToolMeta{.provider = "messaging", .domain = "communications", .risk = "read"},
        [](const Object& args) { return R"({"id":")" + args.getString("thread_id").value_or("") + R"("})"; });
    add("start_conversation",
        ToolMeta{.provider = "messaging", .domain = "communications", .risk = "write", .mutating = true},
        [](const Object&) { return std::string(R"({"id":"new"})"); });
    add("update_profile_nickname",
        ToolMeta{.provider = "settings", .domain = "identity", .risk = "write", .mutating = true},
        [](const Object& args) {
          Object out;
          out.set("nickname", args.getString("nickname").value_or(""));
          return DumpJson(out);
        });
    add("set_notifications", ToolMeta{.provider = "settings", .domain = "settings", .risk = "write", .mutating = true},
        [](const Object& args) {
          Object out;
          out.set("show_notifications", args.getIf<bool>("enabled").value_or(false));
          return DumpJson(out);
        });
  }

  std::vector<std::string> Calls() {
    std::lock_guard lock(mu_);
    return calls_;
  }

  /** The one AssistantReady of a turn. */
  static AgentEvent Ready(const std::vector<AgentEvent>& events) {
    const auto ready = Of(events, AgentEventType::AssistantReady);
    EXPECT_EQ(ready.size(), 1u);
    return ready.empty() ? AgentEvent{} : ready.back();
  }

  static std::string ApprovalId(const std::string& blocks) {
    const std::string key = R"("approval_id":")";
    const size_t at = blocks.find(key);
    if (at == std::string::npos) {
      return {};
    }
    const size_t begin = at + key.size();
    return blocks.substr(begin, blocks.find('"', begin) - begin);
  }

  void ExpectNoModel() { EXPECT_TRUE(LlmRequests().empty()) << "a completions request was made"; }

  /** Everything the stream was sent, in one string. */
  std::string StreamText() {
    std::string all;
    for (const BriefAiRequest& request : Requests()) {
      all += request.message + "\n" + request.summary + "\n";
      for (const BriefAiHistoryTurn& turn : request.history) {
        all += turn.content + "\n";
      }
    }
    return all;
  }

  void SetContacts(std::vector<LocalPlanContact> contacts) {
    std::lock_guard lock(contacts_mu_);
    contacts_ = std::move(contacts);
  }

  MemoryThreadStore store_;
  std::vector<std::string> calls_;
  std::mutex contacts_mu_;
  std::vector<LocalPlanContact> contacts_;
};

// --- A typed hit ---

TEST_F(AgentSessionLocalActionTest, HitRunsOnTheDeviceWithoutStreamOrModel) {
  StartLocal();
  session_->SubmitToThread("t1", "搜索用户 ken");
  const auto events = WaitForTurn();

  EXPECT_TRUE(Requests().empty());
  ExpectNoModel();
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
  const AgentEvent ready = Ready(events);
  EXPECT_EQ(ready.render_mode, RenderMode::PeopleList);
  EXPECT_FALSE(ready.from_model);
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(search_people {"query":"ken"})"});
  // Both sides of the turn are app-action entries: shown, never model history.
  ASSERT_EQ(store_.messages.size(), 2u);
  EXPECT_TRUE(IsLocalActionMessage(store_.messages[0]));
  EXPECT_TRUE(IsLocalActionMessage(store_.messages[1]));
}

TEST_F(AgentSessionLocalActionTest, AllowedWriterAnswersFromTheTemplate) {
  StartLocal(Allow("set_notifications"));
  session_->SubmitToThread("t1", "打开通知");
  const auto events = WaitForTurn();

  Object result;
  result.set("show_notifications", true);
  EXPECT_EQ(Ready(events).text, RenderLocalResult("set_notifications", result, {}));
  EXPECT_EQ(Ready(events).render_mode, RenderMode::Markdown);
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(set_notifications {"enabled":true})"});
  EXPECT_TRUE(Requests().empty());
  ExpectNoModel();
}

// --- Writers: ask -> approve -> execute -> template, never a model ---

TEST_F(AgentSessionLocalActionTest, WriterAsksThenRunsAfterApprovalWithoutTheModel) {
  StartLocal();
  session_->SubmitToThread("t1", "关掉通知");
  const auto first = WaitForTurn();
  const std::string prompt = Ready(first).text;
  EXPECT_NE(prompt.find("tool_permission"), std::string::npos) << prompt;
  EXPECT_TRUE(Calls().empty());
  const std::string approval = ApprovalId(prompt);
  ASSERT_FALSE(approval.empty());

  ASSERT_TRUE(session_->ResumeToolPermission(approval, "allow_once"));
  const auto second = WaitForTurn();
  Object result;
  result.set("show_notifications", false);
  EXPECT_EQ(Ready(second).text, RenderLocalResult("set_notifications", result, {}));
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(set_notifications {"enabled":false})"});
  EXPECT_TRUE(Requests().empty());
  ExpectNoModel();
  for (const ThreadMessage& message : store_.messages) {
    EXPECT_TRUE(IsLocalActionMessage(message)) << message.text; // the prompt and the decision too
  }
}

TEST_F(AgentSessionLocalActionTest, NicknameAsksEvenWhenAlwaysAllowed) {
  StartLocal(Allow("update_profile_nickname"));
  session_->SubmitToThread("t1", "改昵称为 Bob");
  const auto first = WaitForTurn();
  const std::string prompt = Ready(first).text;
  EXPECT_NE(prompt.find("tool_permission"), std::string::npos) << prompt;
  EXPECT_NE(prompt.find("Bob"), std::string::npos) << "the prompt shows the new nickname: " << prompt;
  EXPECT_EQ(prompt.find("allow_always"), std::string::npos) << "identity changes are confirmed every time";
  EXPECT_TRUE(Calls().empty());

  ASSERT_TRUE(session_->ResumeToolPermission(ApprovalId(prompt), "allow_once"));
  const auto second = WaitForTurn();
  Object result;
  result.set("nickname", "Bob");
  EXPECT_EQ(Ready(second).text, RenderLocalResult("update_profile_nickname", result, {}));
  ExpectNoModel();
}

// --- Asking back and the answer ---

TEST_F(AgentSessionLocalActionTest, UnclearOnOffIsAskedAndTheAnswerRunsIt) {
  for (const char* answer : {"关掉", "off"}) {
    SCOPED_TRACE(answer);
    StartLocal(Allow("set_notifications"));
    session_->SubmitToThread("t1", "打开或者关闭通知");
    EXPECT_EQ(Ready(WaitForTurn()).text, Tr("settings.ask.on_or_off.set_notifications"));
    EXPECT_TRUE(Calls().empty());

    session_->SubmitToThread("t1", answer);
    const auto events = WaitForTurn();
    Object result;
    result.set("show_notifications", false);
    EXPECT_EQ(Ready(events).text, RenderLocalResult("set_notifications", result, {}));
    EXPECT_EQ(Calls(), std::vector<std::string>{R"(set_notifications {"enabled":false})"});
    EXPECT_TRUE(Requests().empty());
    ExpectNoModel();
    calls_.clear();
  }
}

TEST_F(AgentSessionLocalActionTest, AnAnswerWithWordlistWordsIsStillTheNickname) {
  StartLocal();
  session_->SubmitToThread("t1", "改昵称");
  EXPECT_EQ(Ready(WaitForTurn()).text, Tr("profile.ask.nickname"));

  session_->SubmitToThread("t1", "搜索达人");
  const std::string prompt = Ready(WaitForTurn()).text;
  EXPECT_NE(prompt.find("tool_permission"), std::string::npos) << prompt;
  EXPECT_NE(prompt.find("搜索达人"), std::string::npos) << prompt;
  EXPECT_TRUE(Calls().empty()); // waiting for the confirmation, not a search
  EXPECT_TRUE(Requests().empty());
  ExpectNoModel();
}

TEST_F(AgentSessionLocalActionTest, CancelWordsDropTheQuestion) {
  for (const char* answer : {"算了", "取消吧", "算了，不改了吧"}) {
    SCOPED_TRACE(answer);
    StartLocal();
    requests_.clear();
    session_->SubmitToThread("t1", "改昵称");
    WaitForTurn();
    session_->SubmitToThread("t1", answer);
    EXPECT_EQ(Ready(WaitForTurn()).text, RenderLocalCancelled());

    session_->SubmitToThread("t1", "Bob"); // no question open any more: an ordinary message
    WaitForTurn();
    EXPECT_EQ(Requests().size(), 1u);
    EXPECT_TRUE(Calls().empty());
    ExpectNoModel();
  }
}

TEST_F(AgentSessionLocalActionTest, AQuestionInsteadOfAnAnswerGoesToTheStream) {
  StartLocal();
  session_->SubmitToThread("t1", "改昵称");
  WaitForTurn();
  session_->SubmitToThread("t1", "这个怎么用？");
  WaitForTurn();
  ASSERT_EQ(Requests().size(), 1u);
  EXPECT_EQ(Requests()[0].message, "这个怎么用？");

  session_->SubmitToThread("t1", "Bob"); // the question was cleared: streamed, not a nickname
  WaitForTurn();
  EXPECT_EQ(Requests().size(), 2u);
  EXPECT_TRUE(Calls().empty());
  ExpectNoModel();
}

TEST_F(AgentSessionLocalActionTest, TwoUnclearAnswersGiveUp) {
  StartLocal(Allow("set_notifications"));
  session_->SubmitToThread("t1", "打开或者关闭通知");
  const std::string question = Ready(WaitForTurn()).text;
  session_->SubmitToThread("t1", "嗯");
  EXPECT_EQ(Ready(WaitForTurn()).text, question); // asked once more
  session_->SubmitToThread("t1", "嗯嗯");
  EXPECT_EQ(Ready(WaitForTurn()).text, RenderLocalGaveUp());

  session_->SubmitToThread("t1", "关掉"); // nothing open: not an answer any more
  WaitForTurn();
  EXPECT_EQ(Requests().size(), 1u);
  EXPECT_TRUE(Calls().empty());
  ExpectNoModel();
}

TEST_F(AgentSessionLocalActionTest, ANewCommandInsteadOfAnAnswerRunsTheNewCommand) {
  StartLocal(Allow("set_notifications"));
  session_->SubmitToThread("t1", "打开或者关闭通知");
  WaitForTurn();
  session_->SubmitToThread("t1", "搜索用户 ken");
  EXPECT_EQ(Ready(WaitForTurn()).render_mode, RenderMode::PeopleList);
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(search_people {"query":"ken"})"});
  EXPECT_TRUE(Requests().empty());
  ExpectNoModel();
}

TEST_F(AgentSessionLocalActionTest, AnExpiredQuestionIsNotAnswered) {
  StartLocal();
  session_->SetLocalAskTtl(std::chrono::milliseconds(1));
  session_->SubmitToThread("t1", "改昵称");
  WaitForTurn();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  session_->SubmitToThread("t1", "Bob");
  WaitForTurn();
  EXPECT_EQ(Requests().size(), 1u);
  EXPECT_TRUE(Calls().empty());
}

TEST_F(AgentSessionLocalActionTest, AMessageInAnotherThreadLeavesTheQuestionOpen) {
  StartLocal();
  session_->SubmitToThread("t1", "改昵称");
  WaitForTurn();
  session_->SubmitToThread("t2", "Bob");
  WaitForTurn();
  EXPECT_EQ(Requests().size(), 1u); // t2: an ordinary question

  session_->SubmitToThread("t1", "Bob");
  const std::string prompt = Ready(WaitForTurn()).text;
  EXPECT_NE(prompt.find("tool_permission"), std::string::npos) << prompt;
  EXPECT_NE(prompt.find("Bob"), std::string::npos) << prompt;
  EXPECT_EQ(Requests().size(), 1u);
  ExpectNoModel();
}

// --- Contacts: a list to pick from, direct actions ---

TEST_F(AgentSessionLocalActionTest, SameNameListsThePeopleAndAPickOpensTheChat) {
  StartLocal();
  session_->SubmitToThread("t1", "打开和 Tom 的聊天");
  const AgentEvent list = Ready(WaitForTurn());
  EXPECT_EQ(list.render_mode, RenderMode::PeopleList);
  EXPECT_FALSE(list.from_model);
  EXPECT_NE(list.text.find(R"("tool":"open_conversation")"), std::string::npos) << list.text;
  EXPECT_NE(list.text.find(R"("thread_id":"d3")"), std::string::npos) << list.text;
  EXPECT_TRUE(Calls().empty());

  session_->SubmitLocalAction("t1", "打开聊天", "open_conversation", Args({{"thread_id", "d3"}, {"contact_id", "c3"}}));
  const auto events = WaitForTurn();
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(open_conversation {"thread_id":"d3"})"});
  LocalRenderContext ctx;
  ctx.name = "Tom";
  EXPECT_EQ(Ready(events).text, RenderLocalResult("open_conversation", Object{}, ctx));
  EXPECT_TRUE(Requests().empty());
  ExpectNoModel();
}

TEST_F(AgentSessionLocalActionTest, IdenticalRowsOfferARemarkInsteadOfOpening) {
  SetContacts({
      {.id = "c4", .thread_id = "", .remark = "老王", .nickname = "Wang"},
      {.id = "c5", .thread_id = "", .remark = "老王", .nickname = "Wang"},
  });
  StartLocal();
  session_->SubmitToThread("t1", "打开和 老王 的聊天");
  const AgentEvent list = Ready(WaitForTurn());
  EXPECT_EQ(list.render_mode, RenderMode::PeopleList);
  EXPECT_NE(list.text.find(R"("tool":"edit_remark")"), std::string::npos) << list.text;
  EXPECT_EQ(list.text.find("open_conversation"), std::string::npos) << list.text;
  EXPECT_EQ(list.text.find("start_conversation"), std::string::npos) << list.text;
  EXPECT_TRUE(Calls().empty());
}

TEST_F(AgentSessionLocalActionTest, DirectActionsOutsideTheWhitelistAreIgnored) {
  StartLocal();
  session_->SubmitLocalAction("t1", "x", "set_notifications", Args({{"enabled", "false"}}));
  session_->SubmitLocalAction("t1", "x", "edit_remark", Args({{"contact_id", "c1"}}));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  AppRuntime::RunUITasks();
  std::vector<AgentEvent> events;
  session_->PollEvents(events);
  EXPECT_TRUE(events.empty());
  EXPECT_TRUE(Calls().empty());
  EXPECT_TRUE(store_.messages.empty());

  session_->SubmitLocalAction("t1", "搜索", "search_people", Args({{"query", "ken"}})); // not left busy
  EXPECT_EQ(Ready(WaitForTurn()).render_mode, RenderMode::PeopleList);
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(search_people {"query":"ken"})"});
}

TEST_F(AgentSessionLocalActionTest, ADeletedContactIsGone) {
  StartLocal();
  session_->SubmitLocalAction("t1", "打开聊天", "open_conversation", Args({{"thread_id", "d9"}, {"contact_id", "c9"}}));
  EXPECT_EQ(Ready(WaitForTurn()).text, RenderLocalContactGone());
  session_->SubmitLocalAction("t1", "发消息", "start_conversation", Args({{"contact_id", "c9"}}));
  EXPECT_EQ(Ready(WaitForTurn()).text, RenderLocalContactGone());
  EXPECT_TRUE(Calls().empty());
  ExpectNoModel();
}

// --- brief_AI hands the turn back ---

TEST_F(AgentSessionLocalActionTest, HandoffForAToolTheSentenceDoesNotNameAsksWithoutTakingParameters) {
  StartLocal();
  Script({Meta(), Handoff("search_people")}, BriefAiOutcome::Handoff);
  session_->SubmitToThread("t1", "ken 在吗");
  const auto events = WaitForTurn();
  EXPECT_EQ(Requests().size(), 1u);
  EXPECT_EQ(Ready(events).text, Tr("contacts.ask.who"));
  EXPECT_TRUE(Calls().empty());

  session_->SubmitToThread("t1", "ken"); // the answer runs the tool; the stream is not asked again
  WaitForTurn();
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(search_people {"query":"ken"})"});
  EXPECT_EQ(Requests().size(), 1u);
  ExpectNoModel();
}

TEST_F(AgentSessionLocalActionTest, HandoffForAnUnknownCapabilityCannotBeDone) {
  StartLocal();
  for (const char* capability : {"messaging", ""}) {
    SCOPED_TRACE(capability);
    Script({Meta(), Handoff(capability)}, BriefAiOutcome::Handoff);
    session_->SubmitToThread("t1", "ken 在吗");
    EXPECT_EQ(Ready(WaitForTurn()).text, RenderLocalUnsupported());
  }
  EXPECT_EQ(Requests().size(), 2u);
  EXPECT_TRUE(Calls().empty());
  ExpectNoModel();
}

// "怎么搜索用户 ken？" is a question, so it streams; handed back for search_people, it is asked (no parameter is
// taken from a question) and never streamed again: one stream request in all.
TEST_F(AgentSessionLocalActionTest, AQuestionHandedBackIsAskedWithOneStreamRequest) {
  StartLocal();
  Script({Meta(), Handoff("search_people")}, BriefAiOutcome::Handoff);
  session_->SubmitToThread("t1", "怎么搜索用户 ken？");
  EXPECT_EQ(Ready(WaitForTurn()).text, Tr("contacts.ask.who"));
  EXPECT_EQ(Requests().size(), 1u);
  EXPECT_TRUE(Calls().empty());
  ExpectNoModel();
}

TEST_F(AgentSessionLocalActionTest, HandoffForAContactToolWithoutANameAsksWhichContact) {
  StartLocal();
  Script({Meta(), Handoff("start_conversation")}, BriefAiOutcome::Handoff);
  session_->SubmitToThread("t1", "ken 在吗");
  EXPECT_EQ(Ready(WaitForTurn()).text, Tr("contacts.ask.which_contact"));
  EXPECT_TRUE(Calls().empty());
  ExpectNoModel();
}

// A typed hit never reaches the stream unless it reads as a question; handed back for the same tool, a reader needs
// no parameter and runs at once.
TEST_F(AgentSessionLocalActionTest, AQuestionHandedBackForTheToolItNamesRunsAReader) {
  StartLocal();
  Script({Meta(), Handoff("get_preferences")}, BriefAiOutcome::Handoff);
  session_->SubmitToThread("t1", "怎么查看我的偏好设置？");
  const auto events = WaitForTurn();
  Object result;
  result.set("appearance", "dark");
  EXPECT_EQ(Ready(events).text, RenderLocalResult("get_preferences", result, {}));
  EXPECT_EQ(Calls(), std::vector<std::string>{"get_preferences {}"});
  EXPECT_EQ(Requests().size(), 1u);
  ExpectNoModel();
}

// --- "@ai" in a person-to-person chat ---

TEST_F(AgentSessionLocalActionTest, SharedAtAiWithAnAppActionIsNotSharedAndAsksNobody) {
  for (const AtAiMode mode : {AtAiMode::SharedFull, AtAiMode::SharedReply}) {
    StartLocal();
    store_.messages.clear();
    session_->SubmitScopedAssist("p1", "搜索用户 ken", std::nullopt, mode);
    const AgentEvent ready = Ready(WaitForTurn());
    EXPECT_EQ(ready.text, RenderLocalNotShareable());
    EXPECT_EQ(ready.shared_ai_mode, AtAiMode::Local) << "the chat must not relay it";
    EXPECT_TRUE(Requests().empty());
    EXPECT_TRUE(Calls().empty());
    ExpectNoModel();
    ASSERT_EQ(store_.messages.size(), 1u); // kept on this device only
    EXPECT_FALSE(store_.messages[0].relay_visible);
  }
  EXPECT_TRUE(session_->IsAppActionRequest("@ai++ 搜索用户 ken") || session_->IsAppActionRequest("搜索用户 ken"));
  EXPECT_FALSE(session_->IsAppActionRequest("今天有什么新闻？"));
}

TEST_F(AgentSessionLocalActionTest, LocalAtAiWithAnAppActionRunsOnTheDevice) {
  StartLocal();
  session_->SubmitScopedAssist("p1", "搜索用户 ken", std::nullopt, AtAiMode::Local);
  const AgentEvent ready = Ready(WaitForTurn());
  EXPECT_TRUE(ready.scoped_assist);
  EXPECT_EQ(ready.render_mode, RenderMode::PeopleList);
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(search_people {"query":"ken"})"});
  EXPECT_TRUE(Requests().empty());
  ExpectNoModel();
  ASSERT_EQ(store_.messages.size(), 1u);
  EXPECT_FALSE(store_.messages[0].relay_visible);
  EXPECT_TRUE(IsLocalActionMessage(store_.messages[0]));
}

// --- Remarks stay on the device ---

TEST_F(AgentSessionLocalActionTest, ARemarkNeverReachesTheStreamOrTheModel) {
  StartLocal();
  session_->SubmitToThread("t1", std::string("打开和 ") + kRemark + " 的聊天");
  const AgentEvent opened = Ready(WaitForTurn());
  EXPECT_NE(opened.text.find(kRemark), std::string::npos) << "the UI may show it: " << opened.text;
  EXPECT_EQ(Calls(), std::vector<std::string>{R"(open_conversation {"thread_id":"d1"})"});

  session_->SubmitToThread("t1", "今天有什么新闻？");
  WaitForTurn();
  ASSERT_EQ(Requests().size(), 1u);
  EXPECT_EQ(StreamText().find(kRemark), std::string::npos) << StreamText();
  EXPECT_EQ(StreamText().find("Wang"), std::string::npos) << "the whole app-action turn stays out: " << StreamText();
  ExpectNoModel();
}

TEST_F(AgentSessionLocalActionTest, ARemarkStaysOutOfTheHomeComposerHistoryToo) {
  StartLocal();
  session_->Submit(std::string("打开和 ") + kRemark + " 的聊天");
  WaitForTurn();
  session_->Submit("今天有什么新闻？");
  WaitForTurn();
  ASSERT_EQ(Requests().size(), 1u);
  EXPECT_EQ(StreamText().find(kRemark), std::string::npos) << StreamText();
  ExpectNoModel();
}

} // namespace
