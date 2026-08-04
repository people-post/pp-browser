#include "app/node/NodeBootstrap.h"

#include "base/crypto/PinResolver.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/data/AppPaths.h"
#include "base/data/Libp2pRole.h"
#include "base/data/ProfileRegistry.h"
#include "base/data/SchemaVersion.h"
#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/MediaRelayService.h"
#include "libp2p/integration/host/Reachability.h"
#include "base/runtime/AppRuntime.h"
#include "common/Logger.h"

#include <utility>

namespace pbr {

Roe<NodeBootstrapResult> BootstrapPpNode(const NodeBootstrapOptions& options) {
  auto log = logging::getLogger("pp-node");

  auto config = Config::Load(options.argc, options.argv);
  if (!config) {
    return config.error();
  }

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

  if (auto secrets = ProfileSecretsService::Instance().Initialize(profile_data_dir); !secrets) {
    return secrets.error();
  }

  auto identity = std::make_unique<IdentityStore>(profile_data_dir, registry->ActiveProfileId());
  ProfileSecretsService::Instance().RegisterDekConsumer(identity.get());

  if (auto unlocked = ProfileSecretsService::Instance().Unlock(*pin); !unlocked) {
    ProfileSecretsService::Instance().UnregisterDekConsumer(identity.get());
    return unlocked.error();
  }

  if (auto loaded = identity->LoadOrCreate(); !loaded) {
    ProfileSecretsService::Instance().UnregisterDekConsumer(identity.get());
    return loaded.error();
  }

  NodeRuntimeConfig runtime_cfg;
  runtime_cfg.host.listen_enabled = true;
  runtime_cfg.host.listen_multiaddr = config->libp2p.listen_multiaddr;
  if (auto priv = identity->GetEd25519PrivateKey()) {
    runtime_cfg.host.ed25519_private_key = *priv;
  }
  if (auto pub = identity->GetEd25519PublicKey()) {
    runtime_cfg.host.ed25519_public_key = *pub;
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

  auto runtime = std::make_unique<NodeRuntime>();
  if (auto started = runtime->Start(runtime_cfg); !started) {
    log.error << "libp2p listen failed: "
              << (runtime->LastError().empty() ? started.error().message : runtime->LastError());
    AppRuntime::Shutdown();
    ProfileSecretsService::Instance().UnregisterDekConsumer(identity.get());
    return started.error();
  }

  if (!runtime->BoundListenMultiaddr().empty()) {
    config->libp2p.listen_multiaddr = runtime->BoundListenMultiaddr();
  }

  auto dial_back = std::make_unique<DialBackService>(*runtime->Host(), *runtime->Sessions());
  dial_back->Start();

  std::unique_ptr<CircuitRelayService> circuit_relay;
  if (config->libp2p.capabilities.circuit_relay) {
    circuit_relay = std::make_unique<CircuitRelayService>(*runtime->Host(), *runtime->Sessions());
    circuit_relay->SetExecutorConfig(runtime->ExecutorConfig());
    circuit_relay->Start();
  }

  std::unique_ptr<MediaRelayService> media_relay;
  // Org seed: media_relay default on (N018); host inbound when enabled.
  if (config->libp2p.capabilities.media_relay) {
    media_relay = std::make_unique<MediaRelayService>(*runtime->Host(), *runtime->Sessions());
    media_relay->SetBudget(config->libp2p.media_relay_budget);
    media_relay->SetPricing(config->libp2p.pricing.media_relay);
    media_relay->Start();
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
  result.identity = std::move(identity);
  result.runtime = std::move(runtime);
  result.dial_back = std::move(dial_back);
  result.circuit_relay = std::move(circuit_relay);
  result.media_relay = std::move(media_relay);
  result.reachability = std::make_unique<ReachabilityService>();

  auto peer_id = result.runtime->Host()->LocalPeerIdBase58();
  log.info << "pp-node listening on " << result.config.libp2p.listen_multiaddr
           << (peer_id ? (" peer=" + *peer_id) : std::string())
           << " dial-back=" << kDialBackProtocolId
           << " circuit-relay=" << (result.config.libp2p.capabilities.circuit_relay ? "on" : "off")
           << " media-relay=" << (result.config.libp2p.capabilities.media_relay ? "on" : "off");
  return result;
}

} // namespace pbr
