#include "app/node/NodeBootstrap.h"
#include "app/node/NodeEnvOverlay.h"
#include "app/node/StatusHttpProtocol.h"
#include "app/node/StatusHttpServer.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/platform/PlatformLogDefaults.h"
#include "base/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "base/p2p/Reachability.h"

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
      << "Headless Brief mesh node (N011). Always Node; fail-loud listen by default (N016).\n"
      << "Precedence: CLI flags → environment → config file → defaults.\n"
      << "\n"
      << "Options:\n"
      << "  --config <path>       Config JSON (or PP_BROWSER_CONFIG)\n"
      << "  --listen <multiaddr>  Override libp2p listen (or PP_NODE_LISTEN)\n"
      << "  --listen-fallback     Allow desktop-style port fallback (or PP_NODE_LISTEN_FALLBACK)\n"
      << "  --status              Print reachability JSON and exit (nr ops)\n"
      << "  --status-addr <addr>  HTTP admin bind (default 127.0.0.1:18518).\n"
      << "                       Use 0.0.0.0:18518 (or a host IP) for console/probes.\n"
      << "                       Empty string disables. Env: PP_NODE_STATUS_ADDR\n"
      << "  --status-token <tok>  Optional Bearer token for /healthz and /status.\n"
      << "                       Env: PP_NODE_STATUS_TOKEN\n"
      << "  --pin <pin>           Profile PIN (or PP_BROWSER_PIN) — required\n"
      << "  --profile <id>        Profile id override (or PP_NODE_PROFILE)\n"
      << "  --debug               Verbose logging\n"
      << "  --help                Show this help\n"
      << "\n"
      << "Deploy env (see docs/ops/CONFIGURATION.md):\n"
      << "  PP_NODE_DATA_DIR, PP_NODE_LISTEN, PP_NODE_BOOTSTRAP_PEERS,\n"
      << "  PP_NODE_CAP_CIRCUIT_RELAY, PP_NODE_CAP_MEDIA_RELAY,\n"
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
  if (boot.mesh && boot.mesh->Runtime()) {
    snap.listen_multiaddr = boot.mesh->Runtime()->BoundListenMultiaddr();
    if (boot.mesh->Host()) {
      if (auto peer = boot.mesh->Host()->LocalPeerIdBase58()) {
        snap.peer_id = *peer;
      }
    }
  }
  snap.circuit_relay = boot.mesh && boot.mesh->CircuitRelay() && boot.mesh->CircuitRelay()->IsStarted();
  snap.media_relay = boot.mesh && boot.mesh->MediaRelay() && boot.mesh->MediaRelay()->IsStarted();
  if (boot.mesh) {
    snap.reachability_json = boot.mesh->Reachability().FormatOpsStatusJson();
  }
  return snap;
}

} // namespace

int main(int argc, char** argv) {
  bool debug_mode = false;
  std::string pin;
  // Env defaults; CLI overwrites below (CLI → env → file).
  std::string profile_override = EnvOrEmpty("PP_NODE_PROFILE");
  std::string listen_override; // CLI only; PP_NODE_LISTEN applied in Bootstrap
  bool listen_fallback = false;
  if (auto from_env = pbr::ParsePpNodeBoolEnv(EnvOrEmpty("PP_NODE_LISTEN_FALLBACK"))) {
    listen_fallback = *from_env;
  }
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
    } else if (std::strcmp(argv[i], "--listen-fallback") == 0) {
      listen_fallback = true;
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
    } else if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
      listen_override = argv[++i];
    } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      ++i; // Config::Load reads --config from argv
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }

  auto root = pbr::logging::getRootLogger();
  root.setLevel(pbr::DefaultRootLogLevel(debug_mode));
  pbr::logging::setEmitFloor(pbr::DefaultEmitFloor(debug_mode));

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  pbr::NodeBootstrapOptions options;
  options.argc = argc;
  options.argv = argv;
  options.pin = pin;
  options.profile_override = profile_override;
  options.listen_override = listen_override;
  options.listen_fallback = listen_fallback;

  auto boot = pbr::BootstrapPpNode(options);
  if (!boot) {
    root.error << "pp-node bootstrap failed: " << boot.error().message;
    return 1;
  }

  if (print_status) {
    const std::string bound = boot->mesh->BoundListenMultiaddr();
    const bool try_upnp = !pbr::ShouldSkipUpnpForListen(bound);
    boot->mesh->Reachability().RunProbeBlocking(*boot->mesh->Runtime(), *boot->mesh->DialBack(), try_upnp);
    std::cout << boot->mesh->Reachability().FormatOpsStatusJson() << std::endl;
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
    if (boot->mesh && boot->mesh->Runtime() && boot->mesh->DialBack()) {
      const bool try_upnp = !pbr::ShouldSkipUpnpForListen(boot->mesh->BoundListenMultiaddr());
      boot->mesh->Reachability().StartProbe(*boot->mesh->Runtime(), *boot->mesh->DialBack(), try_upnp);
    }
  };
  schedule_probe();

  root.info << "pp-node running (SIGINT/SIGTERM to stop)";
  auto last_probe = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    boot->mesh->Tick();
    // Refresh reachability snapshot periodically for /status (non-blocking probe).
    const auto now = std::chrono::steady_clock::now();
    if (now - last_probe > std::chrono::seconds(60)) {
      last_probe = now;
      schedule_probe();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  root.info << "pp-node shutting down";
  status_http.Stop();
  ShutdownNode(*boot);
  return 0;
}
