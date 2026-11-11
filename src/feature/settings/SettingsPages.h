#pragma once

#include <string_view>
#include <vector>

namespace pbr {

/** What the controller does when a page is opened, or when its bindings are synced while it is open. */
enum class SettingsPageAction : unsigned {
  None = 0,
  RefreshBlockedCount = 1u << 0,
  RefreshBannerTip = 1u << 1,
  RefreshDiagnostics = 1u << 2,
  EnterStorage = 1u << 3,
  RefreshDevices = 1u << 4,
  RefreshCasLibrary = 1u << 5,
};

/**
 * One page of Me. A page id is not a handler id: the handlers (profile, security, appearance, storage, network, llm,
 * integrations, about) still own the data, the sync and the save; a page only names the view, the title, where the entry
 * sits on Home, which handlers its rows save through, and what opening it does.
 */
struct SettingsPage {
  const char* id;
  const char* view_key;  // ViewCatalog key
  const char* title_key; // locale key
  /** 0: no list entry (reached from the profile card); 1..3: the Home list group. */
  int home_group;
  /** Handler ids the rows on the page save through (nullptr ends the list). */
  const char* handler_ids[6];
  unsigned actions;
  /** Listed on Home (and the desktop list) only while "show advanced settings" is on (five taps on the version). */
  bool advanced_only;
};

/** All pages in list order (the desktop list reads it as is). */
const std::vector<SettingsPage>& SettingsPages();

const SettingsPage* FindSettingsPage(std::string_view id);

bool SettingsPageHas(const SettingsPage& page, SettingsPageAction action);

/** The pages Home lists: all of them, or all but the advanced-only ones. A page that is not listed cannot be opened. */
std::vector<const SettingsPage*> ListedSettingsPages(bool advanced);

} // namespace pbr
