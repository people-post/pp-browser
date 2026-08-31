#include "app/node/NodeBootstrap.h"
#include "app/node/NodeEnvOverlay.h"

#include "base/crypto/CryptoUtil.h"
#include "base/crypto/PinResolver.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/data/AppPaths.h"
#include "base/data/Libp2pRole.h"
#include "base/data/ProfileRegistry.h"
#include "base/data/SchemaVersion.h"
#include "base/p2p/CircuitRelayService.h"
#include "base/p2p/MediaRelayService.h"
#include "base/p2p/Reachability.h"
#include "base/runtime/AppRuntime.h"
#include "common/Logger.h"

#include <cstdlib>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

Roe<NodeBootstrapResult> BootstrapPpNode(const NodeBootstrapOptions& options) {
  auto log = logging::getLogger("pp-node");

  auto config = Config::Load(options.argc, options.argv);
  if (!config) {
    return config.error();
  }

  // Env overlays (PP_NODE_*), then CLI listen override — CLI wins.
  ApplyPpNodeConfigEnvOverlays(*config);
  if (!options.listen_override.empty()) {
    config->libp2p.listen_multiaddr = options.listen_override;
  }
  config->libp2p.node_enabled = true;
  NormalizeLibp2pConfig(config->libp2p);

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

  auto pin = PinResolver::Require(options.pin);
  if (!pin) {
    return pin.error();
  }

  auto secrets = std::make_unique<ProfileSecretsService>();
  if (auto initialized = secrets->Initialize(profile_data_dir); !initialized) {
    return initialized.error();
  }

  auto identity = std::make_unique<IdentityStore>(profile_data_dir, registry->ActiveProfileId());
  secrets->RegisterDekConsumer(identity.get());

  if (auto unlocked = secrets->Unlock(*pin); !unlocked) {
    secrets->UnregisterDekConsumer(identity.get());
    return unlocked.error();
  }

  if (const char* seed_hex = std::getenv("PP_NODE_IDENTITY_SEED");
      seed_hex != nullptr && seed_hex[0] != '\0') {
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

  NodeRuntimeConfig runtime_cfg;
  runtime_cfg.host.listen_enabled = true;
  runtime_cfg.host.listen_multiaddr = config->libp2p.listen_multiaddr;
  if (auto priv = identity->GetDeviceMlDsaPrivateKey()) {
    runtime_cfg.host.device_ml_dsa_private_key = *priv;
  }
  if (auto pub = identity->GetDeviceMlDsaPublicKey()) {
    runtime_cfg.host.device_ml_dsa_public_key = *pub;
  }
  runtime_cfg.sessions = MakePeerSessionConfig(
      config->libp2p.max_connections, config->libp2p.max_concurrent_dials, config->libp2p.dial_timeout_ms,
      config->libp2p.idle_ttl_ms, config->libp2p.dial_failure_backoff_ms);
  runtime_cfg.bootstrap_peers = config->libp2p.bootstrap_peers;
  AppRuntime::Initialize();

  const ListenBusyPolicy busy =
      options.listen_fallback ? ListenBusyPolicy::DesktopFallback : ListenBusyPolicy::FailLoud;
  runtime_cfg.listen_candidates = BuildLibp2pListenCandidates(config->libp2p.listen_multiaddr, busy);
  AppendIpv6ListenCandidatesForPreferred(config->libp2p.listen_multiaddr, runtime_cfg.listen_candidates);

  MeshHostConfig mesh_cfg;
  mesh_cfg.runtime = std::move(runtime_cfg);
  // Org seed: circuit / media_relay host inbound when enabled (N018).
  mesh_cfg.host_circuit_relay = config->libp2p.capabilities.circuit_relay;
  mesh_cfg.host_media_relay = config->libp2p.capabilities.media_relay;
  mesh_cfg.media_relay_budget = config->libp2p.media_relay_budget;
  mesh_cfg.media_relay_pricing = config->libp2p.pricing.media_relay;
  // pp-node drives reachability probes from its run loop (--status / periodic refresh).
  mesh_cfg.start_reachability_probe = false;
  mesh_cfg.enable_amp_stack = config->libp2p.enable_amp_stack &&
                              mesh_cfg.runtime.host.device_ml_dsa_private_key &&
                              mesh_cfg.runtime.host.device_ml_dsa_public_key;
  mesh_cfg.amp_udp_port =
      config->libp2p.amp_udp_port <= 0 ? 0 : static_cast<uint16_t>(config->libp2p.amp_udp_port);

  auto mesh = std::make_unique<MeshHost>();
  if (auto started = mesh->Start(mesh_cfg); !started) {
    log.error << "mesh start failed: "
              << (mesh->LastError().empty() ? started.error().message : mesh->LastError());
    AppRuntime::Shutdown();
    secrets->UnregisterDekConsumer(identity.get());
    return started.error();
  }
  if (mesh_cfg.enable_amp_stack) {
    if (!mesh->Amp()) {
      log.error << "amp stack failed: " << mesh->AmpLastError();
      AppRuntime::Shutdown();
      secrets->UnregisterDekConsumer(identity.get());
      return Error(mesh->AmpLastError().empty() ? "amp stack failed" : mesh->AmpLastError());
    }
    log.info << "amp stack listen=" << mesh->AmpListenMultiaddr();
  } else {
    log.warning << "amp stack disabled (enable_amp_stack=false); peer mesh underlay off";
  }

  if (!mesh->AmpListenMultiaddr().empty()) {
    config->libp2p.listen_multiaddr = mesh->AmpListenMultiaddr();
  } else if (!mesh->BoundListenMultiaddr().empty()) {
    config->libp2p.listen_multiaddr = mesh->BoundListenMultiaddr();
  }

  NodeBootstrapResult result;
  result.config = std::move(*config);
  result.data_dir = AppPaths::DataDir();
  result.profile_data_dir = profile_data_dir;
  result.profile_id = registry->ActiveProfileId();
  result.config_path = Config::DiscoverConfigPath(options.argc, options.argv);
  if (result.config_path.empty()) {
    result.config_path = AppPaths::ConfigFilePath();
  }
  result.secrets = std::move(secrets);
  result.identity = std::move(identity);
  result.mesh = std::move(mesh);

  std::string peer_id;
  if (result.mesh->Amp()) {
    peer_id = result.mesh->Amp()->LocalPeerId();
  } else if (result.mesh->Host()) {
    if (auto local = result.mesh->Host()->LocalPeerIdBase58()) {
      peer_id = *local;
    }
  }
  log.info << "pp-node listening on " << result.config.libp2p.listen_multiaddr
           << (peer_id.empty() ? std::string() : (" peer=" + peer_id))
           << " underlay=amp"
           << " circuit-relay=" << (result.config.libp2p.capabilities.circuit_relay ? "on" : "off")
           << " media-relay=" << (result.config.libp2p.capabilities.media_relay ? "on" : "off");
  return result;
}

} // namespace pbr
