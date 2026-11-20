// Every way out of the recovery phrase page ends with the words gone: nothing bound, the private copy cleared. The real
// SettingsController (phone layout, a fake local-back stack standing in for the shell's) with its real events, the PIN
// gate stubbed to say yes at once. Also: the "seen" flag is set by the reveal, not by the covered page opening.
#include "common/runtime/AppRuntime.h"
#include "feature/settings/SettingsCommands.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/runtime/AppLifecycle.h"
#include "gui/CallController.h"
#include "gui/SettingsController.h"
#include "gui/tests/CallControllerTestAccess.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/UiEditSession.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class QuietSystem : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return 0.0; }
};

constexpr const char* kContext = "recovery-phrase-exits-test";
const std::vector<std::string> kWords = {"alpha",  "bravo",    "charlie", "delta",  "echo",    "foxtrot",
                                         "golf",   "hotel",    "india",   "juliet", "kilo",    "lima",
                                         "mike",   "november", "oscar",   "papa",   "quebec",  "romeo",
                                         "sierra", "tango",    "uniform", "victor", "whiskey", "xray"};

class RecoveryPhraseExitsTest : public testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    while (UiEditSession::Instance().RemountBlocking()) {
      UiEditSession::Instance().EndRemount();
    }
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render);
    ui::SetSystemInterface(&system);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext(kContext, {400, 800});
    ASSERT_NE(context, nullptr);

    root = std::filesystem::temp_directory_path() /
           ("pp_recovery_exits_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "profile");
    BootstrapResult bootstrap{};
    bootstrap.config = Config::DefaultAppConfig();
    bootstrap.config_path = (root / "config.json").string();
    bootstrap.data_dir = root.string();
    bootstrap.profile_data_dir = (root / "profile").string();
    bootstrap.profile_prefs = UserPreferences::DefaultProfile();
    bootstrap.profile_prefs.pin_is_default = false;
    store.SetUiDemo(false);
    store.Initialize(std::move(bootstrap));

    std::string phrase;
    for (const std::string& word : kWords) {
      phrase += (phrase.empty() ? "" : " ") + word;
    }
    commands.session_store = [this]() -> SessionStore& { return store; };
    commands.recovery_phrase = [phrase](const SecretText&) -> Roe<std::optional<SecretText>> {
      return std::optional<SecretText>(SecretText(phrase));
    };
    settings.BindCommands(commands);
    settings.BindShellFeedback(ports);
    UserFeedback::BindPorts(ports);
    PinPromptPorts pin;
    pin.show_verify = [](const PinVerifyPurpose, PinGateCheck check, std::function<void()> on_ok) {
      check(SecretText("1234"), [on_ok = std::move(on_ok)](Roe<void>) { on_ok(); });
    };
    settings.BindPinPrompt(pin);
    BindNavigation();
    SettingsHost& host = settings;
    host.UiState().show_advanced_settings = "off";
    host.UiState().show_tester_options = "off";
    host.UiState().backend_env = "prod";
    host.PushToView(false);
    ASSERT_TRUE(settings.RegisterModel(context));
    document = context->LoadDocumentFromMemory(
        "<rml><body><div data-model=\"settings\">"
        "<button id=\"toggle_adv\" data-event-click=\"toggle_advanced_settings()\">x</button>"
        "<button id=\"sel_sec\" data-event-click=\"select_section('security')\">x</button>"
        "<button id=\"sel_dev\" data-event-click=\"select_section('devices')\">x</button>"
        "<button id=\"show\" data-event-click=\"show_recovery_phrase()\">x</button>"
        "<button id=\"reveal\" data-event-click=\"reveal_recovery_phrase()\">x</button>"
        "<button id=\"done\" data-event-click=\"hide_recovery_phrase()\">x</button>"
        "<button id=\"close_sub\" data-event-click=\"close_security_page()\">x</button>"
        "<button id=\"open_pin\" data-event-click=\"open_security_page('pin')\">x</button>"
        "<p id=\"sp\">{{security_page}}</p>"
        "</div></body></rml>");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    settings.SyncLayoutMode(); // the phone layout, as the shell reports it
    Click("toggle_adv");       // builds the section list (the Help switch is the one public way here)
    Click("sel_sec");          // Account & security, as a phone opens it
    ASSERT_TRUE(settings.ShowsSecuritySection());
  }
  void TearDown() override {
    AppLifecycle::OnDidEnterForeground();
    settings.BindShellNavigation({});
    UserFeedback::BindPorts({});
    settings.BindShellFeedback({});
    settings.BindPinPrompt({});
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext(kContext);
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
    AppRuntime::Shutdown();
    std::filesystem::remove_all(root);
  }

  void BindNavigation() {
    ShellNavigationPorts nav;
    nav.snapshot = [this]() {
      ShellChromeSnapshot chrome;
      chrome.layout_mode = layout_mode;
      chrome.nav_tab = NavTab::Me;
      chrome.settings_detail_primary = settings_pane_primary;
      return chrome;
    };
    nav.push_local_back = [this](const std::string& id, std::function<void()> commit) {
      back_stack.push_back({id, std::move(commit)});
    };
    nav.has_local_back = [this](const std::string& id) {
      for (const auto& entry : back_stack) {
        if (entry.first == id) {
          return true;
        }
      }
      return false;
    };
    nav.clear_local_back = [this](const std::string& id) {
      for (size_t i = back_stack.size(); i-- > 0;) {
        if (back_stack[i].first == id) {
          back_stack.erase(back_stack.begin() + static_cast<long>(i));
        }
      }
    };
    // The arrow, the system back and the edge swipe all end here: the top entry's commit.
    nav.request_dismiss_instant = [this]() {
      if (back_stack.empty()) {
        return false;
      }
      auto commit = std::move(back_stack.back().second);
      back_stack.pop_back();
      commit();
      return true;
    };
    nav.set_secret_page_open = [this](const bool open) { secret_page_open = open; };
    settings.BindShellNavigation(std::move(nav));
  }
  void Settle() {
    AppRuntime::DrainWorkersThenUI();
    AppRuntime::RunUITasks();
    context->Update();
    AppRuntime::RunUITasks();
    context->Update();
  }
  void Click(const char* id) {
    document->GetElementById(id)->DispatchEvent(ui::EventId::Click, {});
    context->Update();
    Settle();
  }
  std::string Text(const char* id) { return document->GetElementById(id)->GetInnerRML().c_str(); }
  SettingsSecurityActions& Actions() { return settings.SecurityActionsForTest(); }
  /** Opens the covered page through the real row action (PIN stubbed). */
  void Open() {
    Click("show");
    ASSERT_EQ(Text("sp"), "recovery");
    ASSERT_TRUE(Actions().RecoveryPhraseShown());
    ASSERT_TRUE(secret_page_open);
    ASSERT_EQ(Actions().RecoverySecretCountForTest(), 24u) << "covered: the words wait in the private copy";
  }
  void OpenAndReveal() {
    Open();
    Click("reveal");
    ASSERT_EQ(Actions().RecoveryWordsForTest(), kWords);
    ASSERT_EQ(Actions().RecoverySecretCountForTest(), 0u) << "revealed: the private copy is already cleared";
  }
  void ExpectGone() {
    Settle();
    EXPECT_FALSE(Actions().RecoveryPhraseShown());
    EXPECT_TRUE(Actions().RecoveryWordsForTest().empty()) << "no word stays bound";
    EXPECT_EQ(Actions().RecoverySecretCountForTest(), 0u) << "the private copy is cleared";
    EXPECT_EQ(Text("sp"), "") << "the page is closed";
    EXPECT_FALSE(secret_page_open);
  }
  void ExpectGoneWhenRevealedOrCovered(const std::function<void()>& exit) {
    for (const bool reveal : {false, true}) {
      SCOPED_TRACE(reveal ? "revealed" : "covered");
      if (!settings.ShowsSecuritySection()) {
        Click("sel_sec"); // an exit that left the section: back to it
      }
      if (reveal) {
        OpenAndReveal();
      } else {
        Open();
      }
      exit();
      ExpectGone();
      back_stack.clear();
    }
  }

  NullRender render;
  QuietSystem system;
  ShellFeedbackPorts ports;
  SessionStore store;
  SettingsCommands commands;
  SettingsController settings;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  bool initialized = false;
  bool secret_page_open = false;
  LayoutMode layout_mode = LayoutMode::Compact;
  bool settings_pane_primary = false;
  std::filesystem::path root;
  std::vector<std::pair<std::string, std::function<void()>>> back_stack;
};

TEST_F(RecoveryPhraseExitsTest, TheBottomButtonClearsTheWords) {
  ExpectGoneWhenRevealedOrCovered([this]() { Click("done"); });
}

TEST_F(RecoveryPhraseExitsTest, TheBackArrowClearsTheWords) {
  ExpectGoneWhenRevealedOrCovered([this]() { Click("close_sub"); });
}

TEST_F(RecoveryPhraseExitsTest, TheSystemBackAndTheEdgeSwipeClearTheWords) {
  // Both end in the top local-back entry's commit (ApplyCloseSecurityPage, the was-recovery branch).
  ExpectGoneWhenRevealedOrCovered([this]() {
    ASSERT_FALSE(back_stack.empty());
    auto commit = std::move(back_stack.back().second);
    back_stack.pop_back();
    commit();
  });
}

TEST_F(RecoveryPhraseExitsTest, PickingAnotherSectionClearsTheWords) {
  // select_section -> ResetSecurityPage.
  ExpectGoneWhenRevealedOrCovered([this]() { Click("sel_dev"); });
}

TEST_F(RecoveryPhraseExitsTest, TheAppGoingToTheBackgroundClearsTheWords) {
  ExpectGoneWhenRevealedOrCovered([]() {
    AppLifecycle::OnWillEnterBackground();
    AppLifecycle::OnDidEnterForeground();
  });
}

TEST_F(RecoveryPhraseExitsTest, LeavingTheMeTabByATabSwitchClearsTheWords) {
  // ShellHost::SelectNavTab tells Settings the Me surface closed; the controller clears its own page and flag.
  ExpectGoneWhenRevealedOrCovered([this]() { settings.OnMeSurfaceClosed(); });
}

TEST_F(RecoveryPhraseExitsTest, TheTimerClearsTheWordsAndRestartsAtTheReveal) {
  // Generous margins: a loaded CI runner may stall a sleep by hundreds of ms.
  constexpr int kShownMs = 2000;
  Actions().SetRecoveryShownMsForTest(kShownMs);
  const auto pump_for = [](const int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    AppRuntime::RunUITasks();
  };
  const auto closed_by = [this](const int ms) {
    for (int waited = 0; waited < ms && Actions().RecoveryPhraseShown(); waited += 20) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      AppRuntime::RunUITasks();
    }
  };
  // A page nobody reveals closes by itself.
  Open();
  pump_for(kShownMs / 4);
  EXPECT_TRUE(Actions().RecoveryPhraseShown()) << "still up at a quarter of its time";
  closed_by(10 * kShownMs);
  ExpectGone();

  // A reveal at the half restarts the time: at 1.25x the covered page's own time ran out, the revealed words must not.
  Open();
  pump_for(kShownMs / 2);
  Click("reveal");
  pump_for(kShownMs * 3 / 4); // 1.25x after opening, 0.75x after the reveal
  EXPECT_TRUE(Actions().RecoveryPhraseShown()) << "the covered page's timer must not hide a reveal made after it began";
  EXPECT_EQ(Actions().RecoveryWordsForTest(), kWords);
  closed_by(10 * kShownMs);
  ExpectGone();
}

TEST_F(RecoveryPhraseExitsTest, AChatReplacingTheWideSettingsPaneClearsTheWords) {
  // Wide layout: a notification tap selects a chat, the primary pane stops being the settings detail, the tab stays Me.
  Open();
  Click("reveal");
  layout_mode = LayoutMode::Expanded;
  settings_pane_primary = true;
  settings.OnShellLayoutSynced();
  EXPECT_TRUE(Actions().RecoveryPhraseShown()) << "the settings pane is still the one shown";

  settings_pane_primary = false;
  settings.OnShellLayoutSynced();
  ExpectGone();
}

TEST_F(RecoveryPhraseExitsTest, ThePhraseIsMarkedSeenOnlyOnceTheWordsWereShown) {
  ASSERT_FALSE(store.Snapshot().profile_prefs.recovery_phrase_seen);
  Open();
  EXPECT_FALSE(store.Snapshot().profile_prefs.recovery_phrase_seen) << "the covered page is not 'seen'";
  Click("done");
  ExpectGone();
  EXPECT_FALSE(store.Snapshot().profile_prefs.recovery_phrase_seen) << "opened and closed without a reveal";

  Open();
  Click("reveal");
  EXPECT_TRUE(store.Snapshot().profile_prefs.recovery_phrase_seen);
}

// A call screen appearing (CallController::BindCallUiShown, wired by Application to OnCallUiShown): the page closes and
// the words go, masked or revealed; seeing the phrase again takes the PIN again.
TEST_F(RecoveryPhraseExitsTest, ACallScreenAppearingClearsTheWordsMaskedOrRevealed) {
  ExpectGoneWhenRevealedOrCovered([this]() { settings.OnCallUiShown(); });
}

TEST_F(RecoveryPhraseExitsTest, AfterACallScreenTheNextLookAsksForThePinAgain) {
  int pin_asks = 0;
  PinPromptPorts pin;
  pin.show_verify = [&pin_asks](const PinVerifyPurpose, PinGateCheck check, std::function<void()> on_ok) {
    ++pin_asks;
    check(SecretText("1234"), [on_ok = std::move(on_ok)](Roe<void>) { on_ok(); });
  };
  settings.BindPinPrompt(pin);
  OpenAndReveal();
  EXPECT_EQ(pin_asks, 1);
  settings.OnCallUiShown();
  ExpectGone();
  EXPECT_EQ(pin_asks, 1) << "closing never asks";
  back_stack.clear();
  Open(); // the only way to the words again: the row, which asks for the PIN
  EXPECT_EQ(pin_asks, 2);
}

TEST_F(RecoveryPhraseExitsTest, ACallScreenWithoutThePhraseOpenLeavesOtherPagesAlone) {
  Click("open_pin");
  ASSERT_EQ(Text("sp"), "pin");
  settings.OnCallUiShown();
  Settle();
  EXPECT_EQ(Text("sp"), "pin") << "only the recovery phrase is closed by a call screen";
  EXPECT_FALSE(back_stack.empty());
}

TEST_F(RecoveryPhraseExitsTest, ARevealStillBeingCheckedIsVoidWhenACallScreenAppears) {
  // The PIN gate is up (the check not run yet) when the call screen comes: finishing the check afterwards opens nothing.
  PinGateCheck held_check;
  std::function<void()> held_ok;
  PinPromptPorts pin;
  pin.show_verify = [&](const PinVerifyPurpose, PinGateCheck check, std::function<void()> on_ok) {
    held_check = std::move(check);
    held_ok = std::move(on_ok);
  };
  settings.BindPinPrompt(pin);
  Click("show");
  ASSERT_TRUE(held_check);
  ASSERT_FALSE(Actions().RecoveryPhraseShown()) << "nothing is open while the PIN is being asked";
  settings.OnCallUiShown();
  held_check(SecretText("1234"), [&held_ok](Roe<void>) { held_ok(); });
  Settle();
  ExpectGone();
  EXPECT_EQ(Actions().RecoverySecretCountForTest(), 0u);
}

TEST_F(RecoveryPhraseExitsTest, OnTheDesktopTheInlinePhraseGoesWhenACallScreenAppears) {
  layout_mode = LayoutMode::Expanded;
  settings.SyncLayoutMode();
  if (!settings.ShowsSecuritySection()) {
    Click("sel_sec");
  }
  Click("show");
  ASSERT_TRUE(Actions().RecoveryPhraseShown()) << "the desktop shows it inline";
  Click("reveal");
  ASSERT_EQ(Actions().RecoveryWordsForTest(), kWords);
  settings.OnCallUiShown();
  Settle();
  EXPECT_FALSE(Actions().RecoveryPhraseShown());
  EXPECT_TRUE(Actions().RecoveryWordsForTest().empty());
  EXPECT_EQ(Actions().RecoverySecretCountForTest(), 0u);
}

// The real CallController wired to Settings as Application does: every call screen, including a second ring over a
// (minimised) call and a ring replacing a kept failed page, closes the phrase page that was opened meanwhile.
TEST_F(RecoveryPhraseExitsTest, EveryCallScreenIncludingASecondRingOverACallClosesTheOpenPhrase) {
  CallController call;
  call.BindCallUiShown([this]() { settings.OnCallUiShown(); });
  const auto ring = [&call](const std::string& id) {
    CallScreen& screen = CallControllerTestAccess::Screen(call);
    screen = {};
    screen.kind = CallScreen::Kind::Ringing;
    screen.ring_call_id = id;
    screen.caller_identity = "alice";
    CallControllerTestAccess::Render(call);
  };
  const auto answer = [&call]() {
    CallScreen& screen = CallControllerTestAccess::Screen(call);
    screen = {};
    screen.kind = CallScreen::Kind::Answering;
    screen.ring_call_id = "c1";
    CallControllerTestAccess::Render(call);
  };
  for (const bool reveal : {false, true}) {
    SCOPED_TRACE(reveal ? "revealed" : "covered");
    // A call is up and minimised; the phrase page is opened over it; then a second ring comes.
    CallControllerTestAccess::EndCall(call);
    CallControllerTestAccess::InCallSync(call);
    if (!settings.ShowsSecuritySection()) {
      Click("sel_sec");
    }
    reveal ? OpenAndReveal() : Open();
    ring("c2");
    ExpectGone();
    back_stack.clear();

    // A ring replacing a kept failed page.
    CallControllerTestAccess::EndCall(call);
    CallControllerTestAccess::Screen(call) = {};
    CallControllerTestAccess::Failed(call) = CallFailedPage{};
    CallControllerTestAccess::Render(call);
    if (!settings.ShowsSecuritySection()) {
      Click("sel_sec");
    }
    reveal ? OpenAndReveal() : Open();
    ring("c1");
    ExpectGone();
    back_stack.clear();

    // Answering the ring: another notification with the page already closed changes nothing, no error.
    answer();
    ExpectGone();
    CallControllerTestAccess::Failed(call).reset();
    CallControllerTestAccess::EndCall(call);
  }
}

// The harness cannot see the PIN gate's captured secret, so the wipe of a voided reveal is pinned in the source: the
// branch that returns on a count mismatch resets (and so wipes) the decoded phrase first.
TEST_F(RecoveryPhraseExitsTest, AVoidedRevealWipesTheDecodedPhraseBeforeReturning) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/SettingsSecurityActions.cpp");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string source = buffer.str();
  const size_t voided = source.find("if (request != recovery_shown_count_) {");
  ASSERT_NE(voided, std::string::npos);
  const size_t ret = source.find("return;", voided);
  const size_t reset = source.find("revealed->reset();", voided);
  ASSERT_NE(reset, std::string::npos);
  EXPECT_LT(reset, ret) << "reset before the early return";
}

} // namespace
} // namespace pbr
