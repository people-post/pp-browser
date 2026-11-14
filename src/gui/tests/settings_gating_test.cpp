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

TEST(SettingsGatingTest, TheRootOfAdvancedAndTestOptionsIsGatedOnTheirOwnSwitch) {
  for (const char* page : {"advanced", "tester"}) {
    const std::string rml = ReadAsset(std::string("views/settings_section_") + page + ".rml");
    ASSERT_FALSE(rml.empty()) << page;
    const size_t end_of_root = rml.find('>');
    ASSERT_NE(end_of_root, std::string::npos);
    const std::string root = rml.substr(0, end_of_root);
    EXPECT_NE(root.find(std::string("data-if=\"") + page + "\""), std::string::npos) << page;
    EXPECT_EQ(root.find(std::string("data-if=\"") + (std::string(page) == "tester" ? "advanced" : "tester") + "\""),
              std::string::npos)
        << page << " follows only its own switch";
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

// Phases 7 and 8 (Security, Devices and transfer; mockup layout only). Which element carries which event under which
// gate and in which layout is pinned row by row in SettingsSensitiveActionsTest (settings_row_checklist_test.cpp);
// here: the recovery phrase is bound once, only inside the block that exists while it is shown (the PIN gate is in
// show_recovery_phrase()), and neither page nor the transfer block binds a code, phrase or key anywhere else.
TEST(SettingsGatingTest, TheRecoveryPhraseIsBoundOnlyInItsShownBlock) {
  const std::string rml = ReadAsset("views/settings_section_security.rml");
  ASSERT_FALSE(rml.empty());
  ASSERT_EQ(CountOf(rml, "{{recovery_phrase}}"), 1u);
  const size_t phrase = rml.find("{{recovery_phrase}}");
  const size_t block = rml.rfind("data-if=\"recovery_phrase != ''\"", phrase);
  ASSERT_NE(block, std::string::npos);
  const std::string between = rml.substr(block + 10, phrase - block - 10);
  EXPECT_EQ(between.find("data-if="), std::string::npos) << "no other condition between the gate and the phrase";
  EXPECT_LT(rml.rfind("data-if=\"security_has_recovery_phrase\"", block), block)
      << "and that block is inside the has-phrase gate";
}

TEST(SettingsGatingTest, NoViewOfTheTwoPagesBindsACodeAPhraseOrAKeyOutsideTheShownBlock) {
  const std::string devices = ReadAsset("views/settings_section_devices.rml");
  const std::string transfer = ReadAsset("views/settings_security_transfer.rml");
  const std::string security = ReadAsset("views/settings_section_security.rml");
  const std::string pin = ReadAsset("views/settings_security_pin.rml");
  for (const std::string& view : {devices, transfer, pin}) {
    ASSERT_FALSE(view.empty());
    for (const char* secret :
         {"{{recovery_phrase", "{{transfer", "{{link_code", "{{export_code", "{{pin_value", "{{brief_llm_key}}"}) {
      EXPECT_EQ(view.find(secret), std::string::npos) << secret;
    }
  }
  EXPECT_EQ(security.find("{{transfer"), std::string::npos);
  EXPECT_EQ(security.find("{{brief_llm_key}}"), std::string::npos);
}

TEST(SettingsGatingTest, HomeAndTheDesktopListNameNoPageDirectly) {
  const std::string home = ReadAsset("views/settings.rml");
  // Both lists loop over `sections`, which leaves Advanced and Test options out while the flag is off.
  EXPECT_EQ(home.find("select_section('advanced')"), std::string::npos);
  EXPECT_EQ(home.find("select_section('tester')"), std::string::npos);
  EXPECT_EQ(CountOf(home, "data-for=\"section : sections\""), 4u) << "three phone groups and the desktop list";
}

TEST(SettingsGatingTest, AdvancedIsSwitchedOnlyByTheHelpSwitchAndTestOptionsOnlyByTheVersionRow) {
  const std::string help = ReadAsset("views/settings_section_help.rml");
  EXPECT_EQ(help.find("toggle_show_advanced_settings"), std::string::npos);
  EXPECT_EQ(help.find("settings.about.advanced"), std::string::npos);
  EXPECT_EQ(CountOf(help, "data-event-click=\"toggle_advanced_settings()\""), 1u) << "one row for both layouts";
  EXPECT_EQ(CountOf(help, "data-event-click=\"tap_version()\""), 2u) << "phone row and desktop line";
  EXPECT_NE(help.find("data-event-click=\"open_terms()\""), std::string::npos);
  EXPECT_NE(help.find("data-event-click=\"open_privacy_policy()\""), std::string::npos);
  // Neither event is bound anywhere but the Help page.
  for (const char* page :
       {"profile", "privacy", "security", "devices", "notifications", "appearance", "storage", "advanced", "tester"}) {
    const std::string rml = ReadAsset(std::string("views/settings_section_") + page + ".rml");
    EXPECT_EQ(rml.find("toggle_advanced_settings"), std::string::npos) << page;
    EXPECT_EQ(rml.find("tap_version"), std::string::npos) << page;
  }
}

TEST(SettingsGatingTest, TheTestOptionsPageFollowsOnlyTheTesterSwitchAndHasNoServiceRow) {
  const std::string rml = ReadAsset("views/settings_section_tester.rml");
  EXPECT_EQ(rml.find("advanced"), std::string::npos) << "no advanced condition left";
  EXPECT_NE(rml.find("data-if=\"tester\""), std::string::npos);
  EXPECT_EQ(rml.find("on_choose_backend"), std::string::npos);
  EXPECT_EQ(rml.find("settings.network.backend"), std::string::npos);
  EXPECT_EQ(rml.find("backend_is_custom"), std::string::npos);
  EXPECT_EQ(rml.find("reset_section('network')"), std::string::npos);
}

// The Service rows moved from the Test options page as they were: the same events, bindings and conditions (the lines
// below are the ones of `git show HEAD~:assets/views/settings_section_tester.rml` before the move), and nothing wraps them
// but `advanced`, so they show with advanced on and the tester switch off.
TEST(SettingsGatingTest, TheServiceRowsMovedToAdvancedUnchangedAndAreGatedOnlyOnAdvanced) {
  const std::string rml = ReadAsset("views/settings_section_advanced.rml");
  // The custom addresses live in their own file (a page of their own on phones, inline on the desktop).
  const std::string backend = ReadAsset("views/settings_section_advanced_backend.rml");
  for (const char* line : {
           "<div class=\"group-card\" data-if=\"show_backend_switch\">",
           "data-event-click=\"on_choose_backend()\"",
           "{{i18n:settings.network.backend}}</span><button class=\"settings-q-btn settings-phone-only\"",
           "<span class=\"settings-choice-value-text\">{{backend_label}}</span>",
           "<p class=\"text muted settings-help group-foot settings-desktop-only\" data-if=\"show_backend_switch\">"
           "{{i18n:settings.network.backend.help}}</p>",
           "<p class=\"text settings-help group-foot\" data-if=\"show_backend_switch && backend_restart_pending\">"
           "{{i18n:settings.network.backend.restart}}</p>",
       }) {
    EXPECT_NE(rml.find(line), std::string::npos) << line;
  }
  for (const char* line : {
           "<div class=\"stack\" data-if=\"backend_is_custom\">",
           "data-value=\"relay_base_url\" data-event-change=\"on_network_field_changed()\"",
           "data-value=\"directory_base_url\" data-event-change=\"on_network_field_changed()\"",
           "data-value=\"registration_base_url\" data-event-change=\"on_network_field_changed()\"",
           "data-event-click=\"reset_section('network')\">{{i18n:common.reset_defaults}}</button>",
       }) {
    EXPECT_NE(backend.find(line), std::string::npos) << line;
  }
  EXPECT_EQ(CountOf(rml, "data-event-click=\"on_choose_backend()\""), 1u);
  EXPECT_EQ(CountOf(rml, "data-if=\"backend_is_custom\""), 1u) << "only the entry row of the custom addresses page";
  EXPECT_EQ(CountOf(backend, "data-if=\"backend_is_custom\""), 1u) << "the whole file is behind the old gate";
  // Gates around the Service card: the page root (advanced) and nothing else; the banner has none.
  const size_t card = rml.find("data-if=\"show_backend_switch\"");
  ASSERT_NE(card, std::string::npos);
  const std::string before = rml.substr(0, card);
  EXPECT_EQ(CountOf(before, "data-if="), 1u) << "only the page root's data-if comes before the Service card";
  EXPECT_NE(before.find("data-if=\"advanced\""), std::string::npos);
  // The technical-users note: a banner strip on phones and the triangle line on the desktop, both with the warning
  // triangle before the words, tinted by tokens (no colour in the markup).
  EXPECT_EQ(CountOf(rml, "{{i18n:settings.advanced.intro}}"), 2u);
  EXPECT_EQ(CountOf(rml, "alert-triangle.svg"), 2u) << "one per layout";
  size_t triangle = rml.find("icons/v2/alert-triangle.svg");
  for (size_t at = rml.find("{{i18n:settings.advanced.intro}}"); at != std::string::npos;
       at = rml.find("{{i18n:settings.advanced.intro}}", at + 1)) {
    ASSERT_NE(triangle, std::string::npos);
    EXPECT_LT(triangle, at);
    triangle = rml.find("icons/v2/alert-triangle.svg", triangle + 1);
  }
  EXPECT_EQ(rml.find('#'), std::string::npos) << "no hard-coded colour";
  const size_t banner = rml.find("<div class=\"settings-banner settings-phone-only\">");
  ASSERT_NE(banner, std::string::npos);
  EXPECT_LT(banner, card) << "the banner is the first thing on the page";
  EXPECT_NE(rml.find("<div class=\"settings-warn-row settings-desktop-only\">"), std::string::npos);
  const std::string colors = ReadAsset("themes/colors.rcss.tmpl");
  EXPECT_EQ(CountOf(colors, ".settings-warn-icon svg {\n  image-color: {{warn}};"), 1u);
  EXPECT_EQ(CountOf(colors, "background-color: {{warn-tint}};"), 1u);
  // The Service rows come right after the technical-users note, above the connection rows.
  EXPECT_LT(rml.find("{{i18n:settings.advanced.intro}}"), card);
  EXPECT_LT(card, rml.find("{{i18n:settings.advanced.group.connection}}"));
}

} // namespace pbr
