// The phone profile page's nickname row opens the rename dialog; the page shows the new name only once it was saved
// (DEV/PLAN-my-profile-page-a.md, commit 2). A skipped or failed save leaves the saved name on the page.
#include "feature/settings/SettingsCommands.h"
#include "foundation/data/SessionStore.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/SettingsController.h"
#include "gui/SettingsHost.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/UiEditSession.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
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

class QuietSystem : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return 0.0; }
};

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

constexpr const char* kContext = "settings-profile-nickname-test";

class SettingsProfileNicknameTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render);
    ui::SetSystemInterface(&system);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext(kContext, {400, 800});
    ASSERT_NE(context, nullptr);

    stored_name = "Old";
    commands.session_store = [this]() -> SessionStore& { return store; };
    commands.load_profile_identity = [this]() {
      ProfileIdentityView view;
      view.ready = true;
      view.nickname = stored_name;
      return view;
    };
    commands.save_profile_nickname = [this](const std::string& nickname) -> Roe<void> {
      saved_with.push_back(nickname);
      if (save_fails) {
        return AppError::Network(Err::Network::HttpRejected, "refused");
      }
      stored_name = nickname;
      return {};
    };
    settings.BindCommands(commands);
    ShellFeedbackPorts ports;
    ports.show_toast = [this](const std::string& message, ToastDuration) { toasts.push_back(message); };
    ports.show_prompt = [this](const std::string&, const std::string&, const std::string& default_value,
                               std::function<void(bool, std::string)> on_result) {
      prompt_default = default_value;
      ++prompts;
      answer = std::move(on_result);
    };
    settings.BindShellFeedback(ports);
    UserFeedback::BindPorts(ports);

    // The page as it was loaded: the saved name "Old" is the baseline and the bound value.
    const ProfilePreferences& prefs = store.Snapshot().profile_prefs;
    host().UiState().auto_renew_registration = prefs.auto_renew_registration ? "auto" : "off";
    host().UiState().show_notifications = prefs.show_notifications ? "on" : "off";
    host().UiState().show_notification_preview = prefs.show_notification_preview ? "on" : "off";
    host().UiState().stay_connected = prefs.stay_connected ? "on" : "off";
    host().UiState().profile_nickname = "Old";
    UiEditSession::Instance().OnLoaded(kUiFieldProfileNickname, "Old");
    host().PushToView(false);

    ASSERT_TRUE(settings.RegisterModel(context));
    document = context->LoadDocumentFromMemory(
        "<rml><body><div data-model=\"settings\"><p id=\"nick\">{{profile_nickname}}</p>"
        "<button id=\"edit\" data-event-click=\"edit_profile_nickname()\">x</button></div></body></rml>");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }
  void TearDown() override {
    while (UiEditSession::Instance().RemountBlocking()) {
      UiEditSession::Instance().EndRemount();
    }
    UserFeedback::BindPorts({});
    settings.BindShellFeedback({});
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext(kContext);
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
  }

  void TapNicknameRowAndAnswer(bool ok, const std::string& value) {
    document->GetElementById("edit")->DispatchEvent(ui::EventId::Click, {});
    ASSERT_TRUE(answer);
    answer(ok, value);
    context->Update();
  }
  SettingsHost& host() { return settings; }
  std::string ShownName() { return document->GetElementById("nick")->GetInnerRML().c_str(); }
  std::string BoundName() {
    host().PullFromView();
    return host().UiState().profile_nickname;
  }

  NullRender render;
  QuietSystem system;
  SessionStore store;
  SettingsCommands commands;
  SettingsController settings;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  bool initialized = false;
  std::string stored_name;
  bool save_fails = false;
  std::vector<std::string> saved_with;
  std::vector<std::string> toasts;
  std::string prompt_default;
  int prompts = 0;
  std::function<void(bool, std::string)> answer;
};

TEST_F(SettingsProfileNicknameTest, TheDialogStartsFromTheSavedName) {
  document->GetElementById("edit")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(prompts, 1);
  EXPECT_EQ(prompt_default, "Old");
}

TEST_F(SettingsProfileNicknameTest, ASavedNameIsShownOnThePageAndSavedOnce) {
  ASSERT_EQ(ShownName(), "Old");
  TapNicknameRowAndAnswer(true, "New");
  EXPECT_EQ(saved_with, (std::vector<std::string>{"New"}));
  EXPECT_EQ(BoundName(), "New");
  EXPECT_EQ(ShownName(), "New") << "the bound value reached the view";
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], Tr("settings.saved"));
  const std::string* baseline = UiEditSession::Instance().Baseline(kUiFieldProfileNickname);
  ASSERT_NE(baseline, nullptr);
  EXPECT_EQ(*baseline, "New");
}

TEST_F(SettingsProfileNicknameTest, CancelOrTheSameNameSavesNothingAndChangesNothing) {
  TapNicknameRowAndAnswer(false, "New");
  TapNicknameRowAndAnswer(true, "Old");
  EXPECT_TRUE(saved_with.empty());
  EXPECT_EQ(ShownName(), "Old");
  EXPECT_TRUE(toasts.empty());
}

TEST_F(SettingsProfileNicknameTest, WhileARemountIsRunningTheOldNameStaysAndTheUserIsToldItWasNotSaved) {
  UiEditSession::Instance().BeginRemount();
  TapNicknameRowAndAnswer(true, "New");
  UiEditSession::Instance().EndRemount();
  context->Update();
  EXPECT_TRUE(saved_with.empty());
  EXPECT_EQ(BoundName(), "Old");
  EXPECT_EQ(ShownName(), "Old");
  ASSERT_EQ(toasts.size(), 1u);
  EXPECT_EQ(toasts[0], "Couldn't save the name. Try again.");
}

TEST_F(SettingsProfileNicknameTest, AFailedSaveRestoresTheOldNameAndSaysSoNotSaved) {
  save_fails = true;
  TapNicknameRowAndAnswer(true, "New");
  EXPECT_EQ(saved_with, (std::vector<std::string>{"New"}));
  EXPECT_EQ(BoundName(), "Old");
  EXPECT_EQ(ShownName(), "Old");
  for (const std::string& toast : toasts) {
    EXPECT_NE(toast, Tr("settings.saved"));
  }
  EXPECT_FALSE(toasts.empty()) << "the failure is told";
  const std::string* baseline = UiEditSession::Instance().Baseline(kUiFieldProfileNickname);
  ASSERT_NE(baseline, nullptr);
  EXPECT_EQ(*baseline, "Old");
}

TEST_F(SettingsProfileNicknameTest, WithoutThePromptPortTheRowDoesNothing) {
  ShellFeedbackPorts none;
  settings.BindShellFeedback(none);
  document->GetElementById("edit")->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(prompts, 0);
  EXPECT_EQ(ShownName(), "Old");
}

TEST_F(SettingsProfileNicknameTest, ThePhoneRowIsAWholeRowButtonAtLeast52dpAndTheDesktopKeepsItsInput) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const size_t row = rml.find("data-event-click=\"edit_profile_nickname()\"");
  ASSERT_NE(row, std::string::npos);
  EXPECT_EQ(rml.find("edit_profile_nickname()", row + 40), std::string::npos);
  EXPECT_NE(rml.find("group-chevron", row), std::string::npos);
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t tap = css.find(".shell-layer-compact .settings-mock .group-row-tap {");
  ASSERT_NE(tap, std::string::npos);
  EXPECT_NE(css.find("min-height: 52dp", tap), std::string::npos);
  EXPECT_NE(rml.find("data-event-blur=\"on_profile_nickname_commit()\""), std::string::npos);
  EXPECT_EQ(Tr("settings.profile.nickname_prompt"), "Friends will see this name");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.profile.nickname_prompt"), "朋友会看到这个名字");
  EXPECT_EQ(Tr("settings.profile.nickname_not_saved"), "昵称没有保存，请再试一次");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
