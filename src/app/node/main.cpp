#include "app/node/NodeBootstrap.h"
#include "app/node/StatusHttpProtocol.h"
#include "app/node/StatusHttpServer.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/platform/PlatformLogDefaults.h"
#include "base/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "libp2p/integration/host/Reachability.h"

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
      << "\n"
      << "Options:\n"
      << "  --config <path>       Config JSON (or PP_BROWSER_CONFIG)\n"
      << "  --listen <multiaddr>  Override libp2p listen (e.g. /ip4/0.0.0.0/tcp/443)\n"
      << "  --listen-fallback     Allow desktop-style port fallback (not default for ops)\n"
      << "  --status              Print reachability JSON and exit (nr ops)\n"
      << "  --status-addr <addr>  Loopback HTTP admin bind (default 127.0.0.1:18518).\n"
      << "                       Empty string disables. Env: PP_NODE_STATUS_ADDR\n"
      << "  --status-token <tok>  Optional Bearer token for /healthz and /status.\n"
      << "                       Env: PP_NODE_STATUS_TOKEN\n"
      << "  --status-allow-non-loopback\n"
      << "                       Allow binding status HTTP off loopback (dangerous)\n"
      << "  --pin <pin>           Profile PIN (or PP_BROWSER_PIN) — required\n"
      << "  --profile <id>        Profile id override\n"
      << "  --debug               Verbose logging\n"
      << "  --help                Show this help\n"
      << "\n"
      << "Live status (long-running mode) is loopback-only HTTP for DevOps, e.g.:\n"
      << "  curl -sS http://127.0.0.1:18518/status\n"
      << "  kubectl exec deploy/pp-node -- curl -sS http://127.0.0.1:18518/healthz\n";
}

void ShutdownNode(pbr::NodeBootstrapResult& boot) {
  if (boot.dial_back) {
    boot.dial_back->Stop();
  }
  if (boot.circuit_relay) {
    boot.circuit_relay->Stop();
  }
  if (boot.media_relay) {
    boot.media_relay->Stop();
  }
  if (boot.runtime) {
    boot.runtime->Stop();
  }
  if (pbr::AppRuntime::IsRunning()) {
    pbr::AppRuntime::Shutdown();
  }
  if (boot.identity) {
    boot.identity->Flush();
    pbr::ProfileSecretsService::Instance().UnregisterDekConsumer(boot.identity.get());
  }
  pbr::ProfileSecretsService::Instance().Shutdown();
}

pbr::StatusHttpSnapshot MakeSnapshot(pbr::NodeBootstrapResult& boot) {
  pbr::StatusHttpSnapshot snap;
  snap.host_running = boot.runtime && boot.runtime->IsRunning();
  if (boot.runtime) {
    snap.listen_multiaddr = boot.runtime->BoundListenMultiaddr();
    if (boot.runtime->Host()) {
      if (auto peer = boot.runtime->Host()->LocalPeerIdBase58()) {
        snap.peer_id = *peer;
      }
    }
  }
  snap.circuit_relay = boot.circuit_relay && boot.circuit_relay->IsStarted();
  snap.media_relay = boot.media_relay && boot.media_relay->IsStarted();
  if (boot.reachability) {
    snap.reachability_json = boot.reachability->FormatOpsStatusJson();
  }
  return snap;
}

} // namespace

int main(int argc, char** argv) {
  bool debug_mode = false;
  std::string pin;
  std::string profile_override;
  std::string listen_override;
  bool listen_fallback = false;
  bool print_status = false;
  std::string status_addr_spec =
      std::string(pbr::kDefaultStatusHttpBindHost) + ":" + std::to_string(pbr::kDefaultStatusHttpBindPort);
  if (const char* env_addr = std::getenv("PP_NODE_STATUS_ADDR")) {
    status_addr_spec = env_addr;
  }
  std::string status_token = EnvOrEmpty("PP_NODE_STATUS_TOKEN");
  bool status_allow_non_loopback = false;

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
    } else if (std::strcmp(argv[i], "--status-allow-non-loopback") == 0) {
      status_allow_non_loopback = true;
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
    const std::string bound = boot->runtime->BoundListenMultiaddr();
    const bool try_upnp = !pbr::ShouldSkipUpnpForListen(bound);
    boot->reachability->RunProbeBlocking(*boot->runtime, *boot->dial_back, try_upnp);
    std::cout << boot->reachability->FormatOpsStatusJson() << std::endl;
    ShutdownNode(*boot);
    return 0;
  }

  pbr::StatusHttpServer status_http;
  if (auto bind = pbr::ParseStatusHttpBind(status_addr_spec)) {
    pbr::StatusHttpAuthConfig auth;
    auth.bearer_token = status_token;
    pbr::NodeBootstrapResult* boot_ptr = &(*boot);
    if (auto started = status_http.Start(
            *bind, std::move(auth),
            [boot_ptr]() { return MakeSnapshot(*boot_ptr); }, status_allow_non_loopback);
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
    if (boot->reachability && boot->runtime && boot->dial_back) {
      const bool try_upnp = !pbr::ShouldSkipUpnpForListen(boot->runtime->BoundListenMultiaddr());
      boot->reachability->StartProbe(*boot->runtime, *boot->dial_back, try_upnp);
    }
  };
  schedule_probe();

  root.info << "pp-node running (SIGINT/SIGTERM to stop)";
  auto last_probe = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    boot->runtime->Tick();
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
