// The page table (SettingsPages) of Me: what opening a page does, which handlers its rows save through, and that every
// row of the checklist (src/gui/tests/data/settings_row_checklist.txt) saves through a handler its page owns.
#include "feature/settings/SettingsPages.h"
#include "feature/settings/SettingsSections.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::set<std::string> HandlerIdsOf(const SettingsPage& page) {
  std::set<std::string> ids;
  for (const char* const* id = page.handler_ids; *id; ++id) {
    ids.insert(*id);
  }
  return ids;
}

const SettingsPage& Page(const char* id) {
  const SettingsPage* page = FindSettingsPage(id);
  EXPECT_NE(page, nullptr) << id;
  static const SettingsPage none{"", "", "", 0, {nullptr}, 0};
  return page ? *page : none;
}

} // namespace

TEST(SettingsPagesTest, OpeningAPageDoesWhatTheOldSectionDid) {
  const struct {
    const char* id;
    unsigned actions;
  } expected[] = {
      {"profile", 0},
      {"privacy", static_cast<unsigned>(SettingsPageAction::RefreshBlockedCount)},
      {"security", 0},
      {"devices", static_cast<unsigned>(SettingsPageAction::RefreshDevices)},
      {"notifications", static_cast<unsigned>(SettingsPageAction::RefreshBannerTip)},
      {"appearance", 0},
      {"storage", static_cast<unsigned>(SettingsPageAction::EnterStorage)},
      {"help", static_cast<unsigned>(SettingsPageAction::RefreshDiagnostics)},
      {"advanced", 0},
      {"tester", static_cast<unsigned>(SettingsPageAction::RefreshCasLibrary)},
  };
  ASSERT_EQ(SettingsPages().size(), std::size(expected));
  for (size_t i = 0; i < std::size(expected); ++i) {
    EXPECT_EQ(std::string(SettingsPages()[i].id), expected[i].id) << "list order";
    EXPECT_EQ(Page(expected[i].id).actions, expected[i].actions) << expected[i].id;
  }
  EXPECT_TRUE(SettingsPageHas(Page("privacy"), SettingsPageAction::RefreshBlockedCount));
  EXPECT_FALSE(SettingsPageHas(Page("security"), SettingsPageAction::RefreshBlockedCount));
  EXPECT_TRUE(SettingsPageHas(Page("tester"), SettingsPageAction::RefreshCasLibrary));
  EXPECT_FALSE(SettingsPageHas(Page("storage"), SettingsPageAction::RefreshCasLibrary));
}

TEST(SettingsPagesTest, EveryHandlerAPageNamesExists) {
  std::set<std::string> handlers = {"profile", "security"}; // both live in src/gui
  for (const auto& section : CreateSettingsSections()) {
    handlers.insert(section->Id());
  }
  for (const SettingsPage& page : SettingsPages()) {
    EXPECT_FALSE(HandlerIdsOf(page).empty()) << page.id;
    for (const std::string& id : HandlerIdsOf(page)) {
      EXPECT_EQ(handlers.count(id), 1u) << page.id << " names the unknown handler " << id;
    }
  }
}

TEST(SettingsPagesTest, EveryChecklistRowSavesThroughAHandlerItsPageOwns) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/src/gui/tests/data/settings_row_checklist.txt");
  ASSERT_TRUE(in.good());
  std::string line;
  size_t checked = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t at = line.find('\t'); at != std::string::npos; at = line.find('\t', start)) {
      fields.push_back(line.substr(start, at - start));
      start = at + 1;
    }
    fields.push_back(line.substr(start));
    ASSERT_EQ(fields.size(), 7u) << line;
    const std::string& handler = fields[2];
    const std::string& view = fields[3];
    ASSERT_NE(FindSettingsPage(view), nullptr) << view;
    if (handler == "-") {
      continue;
    }
    EXPECT_EQ(HandlerIdsOf(Page(view.c_str())).count(handler), 1u)
        << fields[0] << " saves through " << handler << " but " << view << " does not list it";
    ++checked;
  }
  EXPECT_GT(checked, 40u);
}

// Phase 2: Advanced and Test options are listed only while the advanced flag is on; a page that is not listed cannot be
// opened (SettingsController::OnSelectSection finds pages in the listed rows only).
TEST(SettingsPagesTest, AdvancedAndTestOptionsAreListedOnlyWhileTheFlagIsOn) {
  std::vector<std::string> off;
  for (const SettingsPage* page : ListedSettingsPages(false)) {
    off.push_back(page->id);
  }
  const std::vector<std::string> expected_off = {"profile",       "privacy",    "security", "devices",
                                                 "notifications", "appearance", "storage",  "help"};
  EXPECT_EQ(off, expected_off);

  std::vector<std::string> on;
  for (const SettingsPage* page : ListedSettingsPages(true)) {
    on.push_back(page->id);
  }
  ASSERT_EQ(on.size(), 10u);
  EXPECT_EQ(on[8], "advanced");
  EXPECT_EQ(on[9], "tester");
  EXPECT_TRUE(Page("advanced").advanced_only);
  EXPECT_TRUE(Page("tester").advanced_only);
  EXPECT_FALSE(Page("help").advanced_only);
}

// The Test options entry (the last row of Home's third group) selects page "tester": with the advanced flag on the
// page is listed and its view is the one registered for it; with the flag off nothing lists it, so selecting it opens
// nothing (SettingsController::OnSelectSection returns when the id is not among the listed rows).
TEST(SettingsPagesTest, SelectingTheTesterEntryOpensItsViewOnlyWhileTheFlagIsOn) {
  const std::vector<const SettingsPage*> on = ListedSettingsPages(true);
  ASSERT_FALSE(on.empty());
  EXPECT_STREQ(on.back()->id, "tester") << "last row of the list, so last in the third group";
  EXPECT_EQ(on.back()->home_group, 3);
  EXPECT_EQ(on[on.size() - 2]->home_group, 3) << "after Help and Advanced";
  const SettingsPage* page = FindSettingsPage("tester");
  ASSERT_NE(page, nullptr);
  EXPECT_STREQ(page->view_key, "settings_section_tester");
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" /
                                      (std::string(page->view_key) + ".rml")));
  for (const SettingsPage* listed : ListedSettingsPages(false)) {
    EXPECT_STRNE(listed->id, "tester");
  }
}

} // namespace pbr
