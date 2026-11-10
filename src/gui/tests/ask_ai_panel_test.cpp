// The ask-AI sheet (DEV/PLAN-ui-batch5-ask-ai.md 5a): the real AskAiPanel and AgentTurnController, driven with a
// fake chat screen and fake agent ports. What is checked: each entry opens the sheet; the settings
// "?" is a one-off question in the Home session (never PP 客服, never the shared chat); the shared conversation is one
// thread for every entry and for "Open in AI chat"; closing keeps the draft and the running answer and sends nothing
// again; a guest's single turn queues the question once and shows it (the one-off too); no answer shows plain words
// and "Ask PP Support"; the privacy card shows once. A stopped one-off's late events (Home events carry no thread, only
// their turn) never end or answer for the next one, and an answer with no chat to go in is never stored in the open
// chat.
#include "gui/chat/AskAiPanel.h"
#include "gui/chat/AgentTurnController.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/AiImagePrep.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
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
  /** FinishAssistantReply ran (it alone syncs the chat list): an answer was stored in a chat. */
  int stored_replies = 0;
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
  void SyncShellSessions() override { ++stored_replies; }
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
  int new_threads = 0;
  std::string AskAiNewThread() override {
    shared_thread = "ai-new-" + std::to_string(++new_threads); // the newest chat is the most recent one
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

/** `turn`: the session turn it belongs to (AgentEvent::turn; 0 = not said). */
AgentEvent Loading(const bool loading, const std::string& thread = {}, const uint64_t turn = 0) {
  AgentEvent event;
  event.type = AgentEventType::LoadingChanged;
  event.loading = loading;
  event.thread_id = thread;
  event.turn = turn;
  return event;
}

AgentEvent Ready(const std::string& text, const std::string& thread = {}, const uint64_t turn = 0) {
  AgentEvent event;
  event.type = AgentEventType::AssistantReady;
  event.thread_id = thread;
  event.turn = turn;
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
    // The one-off goes to the Home session in words only (SubmitExplanation); each submit is a new turn there.
    ports.submit_home_explanation = [this](const std::string& text) {
      home_submits_.push_back(text);
      if (home_outcome_ == SubmitOutcome::Started) {
        ++home_turn_;
      }
      return home_outcome_;
    };
    ports.home_turn = [this] { return home_turn_; };
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
  uint64_t home_turn_ = 0;
  std::vector<std::string> cancels_;
};

// --- the entries -----------------------------------------------------------------------------------

TEST_F(AskAiPanelTest, EveryEntryOpensAPlainPageSheet) {
  // Every sheet opens at 4/5 (Kenneth 2026-11-04): the panel asks for no height of its own.
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  ASSERT_EQ(panel_host_.sheets.size(), 1u);
  EXPECT_TRUE(panel_host_.sheets[0].page_sheet);
  EXPECT_EQ(panel_host_.sheets[0].key, "ask_ai");
  panel_host_.AskAiCloseSheet(0);

  ASSERT_TRUE(panel_.Open(AskAiEntry::Dot));
  panel_host_.AskAiCloseSheet(0);
  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "你能做什么？"));
  panel_host_.AskAiCloseSheet(0);
  ASSERT_TRUE(panel_.OpenOnce("「发送崩溃报告」是什么意思？"));
  ASSERT_EQ(panel_host_.sheets.size(), 4u);
  for (const PaneSpec& spec : panel_host_.sheets) {
    EXPECT_TRUE(spec.page_sheet);
    EXPECT_EQ(spec.key, "ask_ai");
  }
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

TEST_F(AskAiPanelTest, AFailedQuestionMarkIsAskedAgain) {
  ASSERT_TRUE(panel_.OpenOnce("q1"));
  agent_.HandleAgentEvent(Loading(true, {}, 1));
  agent_.HandleAgentEvent(Loading(false, {}, 1)); // no answer
  ASSERT_TRUE(panel_.View().error);
  panel_host_.AskAiCloseSheet(0);
  ASSERT_TRUE(panel_.OpenOnce("q1")); // the same "?" again
  ASSERT_EQ(home_submits_.size(), 2u) << "a failed one is asked again";
  EXPECT_EQ(home_submits_[1], "q1");
  EXPECT_EQ(new_home_conversations_, 2);
  EXPECT_FALSE(panel_.View().error);
  EXPECT_TRUE(panel_.View().thinking);
}

// The review's interleaving: one-off A answers; "?" B stops it and starts once the Home session is free; A's late
// answer and end arrive after B has started. Neither is stored in the open chat (a chat with a person here), B is not
// ended or failed by them, and B's own answer reaches the panel.
TEST_F(AskAiPanelTest, AStoppedOneOffsLateEventsNeitherReachTheOpenChatNorEndTheNextOne) {
  turn_host_.active_thread = "chat-with-a-person";
  ASSERT_TRUE(panel_.OpenOnce("q-a"));
  ASSERT_EQ(home_turn_, 1u);
  agent_.HandleAgentEvent(Loading(true, {}, 1));

  home_free_ = false; // A still answers
  ASSERT_TRUE(panel_.OpenOnce("q-b"));
  ASSERT_EQ(cancels_.size(), 1u) << "A is stopped";
  EXPECT_EQ(home_submits_.size(), 1u) << "B waits until A's turn has really ended";
  home_free_ = true; // A's steps ended (its "ended" event is still on its way)
  panel_.Tick();
  ASSERT_EQ(home_submits_.size(), 2u);
  ASSERT_EQ(home_turn_, 2u);
  ASSERT_TRUE(agent_.Busy().BusyIn(kAskAiOnceThread));

  const int syncs = turn_host_.display_syncs;
  agent_.HandleAgentEvent(Ready("A's late answer", {}, 1));
  agent_.HandleAgentEvent(Loading(false, {}, 1));
  EXPECT_EQ(turn_host_.stored_replies, 0) << "nothing stored in any chat";
  EXPECT_EQ(turn_host_.display_syncs, syncs);
  EXPECT_TRUE(agent_.Busy().BusyIn(kAskAiOnceThread)) << "B still answers";
  EXPECT_TRUE(panel_.View().thinking);
  EXPECT_FALSE(panel_.View().error) << "B is not shown as failed";
  ASSERT_EQ(panel_.View().rows.size(), 1u) << "only B's question; A's answer is not B's";

  agent_.HandleAgentEvent(Loading(true, {}, 2));
  agent_.HandleAgentEvent(Ready("B's answer", {}, 2));
  agent_.HandleAgentEvent(Loading(false, {}, 2));
  EXPECT_EQ(turn_host_.stored_replies, 0) << "B's answer is the panel's only";
  EXPECT_FALSE(agent_.Busy().Any());
  EXPECT_FALSE(panel_.View().error);
  ASSERT_EQ(panel_.View().rows.size(), 2u);
  EXPECT_FALSE(panel_.View().rows[1].user);
  EXPECT_NE(std::string(panel_.View().rows[1].content_rml.c_str()).find("B's answer"), std::string::npos);
}

// Defence in depth: a Home answer whose turn the UI no longer knows (no chat it belongs to) is dropped, never stored
// in whichever chat is open.
TEST_F(AskAiPanelTest, AHomeAnswerWithNoChatIsNeverStoredInTheOpenChat) {
  turn_host_.active_thread = "chat-with-a-person";
  agent_.HandleAgentEvent(Ready("an answer nobody waits for"));
  EXPECT_EQ(turn_host_.stored_replies, 0);
  agent_.FinishAssistantReply("entry", "text", /*from_llm=*/true, {}, /*thread_id=*/{});
  EXPECT_EQ(turn_host_.stored_replies, 0);
}

TEST_F(AskAiPanelTest, AOneOffWaitsForAFreeTurnShownAsQueued) {
  max_turns_ = 1;                                                  // a guest
  can_start_ = [](const std::string&) { return TurnStart::Full; }; // another chat's answer takes the one turn
  ASSERT_TRUE(panel_.OpenOnce("q"));
  EXPECT_TRUE(home_submits_.empty()) << "the cap counts the one-off too";
  EXPECT_EQ(std::string(panel_.View().queued_text.c_str()), "q") << "shown as 'AI is busy, sent later'";
  EXPECT_FALSE(panel_.View().thinking);
  EXPECT_TRUE(panel_.View().rows.empty());
  panel_.Tick();
  EXPECT_TRUE(home_submits_.empty());

  can_start_ = [](const std::string&) { return TurnStart::Yes; }; // that answer ended
  panel_.Tick();
  ASSERT_EQ(home_submits_.size(), 1u);
  EXPECT_EQ(home_submits_[0], "q");
  EXPECT_EQ(std::string(panel_.View().queued_text.c_str()), "");
  EXPECT_TRUE(panel_.View().thinking);
  ASSERT_EQ(panel_.View().rows.size(), 1u);
  EXPECT_TRUE(panel_.View().rows[0].user);
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

TEST_F(AskAiPanelTest, TheDotOpensEmptyAndMakesNoChatUntilAQuestionIsSent) {
  panel_host_.messages["ai-used-last"] = {{true, "earlier"}, {false, "earlier answer"}};
  ASSERT_TRUE(panel_.Open(AskAiEntry::Dot));
  EXPECT_TRUE(panel_.View().rows.empty()) << "none of the last chat shows";
  EXPECT_EQ(panel_host_.new_threads, 0);
  panel_.Close();
  panel_host_.AskAiCloseSheet(0);
  EXPECT_EQ(panel_host_.new_threads, 0) << "no question, no chat";
}

TEST_F(AskAiPanelTest, TheDotsFirstQuestionStartsANewChatAndFollowUpsStayInIt) {
  panel_host_.messages["ai-used-last"] = {{true, "earlier"}, {false, "earlier answer"}};
  ASSERT_TRUE(panel_.Open(AskAiEntry::Dot));
  panel_.SetDraftForTest("first");
  panel_.Send();
  ASSERT_EQ(panel_host_.sent.size(), 1u);
  EXPECT_EQ(panel_host_.new_threads, 1);
  EXPECT_EQ(panel_host_.sent[0].thread_id, "ai-new-1");
  EXPECT_EQ(panel_host_.messages["ai-used-last"].size(), 2u) << "the earlier chat is untouched";
  EXPECT_TRUE(panel_.Shows("ai-new-1"));
  EndSharedTurn("ai-new-1", "answer");
  panel_.SetDraftForTest("second");
  panel_.Send();
  ASSERT_EQ(panel_host_.sent.size(), 2u);
  EXPECT_EQ(panel_host_.sent[1].thread_id, "ai-new-1");
  EXPECT_EQ(panel_host_.new_threads, 1);
  panel_.OpenInChat();
  ASSERT_EQ(panel_host_.opened_in_chat.size(), 1u);
  EXPECT_EQ(panel_host_.opened_in_chat[0], "ai-new-1");
}

TEST_F(AskAiPanelTest, ClosingAndTappingTheDotAgainStartsAnotherEmptyPanel) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::Dot));
  panel_.SetDraftForTest("first");
  panel_.Send();
  EndSharedTurn("ai-new-1", "answer");
  EXPECT_FALSE(panel_.View().rows.empty());
  panel_host_.AskAiCloseSheet(0); // runs on_closed itself (moved out: calling it again throws on MSVC)
  ASSERT_TRUE(panel_.Open(AskAiEntry::Dot));
  EXPECT_TRUE(panel_.View().rows.empty());
  panel_.SetDraftForTest("again");
  panel_.Send();
  EXPECT_EQ(panel_host_.new_threads, 2);
  EXPECT_EQ(panel_host_.sent.back().thread_id, "ai-new-2");
}

TEST_F(AskAiPanelTest, TheChatMenuStillContinuesTheMostRecentChat) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::Dot));
  panel_host_.AskAiCloseSheet(0); // runs on_closed itself (moved out: calling it again throws on MSVC)
  panel_host_.messages["ai-used-last"] = {{true, "earlier"}};
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_EQ(panel_.View().rows.size(), 1u);
  panel_.SetDraftForTest("hello");
  panel_.Send();
  EXPECT_EQ(panel_host_.sent.back().thread_id, "ai-used-last");
  EXPECT_EQ(panel_host_.new_threads, 0);
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
  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "你能做什么？"));
  EXPECT_TRUE(panel_host_.sent.empty());
  EXPECT_EQ(std::string(panel_.View().queued_text.c_str()), "你能做什么？");
  EXPECT_EQ(agent_.QueuedCount(), 1u);

  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "你能做什么？")); // tapped again
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

// --- what ChatController does with the panel (source checks: it needs the whole app to run) -------------

/** A source file of the repo with "\r" dropped (a Windows checkout has CRLF line ends). */
std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  std::erase(text, '\r');
  return text;
}

/** The text of a function from its definition line to the closing brace at column 0 ("" when not found). */
std::string Body(const std::string& source, const std::string& signature) {
  const size_t at = source.find("\n" + signature);
  const size_t close = at == std::string::npos ? at : source.find("\n}\n", at);
  return close == std::string::npos ? std::string() : source.substr(at, close + 3 - at);
}

// Kenneth: New chat's "AI chat" starts a new AI chat again; being the newest, it is the one the panel continues.
TEST(AskAiPanelSourceTest, NewChatsAiChatStartsANewAiChat) {
  const std::string header = ReadSource("src/gui/chat/ChatController.h");
  ASSERT_FALSE(header.empty());
  EXPECT_NE(header.find("void StartNewAiChat() { OnNewChat(); }"), std::string::npos);
  EXPECT_EQ(ReadSource("src/gui/chat/ChatController.cpp").find("void ChatController::StartNewAiChat("),
            std::string::npos)
      << "no phone-only path to the shared chat";
}

// Kenneth 2026-11-04: Today's headlines goes to the AI chat page again (users follow up on the news there); only Get
// started and What can you do open the panel.
TEST(AskAiPanelSourceTest, OnlyGetStartedAndCapabilitiesChipsOpenThePanel) {
  const std::string home = ReadSource("assets/views/home.rml");
  ASSERT_FALSE(home.empty());
  EXPECT_NE(home.find("send_suggestion_in_chat('{{i18n:home.suggestion.headlines_prompt}}')"), std::string::npos);
  EXPECT_NE(home.find("send_suggestion('{{i18n:home.suggestion.get_started_prompt}}')"), std::string::npos);
  EXPECT_NE(home.find("send_suggestion('{{i18n:home.suggestion.capabilities_prompt}}')"), std::string::npos);
  const std::string source = ReadSource("src/gui/chat/ChatController.cpp");
  const size_t at = source.find("BindEventCallback(\"send_suggestion_in_chat\"");
  ASSERT_NE(at, std::string::npos);
  const std::string handler = source.substr(at, source.find("}));", at) - at);
  EXPECT_NE(handler.find("SendUserText(text, std::nullopt, /*from_home_chip=*/true)"), std::string::npos);
  EXPECT_EQ(handler.find("ask_ai_"), std::string::npos) << "no panel on this path";
}

// The panel's title row (Kenneth 2026-11-04): the sheets' centred title, "Open in AI chat" an icon button left of
// the X, labelled with the same words.
TEST(AskAiPanelSourceTest, OpenInAiChatIsAnIconInTheSheetHeader) {
  const std::string view = ReadSource("assets/views/ask_ai.rml");
  ASSERT_FALSE(view.empty());
  EXPECT_NE(view.find("<div class=\"sheet-header sheet-header--two-buttons\">"), std::string::npos);
  EXPECT_NE(view.find("<p class=\"sheet-title\">{{i18n:ask_ai.title}}</p>"), std::string::npos);
  const size_t button = view.find("class=\"ask-ai-open-chat\"");
  ASSERT_NE(button, std::string::npos);
  const std::string tag = view.substr(button, view.find("</button>", button) - button);
  EXPECT_NE(tag.find("title=\"{{i18n:ask_ai.open_in_chat}}\""), std::string::npos);
  EXPECT_NE(tag.find("../icons/v2/arrow-up-right.svg"), std::string::npos);
  EXPECT_EQ(tag.find(">{{i18n:ask_ai.open_in_chat}}<"), std::string::npos) << "no text link any more";
}

// --- the input row (5b): camera, attachment, Send ----------------------------------------------------------------

/** The panel's picture ports replaced by recorders: a pick/capture hands over "pick.jpg" at once, prepare answers at once. */
class AskAiInputRowTest : public AskAiPanelTest {
protected:
  AskAiInputRowTest() {
    AskAiImagePorts& ports = panel_.ImagePorts();
    ports.pick = [this](std::function<void(std::string)> picked) {
      ++picks_;
      picked("pick.jpg");
    };
    ports.capture = [this](std::function<void(std::string)> picked) {
      ++captures_;
      picked("shot.jpg");
    };
    ports.prepare = [this](std::string path, std::function<void(Roe<AgentTurnController::PendingAiImage>)> done) {
      prepared_paths_.push_back(path);
      if (prepare_error_) {
        done(Error(static_cast<int32_t>(*prepare_error_), "x"));
        return;
      }
      AgentTurnController::PendingAiImage image;
      image.image.mime = "image/jpeg";
      image.image.data = {1, 2, 3};
      image.file_path = "thumb-" + std::to_string(prepared_paths_.size()) + ".jpg";
      done(std::move(image));
    };
    ports.send = [this](const std::string& thread_id, const std::string& text,
                        AgentTurnController::PendingAiImage image) {
      sends_.push_back({thread_id, text, image.file_path});
      return send_result_;
    };
    ports.drop = [this](const AgentTurnController::PendingAiImage& image) { dropped_.push_back(image.file_path); };
  }

  struct Send {
    std::string thread_id, text, file;
  };
  int picks_ = 0;
  int captures_ = 0;
  std::vector<std::string> prepared_paths_;
  std::optional<AiImagePrepError> prepare_error_;
  std::vector<Send> sends_;
  std::vector<std::string> dropped_;
  AgentTurnController::ImageSend send_result_ = AgentTurnController::ImageSend::Started;
};

TEST_F(AskAiInputRowTest, APickedPictureShowsAThumbnailThatCanBeRemoved) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_TRUE(panel_.View().show_media);
  EXPECT_FALSE(panel_.View().has_picture);
  panel_.PickPicture(/*camera=*/false);
  EXPECT_EQ(picks_, 1);
  EXPECT_EQ(prepared_paths_, std::vector<std::string>{"pick.jpg"});
  EXPECT_TRUE(panel_.View().has_picture);
  EXPECT_FALSE(panel_.View().picture_preparing);
  EXPECT_EQ(std::string(panel_.View().picture_src.c_str()), "thumb-1.jpg");
  // A second pick replaces the first (its thumbnail file goes).
  panel_.PickPicture(/*camera=*/true);
  EXPECT_EQ(captures_, 1);
  EXPECT_EQ(dropped_, std::vector<std::string>{"thumb-1.jpg"});
  EXPECT_EQ(std::string(panel_.View().picture_src.c_str()), "thumb-2.jpg");
  panel_.RemovePicture();
  EXPECT_FALSE(panel_.View().has_picture);
  EXPECT_TRUE(std::string(panel_.View().picture_src.c_str()).empty());
  EXPECT_EQ(dropped_.back(), "thumb-2.jpg");
  panel_.Send();
  EXPECT_TRUE(sends_.empty()) << "nothing to send";
}

TEST_F(AskAiInputRowTest, ANonImageOrTooLargePictureSaysWhyAndShowsNoThumbnail) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  prepare_error_ = AiImagePrepError::TooLarge; // the existing 1.5 MB path refused it
  panel_.PickPicture(false);
  EXPECT_FALSE(panel_.View().has_picture);
  ASSERT_EQ(panel_host_.toasts.size(), 1u);
  EXPECT_EQ(panel_host_.toasts[0], Tr("chat.image.too_large"));
  prepare_error_ = AiImagePrepError::NotAnImage;
  panel_.PickPicture(false);
  EXPECT_EQ(panel_host_.toasts.back(), Tr("chat.image.not_supported"));
}

TEST_F(AskAiInputRowTest, APictureQuestionGoesToTheSharedThread) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  panel_.PickPicture(false);
  panel_.SetDraftForTest("What is this?");
  panel_.Send();
  ASSERT_EQ(sends_.size(), 1u);
  EXPECT_EQ(sends_[0].thread_id, "ai-used-last");
  EXPECT_EQ(sends_[0].text, "What is this?");
  EXPECT_EQ(sends_[0].file, "thumb-1.jpg");
  EXPECT_TRUE(panel_host_.sent.empty()) << "the picture path, not the text queue";
  EXPECT_FALSE(panel_.View().has_picture);
  EXPECT_TRUE(std::string(panel_.View().draft.c_str()).empty());
}

TEST_F(AskAiInputRowTest, APictureAloneIsSentWithoutText) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  panel_.PickPicture(true);
  panel_.Send();
  ASSERT_EQ(sends_.size(), 1u);
  EXPECT_TRUE(sends_[0].text.empty()) << "the controller puts the default question in";
}

TEST_F(AskAiInputRowTest, AFullOrBusyThreadKeepsThePictureAndTheText) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  panel_.PickPicture(false);
  panel_.SetDraftForTest("again");
  send_result_ = AgentTurnController::ImageSend::Full;
  panel_.Send();
  EXPECT_TRUE(panel_.View().has_picture);
  EXPECT_EQ(std::string(panel_.View().draft.c_str()), "again");
  EXPECT_EQ(panel_host_.toasts.back(), Tr("chat.ai_queue.image_busy"));
  send_result_ = AgentTurnController::ImageSend::BusyHere;
  panel_.Send();
  EXPECT_EQ(panel_host_.toasts.back(), Tr("chat.ai_queue.image_busy_here"));
  send_result_ = AgentTurnController::ImageSend::Unavailable;
  panel_.Send();
  EXPECT_TRUE(panel_.View().error) << "no assistant: the error state and the support button";
  EXPECT_TRUE(panel_.View().has_picture) << "the buttons stay";
}

TEST_F(AskAiInputRowTest, TheSettingsQuestionMarkHasNoCameraNorAttachmentAndNoPicture) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  panel_.PickPicture(false);
  ASSERT_TRUE(panel_.View().has_picture);
  ASSERT_TRUE(panel_.OpenOnce("「发送崩溃报告」是什么意思？"));
  EXPECT_FALSE(panel_.View().show_media);
  EXPECT_FALSE(panel_.View().has_picture) << "a picture waiting in the shared mode is not shown here";
  panel_.PickPicture(true);
  EXPECT_EQ(captures_, 0);
  panel_.SetDraftForTest("and more?");
  panel_.Send();
  EXPECT_TRUE(sends_.empty()) << "a one-off is words only";
  panel_host_.AskAiCloseSheet(0);
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_TRUE(panel_.View().show_media);
  EXPECT_TRUE(panel_.View().has_picture) << "the shared mode's picture is still waiting";
}

TEST_F(AskAiInputRowTest, ThePanelPreparesThroughTheChatsOneAndAHalfMegabytePath) {
  // The panel adds no size rule of its own: it prepares with PrepareAiImageFromFile's defaults (<= 1.5 MB, long edge
  // 2048, metadata dropped) through AgentTurnController::PrepareAiImage.
  const std::string header = ReadSource("src/foundation/platform/AiImagePrep.h");
  EXPECT_NE(header.find("size_t max_bytes = 1536u * 1024u"), std::string::npos);
  const std::string controller = ReadSource("src/gui/chat/AgentTurnController.cpp");
  EXPECT_NE(Body(controller, "Roe<PreparedDraft> PrepareAiImageDraft(").find("PrepareAiImageFromFile(path)"),
            std::string::npos);
  EXPECT_NE(Body(controller, "void AgentTurnController::PrepareAiImage(").find("PrepareAiImageDraft(path"),
            std::string::npos);
  const std::string panel = ReadSource("src/gui/chat/AskAiPanel.cpp");
  EXPECT_EQ(panel.find("1536"), std::string::npos);
}

// Kenneth 2026-11-04 ("风格不统一…一会儿在左边，一会儿在右边"), 2026-11-07 (one composer everywhere): the chat
// composer's row, its classes and icons: the pill [input … bold plus], the emoji key at its right end, then Send; no paperclip, no
// camera of its own (the plus card has it).
TEST(AskAiInputRowSourceTest, TheRowIsTheChatComposersPillThenSend) {
  const std::string rml = ReadSource("assets/views/ask_ai.rml");
  const size_t emoji = rml.find("ask_ai_emoji()");
  const size_t pill = rml.find("class=\"prompt-composer-pill\"");
  const size_t input = rml.find("id=\"ask-ai-input\"");
  const size_t attach = rml.find("ask_ai_attach()");
  const size_t send = rml.find("ask_ai_send()");
  ASSERT_NE(emoji, std::string::npos);
  ASSERT_NE(pill, std::string::npos);
  ASSERT_NE(input, std::string::npos);
  ASSERT_NE(attach, std::string::npos);
  ASSERT_NE(send, std::string::npos);
  EXPECT_LT(pill, input);
  EXPECT_LT(input, attach);
  EXPECT_LT(attach, emoji);
  EXPECT_LT(emoji, send);
  EXPECT_EQ(rml.find("ask_ai_camera()"), std::string::npos);
  EXPECT_EQ(rml.find("paperclip.svg"), std::string::npos);
  const auto tag_of = [&rml](const size_t at) {
    return rml.substr(rml.rfind("<button", at), at - rml.rfind("<button", at));
  };
  EXPECT_NE(tag_of(emoji).find("class=\"prompt-emoji-btn\""), std::string::npos);
  EXPECT_NE(tag_of(attach).find("class=\"prompt-attach-btn\""), std::string::npos);
  EXPECT_NE(tag_of(send).find("class=\"prompt-send-btn\""), std::string::npos);
  // The plus only where media shows; Send once there is something to send.
  EXPECT_NE(tag_of(attach).find("data-if=\"show_media\""), std::string::npos);
  EXPECT_NE(tag_of(send).find("data-if=\"draft != '' || has_picture\""), std::string::npos);
  // The same icons as the chat's.
  const std::string composer = ReadSource("assets/views/composer.rml");
  for (const char* icon : {"<svg class=\"prompt-icon-v2\" src=\"../icons/v2/emoji.svg\" width=\"20\" height=\"20\">",
                           "src=\"../icons/v2/plus-bold.svg\" width=\"24\" height=\"24\">",
                           "<span class=\"prompt-send-disc\"><svg src=\"../icons/send.svg\" width=\"14\" height=\"14\" "
                           "crop-to-content=\"true\">"}) {
    EXPECT_NE(rml.find(icon), std::string::npos) << icon;
    EXPECT_NE(composer.find(icon), std::string::npos) << icon;
  }
}

// Kenneth 2026-11-04: the microphone is for voice messages only; dictation is the input method's own business, and
// the panel opens with the cursor already in the field. No microphone, no hint line, no per-platform hints.
TEST(AskAiInputRowSourceTest, ThePanelHasNoMicrophone) {
  const std::string rml = ReadSource("assets/views/ask_ai.rml");
  for (const char* gone : {"mic.svg", "ask_ai_mic", "mic_hint", "show_mic", "ask-ai-mic"}) {
    EXPECT_EQ(rml.find(gone), std::string::npos) << gone;
  }
  std::string panel = ReadSource("src/gui/chat/AskAiPanel.cpp") + ReadSource("src/gui/chat/AskAiPanel.h");
  for (size_t at = panel.find("dynamic_cast"); at != std::string::npos; at = panel.find("dynamic_cast", at)) {
    panel.erase(at, std::string("dynamic_cast").size()); // not a microphone
  }
  EXPECT_EQ(panel.find("Mic"), std::string::npos);
  EXPECT_EQ(panel.find("mic_"), std::string::npos);
  for (const char* locale : {"assets/locales/en.json", "assets/locales/zh-Hans.json"}) {
    EXPECT_EQ(ReadSource(locale).find("\"ask_ai.mic"), std::string::npos) << locale;
  }
  EXPECT_EQ(ReadSource("assets/themes/components.rcss").find(".ask-ai-mic"), std::string::npos);
}

// A permission card tapped in the panel belongs to the panel's AI chat: a stale one is rewritten there, never in the
// chat open under the panel.
TEST(AskAiPanelSourceTest, APanelCardIsAnsweredInItsOwnChat) {
  const std::string source = ReadSource("src/gui/chat/ChatController.cpp");
  const std::string run = Body(source, "void ChatController::AskAiRunAction(");
  ASSERT_FALSE(run.empty());
  EXPECT_NE(run.find("HandleLocalActionIn(thread_id, action.message"), std::string::npos);
  EXPECT_EQ(run.find("HandleLocalAction(action"), std::string::npos);
  const std::string handle = Body(source, "void ChatController::HandleLocalActionIn(");
  ASSERT_FALSE(handle.empty());
  EXPECT_NE(handle.find("CloseApprovalCard(thread_id,"), std::string::npos);
  EXPECT_EQ(handle.find("CloseApprovalCard(ActiveThreadId()"), std::string::npos);
}

} // namespace
} // namespace pbr
