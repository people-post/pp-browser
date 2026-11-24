#include "app/Bootstrap.h"

#include "foundation/crypto/BiometricUnlock.h"
#include "foundation/crypto/GraceUnlock.h"
#include "foundation/crypto/PinResolver.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/crypto/ProfileUnlockGate.h"
#include "foundation/data/AppPaths.h"
#include "foundation/data/BackendProfile.h"
#include "foundation/data/Config.h"
#include "foundation/data/PlatformDefaults.h"
#include "foundation/platform/CallKitDiagnostics.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/OsSecureBlobStore.h"
#include "lib/media/AudioSession.h"
#include "foundation/data/ProfileManifest.h"
#include "feature/product/HubSettingsProjection.h"
#include "feature/product/UiDemoConfig.h"
#include "foundation/data/UiDemoConfig.h"
#include "foundation/product/DevTools.h"
#include "common/StartupTiming.h"
#include "feature/conversations/ConversationsHub.h"
#include "foundation/platform/PlatformLogSink.h"
#include "foundation/platform/PlatformHooks.h"
#include "foundation/tls/ContentSha256Policy.h"
#include "foundation/tls/RelayClientHeader.h"
#include "foundation/platform/Platform.h"
#include "foundation/product/AppVersion.h"
#include "lib/net/ClientCompat.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <filesystem>

namespace pbr {

namespace {

// Profiles of the per-backend layout (manifest schema 1) are not read (pre-release clean cutover, RC017): each goes
// with its keychain items, so no key is left behind for a folder that is gone (profile-model P1). An unreadable or a
// newer manifest is not the old format and is kept.
Roe<void> RemoveOldFormatProfiles(ProfileRegistry& registry) {
  auto log = logging::getLogger("bootstrap");
  for (const std::string& id : registry.OldFormatProfiles()) {
    const std::string dir = registry.ProfileDataDir(id);
    GraceUnlock(OsSecureBlobStore(), dir, id).Forget();
    BiometricUnlock(OsSecureBlobStore(), dir, id).Disable();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
      return Error("Failed to remove old-format profile " + dir + ": " + ec.message());
    }
    log.warning << "Removed profile " << id << " of the old per-backend layout (a clean start, profile-model P1)";
    if (auto forgotten = registry.ForgetProfile(id); !forgotten) {
      return forgotten.error();
    }
  }
  return {};
}

Roe<void> UnlockProfileForBootstrap(ConversationsHub& messaging, ProfileSecretsEngine& secrets, const SecretText& pin) {
  StartupPhase phase("Bootstrap::Unlock+EnsureMessagingReady");
  return UnlockProfileSecretsAndReady(secrets, pin, [&messaging]() { return messaging.EnsureMessagingReady(); });
}

} // namespace

Roe<BootstrapResult> Bootstrap::Run(const BootstrapOptions& options, ConversationsHub& messaging,
                                    ProfileSecretsEngine& secrets) {
  PlatformHooks::Register();
  InstallPlatformLogSink();
  (void)StartupSystemDeviceName(); // read once, here on the main thread; every later use is the kept value

  auto config = Config::Load(options.argc, options.argv);
  if (!config) {
    return config.error();
  }
  // An empty endpoint in the profile's config falls back to the platform's defaults — taken here, after Config::Load
  // latched the backend (the hub is built before it): a built-in backend's table, and nothing at all for a custom
  // backend, which must never be handed another backend's address (profile-model PM002).
  {
    const AppConfig defaults = PlatformDefaults::For(Platform::Detect());
    messaging.SetBackendDefaults({defaults.relay, defaults.directory, defaults.registration});
  }
  SetSendContentSha256(config->relay_content_sha256);
  // Every relay call names this build (O1; SERVICE_ENDPOINTS § Version header): product, version, OS.
  SetRelayClientHeader(RelayClientHeaderValue("pp-app", AppVersionString(), Platform::OsName()));
  // B233: the audio-session timing lines are written wherever the staging origin is compiled in (today every iOS build);
  // timestamps and flags of our own call provider only, sent only through the user's own "send logs"; see
  // CallKitDiagnostics.h.
  AudioSession::SetDiagnosticsEnabled(CallKitDiagnosticsEnabled());
  AppPaths::DataDir(config->data_dir);

  auto registry = ProfileRegistry::Load(AppPaths::DataDir());
  if (!registry) {
    return registry.error();
  }
  if (auto removed = RemoveOldFormatProfiles(*registry); !removed) {
    return removed.error();
  }
  for (const std::string& id : registry->SkippedProfiles()) {
    logging::getLogger("bootstrap").warning << "Profile " << id
                                            << " is left alone: no readable manifest of this version";
  }

  // The profile is the running backend's home profile on its network (profile-model PM001, PM009); a tester build
  // never opens a `main` one, whichever way it is chosen (`--profile` included): release-compatibility RC022.
  const std::string backend_key = kUiDemoBuild ? std::string("prod") : BackendProfileKey(*config);
  const std::string_view network = NetworkOfBackend(backend_key);
  if (network.empty() || !ProfileNetworkAllowed(network, ThisBuildKind())) {
    return Error("this build does not open profiles of backend '" + backend_key + "'");
  }
  if (auto selected = options.profile_override.empty()
                          ? registry->SelectProfileForBackend(backend_key, network)
                          : registry->SelectProfileByLabel(options.profile_override, backend_key, network);
      !selected) {
    return selected.error();
  }

  const std::string profile_data_dir = registry->ActiveProfileDataDir();
  if (const ProfileManifestRead manifest = ReadProfileManifest(profile_data_dir);
      manifest.state != ProfileManifestState::kCurrent || manifest.network != network ||
      !ProfileNetworkAllowed(manifest.network, ThisBuildKind())) {
    return Error("this build does not open profile '" + registry->ActiveProfileId() + "'");
  }
  AppPaths::EnsureDirs(profile_data_dir);

  auto machine_prefs = UserPreferences::LoadMachine(AppPaths::DataDir());
  if (!machine_prefs) {
    return machine_prefs.error();
  }

  auto profile_prefs = UserPreferences::LoadProfile(profile_data_dir);
  if (!profile_prefs) {
    return profile_prefs.error();
  }
  if constexpr (kUiDemoBuild) {
    ApplyUiDemoProfilePrefs(*profile_prefs);
  }
  StartupMark("bootstrap_after_prefs");

  if (auto initialized = secrets.Initialize(profile_data_dir); !initialized) {
    return initialized.error();
  }

  if (auto hub = [&]() -> Roe<void> {
        StartupPhase phase("Bootstrap::ConversationsHub::Initialize");
        return messaging.Initialize(HubSettingsFrom(*config, *profile_prefs), profile_data_dir);
      }();
      !hub) {
    return hub.error();
  }

  // Optional CLI/env unlock for tests/automation only. Interactive / silent default unlock
  // is deferred until after first present (see DeferredStartup / ProfileUnlockGate).
  if (auto pin = PinResolver::Resolve(ResolveBootstrapPin(kUiDemoBuild, options.pin.Reveal())); pin) {
    if (!options.restore_recovery_phrase.empty()) {
      if (auto restored =
              messaging.RestoreFromRecoveryPhrase(options.restore_recovery_phrase, *pin, options.restore_saved_copies);
          !restored) {
        return restored.error();
      }
    }
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
  result.backend_key = backend_key;
  result.config_path = Config::DiscoverConfigPath(options.argc, options.argv);
  if (result.config_path.empty()) {
    result.config_path = AppPaths::ConfigFilePath();
  }
  return result;
}

} // namespace pbr
