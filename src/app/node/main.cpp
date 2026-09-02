#include "app/node/NodeBootstrap.h"
#include "app/node/NodeMeshPublish.h"
#include "app/node/StatusHttpProtocol.h"
#include "app/node/StatusHttpServer.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/platform/PlatformLogDefaults.h"
#include "base/platform/DeploymentProfile.h"
#include "base/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "common/ValueJson.h"
#include "base/mesh/reachability/Reachability.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) {
  g_stop.store(true);
}

const char* EnvOrEmpty(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "\n"
      << "Headless Brief mesh node (N011). Always Node.\n"
      << "Precedence: CLI flags → environment → config file → defaults.\n"
      << "\n"
      << "Options:\n"
      << "  --config <path>       Config JSON (or PP_BROWSER_CONFIG)\n"
      << "  --status              Print reachability JSON and exit (nr ops)\n"
      << "  --status-addr <addr>  HTTP admin bind (default 127.0.0.1:18518).\n"
      << "                       Use 0.0.0.0:18518 (or a host IP) for console/probes.\n"
      << "                       Empty string disables. Env: PP_NODE_STATUS_ADDR\n"
      << "  --status-token <tok>  Optional Bearer token for /healthz and /status.\n"
      << "                       Env: PP_NODE_STATUS_TOKEN\n"
      << "  --pin <pin>           Profile PIN (or PP_BROWSER_PIN) — required\n"
      << "  --profile <id>        Profile id override (or PP_NODE_PROFILE)\n"
      << "  --sandbox             Use sandbox backend (www-en.qa.peoplepost.org)\n"
      << "  --debug               Verbose logging\n"
      << "  --help                Show this help\n"
      << "\n"
      << "Deploy env (see docs/ops/CONFIGURATION.md):\n"
      << "  PP_NODE_DATA_DIR, PP_NODE_AMP_UDP_PORT, PP_NODE_BOOTSTRAP_PEERS,\n"
      << "  PP_NODE_CAP_CIRCUIT_RELAY, PP_NODE_CAP_MEDIA_RELAY, PP_NODE_CAP_DHT,\n"
      << "  PP_NODE_ADVERTISE_MULTIADDRS, PP_NODE_MESH_PUBLISH,\n"
      << "  PP_NODE_STATUS_ADDR, PP_NODE_STATUS_TOKEN\n"
      << "\n"
      << "Live status HTTP (long-running mode), e.g.:\n"
      << "  curl -sS http://127.0.0.1:18518/healthz\n"
      << "  curl -sS http://127.0.0.1:18518/status\n";
}

void ShutdownNode(pbr::NodeBootstrapResult& boot) {
  if (boot.mesh) {
    boot.mesh->Stop();
  }
  if (pbr::AppRuntime::IsRunning()) {
    pbr::AppRuntime::Shutdown();
  }
  if (boot.identity) {
    boot.identity->Flush();
    if (boot.secrets) {
      boot.secrets->UnregisterDekConsumer(boot.identity.get());
    }
  }
  if (boot.secrets) {
    boot.secrets->Shutdown();
  }
}

pbr::StatusHttpSnapshot MakeSnapshot(pbr::NodeBootstrapResult& boot) {
  pbr::StatusHttpSnapshot snap;
  snap.host_running = boot.mesh && boot.mesh->IsRunning();
  if (boot.mesh) {
    snap.listen_multiaddr = boot.mesh->AmpListenMultiaddr();
    if (boot.mesh->Amp()) {
      snap.peer_id = boot.mesh->Amp()->LocalPeerId();
    }
    snap.circuit_relay = boot.mesh->AmpCircuitTunnel() && boot.mesh->AmpCircuitTunnel()->IsStarted() &&
                         boot.mesh->AmpCircuitTunnel()->ServeInbound();
    snap.media_relay = boot.mesh->AmpMediaRelayCoord() && boot.mesh->AmpMediaRelayCoord()->IsStarted() &&
                       boot.mesh->AmpMediaRelayCoord()->ServeInbound();
    snap.dht = boot.mesh->AmpDht() && boot.mesh->AmpDht()->IsStarted();
    snap.reachability_json = boot.mesh->Reachability().FormatOpsStatusJson();
    if (boot.mesh->AmpDht()) {
      snap.dht_json = boot.mesh->AmpDht()->FormatOpsStatusJson();
    }
  }
  return snap;
}

} // namespace

int main(int argc, char** argv) {
  bool debug_mode = false;
  std::string pin;
  // Env defaults; CLI overwrites below (CLI → env → file).
  std::string profile_override = EnvOrEmpty("PP_NODE_PROFILE");
  bool print_status = false;
  std::string status_addr_spec =
      std::string(pbr::kDefaultStatusHttpBindHost) + ":" + std::to_string(pbr::kDefaultStatusHttpBindPort);
  if (const char* env_addr = std::getenv("PP_NODE_STATUS_ADDR")) {
    status_addr_spec = env_addr;
  }
  std::string status_token = EnvOrEmpty("PP_NODE_STATUS_TOKEN");

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--debug") == 0) {
      debug_mode = true;
    } else if (std::strcmp(argv[i], "--sandbox") == 0) {
      // Handled by ResolveSandboxModeFromLaunch below.
    } else if (std::strcmp(argv[i], "--status") == 0) {
      print_status = true;
    } else if (std::strcmp(argv[i], "--status-addr") == 0 && i + 1 < argc) {
      status_addr_spec = argv[++i];
    } else if (std::strcmp(argv[i], "--status-token") == 0 && i + 1 < argc) {
      status_token = argv[++i];
    } else if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
      pin = argv[++i];
    } else if (std::strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
      profile_override = argv[++i];
    } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      ++i; // Config::Load reads --config from argv
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }

  pbr::SetSandboxMode(pbr::ResolveSandboxModeFromLaunch(argc, argv));

  auto root = pbr::logging::getRootLogger();
  root.setLevel(pbr::DefaultRootLogLevel(debug_mode));
  pbr::logging::setEmitFloor(pbr::DefaultEmitFloor(debug_mode));
  if (pbr::SandboxMode()) {
    root.warning << "Sandbox mode: backend origin " << pbr::BriefOrigin()
                 << ", product dir " << pbr::ProductDirBasename();
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  pbr::NodeBootstrapOptions options;
  options.argc = argc;
  options.argv = argv;
  options.pin = pin;
  options.profile_override = profile_override;

  auto boot = pbr::BootstrapPpNode(options);
  if (!boot) {
    root.error << "pp-node bootstrap failed: " << boot.error().message;
    return 1;
  }

  if (print_status) {
    boot->mesh->RunReachabilityProbeBlocking(/*try_upnp_first=*/false);
    auto snap = MakeSnapshot(*boot);
    auto root_obj = pbr::TryParseObject(snap.reachability_json);
    pbr::Object out = root_obj ? std::move(*root_obj) : pbr::Object{};
    out.set("host_running", snap.host_running);
    out.set("dht", snap.dht);
    if (!snap.dht_json.empty()) {
      if (auto dht = pbr::TryParseObject(snap.dht_json)) {
        out.set("dht_stats", *dht);
      }
    }
    std::cout << pbr::DumpJson(out) << std::endl;
    ShutdownNode(*boot);
    return 0;
  }

  pbr::StatusHttpServer status_http;
  if (auto bind = pbr::ParseStatusHttpBind(status_addr_spec)) {
    pbr::StatusHttpAuthConfig auth;
    auth.bearer_token = status_token;
    pbr::NodeBootstrapResult* boot_ptr = &(*boot);
    if (auto started = status_http.Start(*bind, std::move(auth),
                                         [boot_ptr]() { return MakeSnapshot(*boot_ptr); });
        !started) {
      root.error << "status HTTP failed: " << started.error().message;
      ShutdownNode(*boot);
      return 1;
    }
  } else if (!status_addr_spec.empty()) {
    root.error << "invalid --status-addr (use host:port, or empty to disable)";
    ShutdownNode(*boot);
    return 2;
  }

  auto schedule_probe = [&]() {
    boot->mesh->StartReachabilityProbe(/*try_upnp_first=*/false);
  };
  schedule_probe();

  // Publish/renew mesh_node listing when advertise multiaddrs configured (N027).
  if (auto published = pbr::PublishOrRenewMeshNodeListing(boot->config, *boot->identity, ""); !published) {
    root.warning << "mesh directory publish skipped/failed: " << published.error().message;
  }

  root.info << "pp-node running (SIGINT/SIGTERM to stop)";
  auto last_probe = std::chrono::steady_clock::now();
  auto last_mesh_publish = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    boot->mesh->Tick();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_probe > std::chrono::seconds(60)) {
      last_probe = now;
      schedule_probe();
    }
    if (now - last_mesh_publish > std::chrono::hours(12)) {
      last_mesh_publish = now;
      if (auto published = pbr::PublishOrRenewMeshNodeListing(boot->config, *boot->identity, ""); !published) {
        root.warning << "mesh directory renew failed: " << published.error().message;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  root.info << "pp-node shutting down";
  status_http.Stop();
  ShutdownNode(*boot);
  return 0;
}
