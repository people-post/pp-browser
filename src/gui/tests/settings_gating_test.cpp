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

// Phase 8 (Devices and transfer, mockup layout only): the same events as before, and nothing on the page or in the
// transfer block binds a secret; the code is only ever copied by export_link_device() after its PIN gate.
TEST(SettingsGatingTest, TheDevicesPageAndTheTransferBlockKeepTheirEventsAndBindNoSecret) {
  const std::string devices = ReadAsset("views/settings_section_devices.rml");
  const std::string transfer = ReadAsset("views/settings_security_transfer.rml");
  ASSERT_FALSE(devices.empty());
  ASSERT_FALSE(transfer.empty());
  EXPECT_EQ(CountOf(devices, "data-event-click=\""), 6u);
  for (const char* event : {"open_security_page('transfer')", "ask_setting_help('settings.security.transfer_row')",
                            "make_main_device(device_index)", "remove_own_device(device_index)",
                            "save_account_copy()", "ask_setting_help('settings.help.topic.account_copy')"}) {
    EXPECT_EQ(CountOf(devices, std::string("data-event-click=\"") + event + "\""), 1u) << event;
  }
  EXPECT_EQ(CountOf(transfer, "data-event-click=\""), 1u);
  EXPECT_EQ(CountOf(transfer, "data-event-click=\"export_link_device()\""), 1u);
  // The export row keeps its one gate; the three notes come after it.
  const size_t gate = transfer.find("data-if=\"security_can_export_link && !own_devices_not_main\"");
  const size_t export_event = transfer.find("export_link_device()");
  ASSERT_NE(gate, std::string::npos);
  EXPECT_LT(gate, export_event);
  // No binding that would render a code, a phrase or a key on either view.
  for (const std::string& view : {devices, transfer}) {
    for (const char* secret : {"{{recovery_phrase", "{{transfer", "{{link_code", "{{export_code", "{{pin", "{{brief_llm_key}}"}) {
      EXPECT_EQ(view.find(secret), std::string::npos) << secret;
    }
  }
  // Removing a device and saving a copy are buttons that call the gated handlers; nothing renders them already done.
  EXPECT_EQ(devices.find("remove_own_device(0)"), std::string::npos);
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
