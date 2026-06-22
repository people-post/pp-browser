#include "app/Bootstrap.h"

#include "app/AppPaths.h"
#include "app/Config.h"
#include "app/SchemaVersion.h"
#include "messaging/MessagingHub.h"
#include "platform/AndroidAssetLocator.h"
#include "platform/AndroidPathProvider.h"
#include "platform/IAssetLocator.h"
#include "platform/IPathProvider.h"
#include "platform/IosAssetLocator.h"
#include "platform/IosPathProvider.h"
#include "platform/Platform.h"

namespace pbr {

namespace {

void RegisterPlatformServices() {
  const PlatformKind kind = Platform::Detect();
  if (kind == PlatformKind::Android) {
    static AndroidPathProvider paths;
    static AndroidAssetLocator assets;
    IPathProvider::SetInstance(&paths);
    IAssetLocator::SetInstance(&assets);
  } else if (kind == PlatformKind::IOS) {
    static IosPathProvider paths;
    static IosAssetLocator assets;
    IPathProvider::SetInstance(&paths);
    IAssetLocator::SetInstance(&assets);
  }
}

} // namespace

Roe<BootstrapResult> Bootstrap::Run(const BootstrapOptions& options) {
  RegisterPlatformServices();

  auto config = Config::Load(options.argc, options.argv);
  if (!config) {
    return config.error();
  }

  AppPaths::DataDir(config->data_dir);

  auto registry = ProfileRegistry::Load(AppPaths::DataDir());
  if (!registry) {
    return registry.error();
  }

  if (!options.profile_override.empty()) {
    registry->SetSessionProfileOverride(options.profile_override);
  }

  if (auto ensured = registry->EnsureActiveProfile(); !ensured) {
    return ensured.error();
  }

  const std::string profile_data_dir = registry->ActiveProfileDataDir();
  AppPaths::EnsureDirs(profile_data_dir);

  if (auto manifest = SchemaVersion::EnsureProfileManifest(profile_data_dir); !manifest) {
    return manifest.error();
  }

  auto machine_prefs = UserPreferences::LoadMachine(AppPaths::DataDir());
  if (!machine_prefs) {
    return machine_prefs.error();
  }

  if (!options.profile_override.empty()) {
    machine_prefs->active_profile_id = options.profile_override;
  }

  auto profile_prefs = UserPreferences::LoadProfile(profile_data_dir);
  if (!profile_prefs) {
    return profile_prefs.error();
  }

  if (auto hub = MessagingHub::Instance().Initialize(*config, profile_data_dir); !hub) {
    return hub.error();
  }

  BootstrapResult result{};
  result.config = std::move(*config);
  result.machine_prefs = std::move(*machine_prefs);
  result.profile_prefs = std::move(*profile_prefs);
  result.profile_registry = std::move(*registry);
  result.data_dir = AppPaths::DataDir();
  result.profile_data_dir = profile_data_dir;
  result.config_path = Config::DiscoverConfigPath(options.argc, options.argv);
  if (result.config_path.empty()) {
    result.config_path = AppPaths::ConfigFilePath();
  }
  return result;
}

} // namespace pbr
