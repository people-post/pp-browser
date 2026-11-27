#include "app/node/NodeBootstrap.h"

#include "foundation/product/AppVersion.h"
#include "foundation/runtime/AppRuntimeMeshExecutors.h"
#include "foundation/platform/os/OsThreadName.h"
#include "app/node/NodeEnvOverlay.h"
#include "foundation/platform/DeploymentProfile.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/PinResolver.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/data/AppPaths.h"
#include "foundation/data/MeshRole.h"
#include "foundation/data/ProfileRegistry.h"
#include "foundation/data/ProfileManifest.h"
#include "foundation/tls/ContentSha256Policy.h"
#include "foundation/tls/RelayClientHeader.h"
#include "foundation/platform/Platform.h"
#include "lib/net/ClientCompat.h"
#include "domain/node/NodeMeshServices.h"
#include "common/runtime/AppRuntime.h"
#include "common/runtime/AppRuntime.h"
#include "common/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

constexpr char kNodeDataDirName[] = "pp-node";
constexpr char kDataLockFileName[] = "pp-node.lock";

} // namespace

Roe<NodeBootstrapResult> BootstrapPpNode(const NodeBootstrapOptions& options) {
  auto log = logging::getLogger("pp-node");

  // Only a file named for it (--config / PP_BROWSER_CONFIG): never the desktop app's of the same user.
  auto config = Config::Load(options.argc, options.argv, Config::Fallback::None);
  if (!config) {
    return config.error();
  }

  ApplyPpNodeConfigEnvOverlays(*config);
  // The relay it registers at proves the relay id its address names (multi-relay MR008): the compiled prod relay's.
  DefaultNodeRegistrationRelayId(config->registration, BriefRelayBaseUrlFor(CompiledDefaultOrigin()),
                                 CompiledDefaultRelayId());
  SetSendContentSha256(config->relay_content_sha256);
  // Every relay call names this build (O1): its registration, heartbeat and directory calls.
  SetRelayClientHeader(RelayClientHeaderValue("pp-node", AppVersionString(), Platform::OsName()));
  config->mesh.node_enabled = true;
  NormalizeMeshConfig(config->mesh);

  // A data root of its own by default, never the desktop app's (profile-model PM007); a deployment names its volume.
  AppPaths::DataDir(config->data_dir.empty()
                        ? (std::filesystem::path(AppPaths::DataDir()).parent_path() / kNodeDataDirName).string()
                        : config->data_dir);

  // One node per data folder at a time (projects/node-relay NR001): a second process would run the same identity twice
  // and write the same sealed files. The OS drops the lock when the process ends, a crash included.
  std::error_code mkdir_error;
  std::filesystem::create_directories(AppPaths::DataDir(), mkdir_error);
  const std::string lock_file = (std::filesystem::path(AppPaths::DataDir()) / kDataLockFileName).string();
  auto data_lock = os::OsDirLock::Acquire(lock_file);
  if (!data_lock) {
    if (data_lock.error().message.find(os::OsDirLock::kHeldElsewhere) != std::string::npos) {
      return Error("another pp-node is running on " + AppPaths::DataDir() + " (" + lock_file +
                   "); stop it first, or ask it with curl …/status instead of --status");
    }
    return data_lock.error();
  }

  auto registry = ProfileRegistry::Load(AppPaths::DataDir());
  if (!registry) {
    return registry.error();
  }
  // A node's profile from before the network layout keeps its identity (its PeerId): converted in place, once.
  if (auto converted = registry->ConvertOldFormatProfilesToNode(); !converted) {
    return converted.error();
  }
  const std::string backend_id(profile_network::kNode);
  if (auto selected =
          options.profile_override.empty()
              ? registry->SelectProfileForBackend(backend_id, profile_network::kNode)
              : registry->SelectProfileByLabel(options.profile_override, backend_id, profile_network::kNode);
      !selected) {
    return selected.error();
  }

  const std::string profile_data_dir = registry->ActiveProfileDataDir();
  if (const ProfileManifestRead manifest = ReadProfileManifest(profile_data_dir);
      manifest.state != ProfileManifestState::kCurrent || manifest.network != profile_network::kNode) {
    return Error("pp-node does not open profile '" + registry->ActiveProfileId() + "': not a node profile");
  }
  AppPaths::EnsureDirs(profile_data_dir);

  auto pin = PinResolver::Require(options.pin.Reveal());
  if (!pin) {
    return pin.error();
  }

  auto secrets = std::make_unique<ProfileSecretsEngine>();
  if (auto initialized = secrets->Initialize(profile_data_dir); !initialized) {
    return initialized.error();
  }

  auto identity = std::make_unique<IdentityStore>(profile_data_dir, registry->ActiveProfileId());
  // Its registration slot is its backend's (profile-model PM004): moved out of identity.enc at the next save.
  identity->SetBackend((std::filesystem::path(profile_data_dir) / "backends" / backend_id).string(), backend_id);
  secrets->RegisterDekConsumer(identity.get());

  if (auto unlocked = secrets->Unlock(*pin); !unlocked) {
    secrets->UnregisterDekConsumer(identity.get());
    return unlocked.error();
  }

  if (const char* seed_hex = std::getenv("PP_NODE_IDENTITY_SEED"); seed_hex != nullptr && seed_hex[0] != '\0') {
    auto seed_bytes = HexToBytes(seed_hex);
    if (!seed_bytes) {
      secrets->UnregisterDekConsumer(identity.get());
      return Error("PP_NODE_IDENTITY_SEED is not valid hex: " + seed_bytes.error().message);
    }
    if (auto set_seed = identity->SetIdentitySeed(std::move(*seed_bytes)); !set_seed) {
      secrets->UnregisterDekConsumer(identity.get());
      return set_seed.error();
    }
  }

  if (auto loaded = identity->LoadOrCreate(); !loaded) {
    secrets->UnregisterDekConsumer(identity.get());
    return loaded.error();
  }

  AppRuntimeConfig runtime = AppRuntimeConfig{};
  runtime.name_thread = os::SetCurrentThreadName;
  AppRuntime::Initialize(runtime);

  mesh::MeshHostConfig mesh_cfg;
  if (auto priv = identity->GetDeviceMlDsaPrivateKey()) {
    mesh_cfg.host.device_ml_dsa_private_key = *priv;
  }
  if (auto pub = identity->GetDeviceMlDsaPublicKey()) {
    mesh_cfg.host.device_ml_dsa_public_key = *pub;
  }
  // Org seed: circuit / media_relay / DHT hosted per capability, directory always (N018).
  ApplyNodeHosting(mesh_cfg, config->mesh, ResolveMeshRole(config->mesh) == MeshRole::Node);
  // pp-node drives reachability probes from its run loop (--status / periodic refresh).
  mesh_cfg.start_reachability_probe = false;
  mesh_cfg.mesh_enabled =
      config->mesh.mesh_enabled && mesh_cfg.host.device_ml_dsa_private_key && mesh_cfg.host.device_ml_dsa_public_key;
  mesh_cfg.amp_udp_port = config->mesh.amp_udp_port <= 0 ? 0 : static_cast<uint16_t>(config->mesh.amp_udp_port);
  mesh_cfg.bootstrap_peers = config->mesh.bootstrap_peers;

  auto mesh = std::make_unique<mesh::MeshHost>(AppRuntimeMeshExecutors(), MetricsRegistry::Global());
  if (auto started = mesh->Start(mesh_cfg); !started) {
    log.error << "mesh start failed: " << (mesh->LastError().empty() ? started.error().message : mesh->LastError());
    AppRuntime::Shutdown();
    secrets->UnregisterDekConsumer(identity.get());
    return started.error();
  }
  if (mesh_cfg.mesh_enabled) {
    if (!mesh->Amp()) {
      log.error << "amp stack failed: " << mesh->AmpLastError();
      AppRuntime::Shutdown();
      secrets->UnregisterDekConsumer(identity.get());
      return Error(mesh->AmpLastError().empty() ? "amp stack failed" : mesh->AmpLastError());
    }
    log.info << "amp stack listen=" << mesh->AmpListenMultiaddr();
    StartOrgSeedServices(*mesh, *identity, config->mesh, ResolveMeshRole(config->mesh) == MeshRole::Node,
                         AppVersionString());
  } else {
    log.warning << "mesh disabled (mesh_enabled=false); peer mesh underlay off";
  }

  NodeBootstrapResult result;
  result.data_lock = std::move(*data_lock);
  result.config = std::move(*config);
  result.data_dir = AppPaths::DataDir();
  result.profile_data_dir = profile_data_dir;
  result.profile_id = registry->ActiveProfileId();
  result.config_path = Config::DiscoverConfigPath(options.argc, options.argv, Config::Fallback::None);
  result.secrets = std::move(secrets);
  result.identity = std::move(identity);
  result.mesh = std::move(mesh);

  std::string peer_id;
  if (result.mesh->Amp()) {
    peer_id = result.mesh->Amp()->LocalPeerId();
  }
  const std::string listen = [&]() -> std::string {
    if (!result.mesh) {
      return "(none)";
    }
    const auto advertised = result.mesh->AdvertisedListenMultiaddrs();
    if (!advertised.empty()) {
      return advertised.front();
    }
    return result.mesh->AmpListenMultiaddr().empty() ? std::string("(none)") : result.mesh->AmpListenMultiaddr();
  }();
  log.info << "pp-node listening on " << listen << (peer_id.empty() ? std::string() : (" peer=" + peer_id))
           << " underlay=amp"
           << " circuit-relay=" << (result.config.mesh.capabilities.circuit_relay ? "on" : "off")
           << " media-relay=" << (result.config.mesh.capabilities.media_relay ? "on" : "off")
           << " dht=" << (result.config.mesh.capabilities.dht ? "on" : "off");
  return result;
}

} // namespace pbr
