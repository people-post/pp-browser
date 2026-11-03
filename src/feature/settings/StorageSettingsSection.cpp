#include "feature/settings/StorageSettingsSection.h"

#include "foundation/data/AppPaths.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/AttachmentCache.h"

#include <cstdint>
#include <filesystem>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

uint64_t DirectoryByteSize(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec) {
    return 0;
  }

  uint64_t total = 0;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, options, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    const uint64_t size = entry.file_size(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    total += size;
  }
  return total;
}

std::string FormatByteSize(uint64_t bytes) {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = kKiB * 1024.0;
  constexpr double kGiB = kMiB * 1024.0;

  std::ostringstream out;
  out.setf(std::ios::fixed);
  if (bytes < static_cast<uint64_t>(kKiB)) {
    out << bytes << " B";
  } else if (bytes < static_cast<uint64_t>(kMiB)) {
    out.precision(bytes < static_cast<uint64_t>(10 * kKiB) ? 1 : 0);
    out << (static_cast<double>(bytes) / kKiB) << " KB";
  } else if (bytes < static_cast<uint64_t>(kGiB)) {
    out.precision(bytes < static_cast<uint64_t>(10 * kMiB) ? 1 : 0);
    out << (static_cast<double>(bytes) / kMiB) << " MB";
  } else {
    out.precision(1);
    out << (static_cast<double>(bytes) / kGiB) << " GB";
  }
  return out.str();
}

} // namespace

std::string AttachmentDownloadPolicyDisplayLabel(const std::string& policy) {
  if (policy == "always_auto") {
    return Tr("settings.storage.attachment_policy.always_auto");
  }
  if (policy == "on_demand") {
    return Tr("settings.storage.attachment_policy.on_demand");
  }
  return Tr("settings.storage.attachment_policy.smart");
}

bool AttachmentAutoTogglesVisible(const SettingsUiState& state, const PlatformKind platform) {
  return platform != PlatformKind::Desktop && state.attachment_download_policy == "always_auto";
}

void ChooseAttachmentDownloadPolicy(SettingsUiState& state, const std::string& policy) {
  if (policy == "always_auto" && state.attachment_download_policy != "always_auto") {
    state.attachment_auto_on_wifi = "on";
    state.attachment_auto_on_cellular = "off";
  }
  state.attachment_download_policy = policy;
  state.attachment_download_policy_label = AttachmentDownloadPolicyDisplayLabel(policy);
}

void ToggleAttachmentAutoNetwork(SettingsUiState& state, const bool cellular) {
  std::string& tick = cellular ? state.attachment_auto_on_cellular : state.attachment_auto_on_wifi;
  tick = tick == "on" ? "off" : "on";
  if (state.attachment_auto_on_wifi != "on" && state.attachment_auto_on_cellular != "on") {
    ChooseAttachmentDownloadPolicy(state, UserPreferences::DefaultProfile().attachment_download_policy);
    state.attachment_auto_on_wifi = "on";
    state.attachment_auto_on_cellular = "off";
  }
}

const char* StorageSettingsSection::Id() const {
  return "storage";
}

SettingsSectionListItem StorageSettingsSection::ListItem() const {
  return {.id = Id(), .title = Tr("settings.storage.title"), .subtitle = Tr("settings.storage.subtitle")};
}

SettingsFlushMode StorageSettingsSection::FlushMode() const {
  return SettingsFlushMode::Immediate;
}

bool StorageSettingsSection::IsWritable() const {
  return true;
}

void StorageSettingsSection::SyncFromSession(const BootstrapResult& bootstrap, SettingsUiState& state) {
  state.profile_label = bootstrap.profile_registry.ActiveProfileId();
  state.config_dir = AppPaths::ConfigDir();
  state.data_dir = bootstrap.data_dir;
  state.profile_dir = bootstrap.profile_data_dir;
  const uint64_t bytes = DirectoryByteSize(bootstrap.profile_data_dir);
  state.profile_size_label = Tr("settings.storage.profile_size", {{"size", FormatByteSize(bytes)}});
  state.attachment_cache_size_label =
      Tr("settings.storage.attachment_cache_size",
         {{"size", FormatByteSize(AttachmentCacheByteSize(bootstrap.profile_data_dir))}});
  state.attachment_download_policy = bootstrap.profile_prefs.attachment_download_policy;
  state.attachment_download_policy_label = AttachmentDownloadPolicyDisplayLabel(state.attachment_download_policy);
  state.attachment_auto_on_wifi = bootstrap.profile_prefs.attachment_auto_on_wifi ? "on" : "off";
  state.attachment_auto_on_cellular = bootstrap.profile_prefs.attachment_auto_on_cellular ? "on" : "off";
}

bool StorageSettingsSection::IsPersisted(const SettingsUiState& state, const BootstrapResult& bootstrap) const {
  const ProfilePreferences& prefs = bootstrap.profile_prefs;
  return state.attachment_download_policy == prefs.attachment_download_policy &&
         (state.attachment_auto_on_wifi == "on") == prefs.attachment_auto_on_wifi &&
         (state.attachment_auto_on_cellular == "on") == prefs.attachment_auto_on_cellular;
}

Roe<void> StorageSettingsSection::Flush(SettingsUiState& state, SessionStore& store) {
  ProfilePreferences prefs = store.Snapshot().profile_prefs;
  const bool wifi = state.attachment_auto_on_wifi == "on";
  const bool cellular = state.attachment_auto_on_cellular == "on";
  if (state.attachment_download_policy == prefs.attachment_download_policy &&
      wifi == prefs.attachment_auto_on_wifi && cellular == prefs.attachment_auto_on_cellular) {
    return Roe<void>{};
  }
  prefs.attachment_download_policy = state.attachment_download_policy;
  prefs.attachment_auto_on_wifi = wifi;
  prefs.attachment_auto_on_cellular = cellular;
  return store.SaveProfilePrefs(prefs);
}

void StorageSettingsSection::ResetToDefaults(SettingsUiState& state, const SessionStore& /*store*/) {
  state.attachment_download_policy = UserPreferences::DefaultProfile().attachment_download_policy;
  state.attachment_download_policy_label = AttachmentDownloadPolicyDisplayLabel(state.attachment_download_policy);
  state.attachment_auto_on_wifi = UserPreferences::DefaultProfile().attachment_auto_on_wifi ? "on" : "off";
  state.attachment_auto_on_cellular = UserPreferences::DefaultProfile().attachment_auto_on_cellular ? "on" : "off";
}

} // namespace pbr
