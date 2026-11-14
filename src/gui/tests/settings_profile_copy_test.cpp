// "Copy account ID" puts on the clipboard the account ID the row shows (not the node ID next to it), and says so.
#include "feature/settings/SettingsCommands.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/SettingsController.h"
#include "gui/SettingsHost.h"
#include "gui/SettingsProfileActions.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellFeedback.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
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
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class RecordingSystem : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return 0.0; }
  void SetClipboardText(const ui::String& text) override { clipboard = text.c_str(); }
  std::string clipboard;
};

class StubHost : public SettingsHost {
public:
  SessionStore& Store() override { return store; }
  void ReportFailure(const Error& error) override {
    failures.push_back(error);
    UserFeedback::FailFrom(error); // as SettingsController does: the white pill carries it
  }
  void ShowStatus(const std::string& status) override { statuses.push_back(status); }
  void Resync(bool) override {}
  SettingsUiState& UiState() override { return state; }
  void PullFromView() override { ++pulls; }
  void PushToView(bool) override {}
  void SaveProfileNickname() override { ++saves; }
  void RequestSyncLayout(const char*) override {}
  Roe<void> ForgetToolPermissions() override { return {}; }
  void OpenChangePin() override {}
  SessionStore store;
  SettingsUiState state;
  int saves = 0;
  int pulls = 0;
  std::vector<std::string> statuses; // every ShowStatus call, in order
  std::vector<Error> failures;       // every ReportFailure call, in order
};

constexpr const char* kContext = "settings-profile-copy-test";

class SettingsProfileCopyTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    ui::SetRenderInterface(&render);
    ui::SetSystemInterface(&system);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext(kContext, {400, 800});
    ASSERT_NE(context, nullptr);
    actions = std::make_unique<SettingsProfileActions>(host, commands, unlock);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "settings",
                                                   [this](ui::DataModelConstructor& ctor) { actions->Bind(ctor); }));
    document = context->LoadDocumentFromMemory(
        "<rml><body><div data-model=\"settings\"><button id=\"copy\" "
        "data-event-click=\"copy_profile_id()\">x</button>"
        "<button id=\"qr\" data-event-click=\"show_my_qr()\">x</button>"
        "<button id=\"rotate\" data-event-click=\"rotate_brief_llm_key()\">x</button>"
        "<button id=\"update\" data-event-click=\"register_profile()\" "
        "data-class-busy=\"profile_registering\">x</button>"
        "</div></body></rml>");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    ShellFeedbackPorts ports;
    ports.show_toast = [this](const std::string& message, ToastDuration duration) {
      toasts.push_back(message);
      ShellFeedback::ShowToast(compact_toasts, message, duration, 1000.f);
    };
    ports.dismiss_toast = [this]() { ++dismissals; };
    compact_toasts.layout_mode = LayoutMode::Compact;
    UserFeedback::BindPorts(ports);
  }
  void TearDown() override {
    UserFeedback::BindPorts({});
    actions.reset();
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext(kContext);
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  NullRender render;
  RecordingSystem system;
  StubHost host;
  SettingsCommands commands;
  UnlockEnsurePorts unlock;
  std::unique_ptr<SettingsProfileActions> actions;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  bool initialized = false;
  std::vector<std::string> toasts;
  /** The same toasts through the real ShellFeedback on a phone layout: what is on screen after them. */
  ShellState compact_toasts;
  int dismissals = 0;

  /** The page's update button reads "Updating…" (the class its model variable drives). */
  bool Busy() {
    context->Update();
    return document->GetElementById("update")->IsClassSet("busy");
  }
  /** A registration that waits to be answered: each call parks its `done`; the nickname it was sent is kept. */
  void ParkRegistrations() {
    commands.register_identity = [this](const RegisterIdentityArgs& args, std::function<void(Roe<void>)> done) {
      sent_nicknames.push_back(args.nickname);
      parked.push_back(std::move(done));
    };
  }
  void UnlockAtOnce() {
    unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(true); };
  }
  /** The renewal slot: `hold_start` is the start the page handed over (run it when the slot is free). */
  void BindManualHold() {
    actions->BindRunWhenRegistrationIdle([this](SettingsProfileActions::RegistrationSlotRun run) {
      ++holds;
      held_run = std::move(run);
    });
  }
  void StartHeldRun() {
    auto run = std::move(held_run);
    held_run = nullptr;
    ASSERT_TRUE(run);
    run([this]() { ++releases; });
  }
  void BindImmediateHold() {
    actions->BindRunWhenRegistrationIdle([this](SettingsProfileActions::RegistrationSlotRun run) {
      ++holds;
      run([this]() { ++releases; });
    });
  }

  std::vector<std::string> sent_nicknames;
  std::vector<std::function<void(Roe<void>)>> parked;
  SettingsProfileActions::RegistrationSlotRun held_run;
  int holds = 0;
  int releases = 0;
};

TEST_F(SettingsProfileCopyTest, CopiesTheDisplayedAccountIdAndNotTheNodeId) {
  host.state.profile_account_id = "account:AccountId123";
  host.state.profile_peer_id = "QmNodeIdNotTheAccount";
  document->GetElementById("copy")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(system.clipboard, "account:AccountId123");
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], "账号 ID 已复制");
}

TEST_F(SettingsProfileCopyTest, WithoutAnAccountNothingIsCopiedAndTheUserIsToldToSetOneUp) {
  host.state.profile_account_id = "";
  host.state.profile_peer_id = "QmNodeIdNotTheAccount";
  document->GetElementById("copy")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(system.clipboard, "") << "the node ID is never put on the clipboard in its place";
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], "还没有账号，先开通账号。");
}

// The Me card's "QR code" button is in the settings model next to the other buttons, and opens the My QR code page.
TEST_F(SettingsProfileCopyTest, TheQrButtonOpensTheMyQrPage) {
  int opened = 0;
  actions->BindShowMyQr([&]() { ++opened; });
  document->GetElementById("qr")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(opened, 1);
}

TEST_F(SettingsProfileCopyTest, TheQrButtonWithNothingBoundDoesNothing) {
  document->GetElementById("qr")->DispatchEvent(ui::EventId::Click, {});
  SUCCEED();
}

// B216: the reconnect never reads the settings page. Its fields may be empty (page never opened) or stale.
TEST_F(SettingsProfileCopyTest, ReconnectRegistersWithTheStoredNicknameAndLeavesTheSettingsViewAlone) {
  host.state.profile_nickname = "";
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(true); };
  std::vector<std::string> registered_with;
  commands.register_identity = [&](const RegisterIdentityArgs& args, std::function<void(Roe<void>)> done) {
    registered_with.push_back(args.nickname);
    done({});
  };
  commands.load_profile_identity = []() {
    ProfileIdentityView view;
    view.ready = true;
    view.nickname = "Stored Name";
    return view;
  };
  actions->Register(/*quiet_success=*/true);
  EXPECT_EQ(registered_with, (std::vector<std::string>{"Stored Name"}));
  EXPECT_EQ(host.saves, 0) << "the page's nickname is not committed";
  EXPECT_EQ(host.pulls, 0) << "the page's fields are not read";
}

// A failed message's "update my card" waits on this answer: false (PIN cancelled, or refused) clears what it queued.
TEST_F(SettingsProfileCopyTest, ReconnectAnswersFalseWhenThePinIsCancelledAndNeverRegisters) {
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(false); };
  int registrations = 0;
  commands.register_identity = [&](const RegisterIdentityArgs&, std::function<void(Roe<void>)> done) {
    ++registrations;
    done({});
  };
  std::vector<bool> answers;
  actions->Register(/*quiet_success=*/true, [&](const bool ok) { answers.push_back(ok); });
  EXPECT_EQ(answers, (std::vector<bool>{false}));
  EXPECT_EQ(registrations, 0);
}

TEST_F(SettingsProfileCopyTest, ReconnectAnswersFalseWhenTheRegistrationFailsAndTrueWhenItWorks) {
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(true); };
  bool works = false;
  commands.register_identity = [&](const RegisterIdentityArgs&, std::function<void(Roe<void>)> done) {
    if (!works) {
      done(AppError::Network(Err::Network::HttpRejected, "refused"));
      return;
    }
    done({});
  };
  std::vector<bool> answers;
  actions->Register(/*quiet_success=*/true, [&](const bool ok) { answers.push_back(ok); });
  works = true;
  actions->Register(/*quiet_success=*/true, [&](const bool ok) { answers.push_back(ok); });
  EXPECT_EQ(answers, (std::vector<bool>{false, true}));
}

TEST_F(SettingsProfileCopyTest, TheSettingsButtonStillCommitsAndSendsThePagesNickname) {
  host.state.profile_nickname = "Typed";
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(true); };
  std::vector<std::string> registered_with;
  commands.register_identity = [&](const RegisterIdentityArgs& args, std::function<void(Roe<void>)> done) {
    registered_with.push_back(args.nickname);
    done({});
  };
  actions->Register();
  EXPECT_EQ(registered_with, (std::vector<std::string>{"Typed"}));
  EXPECT_EQ(host.saves, 1);
  EXPECT_EQ(host.pulls, 1);
}

// A success is said once, in the bottom white pill. The persistent status line under the page (it stays until the page is
// left) must not carry it too: every ShowStatus call after a success leaves the line empty.
TEST_F(SettingsProfileCopyTest, ARegistrationSaysDoneInThePillAndLeavesNoStatusLine) {
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(true); };
  commands.register_identity = [](const RegisterIdentityArgs&, std::function<void(Roe<void>)> done) { done({}); };
  actions->Register();
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], Tr("settings.registration.done"));
  for (const std::string& status : host.statuses) {
    EXPECT_EQ(status, "") << "no persistent success line";
  }
  host.state.profile_registered = "yes";
  toasts.clear();
  actions->Register();
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], Tr("settings.registration.renewed"));
  for (const std::string& status : host.statuses) {
    EXPECT_EQ(status, "");
  }
}

TEST_F(SettingsProfileCopyTest, ARotatedKeySaysSoInThePillAndLeavesNoStatusLine) {
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(true); };
  commands.rotate_brief_llm_key = []() -> Roe<void> { return {}; };
  document->GetElementById("rotate")->DispatchEvent(ui::EventId::Click, {});
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], Tr("settings.llm.brief_key_rotated"));
  for (const std::string& status : host.statuses) {
    EXPECT_EQ(status, "");
  }
}

// --- The registration runs off the UI thread (DEV/PLAN-register-async.md) -----------------------------------------

constexpr Err::Network kOffline = Err::Network::Unreachable;

TEST_F(SettingsProfileCopyTest, AnUpdateShowsUpdatingUntilItsReplyAndALaterTapSendsNothing) {
  UnlockAtOnce();
  ParkRegistrations();
  actions->Register();
  EXPECT_TRUE(Busy());
  EXPECT_TRUE(actions->RegistrationStarted());
  const int saves = host.saves;
  const int pulls = host.pulls;
  document->GetElementById("update")->DispatchEvent(ui::EventId::Click, {}); // the button again
  actions->Register();
  EXPECT_EQ(parked.size(), 1u) << "one registration at a time";
  EXPECT_EQ(host.saves, saves) << "a tap while updating saves nothing";
  EXPECT_EQ(host.pulls, pulls) << "and reads nothing of the page";
  EXPECT_TRUE(toasts.empty());
  parked[0]({});
  EXPECT_FALSE(Busy());
  EXPECT_FALSE(actions->RegistrationStarted());
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], Tr("settings.registration.done"));
  for (const std::string& status : host.statuses) {
    EXPECT_EQ(status, "");
  }
  actions->Register();
  EXPECT_EQ(parked.size(), 2u) << "after it ended a new one starts";
}

TEST_F(SettingsProfileCopyTest, ARenewalSaysUpdatedOnceAndAFailureSaysItsErrorOnceWithoutASuccessLine) {
  UnlockAtOnce();
  ParkRegistrations();
  host.state.profile_registered = "yes";
  actions->Register();
  parked[0]({});
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], Tr("settings.registration.renewed"));

  toasts.clear();
  actions->Register();
  parked[1](AppError::Storage(Err::Storage::Unavailable, "unavailable").WithUser("the plain error"));
  EXPECT_FALSE(Busy());
  ASSERT_EQ(host.failures.size(), 1u);
  EXPECT_EQ(host.failures[0].user, "the plain error") << "a button's failure keeps the error as it is";
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], "the plain error");
  EXPECT_EQ(dismissals, 0);
}

TEST_F(SettingsProfileCopyTest, AReconnectNotesPleaseWaitThenSaysItsFailureInPlainWordsAndNothingIsDismissed) {
  UnlockAtOnce();
  ParkRegistrations();
  std::vector<bool> answers;
  actions->Register(/*quiet_success=*/true, [&](const bool ok) { answers.push_back(ok); });
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], Tr("chat.reconnect.please_wait"));
  EXPECT_TRUE(Busy()) << "the page's button says Updating… whoever started it";
  parked[0](AppError::Network(kOffline, "no route"));
  ASSERT_EQ(toasts.size(), 2u);
  EXPECT_EQ(toasts[1], Tr("chat.reconnect.offline"));
  EXPECT_EQ(answers, (std::vector<bool>{false}));
  // On a phone the result replaces the note: one pill, the result.
  ASSERT_EQ(compact_toasts.toasts.size(), 1u);
  EXPECT_EQ(std::string(compact_toasts.toasts[0].message.c_str()), Tr("chat.reconnect.offline"));
  EXPECT_EQ(dismissals, 0) << "nothing is dismissed anywhere in the flow";

  toasts.clear();
  actions->Register(/*quiet_success=*/true);
  parked[1](AppError::Network(Err::Network::HttpRejected, "refused"));
  EXPECT_EQ(toasts.back(), Tr("chat.reconnect.refused"));
  EXPECT_EQ(dismissals, 0);
}

TEST_F(SettingsProfileCopyTest, AQuietSuccessSaysNothingMoreAndAButtonsSuccessIsTheLastPill) {
  UnlockAtOnce();
  ParkRegistrations();
  std::vector<bool> answers;
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  parked[0]({});
  EXPECT_EQ(toasts, (std::vector<std::string>{Tr("chat.reconnect.please_wait")}));
  EXPECT_EQ(answers, (std::vector<bool>{true}));
  actions->Register();
  parked[1]({});
  EXPECT_EQ(toasts.back(), Tr("settings.registration.done"));
  EXPECT_EQ(dismissals, 0);
}

TEST_F(SettingsProfileCopyTest, AQuietCallerJoinsTheRunningButtonUpdateAndOnlyTheButtonSpeaks) {
  UnlockAtOnce();
  ParkRegistrations();
  actions->Register();
  std::vector<bool> answers;
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  EXPECT_EQ(parked.size(), 1u);
  EXPECT_TRUE(toasts.empty()) << "joining says nothing";
  parked[0]({});
  EXPECT_EQ(answers, (std::vector<bool>{true, true}));
  ASSERT_EQ(toasts.size(), 1u) << "one pill, from the one that started it";
  EXPECT_EQ(toasts[0], Tr("settings.registration.done"));
}

TEST_F(SettingsProfileCopyTest, AFailureWithAQuietCallerJoinedUsesThePlainWordsOnceAndEveryDoneGetsTheSameError) {
  UnlockAtOnce();
  ParkRegistrations();
  actions->Register();
  std::vector<bool> answers;
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  parked[0](AppError::Network(kOffline, "no route"));
  EXPECT_EQ(answers, (std::vector<bool>{false}));
  ASSERT_EQ(host.failures.size(), 1u);
  EXPECT_EQ(host.failures[0].user, Tr("chat.reconnect.offline"));
  ASSERT_EQ(toasts.size(), 1u);
}

TEST_F(SettingsProfileCopyTest, AButtonTapWhileAReconnectRunsDoesNothingAndTheReconnectSpeaksInItsOwnWords) {
  UnlockAtOnce();
  ParkRegistrations();
  actions->Register(true);
  const int saves = host.saves;
  actions->Register();
  EXPECT_EQ(parked.size(), 1u);
  EXPECT_EQ(host.saves, saves);
  EXPECT_EQ(host.pulls, 0);
  EXPECT_EQ(toasts.size(), 1u) << "just the reconnect's note";
  parked[0](AppError::Network(kOffline, "no route"));
  EXPECT_EQ(toasts.back(), Tr("chat.reconnect.offline"));
  EXPECT_EQ(toasts.size(), 2u);
}

TEST_F(SettingsProfileCopyTest, EveryDoneIsAnsweredOnceWhateverEndsTheRun) {
  UnlockAtOnce();
  ParkRegistrations();
  std::vector<int> counts(6, 0);
  auto done = [&](const int i) { return [&counts, i](bool) { ++counts[i]; }; };
  actions->Register(true, done(0));
  actions->Register(true, done(1));
  parked[0]({});
  actions->Register(true, done(2));
  actions->Register(true, done(3));
  parked[1](AppError::Network(kOffline, "x"));
  actions->Register(true, done(4));
  actions->Register(true, done(5));
  actions->OnMessagingTornDown();
  parked[2]({}); // the dropped reply, late
  EXPECT_EQ(counts, (std::vector<int>{1, 1, 1, 1, 1, 1}));
}

TEST_F(SettingsProfileCopyTest, ATeardownReleasesTheButtonAnswersWaitersFalseAndALateReplyTouchesNothing) {
  UnlockAtOnce();
  ParkRegistrations();
  BindImmediateHold();
  std::vector<bool> answers;
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  ASSERT_TRUE(Busy());
  toasts.clear();
  actions->OnMessagingTornDown(); // only this: no OnProfileDataReset, nothing else
  EXPECT_FALSE(Busy());
  EXPECT_EQ(answers, (std::vector<bool>{false, false}));
  parked[0]({});
  EXPECT_TRUE(toasts.empty()) << "the dropped reply says nothing";
  EXPECT_TRUE(host.failures.empty());
  EXPECT_FALSE(Busy());
  EXPECT_EQ(releases, 1) << "the slot was released by the reply even so";
  EXPECT_EQ(answers.size(), 2u);

  actions->Register(); // a new update works, and the old reply does not end it
  EXPECT_TRUE(Busy());
  ASSERT_EQ(parked.size(), 2u);
  parked[0]({});
  EXPECT_TRUE(Busy());
  parked[1]({});
  EXPECT_FALSE(Busy());
  ASSERT_EQ(toasts.size(), 1u);
}

TEST_F(SettingsProfileCopyTest, ARelasedAfterTheActionsAreGoneDoesNotCrashAndStillReleasesTheSlot) {
  UnlockAtOnce();
  ParkRegistrations();
  BindImmediateHold();
  actions->Register();
  ASSERT_EQ(parked.size(), 1u);
  actions.reset();
  parked[0]({});
  EXPECT_EQ(releases, 1);
  EXPECT_TRUE(toasts.empty());
}

TEST_F(SettingsProfileCopyTest, TheSlotIsReleasedOnceOnEveryWayOut) {
  UnlockAtOnce();
  BindImmediateHold();
  // The request answers at once (Begin failed before the worker): released.
  commands.register_identity = [](const RegisterIdentityArgs&, std::function<void(Roe<void>)> done) {
    done(AppError::Pin(Err::Pin::Required, "locked"));
  };
  actions->Register();
  EXPECT_EQ(releases, 1);
  // The request is missing.
  commands.register_identity = nullptr;
  actions->Register();
  EXPECT_EQ(releases, 2);
  // Success after a wait.
  ParkRegistrations();
  actions->Register();
  EXPECT_EQ(releases, 2);
  parked[0]({});
  EXPECT_EQ(releases, 3);
  // The PIN was not given: the slot was never taken.
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(false); };
  actions->Register();
  EXPECT_EQ(releases, 3);
  EXPECT_EQ(holds, 3);
}

TEST_F(SettingsProfileCopyTest, TheNicknameAndTheSlotAreTakenWhenTheSlotIsGivenNotAtTheTap) {
  UnlockAtOnce();
  ParkRegistrations();
  BindManualHold();
  host.state.profile_nickname = "At the tap";
  actions->Register();
  EXPECT_EQ(holds, 1);
  EXPECT_TRUE(parked.empty());
  EXPECT_TRUE(sent_nicknames.empty()) << "nothing sent before the slot is free";
  EXPECT_TRUE(Busy());
  host.state.profile_nickname = "When the slot came";
  StartHeldRun();
  EXPECT_EQ(sent_nicknames, (std::vector<std::string>{"When the slot came"}));
  EXPECT_EQ(releases, 0);
  parked[0]({});
  EXPECT_EQ(releases, 1);
}

TEST_F(SettingsProfileCopyTest, AReconnectReadsTheStoredNicknameWhenTheSlotIsGiven) {
  UnlockAtOnce();
  ParkRegistrations();
  BindManualHold();
  std::string stored = "At the tap";
  commands.load_profile_identity = [&]() {
    ProfileIdentityView view;
    view.ready = true;
    view.nickname = stored;
    return view;
  };
  actions->Register(true);
  stored = "When the slot came";
  EXPECT_TRUE(sent_nicknames.empty());
  StartHeldRun();
  EXPECT_EQ(sent_nicknames, (std::vector<std::string>{"When the slot came"}));
}

TEST_F(SettingsProfileCopyTest, AStartQueuedBehindTheRenewalDoesNotRegisterAfterATeardownOrWhenTheActionsAreGone) {
  UnlockAtOnce();
  ParkRegistrations();
  BindManualHold();
  actions->Register();
  actions->OnMessagingTornDown();
  StartHeldRun();
  EXPECT_TRUE(parked.empty()) << "no registration for the profile that is gone";
  EXPECT_EQ(releases, 1) << "the slot is still let go";
  EXPECT_FALSE(Busy());
  EXPECT_TRUE(toasts.empty());

  actions->Register();
  actions.reset();
  StartHeldRun();
  EXPECT_TRUE(parked.empty());
  EXPECT_EQ(releases, 2);
}

TEST_F(SettingsProfileCopyTest, WhileThePinPageIsOpenASecondTapOrReconnectDoesNotAskAgainAndOneRegistrationFollows) {
  std::function<void(bool)> pin;
  int pin_asked = 0;
  unlock.ensure_unlocked = [&](std::function<void(bool)> then) {
    ++pin_asked;
    pin = std::move(then);
  };
  ParkRegistrations();
  std::vector<bool> answers;
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  actions->Register();
  EXPECT_EQ(pin_asked, 1);
  EXPECT_EQ(host.saves, 0);
  EXPECT_EQ(host.pulls, 0);
  EXPECT_FALSE(Busy()) << "no Updating… behind the PIN page";
  EXPECT_TRUE(actions->RegistrationStarted());
  pin(true);
  EXPECT_TRUE(Busy());
  ASSERT_EQ(parked.size(), 1u);
  parked[0]({});
  EXPECT_EQ(answers, (std::vector<bool>{true, true}));
}

TEST_F(SettingsProfileCopyTest, ACancelledPinAnswersEveryoneFalseOnceNeverSetsTheButtonAndAllowsAnotherTry) {
  std::function<void(bool)> pin;
  int pin_asked = 0;
  unlock.ensure_unlocked = [&](std::function<void(bool)> then) {
    ++pin_asked;
    pin = std::move(then);
  };
  ParkRegistrations();
  std::vector<bool> answers;
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  pin(false);
  EXPECT_EQ(answers, (std::vector<bool>{false, false}));
  EXPECT_FALSE(Busy());
  EXPECT_FALSE(actions->RegistrationStarted());
  EXPECT_TRUE(parked.empty());
  actions->Register();
  EXPECT_EQ(pin_asked, 2) << "tapping again asks for the PIN again";
}

TEST_F(SettingsProfileCopyTest, ATeardownWhileThePinPageIsOpenLeavesTheLatePinAnswerHarmless) {
  std::function<void(bool)> pin;
  unlock.ensure_unlocked = [&](std::function<void(bool)> then) { pin = std::move(then); };
  ParkRegistrations();
  std::vector<bool> answers;
  actions->Register(true, [&](const bool ok) { answers.push_back(ok); });
  actions->OnMessagingTornDown();
  EXPECT_EQ(answers, (std::vector<bool>{false}));
  pin(true);
  EXPECT_TRUE(parked.empty()) << "no registration for a profile that is gone";
  EXPECT_FALSE(Busy());
  EXPECT_EQ(answers.size(), 1u);
}

TEST_F(SettingsProfileCopyTest, ThePinCancelledReconnectNeverShowsTheButtonAsBusy) {
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(false); };
  actions->Register(true);
  EXPECT_FALSE(Busy());
}

// All three buttons (the status pill, the desktop button, the phone button) are greyed and read "Updating…" together.
TEST(SettingsProfileRegisteringViewTest, EveryUpdateButtonIsGreyedAndSwitchesItsTextWhileUpdating) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "settings_section_profile.rml");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string rml = buffer.str();
  const auto count = [&rml](const std::string& needle) {
    int found = 0;
    for (size_t at = rml.find(needle); at != std::string::npos; at = rml.find(needle, at + 1)) {
      ++found;
    }
    return found;
  };
  EXPECT_EQ(count("data-event-click=\"register_profile()\""), 3);
  EXPECT_EQ(count("data-attrif-disabled=\"profile_registering\""), 3);
  EXPECT_EQ(count("data-class-settings-registering=\"profile_registering\""), 3)
      << ":disabled does not grey a button here";
  EXPECT_EQ(count("{{i18n:settings.profile.updating}}"), 3);
  EXPECT_EQ(count("data-if=\"!profile_registering\""), 3);
  std::ifstream css_in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "themes" / "components.rcss");
  std::stringstream css;
  css << css_in.rdbuf();
  EXPECT_NE(css.str().find(".settings-registering {"), std::string::npos);
  LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR);
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.profile.updating"), "Updating…");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.profile.updating"), "更新中…");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
