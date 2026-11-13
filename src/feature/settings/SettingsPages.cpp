#include "feature/settings/SettingsPages.h"

namespace pbr {
namespace {

constexpr unsigned Bit(SettingsPageAction action) {
  return static_cast<unsigned>(action);
}

} // namespace

const std::vector<SettingsPage>& SettingsPages() {
  static const std::vector<SettingsPage> pages = {
      {"profile", "settings_section_profile", "settings.profile.title", 0, {"profile", nullptr}, 0},
      {"privacy",
       "settings_section_privacy",
       "settings.privacy.title",
       1,
       {"security", nullptr},
       Bit(SettingsPageAction::RefreshBlockedCount)},
      {"security", "settings_section_security", "settings.security.title", 1, {"security", nullptr}, 0},
      {"devices",
       "settings_section_devices",
       "settings.devices.title",
       1,
       {"security", nullptr},
       Bit(SettingsPageAction::RefreshDevices)},
      {"notifications",
       "settings_section_notifications",
       "settings.notifications.title",
       2,
       {"profile", nullptr},
       Bit(SettingsPageAction::RefreshBannerTip)},
      {"appearance", "settings_section_appearance", "settings.appearance.title", 2, {"appearance", nullptr}, 0},
      {"storage",
       "settings_section_storage",
       "settings.storage.title",
       2,
       {"storage", nullptr},
       Bit(SettingsPageAction::EnterStorage)},
      {"help",
       "settings_section_help",
       "settings.help.title",
       3,
       {"about", "security", nullptr},
       Bit(SettingsPageAction::RefreshDiagnostics)},
      {"advanced",
       "settings_section_advanced",
       "settings.advanced.title",
       3,
       {"network", "llm", "integrations", "security", "profile", nullptr},
       0,
       SettingsPageGate::Advanced},
      {"tester",
       "settings_section_tester",
       "settings.tester.title",
       3,
       {"network", "appearance", "security", "storage", nullptr},
       Bit(SettingsPageAction::RefreshCasLibrary),
       SettingsPageGate::Tester},
  };
  return pages;
}

const SettingsPage* FindSettingsPage(const std::string_view id) {
  for (const SettingsPage& page : SettingsPages()) {
    if (id == page.id) {
      return &page;
    }
  }
  return nullptr;
}

bool SettingsPageHas(const SettingsPage& page, const SettingsPageAction action) {
  return (page.actions & Bit(action)) != 0;
}

std::vector<const SettingsPage*> ListedSettingsPages(const bool advanced, const bool tester) {
  std::vector<const SettingsPage*> listed;
  for (const SettingsPage& page : SettingsPages()) {
    const bool shown = page.gate == SettingsPageGate::None || (page.gate == SettingsPageGate::Advanced && advanced) ||
                       (page.gate == SettingsPageGate::Tester && tester);
    if (shown) {
      listed.push_back(&page);
    }
  }
  return listed;
}

} // namespace pbr
