#include "feature/settings/SettingsSections.h"

#include "feature/settings/AboutSettingsSection.h"
#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/settings/IntegrationsSettingsSection.h"
#include "feature/settings/LlmSettingsSection.h"
#include "feature/settings/NetworkSettingsSection.h"
#include "feature/settings/StorageSettingsSection.h"

namespace pbr {

std::vector<std::unique_ptr<SettingsSectionHandler>> CreateSettingsSections() {
  std::vector<std::unique_ptr<SettingsSectionHandler>> sections;
  sections.push_back(std::make_unique<LlmSettingsSection>());
  sections.push_back(std::make_unique<IntegrationsSettingsSection>());
  sections.push_back(std::make_unique<NetworkSettingsSection>());
  sections.push_back(std::make_unique<AppearanceSettingsSection>());
  sections.push_back(std::make_unique<StorageSettingsSection>());
  sections.push_back(std::make_unique<AboutSettingsSection>());
  return sections;
}

} // namespace pbr
