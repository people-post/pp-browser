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
  // Every row that picks one value still has its handler (the controller decides menu or page).
  for (const char* view : {"views/settings_section_appearance.rml", "views/settings_section_security.rml",
                           "views/settings_section_network.rml", "views/settings_section_storage.rml"}) {
    EXPECT_NE(ReadAsset(view).find("data-event-click=\"on_choose_"), std::string::npos) << view;
  }
}

TEST(SettingsAppearancePageTest, HasNoReduceTransparencyRow) {
  // Kenneth 2026-11-04 (PLAN-ui-refresh-impl 62.1): the opaque-bar switch made no visible difference; the page keeps
  // theme, language and call diagnostics, and the bars stay as they were by default.
  const std::string rml = ReadAsset("views/settings_section_appearance.rml");
  ASSERT_FALSE(rml.empty());
  EXPECT_EQ(rml.find("reduce_transparency"), std::string::npos);
  EXPECT_NE(rml.find("on_choose_theme()"), std::string::npos);
  EXPECT_NE(rml.find("toggle_call_diagnostics()"), std::string::npos);
}

} // namespace
} // namespace pbr
