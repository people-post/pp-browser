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

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

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
  void ReportFailure(const Error&) override {}
  void ShowStatus(const std::string&) override {}
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
        "<button id=\"card\" data-if=\"profile_share_card_shown\" data-event-click=\"share_profile_card()\">x</button>"
        "</div></body></rml>");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    ShellFeedbackPorts ports;
    ports.show_toast = [this](const std::string& message, ToastDuration) { toasts.push_back(message); };
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
};

TEST_F(SettingsProfileCopyTest, CopiesTheDisplayedAccountIdAndNotTheNodeId) {
  host.state.profile_relay_id = "relay:AccountId123";
  host.state.profile_peer_id = "QmNodeIdNotTheAccount";
  document->GetElementById("copy")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(system.clipboard, "relay:AccountId123");
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], "账号 ID 已复制");
}

TEST_F(SettingsProfileCopyTest, WithoutAnAccountNothingIsCopiedAndTheUserIsToldToSetOneUp) {
  host.state.profile_relay_id = "";
  host.state.profile_peer_id = "QmNodeIdNotTheAccount";
  document->GetElementById("copy")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(system.clipboard, "") << "the node ID is never put on the clipboard in its place";
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], "还没有账号，先开通账号。");
}

// The Me card's "QR code" tile is in the settings model next to the other tiles, and opens the My QR code page.
TEST_F(SettingsProfileCopyTest, TheQrTileOpensTheMyQrPage) {
  int opened = 0;
  actions->BindShowMyQr([&]() { ++opened; });
  document->GetElementById("qr")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(opened, 1);
}

TEST_F(SettingsProfileCopyTest, TheQrTileWithNothingBoundDoesNothing) {
  document->GetElementById("qr")->DispatchEvent(ui::EventId::Click, {});
  SUCCEED();
}

// Commit 1 of the plan: nothing sends my card yet, so its tile is not in the page; binding the sender shows it.
TEST_F(SettingsProfileCopyTest, TheShareCardTileIsHiddenUntilSomethingSendsTheCard) {
  context->Update();
  ASSERT_NE(document->GetElementById("card"), nullptr);
  EXPECT_FALSE(document->GetElementById("card")->IsVisible());
  int sent = 0;
  actions->BindShareOwnCard([&]() { ++sent; });
  context->Update();
  EXPECT_TRUE(document->GetElementById("card")->IsVisible());
  document->GetElementById("card")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(sent, 1);
}

// B216: the reconnect never reads the settings page. Its fields may be empty (page never opened) or stale.
TEST_F(SettingsProfileCopyTest, ReconnectRegistersWithTheStoredNicknameAndLeavesTheSettingsViewAlone) {
  host.state.profile_nickname = "";
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(true); };
  std::vector<std::string> registered_with;
  commands.register_identity = [&](const RegisterIdentityArgs& args) -> Roe<void> {
    registered_with.push_back(args.nickname);
    return {};
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
  commands.register_identity = [&](const RegisterIdentityArgs&) -> Roe<void> {
    ++registrations;
    return {};
  };
  std::vector<bool> answers;
  actions->Register(/*quiet_success=*/true, [&](const bool ok) { answers.push_back(ok); });
  EXPECT_EQ(answers, (std::vector<bool>{false}));
  EXPECT_EQ(registrations, 0);
}

TEST_F(SettingsProfileCopyTest, ReconnectAnswersFalseWhenTheRegistrationFailsAndTrueWhenItWorks) {
  unlock.ensure_unlocked = [](std::function<void(bool)> then) { then(true); };
  bool works = false;
  commands.register_identity = [&](const RegisterIdentityArgs&) -> Roe<void> {
    if (!works) {
      return AppError::Network(Err::Network::HttpRejected, "refused");
    }
    return {};
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
  commands.register_identity = [&](const RegisterIdentityArgs& args) -> Roe<void> {
    registered_with.push_back(args.nickname);
    return {};
  };
  actions->Register();
  EXPECT_EQ(registered_with, (std::vector<std::string>{"Typed"}));
  EXPECT_EQ(host.saves, 1);
  EXPECT_EQ(host.pulls, 1);
}

} // namespace
} // namespace pbr
