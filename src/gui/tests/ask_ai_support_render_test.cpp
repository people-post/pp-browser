// A settings "?" on a phone (DEV/PLAN-help-to-support-panel.md): the half-screen sheet IS the PP 客服 chat. The real
// ask_ai.rml with the real stylesheets over the panel's real data model; a fake chat screen holds the PP 客服 chat.
// What is checked, as drawn: the title is PP 客服 and nothing of the AI shows (no card, no "Ask PP Support", no plus);
// the "?"'s question goes out at once, once (a second tap or the sheet opened again sends nothing); a waiting line
// shows until a reply comes, after 90 s it says the reply will come here; a typed follow-up goes to the same chat;
// PP 客服 not configured or messaging not up yet says so in the sheet and sends nothing.
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/AgentTurnController.h"
#include "gui/chat/AskAiPanel.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellFeedback.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <ui/widgets/ElementFormControlTextArea.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& dimensions, const ui::String&) override {
    dimensions = {24, 24};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string Asset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

/** {{i18n:key}} filled with the current language's words, as the app's text view does. */
std::string Translated(std::string rml) {
  const std::regex key(R"(\{\{i18n:([^}]*)\}\})");
  std::smatch match;
  std::string out;
  while (std::regex_search(rml, match, key)) {
    out += match.prefix().str() + Tr(match[1].str());
    rml = match.suffix().str();
  }
  return out + rml;
}

/** The chat screen as the agent controller sees it: nothing of it is used by the PP 客服 mode. */
class FakeTurnHost : public AgentTurnHost {
public:
  SessionStore store;
  std::string active_thread;

  ConversationsFacade* Facade() const override { return nullptr; }
  bool MessagingIsReady() const override { return false; }
  bool MessagingInitialized() const override { return false; }
  bool MessagingReady() const override { return false; }
  const std::string& ActiveThreadId() const override { return active_thread; }
  ShellChromeSnapshot ChromeSnapshot() const override { return {}; }
  bool InAiComposerContext() const override { return true; }
  void SyncDisplayFromThread() override {}
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
  QueuedSendOutcome RunQueuedAgentSend(const QueuedAgentSend&) override { return QueuedSendOutcome::NotStarted; }
  void PutBackDraft(const std::string&, const std::string&) override {}
  void OnAgentTurnEnded(const std::string&) override {}
  bool AskAiPanelShows(const std::string&) const override { return false; }
};

constexpr int64_t kT0 = 1'700'000'000'000;
constexpr const char* kSupportThread = "support-thread";

/** The chat screen as the panel sees it, holding the PP 客服 chat. */
class FakeSupportHost : public AskAiPanelHost {
public:
  struct Message {
    bool own = false;
    std::string text;
    int64_t at_ms = 0;
    SendMark mark = SendMark::None;
  };
  int64_t* now = nullptr;
  bool ready = true;
  /** What finding the PP 客服 chat answers: its id, or the error (with the user's words) the app would give. */
  Roe<std::string> ensure = std::string(kSupportThread);
  int ensures = 0;
  std::map<std::string, std::vector<Message>> chats;
  std::vector<std::pair<std::string, std::string>> sent; // (thread, text)
  SendMark sent_mark = SendMark::None;                   // Queued / Failed: offline
  int fetches = 0;
  std::function<void()> on_closed;
  int sheets = 0;
  std::vector<std::string> opened_in_chat;

  bool AskAiCompact() const override { return true; }
  std::string AskAiSharedThread(bool) override { return "ai-used-last"; }
  std::string AskAiNewThread() override { return "ai-new"; }
  std::vector<AskAiRow> AskAiRecentRows(const std::string& thread_id, size_t) override {
    std::vector<AskAiRow> rows;
    int n = 0;
    for (const Message& message : chats[thread_id]) {
      const std::string rml = "<div class=\"bubble\"><p class=\"bubble-text\">" + message.text + "</p></div>";
      rows.push_back({("m" + std::to_string(++n)).c_str(), message.own, rml.c_str()});
    }
    return rows;
  }
  bool AskAiLastIsQuestion(const std::string&) override { return false; }
  QueuedSendOutcome AskAiSendNow(const QueuedAgentSend&) override { return QueuedSendOutcome::NotStarted; }
  std::string AskAiStoreQuestion(const std::string&, const std::string&) override { return {}; }
  int AskAiPushSheet(const PaneSpec&, std::function<void()> closed) override {
    on_closed = std::move(closed);
    return 50 + ++sheets;
  }
  void AskAiCloseSheet(int) override {
    if (on_closed) {
      auto closed = std::move(on_closed);
      closed();
    }
  }
  void AskAiOpenInChat(const std::string& thread_id) override { opened_in_chat.push_back(thread_id); }
  void AskAiSupport(const std::string&, std::function<void()>) override {}
  void AskAiRunAction(const std::string&, const std::string&, int) override {}
  void AskAiOpenLink(const std::string&, const std::string&, int) override {}
  void AskAiOpenUrl(const std::string&) override {}
  bool AskAiIntroSeen() const override { return false; } // the AI card would show in the AI mode
  void AskAiMarkIntroSeen() override {}
  void AskAiToast(const std::string&) override {}
  bool AskAiMessagingReady() const override { return ready; }
  Roe<std::string> AskAiEnsureSupport() override {
    ++ensures;
    return ensure;
  }
  Roe<void> AskAiSendToSupport(const std::string& thread_id, const std::string& text) override {
    sent.emplace_back(thread_id, text);
    chats[thread_id].push_back({.own = true, .text = text, .at_ms = *now, .mark = sent_mark});
    return {};
  }
  std::vector<SupportMessage> AskAiSupportMessages(const std::string& thread_id, size_t) override {
    std::vector<SupportMessage> out;
    for (const Message& message : chats[thread_id]) {
      out.push_back({.own = message.own, .text = message.text, .at_ms = message.at_ms, .mark = message.mark});
    }
    return out;
  }
  void AskAiSyncInbox() override { ++fetches; }
};

constexpr const char* kContext = "ask-ai-support-render-test";

class AskAiSupportRenderTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    turn_host_.store.Initialize(BootstrapResult{});
    host_.now = &now_;
    panel_.SetClockForTest([this] { return now_; });
    ui::SetRenderInterface(&render_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext(kContext, {390, 844});
    ASSERT_NE(context_, nullptr);
    context_->ActivateTheme("light", true);
    ASSERT_TRUE(panel_.Register(context_));
    const std::string rml = "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
                            Asset("themes/colors-light.rcss") +
                            "</style></head><body style=\"padding: 0;\"><div class=\"shell-layer-compact\">" +
                            Translated(Asset("views/ask_ai.rml")) + "</div></body></rml>";
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext(kContext);
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  /** The elements matching `selector` that are drawn (data-if hides with display: none). */
  std::vector<ui::Element*> Drawn(const std::string& selector) {
    context_->Update();
    ui::ElementList found;
    document_->QuerySelectorAll(found, selector);
    std::vector<ui::Element*> drawn;
    for (ui::Element* element : found) {
      bool shown = true;
      for (ui::Element* e = element; e && e != document_; e = e->GetParentNode()) {
        if (e->GetComputedValues().display() == ui::Style::Display::None) {
          shown = false;
          break;
        }
      }
      if (shown) {
        drawn.push_back(element);
      }
    }
    return drawn;
  }
  bool Shows(const std::string& selector) { return !Drawn(selector).empty(); }
  /** The text of the one drawn element matching `selector` ("" when none or several). */
  std::string TextOf(const std::string& selector) {
    const std::vector<ui::Element*> drawn = Drawn(selector);
    return drawn.size() == 1 ? std::string(drawn[0]->GetInnerRML().c_str()) : std::string();
  }
  size_t Bubbles() { return Drawn(".ask-ai-row").size(); }
  /** Types into the sheet's input, as the keyboard does (the field's binding fills the draft). */
  void Type(const std::string& text) {
    auto* input = ui_dynamic_cast<ui::ElementFormControlTextArea*>(document_->GetElementById("ask-ai-input"));
    ASSERT_NE(input, nullptr);
    input->SetValue(text.c_str());
    input->DispatchEvent(ui::EventId::Change, {{"value", ui::Variant(ui::String(text.c_str()))}});
    context_->Update();
  }
  void Tap(const std::string& selector) {
    const std::vector<ui::Element*> drawn = Drawn(selector);
    ASSERT_EQ(drawn.size(), 1u) << selector;
    drawn[0]->DispatchEvent(ui::EventId::Click, {});
    context_->Update();
  }
  /** One frame `ms` later. */
  void Later(const int64_t ms) {
    now_ += ms;
    panel_.Tick();
    context_->Update();
  }
  /** PP 客服 replies (stored, then the app's "messages changed"). */
  void Reply(const std::string& text) {
    host_.chats[kSupportThread].push_back({.own = false, .text = text, .at_ms = now_});
    panel_.OnMessagesChanged();
    context_->Update();
  }

  const std::string question_ = "「PIN 码」是什么意思？";
  int64_t now_ = kT0;
  NullRender render_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  FakeTurnHost turn_host_;
  FakeSupportHost host_;
  ChatScreenState chat_;
  std::map<std::string, std::vector<std::string>> chat_links_;
  ChatWidgetHost widgets_;
  bool ws_active_ = false;
  ui::String ws_title_, ws_subtitle_, ws_rml_;
  TurnWidgetState ws_widget_;
  WorkingSetController working_set_{{ws_active_, ws_title_, ws_subtitle_, ws_rml_, ws_widget_}};
  ShellFeedbackPorts feedback_;
  AgentTurnController agent_{turn_host_, chat_, chat_links_, widgets_, working_set_, feedback_};
  AskAiPanel panel_{host_, agent_};
};

// ① The sheet is PP 客服's: its title and privacy line; no AI card, no "Ask PP Support", no plus, no AI reminder.
TEST_F(AskAiSupportRenderTest, TheSheetIsPPSupportWithNothingOfTheAi) {
  ASSERT_TRUE(panel_.OpenSupport(question_));
  ASSERT_TRUE(panel_.IsOpen());
  EXPECT_EQ(TextOf(".sheet-title"), Tr("support.entry.title"));
  EXPECT_EQ(TextOf(".ask-ai-privacy"), Tr("ask_ai.support.privacy"));
  EXPECT_FALSE(Shows(".ask-ai-intro")) << "the AI privacy card is the AI's";
  EXPECT_FALSE(Shows(".ask-ai-support")) << "no \"Ask PP Support\" under it: this is PP 客服";
  EXPECT_FALSE(Shows(".ask-ai-section")) << "no one-off AI title";
  EXPECT_FALSE(Shows(".prompt-attach-btn"));
  EXPECT_FALSE(Shows(".ask-ai-reminder"));
  EXPECT_FALSE(Shows(".thinking:not(.ask-ai-support-waiting):not(.ask-ai-support-slow)")) << "no AI thinking";
  const std::vector<ui::Element*> open = Drawn(".ask-ai-open-chat");
  ASSERT_EQ(open.size(), 1u);
  EXPECT_EQ(std::string(open[0]->GetAttribute<ui::String>("title", "").c_str()), Tr("ask_ai.support.open_in_chat"));
  EXPECT_EQ(TextOf(".prompt-composer-label"), Tr("chat.composer.placeholder"));

  // "Open in chat": the PP 客服 chat in the full chat page.
  Tap(".ask-ai-open-chat");
  EXPECT_EQ(host_.opened_in_chat, std::vector<std::string>{kSupportThread});
  EXPECT_FALSE(panel_.IsOpen());
}

// ② The "?"'s question goes out at once into the PP 客服 chat, and the waiting line shows.
TEST_F(AskAiSupportRenderTest, TheQuestionGoesOutAtOnceAndTheWaitingLineShows) {
  ASSERT_TRUE(panel_.OpenSupport(question_));
  ASSERT_EQ(host_.sent.size(), 1u);
  EXPECT_EQ(host_.sent[0], std::make_pair(std::string(kSupportThread), question_));
  EXPECT_EQ(Bubbles(), 1u);
  EXPECT_EQ(TextOf(".ask-ai-support-waiting"), Tr("ask_ai.support.waiting"));
  EXPECT_FALSE(Shows(".ask-ai-support-slow"));
  EXPECT_FALSE(Shows(".ask-ai-support-note"));
  EXPECT_GE(host_.fetches, 1) << "the inbox is fetched right away";
}

// ③ The reply comes: its bubble shows and the waiting line goes.
TEST_F(AskAiSupportRenderTest, TheReplyShowsAndTheWaitingLineGoes) {
  ASSERT_TRUE(panel_.OpenSupport(question_));
  Later(12'000);
  ASSERT_TRUE(Shows(".ask-ai-support-waiting"));
  Reply("PIN 码是打开 PP 时输入的数字。");
  EXPECT_EQ(Bubbles(), 2u);
  EXPECT_FALSE(Shows(".ask-ai-support-waiting"));
  EXPECT_FALSE(Shows(".ask-ai-support-slow"));
}

// ④ No reply after 90 s: "got it, the reply comes here"; the bubble is not repeated, nothing is sent again, and it
// still listens (a late reply shows). The inbox is fetched about every 15 s until then, not after.
TEST_F(AskAiSupportRenderTest, AfterNinetySecondsItSaysTheReplyWillComeHere) {
  ASSERT_TRUE(panel_.OpenSupport(question_));
  const int fetched = host_.fetches;
  Later(15'000);
  EXPECT_EQ(host_.fetches, fetched + 1);
  Later(5'000);
  EXPECT_EQ(host_.fetches, fetched + 1) << "not every frame";
  for (int i = 0; i < 7; ++i) {
    Later(10'000);
  }
  EXPECT_EQ(TextOf(".ask-ai-support-slow"), Tr("ask_ai.support.slow"));
  EXPECT_FALSE(Shows(".ask-ai-support-waiting"));
  EXPECT_EQ(Bubbles(), 1u);
  EXPECT_EQ(host_.sent.size(), 1u);
  const int at_slow = host_.fetches;
  Later(30'000);
  EXPECT_EQ(host_.fetches, at_slow) << "the panel's own fetches stop at 90 s";
  Reply("Sorry for the wait.");
  EXPECT_EQ(Bubbles(), 2u);
  EXPECT_FALSE(Shows(".ask-ai-support-slow"));
}

// Risk 1: a second tap, or the sheet closed and opened again, sends nothing twice; a follow-up typed is sent.
TEST_F(AskAiSupportRenderTest, TheSameQuestionMarkAgainSendsNothingButATypedFollowUpGoes) {
  ASSERT_TRUE(panel_.OpenSupport(question_));
  ASSERT_TRUE(panel_.OpenSupport(question_)); // a second tap
  panel_.Close();
  Later(60'000);
  ASSERT_TRUE(panel_.OpenSupport(question_)); // closed and opened again
  EXPECT_EQ(host_.sent.size(), 1u);
  EXPECT_EQ(Bubbles(), 1u);
  EXPECT_TRUE(Shows(".ask-ai-support-waiting")) << "still waiting for that question's reply";

  Type("还有呢？");
  Tap(".prompt-send-btn:not(.prompt-send-btn--idle)");
  ASSERT_EQ(host_.sent.size(), 2u);
  EXPECT_EQ(host_.sent[1], std::make_pair(std::string(kSupportThread), std::string("还有呢？")));
  EXPECT_EQ(Bubbles(), 2u);
  EXPECT_EQ(std::string(panel_.View().draft.c_str()), "");
}

// Risk 4: offline, ours has not gone out: no "replying" line (its own clock / red mark tells), no 90 s line.
TEST_F(AskAiSupportRenderTest, AQuestionThatHasNotGoneOutShowsNoWaitingLine) {
  host_.sent_mark = SendMark::Queued;
  ASSERT_TRUE(panel_.OpenSupport(question_));
  EXPECT_EQ(Bubbles(), 1u);
  EXPECT_FALSE(Shows(".ask-ai-support-waiting"));
  Later(120'000);
  EXPECT_FALSE(Shows(".ask-ai-support-slow"));
}

// ⑤ PP 客服 is not configured: the sheet stays open and says so; nothing is sent.
TEST_F(AskAiSupportRenderTest, PPSupportNotConfiguredSaysSoAndSendsNothing) {
  host_.ensure = Error("Support is not available").WithUser(Tr("settings.support.unavailable"));
  ASSERT_TRUE(panel_.OpenSupport(question_));
  EXPECT_TRUE(panel_.IsOpen());
  EXPECT_EQ(TextOf(".ask-ai-support-note"), Tr("settings.support.unavailable"));
  EXPECT_TRUE(host_.sent.empty());
  EXPECT_FALSE(Shows(".ask-ai-empty")) << "no \"ask anything\" over the reason";
  EXPECT_FALSE(Shows(".ask-ai-support-waiting"));
  panel_.SetDraftForTest("hello?");
  panel_.Send();
  EXPECT_TRUE(host_.sent.empty());
  EXPECT_EQ(std::string(panel_.View().draft.c_str()), "hello?") << "the text stays typed";
}

// ⑥ Just unlocked, messaging not up: "getting ready", nothing sent; once up, asked once.
TEST_F(AskAiSupportRenderTest, MessagingNotUpYetGetsReadyThenAsksOnce) {
  host_.ready = false;
  ASSERT_TRUE(panel_.OpenSupport(question_));
  EXPECT_EQ(TextOf(".ask-ai-support-note"), Tr("ask_ai.support.preparing"));
  EXPECT_TRUE(host_.sent.empty());
  EXPECT_EQ(host_.ensures, 0);
  Later(500);
  EXPECT_TRUE(host_.sent.empty());
  host_.ready = true;
  Later(16);
  ASSERT_EQ(host_.sent.size(), 1u);
  EXPECT_EQ(host_.sent[0].second, question_);
  EXPECT_FALSE(Shows(".ask-ai-support-note"));
  EXPECT_TRUE(Shows(".ask-ai-support-waiting"));
  Later(16);
  EXPECT_EQ(host_.sent.size(), 1u);
}

TEST_F(AskAiSupportRenderTest, MessagingNeverUpSaysTryLaterNotUnavailable) {
  host_.ready = false;
  ASSERT_TRUE(panel_.OpenSupport(question_));
  Later(4'000);
  EXPECT_EQ(TextOf(".ask-ai-support-note"), Tr("ask_ai.support.preparing"));
  Later(1'500);
  EXPECT_EQ(TextOf(".ask-ai-support-note"), Tr("ask_ai.support.try_later"));
  EXPECT_NE(Tr("ask_ai.support.try_later"), Tr("settings.support.unavailable"));
  EXPECT_EQ(host_.ensures, 0);
  host_.ready = true;
  Later(16);
  EXPECT_TRUE(host_.sent.empty()) << "retried once only: the next \"?\" asks again";
}

} // namespace
} // namespace pbr
