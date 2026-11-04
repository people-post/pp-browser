// The ask-AI half sheet (DEV/PLAN-ui-batch5-ask-ai.md 5a): the real AskAiPanel and AgentTurnController, driven with a
// fake chat screen and fake agent ports. What is checked: each entry opens the sheet at the right height; the settings
// "?" is a one-off question in the Home session (never PP 客服, never the shared chat); the shared conversation is one
// thread for every entry and for "Open in AI chat"; closing keeps the draft and the running answer and sends nothing
// again; a guest's single turn queues the question once and shows it; no answer shows plain words and "Ask PP
// Support"; the privacy card shows once.
#include "gui/chat/AskAiPanel.h"
#include "gui/chat/AgentTurnController.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

/** The chat screen as the agent controller sees it; panel events are passed on as ChatController does. */
class FakeTurnHost : public AgentTurnHost {
public:
  SessionStore store;
  std::string active_thread;
  bool messaging_ready = false;
  int display_syncs = 0;
  AskAiPanel* panel = nullptr;
  /** What a started queued send does (ChatController marks that chat busy). */
  std::function<void(const std::string&)> on_started;
  std::vector<QueuedAgentSend> handed_over;

  ConversationsFacade* Facade() const override { return nullptr; }
  bool MessagingIsReady() const override { return messaging_ready; }
  bool MessagingInitialized() const override { return false; }
  bool MessagingReady() const override { return false; }
  const std::string& ActiveThreadId() const override { return active_thread; }
  ShellChromeSnapshot ChromeSnapshot() const override { return {}; }
  bool InAiComposerContext() const override { return true; }
  void SyncDisplayFromThread() override { ++display_syncs; }
  void SyncShellSessions() override {}
  void SyncComposerInputState() override {}
  void UpdateSidebarPreview(const std::string&) override {}
  bool EnsureHomeOutboundSession(const std::string&) override { return true; }
  void SendUserText(const std::string&, std::optional<std::string>, bool) override {}
  void ShellSetActivity(bool, const ui::String&) override {}
  void ShowAttachChoice(std::function<void()>, std::function<void()>) override {}
  void RegisterMessagingTools(ToolRegistry&) override {}
  SessionStore& Store() override { return store; }
  void HandleLocalAction(const std::string&, const std::optional<std::string>&, const std::string&) override {}
  QueuedSendOutcome RunQueuedAgentSend(const QueuedAgentSend& item) override {
    handed_over.push_back(item);
    if (on_started) {
      on_started(item.thread_id);
    }
    return QueuedSendOutcome::Started;
  }
  void PutBackDraft(const std::string&, const std::string&) override {}
  void OnAgentTurnEnded(const std::string& thread_id) override {
    if (panel) {
      panel->OnTurnEnded(thread_id);
    }
  }
  bool AskAiPanelShows(const std::string& thread_id) const override { return panel && panel->Shows(thread_id); }
  void OnAskAiOnceReply(const AgentEvent& event) override {
    if (panel) {
      panel->OnOnceReply(event);
    }
  }
};

/** The chat screen as the panel sees it. */
class FakePanelHost : public AskAiPanelHost {
public:
  bool compact = true;
  std::string shared_thread = "ai-used-last";
  bool intro_seen = false;
  int intro_marks = 0;
  /** Messages per thread: (from the user, text). */
  std::map<std::string, std::vector<std::pair<bool, std::string>>> messages;
  std::vector<QueuedAgentSend> sent;
  QueuedSendOutcome send_outcome = QueuedSendOutcome::Started;
  std::function<void(const std::string&)> on_sent;
  std::vector<PaneSpec> sheets;
  std::function<void()> on_closed;
  int closes = 0;
  std::vector<std::string> opened_in_chat;
  std::vector<std::string> support;
  std::vector<std::string> toasts;

  bool AskAiCompact() const override { return compact; }
  std::string AskAiSharedThread(bool create) override {
    if (shared_thread.empty() && create) {
      shared_thread = "ai-created";
    }
    return shared_thread;
  }
  std::vector<AskAiRow> AskAiRecentRows(const std::string& thread_id, size_t limit) override {
    std::vector<AskAiRow> rows;
    for (const auto& [user, text] : messages[thread_id]) {
      rows.push_back({text.c_str(), user, text.c_str()});
    }
    if (rows.size() > limit) {
      rows.erase(rows.begin(), rows.end() - static_cast<long>(limit));
    }
    return rows;
  }
  bool AskAiLastIsQuestion(const std::string& thread_id) override {
    const auto& list = messages[thread_id];
    return !list.empty() && list.back().first;
  }
  QueuedSendOutcome AskAiSendNow(const QueuedAgentSend& item) override {
    sent.push_back(item);
    if (send_outcome == QueuedSendOutcome::Started) {
      messages[item.thread_id].push_back({true, item.text}); // the router stores the question
      if (on_sent) {
        on_sent(item.thread_id);
      }
    }
    return send_outcome;
  }
  std::string AskAiStoreQuestion(const std::string& thread_id, const std::string& text) override {
    messages[thread_id].push_back({true, text});
    return "q-" + std::to_string(messages[thread_id].size());
  }
  int AskAiPushSheet(const PaneSpec& spec, std::function<void()> closed) override {
    sheets.push_back(spec);
    on_closed = std::move(closed);
    return 40 + static_cast<int>(sheets.size());
  }
  void AskAiCloseSheet(int) override {
    ++closes;
    if (on_closed) {
      auto closed = std::move(on_closed);
      closed();
    }
  }
  void AskAiOpenInChat(const std::string& thread_id) override { opened_in_chat.push_back(thread_id); }
  void AskAiSupport(const std::string& question) override { support.push_back(question); }
  void AskAiRunAction(const std::string&, const std::string&, int) override {}
  void AskAiOpenLink(const std::string&, const std::string&, int) override {}
  void AskAiOpenUrl(const std::string&) override {}
  bool AskAiIntroSeen() const override { return intro_seen; }
  void AskAiMarkIntroSeen() override {
    intro_seen = true;
    ++intro_marks;
  }
  void AskAiToast(const std::string& text) override { toasts.push_back(text); }
};

AgentEvent Loading(const bool loading, const std::string& thread = {}) {
  AgentEvent event;
  event.type = AgentEventType::LoadingChanged;
  event.loading = loading;
  event.thread_id = thread;
  return event;
}

AgentEvent Ready(const std::string& text, const std::string& thread = {}) {
  AgentEvent event;
  event.type = AgentEventType::AssistantReady;
  event.thread_id = thread;
  event.text = text;
  event.render_mode = RenderMode::Markdown;
  event.from_model = true;
  return event;
}

class AskAiPanelTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }

  AskAiPanelTest() {
    // An assistant that answers (no base_url: ready once the session exists).
    turn_host_.store.Initialize(BootstrapResult{});
    turn_host_.store.Mutable().config.llm.base_url.clear();
    turn_host_.panel = &panel_;
    agent_.SetUseLlm(true);
    AgentUiPorts ports;
    ports.has_session = [] { return true; };
    ports.can_start = [this](const std::string& thread_id) { return can_start_(thread_id); };
    ports.max_thread_turns = [this] { return max_turns_; };
    ports.submit = [this](const std::string& text, std::optional<std::string>) {
      home_submits_.push_back(text);
      return home_outcome_;
    };
    ports.start_new_home_conversation = [this] {
      ++new_home_conversations_;
      return home_free_;
    };
    ports.cancel = [this](const std::string& thread_id) { cancels_.push_back(thread_id); };
    ports.is_busy = [](const std::string&) { return true; };
    agent_.BindPorts(std::move(ports));
    // A started send makes that chat busy (ChatController::RunQueuedAgentSend → MarkAgentBusy).
    panel_host_.on_sent = [this](const std::string& thread_id) { agent_.MarkAgentBusy(thread_id); };
    turn_host_.on_started = [this](const std::string& thread_id) { agent_.MarkAgentBusy(thread_id); };
  }

  /** The shared chat's turn ends with (or without) an answer stored. */
  void EndSharedTurn(const std::string& thread_id, const std::optional<std::string>& answer) {
    if (answer) {
      panel_host_.messages[thread_id].push_back({false, *answer});
    }
    agent_.HandleAgentEvent(Loading(false, thread_id));
  }

  FakeTurnHost turn_host_;
  FakePanelHost panel_host_;
  ChatScreenState chat_;
  std::map<std::string, std::vector<std::string>> chat_links_;
  ChatWidgetHost widgets_;
  bool ws_active_ = false;
  ui::String ws_title_, ws_subtitle_, ws_rml_;
  TurnWidgetState ws_widget_;
  WorkingSetController working_set_{{ws_active_, ws_title_, ws_subtitle_, ws_rml_, ws_widget_}};
  ShellFeedbackPorts feedback_;
  AgentTurnController agent_{turn_host_, chat_, chat_links_, widgets_, working_set_, feedback_};
  AskAiPanel panel_{panel_host_, agent_};

  std::function<TurnStart(const std::string&)> can_start_ = [](const std::string&) { return TurnStart::Yes; };
  size_t max_turns_ = 3;
  std::vector<std::string> home_submits_;
  SubmitOutcome home_outcome_ = SubmitOutcome::Started;
  int new_home_conversations_ = 0;
  bool home_free_ = true;
  std::vector<std::string> cancels_;
};

// --- the entries -----------------------------------------------------------------------------------

TEST_F(AskAiPanelTest, EveryEntryOpensTheHalfStopSheetAndTheHomeFieldOpensItFull) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  ASSERT_EQ(panel_host_.sheets.size(), 1u);
  EXPECT_TRUE(panel_host_.sheets[0].page_sheet);
  EXPECT_TRUE(panel_host_.sheets[0].sheet_half_stop);
  EXPECT_EQ(panel_host_.sheets[0].sheet_detent, SheetDetent::Half) << "the chat stays in sight above it";
  EXPECT_EQ(panel_host_.sheets[0].key, "ask_ai");
  panel_host_.AskAiCloseSheet(0);

  ASSERT_TRUE(panel_.Open(AskAiEntry::HomeInput));
  EXPECT_EQ(panel_host_.sheets.back().sheet_detent, SheetDetent::Full) << "typing at once: full height, no jump";
  panel_host_.AskAiCloseSheet(0);

  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "今日要闻"));
  EXPECT_EQ(panel_host_.sheets.back().sheet_detent, SheetDetent::Half);
  panel_host_.AskAiCloseSheet(0);

  ASSERT_TRUE(panel_.OpenOnce("「发送崩溃报告」是什么意思？"));
  EXPECT_EQ(panel_host_.sheets.back().sheet_detent, SheetDetent::Half);
  EXPECT_EQ(panel_host_.sheets.size(), 4u);
}

TEST_F(AskAiPanelTest, TheDesktopHasNoPanel) {
  panel_host_.compact = false;
  EXPECT_FALSE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_FALSE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "hi"));
  EXPECT_FALSE(panel_.OpenOnce("q"));
  EXPECT_TRUE(panel_host_.sheets.empty());
  EXPECT_TRUE(panel_host_.sent.empty());
  EXPECT_TRUE(home_submits_.empty());
}

TEST_F(AskAiPanelTest, OpenWhileOpenPushesNoSecondSheet) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_EQ(panel_host_.sheets.size(), 1u);
}

// --- the settings "?": a one-off in the Home session --------------------------------------------------

TEST_F(AskAiPanelTest, TheSettingsQuestionIsAskedOnceInTheHomeSessionNotOfSupport) {
  const std::string question = "「发送崩溃报告」是什么意思？";
  ASSERT_TRUE(panel_.OpenOnce(question));
  EXPECT_EQ(new_home_conversations_, 1) << "nothing said before goes along";
  ASSERT_EQ(home_submits_.size(), 1u);
  EXPECT_EQ(home_submits_[0], question);
  EXPECT_TRUE(panel_host_.sent.empty()) << "not into the shared AI chat";
  EXPECT_TRUE(panel_host_.support.empty()) << "PP 客服 is only the button";
  EXPECT_TRUE(panel_.View().once);
  EXPECT_TRUE(panel_.View().thinking);
  ASSERT_EQ(panel_.View().rows.size(), 1u);
  EXPECT_TRUE(panel_.View().rows[0].user);
  EXPECT_FALSE(panel_.View().show_support) << "not while it answers";

  // The Home session answers: the reply shows in the panel and is stored in no chat.
  agent_.HandleAgentEvent(Loading(true));
  const int syncs = turn_host_.display_syncs;
  agent_.HandleAgentEvent(Ready("It sends a report when the app breaks."));
  EXPECT_EQ(turn_host_.display_syncs, syncs) << "FinishAssistantReply never ran: no chat stores it";
  agent_.HandleAgentEvent(Loading(false));
  ASSERT_EQ(panel_.View().rows.size(), 2u);
  EXPECT_FALSE(panel_.View().rows[1].user);
  EXPECT_NE(std::string(panel_.View().rows[1].content_rml.c_str()).find("sends a report"), std::string::npos);
  EXPECT_FALSE(panel_.View().thinking);
  EXPECT_TRUE(panel_.View().show_support);
  EXPECT_FALSE(agent_.Busy().Any());

  panel_.AskSupport();
  ASSERT_EQ(panel_host_.support.size(), 1u);
  EXPECT_EQ(panel_host_.support[0], question);
  EXPECT_EQ(panel_host_.closes, 1) << "the panel closes for the support chat";
}

TEST_F(AskAiPanelTest, TheSameQuestionMarkAgainSendsNothingTwice) {
  ASSERT_TRUE(panel_.OpenOnce("q1"));
  panel_host_.AskAiCloseSheet(0);
  ASSERT_TRUE(panel_.OpenOnce("q1"));
  EXPECT_EQ(home_submits_.size(), 1u);
  EXPECT_EQ(new_home_conversations_, 1);
}

TEST_F(AskAiPanelTest, AnotherQuestionMarkStartsAFreshOneOffOnceTheHomeSessionIsFree) {
  ASSERT_TRUE(panel_.OpenOnce("q1"));
  agent_.HandleAgentEvent(Loading(true));
  panel_host_.AskAiCloseSheet(0);
  home_free_ = false; // q1 still answering
  ASSERT_TRUE(panel_.OpenOnce("q2"));
  EXPECT_EQ(home_submits_.size(), 1u) << "waits for the earlier one-off";
  ASSERT_EQ(cancels_.size(), 1u);
  EXPECT_EQ(cancels_[0], "") << "the earlier one-off (Home session) is stopped";
  EXPECT_TRUE(panel_.View().thinking);
  panel_.Tick();
  EXPECT_EQ(cancels_.size(), 1u) << "stopped once, not every frame";
  agent_.HandleAgentEvent(Loading(false));
  home_free_ = true;
  panel_.Tick();
  ASSERT_EQ(home_submits_.size(), 2u);
  EXPECT_EQ(home_submits_[1], "q2");
  ASSERT_FALSE(panel_.View().rows.empty());
  EXPECT_EQ(panel_.View().rows.size(), 1u) << "q1 is gone from the panel";
}

TEST_F(AskAiPanelTest, AFollowUpStaysInTheOneOff) {
  ASSERT_TRUE(panel_.OpenOnce("q1"));
  agent_.HandleAgentEvent(Loading(true));
  agent_.HandleAgentEvent(Ready("a1"));
  agent_.HandleAgentEvent(Loading(false));
  panel_.SetDraftForTest("and then?");
  panel_.Send();
  EXPECT_EQ(new_home_conversations_, 1) << "a follow-up keeps the one-off's context";
  ASSERT_EQ(home_submits_.size(), 2u);
  EXPECT_EQ(home_submits_[1], "and then?");
  EXPECT_TRUE(panel_host_.sent.empty());
}

TEST_F(AskAiPanelTest, AOneOffWithNoAnswerSaysSoAndOffersSupport) {
  ASSERT_TRUE(panel_.OpenOnce("q1"));
  agent_.HandleAgentEvent(Loading(true));
  agent_.HandleAgentEvent(Loading(false)); // ended with no answer (no backend, timeout, …)
  EXPECT_TRUE(panel_.View().error);
  EXPECT_FALSE(panel_.View().thinking);
  EXPECT_TRUE(panel_.View().show_support);
  panel_.AskSupport();
  ASSERT_EQ(panel_host_.support.size(), 1u);
  EXPECT_EQ(panel_host_.support[0], "q1");
}

// --- the shared conversation -------------------------------------------------------------------------

TEST_F(AskAiPanelTest, EveryEntryAndOpenInAiChatUseTheOneSharedThread) {
  panel_host_.messages["ai-used-last"] = {{true, "earlier"}, {false, "earlier answer"}};
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_EQ(panel_.View().rows.size(), 2u) << "the conversation so far shows";
  panel_.SetDraftForTest("hello");
  panel_.Send();
  ASSERT_EQ(panel_host_.sent.size(), 1u);
  EXPECT_EQ(panel_host_.sent[0].thread_id, "ai-used-last");
  EXPECT_EQ(panel_host_.sent[0].text, "hello");
  EXPECT_TRUE(home_submits_.empty()) << "never the one-off's Home session";
  EXPECT_TRUE(panel_.Shows("ai-used-last"));
  panel_.OpenInChat();
  ASSERT_EQ(panel_host_.opened_in_chat.size(), 1u);
  EXPECT_EQ(panel_host_.opened_in_chat[0], "ai-used-last");
  EXPECT_FALSE(panel_.IsOpen());
}

TEST_F(AskAiPanelTest, WithNoAiChatYetTheFirstQuestionMakesOne) {
  panel_host_.shared_thread.clear();
  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "hi"));
  ASSERT_EQ(panel_host_.sent.size(), 1u);
  EXPECT_EQ(panel_host_.sent[0].thread_id, "ai-created");
  EXPECT_EQ(panel_.SharedThread(), "ai-created");
}

TEST_F(AskAiPanelTest, ClosingKeepsTheDraftAndTheRunningAnswerAndSendsNothingAgain) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  panel_.SetDraftForTest("q");
  panel_.Send();
  ASSERT_EQ(panel_host_.sent.size(), 1u);
  EXPECT_TRUE(panel_.View().thinking);
  panel_.SetDraftForTest("half typed");
  panel_host_.AskAiCloseSheet(0); // swiped down
  EXPECT_FALSE(panel_.IsOpen());
  EXPECT_TRUE(cancels_.empty()) << "the answer goes on";
  EXPECT_TRUE(agent_.Busy().BusyIn("ai-used-last"));

  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_EQ(std::string(panel_.View().draft.c_str()), "half typed");
  EXPECT_EQ(panel_host_.sent.size(), 1u) << "opening again sends nothing";
  EXPECT_TRUE(panel_.View().thinking);

  EndSharedTurn("ai-used-last", "the answer");
  EXPECT_FALSE(panel_.View().thinking);
  EXPECT_FALSE(panel_.View().error);
  ASSERT_EQ(panel_.View().rows.size(), 2u);
  EXPECT_FALSE(panel_.View().rows[1].user);
}

TEST_F(AskAiPanelTest, AGuestsOneTurnQueuesTheQuestionOnceAndShowsIt) {
  max_turns_ = 1;
  can_start_ = [](const std::string&) { return TurnStart::Full; }; // another chat answers
  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "今日要闻"));
  EXPECT_TRUE(panel_host_.sent.empty());
  EXPECT_EQ(std::string(panel_.View().queued_text.c_str()), "今日要闻");
  EXPECT_EQ(agent_.QueuedCount(), 1u);

  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "今日要闻")); // tapped again
  EXPECT_EQ(agent_.QueuedCount(), 1u) << "the same question is not queued twice";
  EXPECT_TRUE(panel_host_.toasts.empty());

  // The other chat's turn ends: the queued question goes out into the shared chat.
  can_start_ = [](const std::string&) { return TurnStart::Yes; };
  agent_.DrainQueue();
  ASSERT_EQ(turn_host_.handed_over.size(), 1u);
  EXPECT_EQ(turn_host_.handed_over[0].thread_id, "ai-used-last");
  panel_.Tick();
  EXPECT_EQ(std::string(panel_.View().queued_text.c_str()), "");
}

TEST_F(AskAiPanelTest, AnUnansweredQuestionShowsPlainWordsAndSupport) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  panel_.SetDraftForTest("why?");
  panel_.Send();
  EndSharedTurn("ai-used-last", std::nullopt); // timed out, no answer stored
  EXPECT_TRUE(panel_.View().error);
  EXPECT_TRUE(panel_.View().show_support);
  EXPECT_FALSE(panel_.View().thinking);
  panel_.AskSupport();
  ASSERT_EQ(panel_host_.support.size(), 1u);
  EXPECT_EQ(panel_host_.support[0], "why?");
}

TEST_F(AskAiPanelTest, NoAssistantSetUpIsAnErrorNotASilence) {
  agent_.SetUseLlm(false); // and no demo answers
  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "hi"));
  EXPECT_TRUE(panel_.View().error);
  EXPECT_TRUE(panel_.View().show_support);
  EXPECT_TRUE(panel_host_.sent.empty());

  ASSERT_TRUE(panel_.OpenOnce("q"));
  EXPECT_TRUE(panel_.View().error);
  EXPECT_TRUE(home_submits_.empty());
}

TEST_F(AskAiPanelTest, ThePrivacyCardShowsOnlyUntilGotIt) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_TRUE(panel_.View().show_intro);
  panel_.DismissIntro();
  EXPECT_FALSE(panel_.View().show_intro);
  EXPECT_EQ(panel_host_.intro_marks, 1);
  panel_host_.AskAiCloseSheet(0);
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_FALSE(panel_.View().show_intro);
}

// --- the demo app's made-up answers ----------------------------------------------------------------------

TEST_F(AskAiPanelTest, TheDemoStreamsAMadeUpOneOffAnswer) {
  agent_.SetUseLlm(false);
  turn_host_.messaging_ready = true;
  panel_.SetFakeAnswers([](const std::string&) { return std::string("Made-up answer, streamed."); });
  ASSERT_TRUE(panel_.OpenOnce("q"));
  EXPECT_TRUE(agent_.Busy().BusyIn(kAskAiOnceThread));
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  panel_.Tick(); // a part of it streams
  const std::string streamed = agent_.StreamingText(kAskAiOnceThread);
  EXPECT_FALSE(streamed.empty());
  EXPECT_EQ(std::string("Made-up answer, streamed.").rfind(streamed, 0), 0u) << streamed;
  for (int i = 0; i < 50 && agent_.Busy().BusyIn(kAskAiOnceThread); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    panel_.Tick();
  }
  EXPECT_FALSE(agent_.Busy().Any());
  ASSERT_EQ(panel_.View().rows.size(), 2u);
  EXPECT_NE(std::string(panel_.View().rows[1].content_rml.c_str()).find("Made-up answer"), std::string::npos);
  EXPECT_TRUE(panel_.View().show_support);
}

} // namespace
} // namespace pbr
