// "Show advanced settings" (Me → Help) and the tester switch (five taps on the version row) are two switches: the first
// shows the Advanced page and the technical rows, the second the Test options page and the QA entry of the Service row
// (DEV/PLAN-advanced-switch-and-tester-mode.md). Driven through the real model: events dispatched on a small document
// bound to the settings model, the session store on a temporary profile.
#include "feature/settings/SettingsCommands.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
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

constexpr const char* kContext = "settings-advanced-tester-test";

class SettingsAdvancedTesterTest : public testing::Test {
protected:
  void SetUp() override {
    // A remount left running by another test would suppress the saves below.
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
           ("pp_adv_tester_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "profile");
    BootstrapResult bootstrap{};
    bootstrap.config = Config::DefaultAppConfig();
    bootstrap.config_path = (root / "config.json").string();
    bootstrap.data_dir = root.string();
    bootstrap.profile_data_dir = (root / "profile").string();
    bootstrap.profile_prefs = UserPreferences::DefaultProfile();
    store.SetUiDemo(false);
    store.Initialize(std::move(bootstrap));

    commands.session_store = [this]() -> SessionStore& { return store; };
    settings.BindCommands(commands);
    ShellFeedbackPorts ports;
    ports.show_toast = [this](const std::string& message, ToastDuration) { toasts.push_back(message); };
    settings.BindShellFeedback(ports);
    UserFeedback::BindPorts(ports);

    host().UiState().show_advanced_settings = "off";
    host().UiState().show_tester_options = "off";
    host().UiState().backend_env = "prod";
    host().UiState().backend_qa_available = true; // a build with a QA address
    host().PushToView(false);

    ASSERT_TRUE(settings.RegisterModel(context));
    document = context->LoadDocumentFromMemory(
        "<rml><body><div data-model=\"settings\">"
        "<p id=\"adv\">{{advanced}}</p><p id=\"tst\">{{tester}}</p><p id=\"lbl\">{{backend_label}}</p>"
        "<button id=\"toggle_adv\" data-event-click=\"toggle_advanced_settings()\">x</button>"
        "<button id=\"version\" data-event-click=\"tap_version()\">x</button>"
        "</div></body></rml>");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }
  void TearDown() override {
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
    std::filesystem::remove_all(root);
  }

  SettingsHost& host() { return settings; }
  void Click(const char* id) {
    document->GetElementById(id)->DispatchEvent(ui::EventId::Click, {});
    context->Update();
  }
  std::string Text(const char* id) { return document->GetElementById(id)->GetInnerRML().c_str(); }
  NullRender render;
  QuietSystem system;
  SessionStore store;
  SettingsCommands commands;
  SettingsController settings;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  bool initialized = false;
  std::filesystem::path root;
  std::vector<std::string> toasts;
};

TEST_F(SettingsAdvancedTesterTest, TheHelpSwitchShowsAdvancedAndLeavesTheTesterOptionsAlone) {
  EXPECT_EQ(Text("adv"), "0");
  EXPECT_EQ(Text("tst"), "0");
  Click("toggle_adv");
  EXPECT_EQ(Text("adv"), "1");
  EXPECT_EQ(Text("tst"), "0");
  EXPECT_TRUE(store.Snapshot().profile_prefs.show_advanced_settings) << "saved at once";
  EXPECT_FALSE(store.Snapshot().profile_prefs.show_tester_options);
  Click("toggle_adv");
  EXPECT_EQ(Text("adv"), "0");
  EXPECT_FALSE(store.Snapshot().profile_prefs.show_advanced_settings);
}

TEST_F(SettingsAdvancedTesterTest, FiveTapsOnTheVersionSwitchOnlyTheTesterOptions) {
  Click("toggle_adv");
  for (int i = 0; i < 5; ++i) {
    Click("version");
  }
  EXPECT_EQ(Text("tst"), "1");
  EXPECT_EQ(Text("adv"), "1") << "the advanced switch keeps its value";
  EXPECT_TRUE(store.Snapshot().profile_prefs.show_tester_options);
  EXPECT_TRUE(store.Snapshot().profile_prefs.show_advanced_settings);
  EXPECT_EQ(toasts.back(), Tr("settings.help.tester_on"));
  for (int i = 0; i < 5; ++i) {
    Click("version");
  }
  EXPECT_EQ(Text("tst"), "0");
  EXPECT_EQ(Text("adv"), "1");
  EXPECT_EQ(toasts.back(), Tr("settings.help.tester_off"));
}

TEST_F(SettingsAdvancedTesterTest, TheTapHintsNameOnlyTheTesterOptions) {
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const char* key : {"settings.help.tap_open", "settings.help.tap_close", "settings.help.tester_on",
                            "settings.help.tester_off"}) {
      const std::string text = Tr(key, {{"n", "2"}});
      EXPECT_EQ(text.find("n}"), std::string::npos) << key;
      EXPECT_EQ(text.find("Advanced"), std::string::npos) << language << " " << key << ": " << text;
      EXPECT_EQ(text.find("高级"), std::string::npos) << language << " " << key << ": " << text;
    }
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.help.tap_open", {{"n", "2"}}), "2 more taps to show Test options");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.help.tap_open", {{"n", "2"}}), "再点 2 次打开测试选项");
  EXPECT_EQ(Tr("settings.help.tap_close", {{"n", "2"}}), "再点 2 次收起测试选项");
  EXPECT_EQ(Tr("settings.help.tester_on"), "已显示测试选项");
  EXPECT_EQ(Tr("settings.help.tester_off"), "已隐藏测试选项");
}

TEST_F(SettingsAdvancedTesterTest, TheTesterBindingIsBoundAndRedrawnLikeTheAdvancedOne) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/SettingsController.cpp");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string source = buffer.str();
  for (const char* name : {"show_tester_options", "tester"}) {
    EXPECT_NE(source.find(std::string("ctor.Bind(\"") + name + "\""), std::string::npos) << name << " in Bind";
    EXPECT_NE(source.find(std::string("host.Dirty(\"settings\", \"") + name + "\")"), std::string::npos)
        << name << " in Dirty";
  }
}

TEST_F(SettingsAdvancedTesterTest, TheHelpPageHasTheSwitchRowInBothLayoutsWithItsWords) {
  const std::string help = ReadAsset("views/settings_section_help.rml");
  EXPECT_NE(help.find("data-event-click=\"toggle_advanced_settings()\""), std::string::npos);
  EXPECT_NE(help.find("data-class-settings-switch--on=\"show_advanced_settings == 'on'\""), std::string::npos);
  EXPECT_EQ(Tr("settings.help.show_advanced"), "Show advanced settings");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.help.show_advanced"), "显示高级设置");
  EXPECT_EQ(Tr("settings.help.show_advanced.hint"), "打开后可以看到连接、名片有效期等更多参数。一般不用打开。");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
