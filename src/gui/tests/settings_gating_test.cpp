// Advanced and Test options are hidden pages (phase 2 of DEV/PLAN-settings-reorg.md): their views are gated on the
// advanced flag at the root, and Home / the desktop list reach every page only through the listed rows.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

size_t CountOf(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

} // namespace

TEST(SettingsGatingTest, TheRootOfAdvancedAndTestOptionsIsGatedOnTheFlag) {
  for (const char* page : {"advanced", "tester"}) {
    const std::string rml = ReadAsset(std::string("views/settings_section_") + page + ".rml");
    ASSERT_FALSE(rml.empty()) << page;
    const size_t end_of_root = rml.find('>');
    ASSERT_NE(end_of_root, std::string::npos);
    EXPECT_NE(rml.substr(0, end_of_root).find("data-if=\"advanced\""), std::string::npos) << page;
  }
  // Every other page is not gated at its root.
  for (const char* page :
       {"profile", "privacy", "security", "devices", "notifications", "appearance", "storage", "help"}) {
    const std::string rml = ReadAsset(std::string("views/settings_section_") + page + ".rml");
    const size_t end_of_root = rml.find('>');
    ASSERT_NE(end_of_root, std::string::npos);
    EXPECT_EQ(rml.substr(0, end_of_root).find("data-if"), std::string::npos) << page;
  }
}

TEST(SettingsGatingTest, TheAiCredentialRowLivesOnlyOnTheAdvancedPage) {
  const std::string profile = ReadAsset("views/settings_section_profile.rml");
  EXPECT_EQ(profile.find("rotate_brief_llm_key"), std::string::npos);
  EXPECT_EQ(profile.find("settings.llm.rotate_brief_key"), std::string::npos);
  const std::string advanced = ReadAsset("views/settings_section_advanced.rml");
  EXPECT_EQ(CountOf(advanced, "data-event-click=\"rotate_brief_llm_key()\""), 1u);
  EXPECT_EQ(CountOf(advanced, "data-if=\"advanced && profile_show_rotate\""), 1u) << "the gate moved unchanged";
}

// Phase 7 (Security, mockup layout only): the page's events are exactly the ones it had, and the recovery phrase is
// bound once, inside the block that only exists while the phrase is shown (the PIN gate is in show_recovery_phrase()).
TEST(SettingsGatingTest, TheSecurityPageHasTheSameEventsAndTheRecoveryPhraseOnlyInItsShownBlock) {
  const std::string rml = ReadAsset("views/settings_section_security.rml");
  ASSERT_FALSE(rml.empty());
  EXPECT_EQ(CountOf(rml, "data-event-click=\""), 6u);
  for (const char* event : {"toggle_biometric()", "open_security_page('change_pin')", "on_choose_unlock_grace()",
                            "ask_setting_help('settings.security.recovery')", "show_recovery_phrase()",
                            "hide_recovery_phrase()"}) {
    EXPECT_EQ(CountOf(rml, std::string("data-event-click=\"") + event + "\""), 1u) << event;
  }
  ASSERT_EQ(CountOf(rml, "{{recovery_phrase}}"), 1u);
  const size_t phrase = rml.find("{{recovery_phrase}}");
  const size_t block = rml.rfind("data-if=\"recovery_phrase != ''\"", phrase);
  ASSERT_NE(block, std::string::npos);
  const std::string between = rml.substr(block + 10, phrase - block - 10);
  EXPECT_EQ(between.find("data-if="), std::string::npos) << "no other condition between the gate and the phrase";
  EXPECT_LT(rml.rfind("data-if=\"security_has_recovery_phrase\"", block), block) << "and that block is inside the has-phrase gate";
  // The "show" row sits in the not-shown block, so the button never renders the phrase itself.
  const size_t show = rml.find("show_recovery_phrase()");
  EXPECT_NE(rml.rfind("data-if=\"recovery_phrase == ''\"", show), std::string::npos);
  EXPECT_LT(show, phrase);
}

TEST(SettingsGatingTest, HomeAndTheDesktopListNameNoPageDirectly) {
  const std::string home = ReadAsset("views/settings.rml");
  // Both lists loop over `sections`, which leaves Advanced and Test options out while the flag is off.
  EXPECT_EQ(home.find("select_section('advanced')"), std::string::npos);
  EXPECT_EQ(home.find("select_section('tester')"), std::string::npos);
  EXPECT_EQ(CountOf(home, "data-for=\"section : sections\""), 4u) << "three phone groups and the desktop list";
}

TEST(SettingsGatingTest, TheFlagIsSwitchedOnlyByTheVersionRow) {
  const std::string help = ReadAsset("views/settings_section_help.rml");
  EXPECT_EQ(help.find("toggle_show_advanced_settings"), std::string::npos) << "the switch row is gone";
  EXPECT_EQ(help.find("settings.about.advanced"), std::string::npos);
  EXPECT_EQ(CountOf(help, "data-event-click=\"tap_version()\""), 2u) << "phone row and desktop line";
  EXPECT_NE(help.find("data-event-click=\"open_terms()\""), std::string::npos);
  EXPECT_NE(help.find("data-event-click=\"open_privacy_policy()\""), std::string::npos);
}

} // namespace pbr
