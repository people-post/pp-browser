#include "app/node/NodeBootstrap.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/platform/PlatformLogDefaults.h"
#include "common/Logger.h"

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
      << "  --pin <pin>           Profile PIN (or PP_BROWSER_PIN) — required\n"
      << "  --profile <id>        Profile id override\n"
      << "  --debug               Verbose logging\n"
      << "  --help                Show this help\n";
}

} // namespace

int main(int argc, char** argv) {
  bool debug_mode = false;
  std::string pin;
  std::string profile_override;
  std::string listen_override;
  bool listen_fallback = false;
  bool print_status = false;

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
    if (boot->dial_back) {
      boot->dial_back->Stop();
    }
    if (boot->circuit_relay) {
      boot->circuit_relay->Stop();
    }
    if (boot->media_relay) {
      boot->media_relay->Stop();
    }
    if (boot->runtime) {
      boot->runtime->Stop();
    }
    if (boot->identity) {
      boot->identity->Flush();
      pbr::ProfileSecretsService::Instance().UnregisterDekConsumer(boot->identity.get());
    }
    pbr::ProfileSecretsService::Instance().Shutdown();
    return 0;
  }

  root.info << "pp-node running (SIGINT/SIGTERM to stop)";
  while (!g_stop.load()) {
    boot->runtime->Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  root.info << "pp-node shutting down";
  if (boot->media_relay) {
    boot->media_relay->Stop();
  }
  if (boot->circuit_relay) {
    boot->circuit_relay->Stop();
  }
  if (boot->dial_back) {
    boot->dial_back->Stop();
  }
  if (boot->runtime) {
    boot->runtime->Stop();
  }
  if (boot->identity) {
    boot->identity->Flush();
    pbr::ProfileSecretsService::Instance().UnregisterDekConsumer(boot->identity.get());
  }
  pbr::ProfileSecretsService::Instance().Shutdown();
  return 0;
}
