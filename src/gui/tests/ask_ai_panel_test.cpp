// The ask-AI sheet (DEV/PLAN-ui-batch5-ask-ai.md 5a): the real AskAiPanel and AgentTurnController, driven with a
// fake chat screen and fake agent ports. What is checked: each entry opens the sheet; the shared conversation is one
// thread for every entry and for "Open in AI chat"; closing keeps the draft and the running answer and sends nothing
// again; a guest's single turn queues the question once and shows it; no answer shows plain words and "Ask PP
// Support", which (like the privacy card's "?") reopens the sheet on the PP 客服 chat (the settings "?"'s mode, tested
// on the real page in ask_ai_support_render_test.cpp); the privacy card shows once. An answer with no chat to go in is
// never stored in the open chat.
#include "gui/chat/AskAiPanel.h"
#include "gui/chat/AgentTurnController.h"
#include "gui/SettingsController.h"
#include "gui/UserFeedback.h"
#include "gui/shell/ShellFeedback.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/AiImagePrep.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
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
  /** What went to the PP 客服 chat (the sheet reopened in its PP 客服 mode sends it there). */
  std::vector<std::string> support;
  bool support_opens = true; // false: PP 客服 cannot be reached (not configured)
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
  void AskAiLater(std::function<void()> fn) override { fn(); }
  void AskAiRunAction(const std::string&, const std::string&, int) override {}
  void AskAiOpenLink(const std::string&, const std::string&, int) override {}
  bool AskAiIntroSeen() const override { return intro_seen; }
  void AskAiMarkIntroSeen() override {
    intro_seen = true;
    ++intro_marks;
  }
  void AskAiToast(const std::string& text) override { toasts.push_back(text); }
  // The PP 客服 mode in short (it is tested on the real page in ask_ai_support_render_test.cpp).
  bool AskAiMessagingReady() const override { return true; }
  Roe<std::string> AskAiEnsureSupport() override {
    if (!support_opens) {
      return Error("Support is not available").WithUser(Tr("settings.support.unavailable"));
    }
    return std::string("support-thread");
  }
  Roe<void> AskAiSendToSupport(const std::string& thread_id, const std::string& text) override {
    support.push_back(text);
    messages[thread_id].push_back({true, text});
    return {};
  }
  std::vector<SupportMessage> AskAiSupportMessages(const std::string& thread_id, size_t) override {
    std::vector<SupportMessage> out;
    for (const auto& [user, text] : messages[thread_id]) {
      out.push_back({.own = user, .text = text, .at_ms = 0});
    }
    return out;
  }
  void AskAiSyncInbox() override {}
  bool AskAiAttentive() const override { return true; }
  void AskAiMarkRead(const std::string&) override {}
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
  ASSERT_TRUE(panel_.OpenSupport("「发送崩溃报告」是什么意思？"));
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
  EXPECT_FALSE(panel_.OpenSupport("q"));
  EXPECT_TRUE(panel_host_.sheets.empty());
  EXPECT_TRUE(panel_host_.sent.empty());
  EXPECT_TRUE(panel_host_.support.empty());
}

TEST_F(AskAiPanelTest, OpenWhileOpenPushesNoSecondSheet) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_EQ(panel_host_.sheets.size(), 1u);
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
  EXPECT_EQ(panel_host_.closes, 1);
  EXPECT_TRUE(panel_.View().support) << "opened again on PP 客服, not switched in place";
  ASSERT_EQ(panel_.View().rows.size(), 1u) << "only the question in the PP 客服 chat: no AI row stays";
  EXPECT_EQ(std::string(panel_.View().rows[0].content_rml.c_str()), "why?");
}

TEST_F(AskAiPanelTest, NoAssistantSetUpIsAnErrorNotASilence) {
  agent_.SetUseLlm(false); // and no demo answers
  ASSERT_TRUE(panel_.OpenAndAsk(AskAiEntry::HomeChip, "hi"));
  EXPECT_TRUE(panel_.View().error);
  EXPECT_TRUE(panel_.View().show_support);
  EXPECT_TRUE(panel_host_.sent.empty());
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

TEST_F(AskAiPanelTest, WhenPPSupportCannotBeReachedItsSheetSaysWhyAndSendsNothing) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  panel_host_.support_opens = false;
  panel_.AskSupportOwnAi();
  EXPECT_TRUE(panel_host_.support.empty());
  EXPECT_TRUE(panel_.IsOpen()) << "the sheet stays, saying why";
  EXPECT_TRUE(panel_.View().support);
  EXPECT_EQ(std::string(panel_.View().support_note.c_str()), Tr("settings.support.unavailable"));
  EXPECT_TRUE(panel_host_.toasts.empty()) << "said in the sheet, not in a pill over it";
}

std::string ReadSource(const std::string& relative);

TEST(AskAiPanelSourceTest, UnavailableSupportShowsOneSettingsWordingNotTheGenericPill) {
  std::string shown;
  ShellFeedbackPorts ports;
  ports.show_toast = [&shown](const std::string& text, ToastDuration) { shown = text; };
  UserFeedback::BindPorts(ports);
  const std::string app = ReadSource("src/app/Application.cpp");
  EXPECT_NE(app.find("CheckSupportConfigured(support_discovery_)"), std::string::npos);
  EXPECT_EQ(app.find("Error(Tr(\"support.unavailable\"))"), std::string::npos);
  const std::string check = ReadSource("src/feature/settings/SupportThread.cpp");
  EXPECT_NE(check.find("Error(\"Support is not available\").WithUser(Tr(\"settings.support.unavailable\"))"),
            std::string::npos);
  (void)UserFeedback::FailFrom(Error("Support is not available").WithUser(Tr("settings.support.unavailable")));
  UserFeedback::BindPorts({});
  EXPECT_EQ(shown, Tr("settings.support.unavailable"));
  EXPECT_NE(shown, Tr("error.generic"));
}

TEST_F(AskAiPanelTest, TheQuestionMarkClosesTheSheetAndAsksSupportOnceWithoutMarkingTheCardSeen) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  ASSERT_TRUE(panel_.View().show_intro);
  panel_.AskSupportOwnAi();
  EXPECT_EQ(panel_host_.closes, 1);
  ASSERT_EQ(panel_host_.support.size(), 1u);
  EXPECT_EQ(panel_host_.support[0], Tr("ask_ai.intro.support_question"));
  EXPECT_FALSE(panel_host_.support[0].empty());
  EXPECT_TRUE(panel_host_.sent.empty()) << "straight to PP Support, never to the AI";
  EXPECT_EQ(panel_host_.intro_marks, 0);
  EXPECT_FALSE(panel_host_.intro_seen);
  EXPECT_TRUE(panel_.View().support);
  EXPECT_FALSE(panel_.View().show_intro) << "no AI card on PP 客服's sheet";
  panel_host_.AskAiCloseSheet(0);
  ASSERT_TRUE(panel_.Open(AskAiEntry::ChatMenu));
  EXPECT_TRUE(panel_.View().show_intro) << "only \"Got it\" marks it seen";
}

std::string ReadSource(const std::string& relative); // below

TEST(AskAiPanelSourceTest, TheQuestionTextIsTheSameInBothLanguagesAndNeverEmpty) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("ask_ai.intro.support_question"), "怎么把 AI 换成我自己的？");
  EXPECT_EQ(Tr("ask_ai.intro.lead"), "和朋友聊天是私密的。");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("ask_ai.intro.support_question"), "How do I switch the AI to my own?");
  EXPECT_EQ(Tr("ask_ai.intro.lead"), "Chats with friends are private.");
}

// The card: a bold first sentence, the rest, then its own paragraph with the settings pages' "?" button.
TEST(AskAiPanelSourceTest, ThePrivacyCardHasABoldLeadTwoParagraphsAndAHelpQuestionMark) {
  const std::string view = ReadSource("assets/views/ask_ai.rml");
  const size_t card = view.find("<div class=\"ask-ai-intro\"");
  ASSERT_NE(card, std::string::npos);
  const std::string body = view.substr(card, view.find("ask-ai-intro-ok", card) - card);
  EXPECT_NE(body.find("<span class=\"ask-ai-intro-lead\">{{i18n:ask_ai.intro.lead}}</span>"), std::string::npos);
  EXPECT_NE(body.find("{{i18n:ask_ai.intro.body}}"), std::string::npos);
  EXPECT_NE(body.find("{{i18n:ask_ai.intro.own_ai}}"), std::string::npos);
  size_t paragraphs = 0;
  for (size_t at = body.find("<p "); at != std::string::npos; at = body.find("<p ", at + 1)) {
    ++paragraphs;
  }
  EXPECT_EQ(paragraphs, 2u);
  // The "?" is inline inside the same <p> as the sentence (right after it), not in a separate container.
  const size_t own_text = body.find("{{i18n:ask_ai.intro.own_ai}}");
  const size_t own_p_end = body.find("</p>", own_text);
  const size_t q_button = body.find("<button class=\"settings-q-btn ask-ai-intro-q\"");
  ASSERT_NE(q_button, std::string::npos);
  EXPECT_LT(q_button, own_p_end) << "inside the paragraph";
  EXPECT_EQ(body.substr(own_text + std::string("{{i18n:ask_ai.intro.own_ai}}").size(),
                        q_button - own_text - std::string("{{i18n:ask_ai.intro.own_ai}}").size()),
            "")
      << "directly after the text";
  EXPECT_EQ(body.find("ask-ai-intro-own-text"), std::string::npos);
  // Not bold: the second paragraph carries no lead span.
  EXPECT_LT(body.find("ask-ai-intro-lead"), body.find("{{i18n:ask_ai.intro.own_ai}}"));
  EXPECT_EQ(body.find("ask-ai-intro-lead", body.find("{{i18n:ask_ai.intro.own_ai}}")), std::string::npos);
  const size_t button = body.find("<button class=\"settings-q-btn ask-ai-intro-q\"");
  ASSERT_NE(button, std::string::npos);
  EXPECT_GT(button, body.find("{{i18n:ask_ai.intro.own_ai}}")) << "after the paragraph, at its end";
  const std::string tag = body.substr(button, body.find("</button>", button) - button);
  EXPECT_NE(tag.find("data-event-click=\"ask_ai_own_ai_help()\""), std::string::npos);
  EXPECT_NE(tag.find("class=\"settings-q-icon\" src=\"../icons/v2/help-circle.svg\""), std::string::npos);
  EXPECT_EQ(body.find("ask_ai_support()"), std::string::npos) << "not the failed-answer button";
  // The 44dp target and the small glyph are the settings button's own rules.
  const std::string css = ReadSource("assets/themes/components.rcss");
  EXPECT_NE(css.find(".settings-q-btn {\n  display: flex;"), std::string::npos);
}

TEST(AskAiPanelSourceTest, TheTesterPageCanShowThePrivacyCardAgain) {
  const std::string tester = ReadSource("assets/views/settings_section_tester.rml");
  EXPECT_NE(tester.find("data-event-click=\"show_ask_ai_intro_again()\""), std::string::npos);
  EXPECT_NE(tester.find("{{i18n:settings.tester.show_ask_ai_intro}}"), std::string::npos);
  const std::string controller = ReadSource("src/gui/SettingsController.cpp");
  EXPECT_NE(controller.find("\"show_ask_ai_intro_again\""), std::string::npos);
}

TEST(AskAiPanelSourceTest, TheTesterRowClearsTheSeenFlag) {
  namespace fs = std::filesystem;
  const fs::path root =
      fs::temp_directory_path() / ("pp_ask_ai_intro_" + std::to_string(reinterpret_cast<uintptr_t>(&root)));
  fs::remove_all(root);
  fs::create_directories(root / "profile");
  BootstrapResult bootstrap{};
  bootstrap.config = Config::DefaultAppConfig();
  bootstrap.config_path = (root / "config.json").string();
  bootstrap.data_dir = root.string();
  bootstrap.profile_data_dir = (root / "profile").string();
  bootstrap.profile_prefs = UserPreferences::DefaultProfile();
  SessionStore store;
  store.SetUiDemo(false);
  store.Initialize(std::move(bootstrap));
  ProfilePreferences prefs = store.Snapshot().profile_prefs;
  prefs.ai_panel_intro_seen = true;
  ASSERT_TRUE(static_cast<bool>(store.SaveProfilePrefs(prefs)));
  ASSERT_TRUE(store.Snapshot().profile_prefs.ai_panel_intro_seen);
  ASSERT_TRUE(static_cast<bool>(ClearAskAiIntroSeen(store)));
  EXPECT_FALSE(store.Snapshot().profile_prefs.ai_panel_intro_seen);
  fs::remove_all(root);
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
  EXPECT_NE(view.find("<p class=\"sheet-title\" data-if=\"!support\">{{i18n:ask_ai.title}}</p>"), std::string::npos);
  const size_t button = view.find("class=\"ask-ai-open-chat\"");
  ASSERT_NE(button, std::string::npos);
  const std::string tag = view.substr(button, view.find("</button>", button) - button);
  EXPECT_NE(tag.find("data-attr-title=\"open_chat_title\""), std::string::npos)
      << "one button, the controller picks the words";
  EXPECT_EQ(view.find("class=\"ask-ai-open-chat\"", button + 1), std::string::npos) << "only one such button";
  EXPECT_NE(tag.find("../icons/v2/arrow-up-right.svg"), std::string::npos);
  EXPECT_EQ(tag.find(">{{i18n:ask_ai.open_in_chat}}<"), std::string::npos) << "no text link any more";
}

TEST_F(AskAiPanelTest, TheControllerPicksTheModesWords) {
  ASSERT_TRUE(panel_.Open(AskAiEntry::HomeChip));
  EXPECT_EQ(std::string(panel_.View().open_chat_title.c_str()), Tr("ask_ai.open_in_chat"));
  EXPECT_EQ(std::string(panel_.View().privacy_text.c_str()), Tr("ask_ai.privacy"));
  EXPECT_EQ(std::string(panel_.View().placeholder_text.c_str()), Tr("chat.ai_thread.placeholder"));
  ASSERT_TRUE(panel_.OpenSupport(""));
  EXPECT_EQ(std::string(panel_.View().open_chat_title.c_str()), Tr("ask_ai.support.open_in_chat"));
  EXPECT_EQ(std::string(panel_.View().privacy_text.c_str()), Tr("ask_ai.support.privacy"));
  EXPECT_EQ(std::string(panel_.View().placeholder_text.c_str()), Tr("chat.composer.placeholder"));
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
  ASSERT_TRUE(panel_.OpenSupport("「发送崩溃报告」是什么意思？"));
  EXPECT_FALSE(panel_.View().show_media);
  EXPECT_FALSE(panel_.View().has_picture) << "a picture waiting in the shared mode is not shown here";
  panel_.PickPicture(true);
  EXPECT_EQ(captures_, 0);
  panel_.SetDraftForTest("and more?");
  panel_.Send();
  EXPECT_TRUE(sends_.empty()) << "PP 客服's chat is words only here";
  EXPECT_EQ(panel_host_.support.back(), "and more?");
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
