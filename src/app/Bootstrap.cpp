#include "app/Bootstrap.h"

#include "base/crypto/PinResolver.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/crypto/ProfileUnlockGate.h"
#include "base/data/AppPaths.h"
#include "base/data/Config.h"
#include "foundation/platform/DeploymentProfile.h"
#include "base/data/SchemaVersion.h"
#include "common/StartupTiming.h"
#include "feature/messaging/MessagingHub.h"
#include "foundation/platform/PlatformLogSink.h"
#include "foundation/platform/PlatformServices.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<void> UnlockProfileForBootstrap(MessagingHub& messaging, ProfileSecretsService& secrets,
                                    const std::string& pin) {
  StartupPhase phase("Bootstrap::Unlock+EnsureMessagingReady");
  return UnlockProfileSecretsAndReady(secrets, pin,
                                      [&messaging]() { return messaging.EnsureMessagingReady(); });
}

} // namespace

Roe<BootstrapResult> Bootstrap::Run(const BootstrapOptions& options, MessagingHub& messaging,
                                    ProfileSecretsService& secrets) {
  PlatformServices::Register();
  InstallPlatformLogSink();

  auto config = Config::Load(options.argc, options.argv);
  if (!config) {
    return config.error();
  }
  if (SandboxMode()) {
    const std::string config_path = Config::DiscoverConfigPath(options.argc, options.argv);
    logging::getLogger("Bootstrap").warning
        << "Sandbox mode active — config "
        << (config_path.empty() ? AppPaths::ConfigFilePath() : config_path) << ", data "
        << AppPaths::DataDir(config->data_dir);
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
  StartupMark("bootstrap_after_prefs");

  if (auto initialized = secrets.Initialize(profile_data_dir); !initialized) {
    return initialized.error();
  }

  if (auto hub = [&]() -> Roe<void> {
        StartupPhase phase("Bootstrap::MessagingHub::Initialize");
        return messaging.Initialize(*config, profile_data_dir);
      }();
      !hub) {
    return hub.error();
  }

  // Optional CLI/env unlock for tests/automation only. Interactive / silent default unlock
  // is deferred until after first present (see DeferredStartup / ProfileUnlockGate).
  if (auto pin = PinResolver::Resolve(options.pin); pin) {
    if (auto unlocked = UnlockProfileForBootstrap(messaging, secrets, *pin); !unlocked) {
      return unlocked.error();
    }
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
