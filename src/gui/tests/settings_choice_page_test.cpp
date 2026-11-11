// Phone settings that pick one value (Kenneth 2026-11-04, ui-mockups/09-sheets-unify ①): a pushed page with one row
// per value and a check on the current one; a tap applies it through the old menu's path, the check moves, and the
// page goes back after a short beat.
#include "gui/SettingsChoicePage.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::vector<SettingsChoicePage::Option> CallOptions() {
  return {{"everyone", "Everyone", true}, {"contacts_only", "My contacts", false}, {"nobody", "Nobody", false}};
}

std::vector<std::string> Checked(const SettingsChoicePage& page) {
  std::vector<std::string> ids;
  for (const SettingsChoicePage::Option& option : page.Options()) {
    if (option.selected) {
      ids.push_back(option.id.c_str());
    }
  }
  return ids;
}

TEST(SettingsChoicePageTest, ATapAppliesTheValueAndMovesTheCheck) {
  SettingsChoicePage page;
  std::vector<std::string> applied;
  page.Open("call_invite_policy", "Who can call me", "Others ring silently.", "", CallOptions(),
            [&applied](const std::string& id) { applied.push_back(id); });
  ASSERT_TRUE(page.IsOpen());
  EXPECT_EQ(page.Title(), "Who can call me");
  EXPECT_EQ(page.Foot(), "Others ring silently.");
  EXPECT_EQ(Checked(page), (std::vector<std::string>{"everyone"}));

  EXPECT_TRUE(page.Pick("contacts_only"));
  EXPECT_EQ(applied, (std::vector<std::string>{"contacts_only"}));
  EXPECT_EQ(Checked(page), (std::vector<std::string>{"contacts_only"}));
  EXPECT_TRUE(page.Picked()) << "the page goes back after the beat";
  EXPECT_EQ(SettingsChoicePage::kPopDelayMs, 250);
}

TEST(SettingsChoicePageTest, TheLeavingPageTakesNoSecondTapAndUnknownRowsDoNothing) {
  SettingsChoicePage page;
  std::vector<std::string> applied;
  page.Open("theme", "Appearance", "", "", {{"system", "System", true}, {"dark", "Dark", false}},
            [&applied](const std::string& id) { applied.push_back(id); });
  EXPECT_FALSE(page.Pick("sepia"));
  EXPECT_FALSE(page.Picked());
  EXPECT_TRUE(page.Pick("dark"));
  EXPECT_FALSE(page.Pick("system")) << "already on its way back";
  EXPECT_EQ(applied, (std::vector<std::string>{"dark"}));
}

TEST(SettingsChoicePageTest, TheCurrentValueTappedAgainStillGoesBack) {
  SettingsChoicePage page;
  int applied = 0;
  page.Open("language", "Language", "", "", {{"system", "System", true}, {"en", "English", false}},
            [&applied](const std::string&) { ++applied; });
  EXPECT_TRUE(page.Pick("system"));
  EXPECT_EQ(applied, 1);
  EXPECT_TRUE(page.Picked());
}

TEST(SettingsChoicePageTest, ClosedItForgetsItsRows) {
  SettingsChoicePage page;
  page.Open("theme", "Appearance", "", "settings.theme", CallOptions(), [](const std::string&) {});
  EXPECT_EQ(page.HelpTopic(), "settings.theme");
  page.Close();
  EXPECT_FALSE(page.IsOpen());
  EXPECT_TRUE(page.Options().empty());
  EXPECT_FALSE(page.Pick("everyone"));
}

TEST(SettingsChoicePageTest, ThePushedPageHasTheRowsTheCheckTheFootAndTheHelp) {
  const std::string rml = ReadAsset("views/settings.rml");
  EXPECT_NE(rml.find("data-if=\"security_page == 'choice'\""), std::string::npos);
  EXPECT_NE(rml.find("data-for=\"option : choice_options\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"pick_choice(option.id)\""), std::string::npos);
  EXPECT_NE(rml.find("class=\"settings-choice-check\" data-if=\"option.selected\""), std::string::npos);
  EXPECT_NE(rml.find("{{choice_foot}}"), std::string::npos);
  EXPECT_NE(rml.find("ask_setting_help(choice_help_topic)"), std::string::npos);
  // Every row that picks one value still has its handler (the controller decides menu or page). Storage picks its
  // download policy with ticked rows in the page itself (choose_attachment_download_policy), not a choice page.
  for (const char* view : {"views/settings_section_appearance.rml", "views/settings_section_security.rml",
                           "views/settings_section_advanced.rml"}) {
    EXPECT_NE(ReadAsset(view).find("data-event-click=\"on_choose_"), std::string::npos) << view;
  }
}

TEST(SettingsAppearancePageTest, FollowsTheMockupWidgetsGroupWithTheSecondRowGreyedOutWhileTheFirstIsOff) {
  const std::string rml = ReadAsset("views/settings_section_appearance.rml");
  EXPECT_NE(rml.find("settings-section--grouped settings-mock stack"), std::string::npos);
  const size_t head = rml.find("{{i18n:settings.widgets.title}}");
  ASSERT_NE(head, std::string::npos);
  const size_t toggle = rml.find("toggle_home_show_dynamic()", head);
  const size_t dim = rml.find("data-class-group-row--dim=\"home_show_dynamic != 'on'\"", head);
  const size_t choose = rml.find("open_home_edit()", head);
  ASSERT_NE(toggle, std::string::npos);
  ASSERT_NE(dim, std::string::npos);
  ASSERT_NE(choose, std::string::npos);
  EXPECT_LT(toggle, dim) << "the switch comes first, the greyed row second, in one card";
  EXPECT_LT(dim, choose);
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(css.find(".shell-layer-compact .settings-mock .group-row--dim {\n  opacity: 0.4;\n  pointer-events: none;"),
            std::string::npos);
  // The reset button keeps its place; a line under it says what it resets (the call diagnostics switch included).
  EXPECT_NE(rml.find("reset_section('appearance')"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.appearance.reset_help}}"), std::string::npos);
}

TEST(SettingsAppearancePageTest, HasTheTransparencySwitchOnByDefault) {
  // Kenneth 2026-11-04 (PLAN-ui-refresh-impl 67): the switch is back as "Transparency", on unless reduce_transparency
  // is on (its default is off), with a one-line help under it.
  const std::string rml = ReadAsset("views/settings_section_appearance.rml");
  ASSERT_FALSE(rml.empty());
  EXPECT_NE(rml.find("{{i18n:settings.transparency.label}}"), std::string::npos);
  EXPECT_NE(rml.find("data-class-settings-switch--on=\"reduce_transparency != 'on'\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"toggle_transparency()\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.transparency.help}}"), std::string::npos);
  EXPECT_NE(rml.find("on_choose_theme()"), std::string::npos);
  EXPECT_NE(ReadAsset("views/settings_section_tester.rml").find("toggle_call_diagnostics()"), std::string::npos)
      << "call diagnostics moved to the test options page";

  const std::string controller = ReadFile(std::string(PP_SOURCE_DIR) + "/src/gui/SettingsController.cpp");
  EXPECT_NE(controller.find("BindEventCallback(\"toggle_transparency\""), std::string::npos);
  const std::string settings_header = ReadFile(std::string(PP_SOURCE_DIR) + "/src/gui/SettingsController.h");
  EXPECT_NE(settings_header.find("ui::String reduce_transparency = \"off\";"), std::string::npos)
      << "the switch is on by default";
}

} // namespace
} // namespace pbr
