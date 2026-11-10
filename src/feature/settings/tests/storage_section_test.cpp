// Me → Storage's first page (DEV/PLAN-storage-manager.md ①): the auto-download choice is three rows shown in place
// (no menu) whose tap is stored, the Wi-Fi / mobile-data switches only under "everything" on a phone, and the two
// bulk buttons are gone.
#include "feature/settings/StorageSettingsSection.h"

#include "foundation/data/BootstrapTypes.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadView(const char* name) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / name);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

size_t CountOf(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

} // namespace

TEST(StorageSectionTest, AutoDownloadIsThreeRowsInPlaceAndTheBulkButtonsAreGone) {
  const std::string rml = ReadView("settings_section_storage.rml");
  ASSERT_FALSE(rml.empty());
  for (const char* policy : {"smart", "always_auto", "on_demand"}) {
    const std::string tap = std::string("choose_attachment_download_policy('") + policy + "')";
    EXPECT_EQ(CountOf(rml, tap), 1u) << tap;
    const std::string check = std::string("attachment_download_policy == '") + policy + "'";
    EXPECT_GE(CountOf(rml, check), 2u) << "a check on the chosen row and its own footer: " << check;
  }
  EXPECT_EQ(CountOf(rml, "../icons/check.svg\" width=\"16\""), 3u);
  EXPECT_EQ(rml.find("on_choose_attachment_download_policy"), std::string::npos) << "no popup menu any more";
  EXPECT_EQ(rml.find("drain_pending_attachment_media"), std::string::npos);
  EXPECT_EQ(rml.find("clear_downloaded_attachments"), std::string::npos);
  // The switches show only while "everything" is chosen on a phone (AttachmentAutoTogglesVisible).
  EXPECT_EQ(CountOf(rml, "data-if=\"attachment_auto_toggles_visible\""), 2u);
  EXPECT_NE(rml.find("toggle_attachment_auto_wifi()"), std::string::npos);
  EXPECT_NE(rml.find("toggle_attachment_auto_cellular()"), std::string::npos);
  // The deeper pages live in the same section.
  for (const char* page : {"storage_page == 'main'", "storage_page == 'manage'", "storage_page == 'items'"}) {
    EXPECT_EQ(CountOf(rml, page), 1u) << page;
  }
}

TEST(StorageSectionTest, TheChosenRowIsStored) {
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  pbr::LocalizationService::Instance().SetPreferredLanguage("en");
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path profile_dir =
      std::filesystem::temp_directory_path() / ("pp_storage_section_" + std::to_string(stamp));
  std::filesystem::create_directories(profile_dir);

  pbr::BootstrapResult bootstrap{};
  bootstrap.profile_data_dir = profile_dir.string();
  pbr::SessionStore store;
  store.Initialize(bootstrap);

  pbr::StorageSettingsSection section;
  pbr::SettingsUiState state;
  section.SyncFromSession(store.Snapshot(), state);
  for (const char* policy : {"on_demand", "always_auto", "smart"}) {
    pbr::ChooseAttachmentDownloadPolicy(state, policy);
    ASSERT_TRUE(section.Flush(state, store)) << policy;
    EXPECT_EQ(store.Snapshot().profile_prefs.attachment_download_policy, policy);
    EXPECT_TRUE(section.IsPersisted(state, store.Snapshot()));
  }

  // "Everything": the switches show on a phone, never on a desktop; both off goes back to the default (stored).
  pbr::ChooseAttachmentDownloadPolicy(state, "always_auto");
  EXPECT_TRUE(pbr::AttachmentAutoTogglesVisible(state, pbr::PlatformKind::IOS));
  EXPECT_FALSE(pbr::AttachmentAutoTogglesVisible(state, pbr::PlatformKind::Desktop));
  pbr::ToggleAttachmentAutoNetwork(state, /*cellular=*/false); // Wi-Fi off, mobile data was off
  EXPECT_EQ(state.attachment_download_policy, "smart");
  ASSERT_TRUE(section.Flush(state, store));
  EXPECT_EQ(store.Snapshot().profile_prefs.attachment_download_policy, "smart");

  std::error_code ec;
  std::filesystem::remove_all(profile_dir, ec);
}
