// About shows which build this is, under the version: "Build a1b2c3d4" (the tag in front when the build has one). The real
// Help page over the settings model, with its {{i18n:…}} words filled from the app's text.
#include "common/runtime/AppRuntime.h"
#include "feature/settings/SettingsCommands.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/product/BuildInfo.h"
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

std::string FilledView(const std::string& name) {
  const std::string rml = ReadAsset("views/" + name);
  std::string out;
  static const std::regex word(R"(\{\{i18n:([^}]*)\}\})");
  auto last = rml.cbegin();
  for (std::sregex_iterator it(rml.begin(), rml.end(), word), end; it != end; ++it) {
    out.append(last, (*it)[0].first);
    out += Tr((*it)[1].str());
    last = (*it)[0].second;
  }
  out.append(last, rml.cend());
  return out;
}

constexpr const char* kContext = "settings-about-build-render-test";

class SettingsAboutBuildRenderTest : public testing::Test {
protected:
  void SetUp() override {
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
           ("pp_about_build_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
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
  }
  void TearDown() override {
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

  /** Loads the real Help page bound to the model, with the build line the About section would hand it. */
  void ShowHelpPage(const std::string& build, const std::string& tester_build_number = {}) {
    SettingsHost& host = settings;
    host.UiState().app_version = "1.0.0-test";
    host.UiState().app_build = build;
    host.UiState().tester_build_number = tester_build_number;
    host.PushToView(false);
    ASSERT_TRUE(settings.RegisterModel(context));
    document = context->LoadDocumentFromMemory("<rml><body><div data-model=\"settings\">" +
                                               FilledView("settings_section_help.rml") + "</div></body></rml>");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }
  /** The text of every element matching `selector` that is shown (a `data-if` that is false hides its element). */
  std::vector<std::string> Texts(const std::string& selector) {
    ui::ElementList found;
    document->QuerySelectorAll(found, selector);
    std::vector<std::string> out;
    for (ui::Element* element : found) {
      if (element->IsVisible(true)) {
        out.emplace_back(element->GetInnerRML().c_str());
      }
    }
    return out;
  }

  NullRender render;
  QuietSystem system;
  SessionStore store;
  SettingsCommands commands;
  SettingsController settings;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  bool initialized = false;
  std::filesystem::path root;
};

TEST_F(SettingsAboutBuildRenderTest, PhoneRowShowsBuildUnderTheVersion) {
  ShowHelpPage("a1b2c3d4");
  const std::vector<std::string> labels = Texts(".settings-phone-only .group-row-body > .group-row-text");
  ASSERT_GE(labels.size(), 2u);
  EXPECT_EQ(labels[0], "Version");
  EXPECT_EQ(labels[1], "Build");
  const std::vector<std::string> values = Texts(".settings-phone-only .group-row-value");
  ASSERT_GE(values.size(), 2u);
  EXPECT_EQ(values[0], "1.0.0-test");
  EXPECT_EQ(values[1], "a1b2c3d4");
}

TEST_F(SettingsAboutBuildRenderTest, DesktopFootShowsBuildAfterTheVersion) {
  ShowHelpPage(FormatBuildLabel("a1b2c3d4-dirty", "internal/141"));
  const std::vector<std::string> feet = Texts("p.settings-desktop-only");
  bool found = false;
  for (const std::string& foot : feet) {
    if (foot.find("Build") == 0) {
      EXPECT_NE(foot.find("internal/141 \xC2\xB7 a1b2c3d4-dirty"), std::string::npos) << foot;
      found = true;
    }
  }
  EXPECT_TRUE(found) << "a Build line on the desktop";
}

// Release-compatibility RC022: a tester build says so under the build, with its TestFlight build number, on the phone
// and on the desktop; a store build (an empty number) shows no such line.
TEST_F(SettingsAboutBuildRenderTest, ATesterBuildNamesItselfWithItsBuildNumber) {
  ShowHelpPage(FormatBuildLabel("a1b2c3d4", "internal/141"), TesterBuildNumber(true, "internal/141", "1.0.0"));
  const std::vector<std::string> labels = Texts(".settings-phone-only .group-row-body > .group-row-text");
  const std::vector<std::string> values = Texts(".settings-phone-only .group-row-value");
  ASSERT_GE(labels.size(), 3u);
  ASSERT_GE(values.size(), 3u);
  EXPECT_EQ(labels[2], "Tester build");
  EXPECT_EQ(values[2], "141");
  bool foot = false;
  for (const std::string& line : Texts("p.settings-desktop-only")) {
    foot = foot || line.find("Tester build <span>141</span>") == 0;
  }
  EXPECT_TRUE(foot) << "a Tester build line on the desktop";
}

TEST_F(SettingsAboutBuildRenderTest, AStoreBuildShowsNoTesterLine) {
  ShowHelpPage("v1.0.0-rc.3 \xC2\xB7 a1b2c3d4", TesterBuildNumber(false, "v1.0.0-rc.3", "1.0.0"));
  EXPECT_EQ(Texts(".settings-phone-only .group-row-body > .group-row-text").size(), 2u) << "Version and Build only";
  for (const std::string& line : Texts("p.settings-desktop-only")) {
    EXPECT_EQ(line.find("Tester build"), std::string::npos) << line;
  }
}

// This build's About, as the section fills it: the line exactly in a tester build.
TEST_F(SettingsAboutBuildRenderTest, ThisBuildShowsTheTesterLineExactlyWhenItIsOne) {
  ShowHelpPage(BuildLabel(), TesterBuildNumber(IsTesterBuild(), BuildTag(), "1.0.0"));
  const std::vector<std::string> labels = Texts(".settings-phone-only .group-row-body > .group-row-text");
  EXPECT_EQ(std::find(labels.begin(), labels.end(), "Tester build") != labels.end(), IsTesterBuild());
}

TEST_F(SettingsAboutBuildRenderTest, ChineseNamesTheRowBuild) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  ShowHelpPage("a1b2c3d4");
  const std::vector<std::string> labels = Texts(".settings-phone-only .group-row-body > .group-row-text");
  ASSERT_GE(labels.size(), 2u);
  EXPECT_EQ(labels[1], "\xE6\x9E\x84\xE5\xBB\xBA"); // 构建
  EXPECT_EQ(Texts(".settings-phone-only .group-row-value")[1], "a1b2c3d4");
}

} // namespace
} // namespace pbr
