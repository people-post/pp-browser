// "Show advanced settings" (Me → Help) and the tester switch (five taps on the version row) are two switches: the first
// shows the Advanced page and the technical rows, the second the Test options page and the QA entry of the Service row
// (DEV/PLAN-advanced-switch-and-tester-mode.md). Driven through the real model: events dispatched on a small document
// bound to the settings model, the session store on a temporary profile.
#include "common/runtime/AppRuntime.h"
#include "feature/settings/SettingsCommands.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/DeploymentProfile.h"
#include "gui/SettingsController.h"
#include "gui/SettingsHost.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/UiEditSession.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/dom/EventListener.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
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

    root =
        std::filesystem::temp_directory_path() / ("pp_adv_tester_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
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
        "<p id=\"sp\">{{security_page}}</p><p id=\"spt\">{{security_page_title}}</p>"
        "<button id=\"sel_adv\" data-event-click=\"select_section('advanced')\">x</button>"
        "<button id=\"open_ep\" data-event-click=\"open_security_page('ai_endpoint')\">x</button>"
        "<button id=\"open_int\" data-event-click=\"open_security_page('integrations')\">x</button>"
        "<button id=\"open_be\" data-event-click=\"open_security_page('backend_custom')\">x</button>"
        "<button id=\"close_sub\" data-event-click=\"close_security_page()\">x</button>"
        "<button id=\"sel_dev\" data-event-click=\"select_section('devices')\">x</button>"
        "<div id=\"xrow\" data-event-click=\"open_security_page('transfer')\">"
        "<button id=\"xq\" data-event-click=\"ask_setting_help('settings.security.transfer_row')\">x</button></div>"
        "<button id=\"copy_code\" data-event-click=\"export_link_device()\">x</button>"
        "<p id=\"copied\">{{transfer_code_copied}}</p>"
        "<button id=\"edit_llm\" data-event-click=\"on_llm_field_changed()\">x</button>"
        "<button id=\"edit_net\" data-event-click=\"on_network_field_changed()\">x</button>"
        "<button id=\"edit_int\" data-event-click=\"on_integrations_field_changed()\">x</button>"
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
  std::vector<std::string> ServiceIds() {
    std::vector<std::string> ids;
    for (const SettingsChoicePage::Option& option : settings.BackendChoiceOptions()) {
      ids.push_back(option.id.c_str());
    }
    return ids;
  }

  NullRender render;
  ShellFeedbackPorts ports;
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

// Whatever service is saved, the two switches never change it or the restart-pending state.
// "prod" and "custom" exist in every build; "qa" is saved as "prod" by a build without a QA
// address, so its variant runs only where the build has one.
class SettingsAdvancedTesterServiceTest : public SettingsAdvancedTesterTest {
protected:
  void ExpectSwitchesLeaveTheSavedServiceAlone(const char* env) {
    AppConfig config = store.Snapshot().config;
    config.backend.env = env;
    ASSERT_TRUE(static_cast<bool>(store.SaveConfig(config)));
    ASSERT_EQ(store.Snapshot().config.backend.env, env) << "the build keeps this choice as saved";
    host().UiState().backend_env = env;
    host().PushToView(false);
    const bool pending = host().UiState().backend_restart_pending;
    for (int round = 0; round < 2; ++round) {
      Click("toggle_adv");
      for (int i = 0; i < 5; ++i) {
        Click("version");
      }
    }
    EXPECT_EQ(store.Snapshot().config.backend.env, env);
    EXPECT_EQ(host().UiState().backend_env, env);
    EXPECT_EQ(host().UiState().backend_restart_pending, pending);
  }
};

TEST_F(SettingsAdvancedTesterServiceTest, TheSwitchesNeverMoveTheSavedServiceAwayFromProd) {
  ExpectSwitchesLeaveTheSavedServiceAlone("prod");
}

TEST_F(SettingsAdvancedTesterServiceTest, TheSwitchesNeverMoveTheSavedServiceAwayFromCustom) {
  ExpectSwitchesLeaveTheSavedServiceAlone("custom");
}

TEST_F(SettingsAdvancedTesterServiceTest, TheSwitchesNeverMoveTheSavedServiceAwayFromQa) {
  if (!QaBackendAvailable()) {
    GTEST_SKIP() << "this build has no QA address (-DPP_API_URL_QA), so a saved \"qa\" is read as \"prod\"";
  }
  ExpectSwitchesLeaveTheSavedServiceAlone("qa");
}

TEST_F(SettingsAdvancedTesterTest, PickingCustomWithTheTesterOptionsOffSavesExactlyCustom) {
  AppConfig config = store.Snapshot().config;
  config.backend.env = "qa";
  ASSERT_TRUE(static_cast<bool>(store.SaveConfig(config)));
  host().UiState().backend_env = "qa";
  host().PushToView(false);
  Click("toggle_adv");
  ASSERT_EQ(Text("tst"), "0");
  const std::vector<SettingsChoicePage::Option> options = settings.BackendChoiceOptions();
  ASSERT_EQ(options.size(), 2u);
  EXPECT_STREQ(options[0].id.c_str(), "prod");
  EXPECT_STREQ(options[1].id.c_str(), "custom");
  settings.ChooseBackendForTest(options[1].id.c_str());
  EXPECT_EQ(store.Snapshot().config.backend.env, "custom");
}

TEST_F(SettingsAdvancedTesterTest, TheNetworkResetButtonIsBoundOnlyUnderTheCustomAddressCondition) {
  const std::string rml = ReadAsset("views/settings_section_advanced_backend.rml");
  const size_t button = rml.find("data-event-click=\"reset_section('network')\"");
  ASSERT_NE(button, std::string::npos);
  const size_t gate = rml.rfind("data-if=", button);
  ASSERT_NE(gate, std::string::npos);
  EXPECT_EQ(rml.compare(gate, std::string("data-if=\"backend_is_custom\"").size(), "data-if=\"backend_is_custom\""), 0)
      << "the nearest condition before the button is backend_is_custom";
}

TEST_F(SettingsAdvancedTesterTest, TheTapHintsNameOnlyTheTesterOptions) {
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const char* key :
         {"settings.help.tap_open", "settings.help.tap_close", "settings.help.tester_on", "settings.help.tester_off"}) {
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

TEST_F(SettingsAdvancedTesterTest, QaIsOfferedOnlyWhileTheTesterOptionsAreOnAndWithoutARestart) {
  EXPECT_EQ(ServiceIds(), (std::vector<std::string>{"prod", "custom"}));
  Click("toggle_adv");
  EXPECT_EQ(ServiceIds(), (std::vector<std::string>{"prod", "custom"})) << "Advanced alone does not offer QA";
  for (int i = 0; i < 5; ++i) {
    Click("version");
  }
  EXPECT_EQ(ServiceIds(), (std::vector<std::string>{"prod", "qa", "custom"})) << "same session, no restart";
  for (int i = 0; i < 5; ++i) {
    Click("version");
  }
  EXPECT_EQ(ServiceIds(), (std::vector<std::string>{"prod", "custom"}));
}

TEST_F(SettingsAdvancedTesterTest, WithoutAQaAddressInTheBuildQaIsNeverOffered) {
  host().UiState().backend_qa_available = false;
  host().PushToView(false);
  for (int i = 0; i < 5; ++i) {
    Click("version");
  }
  ASSERT_EQ(Text("tst"), "1");
  EXPECT_EQ(ServiceIds(), (std::vector<std::string>{"prod", "custom"}));
}

TEST_F(SettingsAdvancedTesterTest, RunningOnQaWithTheTesterOptionsOffStillShowsQaAndTheChooserHasNoSelectedRow) {
  host().UiState().backend_env = "qa";
  host().PushToView(false);
  context->Update();
  EXPECT_EQ(Text("lbl"), Tr("settings.network.backend.qa")) << "the row tells the truth about the current service";
  const std::vector<SettingsChoicePage::Option> options = settings.BackendChoiceOptions();
  ASSERT_EQ(options.size(), 2u);
  for (const SettingsChoicePage::Option& option : options) {
    EXPECT_FALSE(option.selected) << option.id.c_str();
  }
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

// The Advanced page's free-text fields live on second-level pages on phones (custom endpoint, integration settings,
// custom addresses) and inline on the desktop; the same files serve both, so the inputs keep their bindings and events.
class SettingsAdvancedSubpageTest : public SettingsAdvancedTesterTest {
protected:
  void SetUp() override {
    AppRuntime::ShutdownUI(); // a fresh UI queue; this page's own posted tasks are dropped in TearDown
    AppRuntime::InitializeUI();
    SettingsAdvancedTesterTest::SetUp();
    ShellNavigationPorts nav;
    nav.snapshot = []() {
      ShellChromeSnapshot chrome;
      chrome.layout_mode = LayoutMode::Compact;
      chrome.nav_tab = NavTab::Me;
      return chrome;
    };
    nav.push_local_back = [this](const std::string& id, std::function<void()> commit) {
      pushed.push_back(id);
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
    settings.BindShellNavigation(std::move(nav));
    Click("toggle_adv");
    Click("sel_adv"); // the Advanced page, as a phone opens it
    pushed.clear();
  }
  void TearDown() override {
    settings.BindShellNavigation({});
    SettingsAdvancedTesterTest::TearDown();
    AppRuntime::ShutdownUI(); // tasks left queued hold this controller, which is gone
    AppRuntime::InitializeUI();
  }
  std::vector<std::string> pushed;
  std::vector<std::pair<std::string, std::function<void()>>> back_stack;
};

TEST_F(SettingsAdvancedSubpageTest, EachEntryOpensItsPageOnTheLocalBackStackAndBackClosesIt) {
  AppConfig config = store.Snapshot().config;
  config.backend.env = "custom";
  ASSERT_TRUE(static_cast<bool>(store.SaveConfig(config)));
  host().UiState().backend_env = "custom";
  host().PushToView(false);
  const struct {
    const char* button;
    const char* page;
    const char* en;
    const char* zh;
  } entries[] = {{"open_ep", "ai_endpoint", "Custom endpoint", "自定义接口"},
                 {"open_int", "integrations", "Integration settings", "集成设置"},
                 {"open_be", "backend_custom", "Custom addresses", "自定义地址"}};
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const auto& entry : entries) {
      pushed.clear();
      Click(entry.button);
      EXPECT_EQ(Text("sp"), entry.page) << entry.button;
      EXPECT_EQ(Text("spt"), std::string(language) == "en" ? entry.en : entry.zh);
      // One level on the same back stack entry the transfer and choice pages use, so the arrow, the system back and
      // the edge swipe (ShellHost::HiddenTabBack / BackTarget, pinned in shell_host_test) close it.
      ASSERT_EQ(pushed.size(), 1u);
      EXPECT_EQ(pushed[0], "settings_subpage");
      Click("close_sub");       // the arrow: goes through the back stack
      AppRuntime::RunUITasks(); // the page's redraw is posted
      context->Update();
      ASSERT_EQ(back_stack.size(), 1u) << "only the section page itself is left";
      EXPECT_EQ(back_stack.back().first, "settings_detail");
      EXPECT_EQ(Text("sp"), "") << "the page is closed";
    }
  }
  // The custom addresses page only opens while the service is Custom.
  host().UiState().backend_env = "prod";
  host().PushToView(false);
  pushed.clear();
  Click("open_be");
  EXPECT_TRUE(pushed.empty());
  EXPECT_EQ(Text("sp"), "");
}

TEST_F(SettingsAdvancedSubpageTest, EditingOnASubpageSavesThroughTheSameHandlerAndLeavingLosesNothing) {
  // LLM field: the sub-page's inputs carry on_llm_field_changed() (pinned by the row checklist); the event saves it.
  host().UiState().llm_model = "model-from-subpage";
  host().PushToView(false);
  Click("open_ep");
  ASSERT_EQ(Text("sp"), "ai_endpoint");
  Click("edit_llm");
  Click("close_sub"); // leave before the debounce fires
  context->Update();
  settings.OnMeSurfaceClosed(); // leaving Me flushes whatever is dirty
  EXPECT_EQ(store.Snapshot().config.llm.model, "model-from-subpage");
  // Integrations field.
  host().UiState().search_provider = "example-search";
  host().PushToView(false);
  Click("open_int");
  ASSERT_EQ(Text("sp"), "integrations");
  Click("edit_int");
  Click("close_sub");
  context->Update();
  settings.OnMeSurfaceClosed(); // leaving Me flushes whatever is dirty
  EXPECT_EQ(store.Snapshot().config.search.provider, "example-search");
}

TEST_F(SettingsAdvancedSubpageTest, EverySubpageFileIsIncludedOnceInTheDesktopPageAndOnceInItsPhonePage) {
  const std::string advanced = ReadAsset("views/settings_section_advanced.rml");
  const std::string settings_rml = ReadAsset("views/settings.rml");
  const std::string catalog = [] {
    std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / "src/lib/ui/ViewCatalog.cpp");
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }();
  const struct {
    const char* file;
    const char* page;
  } files[] = {{"settings_section_advanced_endpoint", "ai_endpoint"},
               {"settings_section_advanced_integrations", "integrations"},
               {"settings_section_advanced_backend", "backend_custom"}};
  const auto count = [](const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
      ++n;
    }
    return n;
  };
  for (const auto& f : files) {
    const std::string include = std::string("{{include:") + f.file + "}}";
    EXPECT_EQ(count(advanced, include), 1u) << f.file;
    // The desktop copy is hidden on phones; the phone entry row is hidden on the desktop.
    const size_t at = advanced.find(include);
    EXPECT_NE(advanced.rfind("settings-desktop-only", at), std::string::npos);
    EXPECT_EQ(count(settings_rml, include), 1u) << f.file;
    EXPECT_NE(settings_rml.find(std::string("data-if=\"security_page == '") + f.page + "'\""), std::string::npos);
    // The custom addresses have a second entry for builds without the Service row (the demo build).
    EXPECT_EQ(count(advanced, std::string("data-event-click=\"open_security_page('") + f.page + "')\""),
              std::string(f.page) == "backend_custom" ? 2u : 1u);
    EXPECT_NE(catalog.find(std::string("{\"") + f.file + "\", \"views/" + f.file + ".rml\"}"), std::string::npos);
  }
}

} // namespace
// Devices & transfer (2026-11-14): the "?" inside the transfer row stops its click, and the "copied" flag of the transfer
// page is cleared when the page is left. Driven through the real settings model with the old events.
TEST_F(SettingsAdvancedSubpageTest, ATapOnTheHelpMarkInTheTransferRowDoesNotOpenThePage) {
  Click("sel_dev");
  ASSERT_EQ(Text("sp"), "");
  Click("xq");
  EXPECT_EQ(Text("sp"), "") << "the \"?\" asks; the row's own click never fires";
  Click("xrow");
  EXPECT_EQ(Text("sp"), "transfer") << "a tap on the row itself still opens the page";
}

TEST_F(SettingsAdvancedSubpageTest, TheCopiedFlagIsSetByTheExportAndClearedWhenThePageIsLeftOrReopened) {
  AppRuntime::Initialize();
  commands.export_link_device = []() -> Roe<std::string> { return std::string("code"); };
  commands.load_pin_protection = []() { return PinProtectionView{.ready = true, .unlocked = true}; };
  int confirms = 0;
  bool accept = true;
  ports.show_confirm = [&](const std::string&, const std::string&, std::function<void(bool)> on_result,
                           const std::string&) {
    ++confirms;
    on_result(accept);
  };
  settings.BindShellFeedback(ports);
  settings.BindCommands(commands);
  ProfilePreferences prefs = store.Snapshot().profile_prefs;
  prefs.pin_is_default = true; // the app's public PIN: the export only confirms (the PIN gate has its own tests)
  ASSERT_TRUE(static_cast<bool>(store.SaveProfilePrefs(prefs)));
  Click("sel_dev");
  Click("xrow");
  ASSERT_EQ(Text("sp"), "transfer");
  EXPECT_EQ(Text("copied"), "0");

  accept = false;
  Click("copy_code");
  AppRuntime::DrainWorkersThenUI();
  AppRuntime::RunUITasks();
  context->Update();
  EXPECT_EQ(confirms, 1);
  EXPECT_EQ(Text("copied"), "0") << "a cancelled confirm leaves the button";

  accept = true;
  Click("copy_code");
  AppRuntime::DrainWorkersThenUI();
  AppRuntime::RunUITasks();
  context->Update();
  EXPECT_EQ(Text("copied"), "1");

  Click("close_sub");
  AppRuntime::RunUITasks();
  context->Update();
  EXPECT_EQ(Text("copied"), "0") << "leaving the page clears it";
  AppRuntime::Shutdown();
}

// --- the real views, real clicks (Kenneth 2026-11-14 review) ---------------------------------------------------------------

namespace {

// A view the way the app loads it: localized, its {{include:x}} files spelled out.
std::string ExpandedView(const std::string& key, int depth = 0) {
  std::string text = LocalizationService::Instance().LocalizeText(ReadAsset("views/" + key + ".rml"));
  const std::string marker = "{{include:";
  for (size_t at = text.find(marker); at != std::string::npos && depth < 8; at = text.find(marker)) {
    const size_t end = text.find("}}", at);
    const std::string name = text.substr(at + marker.size(), end - at - marker.size());
    text.replace(at, end + 2 - at, ExpandedView(name, depth + 1));
  }
  return text;
}

// Counts the "change" events that reach an input, by the binding it carries.
class ChangeCounter : public ui::EventListener {
public:
  void ProcessEvent(ui::Event& ev) override {
    if (const ui::Element* target = ev.GetTargetElement()) {
      const ui::String binding = target->GetAttribute<ui::String>("data-value", "");
      if (!binding.empty()) {
        ++by_target[target];
        ++by_binding[std::string(binding.c_str())];
      }
    }
  }
  std::map<const ui::Element*, int> by_target;
  std::map<std::string, int> by_binding;
};

} // namespace

class SettingsRealViewTest : public SettingsAdvancedSubpageTest {
protected:
  void SetUp() override {
    SettingsAdvancedSubpageTest::SetUp();
    // The phone's settings view with the sections beside it, both bound to the settings model.
    real = context->LoadDocumentFromMemory("<rml><body>" + ExpandedView("settings") + "<div data-model=\"settings\">" +
                                           ExpandedView("settings_sections") + "</div></body></rml>");
    ASSERT_NE(real, nullptr);
    real->Show();
    context->Update();
  }
  void TearDown() override {
    if (real) {
      real->Close();
      real = nullptr;
    }
    SettingsAdvancedSubpageTest::TearDown();
  }
  // The `n`th element whose data-event-click starts with `event` under `root` (a CSS selector scope).
  ui::Element* ClickTarget(const std::string& scope, const std::string& event, size_t n = 0) {
    ui::ElementList found;
    real->QuerySelectorAll(found, scope + " [data-event-click^=\"" + event + "\"]");
    return n < found.size() ? found[n] : nullptr;
  }
  void RealClick(ui::Element* element) {
    ASSERT_NE(element, nullptr);
    element->DispatchEvent(ui::EventId::Click, {});
    context->Update();
    AppRuntime::RunUITasks();
    context->Update();
  }
  void UseCustomService() {
    AppConfig config = store.Snapshot().config;
    config.backend.env = "custom";
    ASSERT_TRUE(static_cast<bool>(store.SaveConfig(config)));
    host().UiState().backend_env = "custom";
    host().PushToView(false);
  }
  ui::ElementDocument* real = nullptr;
};

// The "Custom addresses" page is empty once the service stops being Custom: a confirmed reset closes it (the Advanced page,
// where the restart line shows, is what is left); a cancelled one leaves it.
TEST_F(SettingsRealViewTest, ResettingTheCustomAddressesClosesThePageAndCancellingLeavesIt) {
  UseCustomService();
  std::function<void(bool)> answer;
  ports.show_confirm_destructive = [&answer](const std::string&, const std::string&,
                                             std::function<void(bool)> on_result,
                                             const std::string&) { answer = std::move(on_result); };
  settings.BindShellFeedback(ports);

  Click("open_be");
  ASSERT_EQ(Text("sp"), "backend_custom");
  ASSERT_EQ(back_stack.size(), 2u) << "the section page and the sub-page";
  RealClick(ClickTarget("#settings-subpage-view", "reset_section('network')"));
  ASSERT_TRUE(static_cast<bool>(answer)) << "the reset asks first";

  answer(false);
  AppRuntime::RunUITasks();
  context->Update();
  EXPECT_EQ(Text("sp"), "backend_custom") << "cancelled: still on the sub-page";
  EXPECT_EQ(back_stack.size(), 2u);
  EXPECT_EQ(store.Snapshot().config.backend.env, "custom");

  answer(true);
  AppRuntime::RunUITasks();
  context->Update();
  EXPECT_EQ(Text("sp"), "") << "no longer Custom: the page is closed";
  ASSERT_EQ(back_stack.size(), 1u);
  EXPECT_EQ(back_stack.back().first, "settings_detail") << "the Advanced page is current";
  EXPECT_NE(store.Snapshot().config.backend.env, "custom");
}

// A "?" inside a row that opens a page or toggles a switch only shows its note.
TEST_F(SettingsRealViewTest, TheHelpMarksOfTheServiceRowAndTheAutoUpdateRowOnlyShowTheirNote) {
  std::vector<std::string> alerts;
  ports.show_alert = [&alerts](const std::string& title, const std::string&, std::function<void()>,
                               const std::string&) { alerts.push_back(title); };
  settings.BindShellFeedback(ports);
  settings.SyncLayoutMode(); // a phone: the Service row opens its chooser as a page

  // Service: the "?" sits inside the row's own button.
  ui::Element* service_q = ClickTarget("", "show_setting_note('settings.network.backend'");
  ASSERT_NE(service_q, nullptr);
  RealClick(service_q);
  ASSERT_EQ(alerts.size(), 1u);
  EXPECT_EQ(alerts[0], "Service");
  EXPECT_EQ(Text("sp"), "") << "the Service chooser did not open";
  RealClick(ClickTarget("", "on_choose_backend()"));
  EXPECT_EQ(Text("sp"), "choice") << "control: a tap on the row itself still opens it";
  Click("close_sub");
  AppRuntime::RunUITasks();
  context->Update();

  // Auto-update: the "?" never flips the switch.
  ui::Element* knob = ClickTarget("", "toggle_auto_renew_registration()");
  ASSERT_NE(knob, nullptr);
  const bool before = knob->IsClassSet("settings-switch--on");
  RealClick(ClickTarget("", "show_setting_note('settings.auto_renew.short'"));
  ASSERT_EQ(alerts.size(), 2u);
  EXPECT_EQ(alerts[1], "Auto-update");
  EXPECT_EQ(knob->IsClassSet("settings-switch--on"), before) << "the switch did not move";
  RealClick(knob);
  EXPECT_NE(knob->IsClassSet("settings-switch--on"), before) << "control: the switch itself still moves";
}

// Each free-text field exists twice (the phone's sub-page and the desktop's inline copy of the same file). Typing into the
// visible one fires one change, on that input only, and the value is saved once.
TEST_F(SettingsRealViewTest, TypingIntoATwinInputFiresTheChangeHandlerOnceAndSavesOnce) {
  UseCustomService();
  ChangeCounter counter;
  real->AddEventListener(ui::EventId::Change, &counter, true);
  const struct {
    const char* entry;
    const char* page;
    const char* binding;
  } fields[] = {{"open_ep", "ai_endpoint", "llm_model"}, {"open_be", "backend_custom", "relay_base_url"}};
  for (const auto& field : fields) {
    SCOPED_TRACE(field.binding);
    counter.by_target.clear();
    counter.by_binding.clear();
    toasts.clear();
    Click(field.entry);
    ASSERT_EQ(Text("sp"), field.page);
    const std::string selector = std::string("input[data-value=\"") + field.binding + "\"]";
    ui::ElementList all;
    real->QuerySelectorAll(all, selector);
    ASSERT_EQ(all.size(), 2u) << "the phone's page and the desktop's inline copy";
    ui::Element* phone_copy = nullptr;
    ui::ElementList in_page;
    real->QuerySelectorAll(in_page, "#settings-subpage-view " + selector);
    ASSERT_EQ(in_page.size(), 1u);
    phone_copy = in_page[0];
    ASSERT_TRUE(phone_copy->Focus());
    context->Update();
    context->ProcessTextInput("x");
    context->Update();
    phone_copy->Blur();
    context->Update();
    EXPECT_EQ(counter.by_binding[field.binding], 1) << "one change event for the binding, not one per copy";
    EXPECT_EQ(counter.by_target[phone_copy], 1);
    for (ui::Element* copy : all) {
      if (copy != phone_copy) {
        EXPECT_EQ(counter.by_target[copy], 0) << "the other copy is not touched";
      }
    }
    settings.OnMeSurfaceClosed(); // leaving Me flushes whatever is dirty
    EXPECT_EQ(std::count(toasts.begin(), toasts.end(), Tr("settings.saved")), 1) << "saved once";
    Click("close_sub");
    AppRuntime::RunUITasks();
    context->Update();
  }
  EXPECT_NE(store.Snapshot().config.llm.model.find('x'), std::string::npos);
  real->RemoveEventListener(ui::EventId::Change, &counter, true);
}

} // namespace pbr
