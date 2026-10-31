#include "app/node/NodeBootstrap.h"
#include "app/node/NodeHeartbeat.h"
#include "app/node/NodeMetrics.h"
#include "app/node/NodeMeshPublish.h"
#include "app/node/StatusHttpProtocol.h"
#include "app/node/StatusHttpServer.h"

#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/platform/PlatformLogDefaults.h"
#include "foundation/platform/DeploymentProfile.h"
#include "common/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "common/ValueJson.h"
#include "mesh/dht/AmpDhtProtocol.h"
#include "lib/net/HttpClient.h"
#include "lib/net/RegistrationSignPayload.h"
#include "mesh/reachability/Reachability.h"
#if defined(PP_BROWSER_HAS_LEDGER)
#include "domain/node/NodeLedgerRole.h"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include "common/PbrCompat.h"

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
      << "  --config <path>       Config JSON (or PP_BROWSER_CONFIG). None: defaults + environment\n"
      << "                        (never the desktop app's config of the same user)\n"
      << "  --status              Print reachability JSON and exit (nr ops)\n"
      << "  --status-addr <addr>  HTTP admin bind (default 127.0.0.1:18518).\n"
      << "                       Use 0.0.0.0:18518 (or a host IP) for console/probes.\n"
      << "                       Empty string disables. Env: PP_NODE_STATUS_ADDR\n"
      << "  --status-token <tok>  Optional Bearer token for /healthz, /status and /metrics.\n"
      << "                       Env: PP_NODE_STATUS_TOKEN\n"
      << "  --pin <pin>           Profile PIN (or PP_BROWSER_PIN) — required\n"
      << "  --ledger-role <role>  Run a pp-ledger relay or miner on this node's mesh (relay|miner;\n"
      << "                        or PP_NODE_LEDGER_ROLE)\n"
      << "  --ledger-dir <path>   Its work directory: config.json, keys, data (or PP_NODE_LEDGER_DIR;\n"
      << "                        default <profile data dir>/ledger)\n"
      << "  --profile <id>        Profile id override (or PP_NODE_PROFILE)\n"
      << "  --sandbox             Use sandbox backend (www-en.qa.peoplepost.org)\n"
      << "  --log-level <level>   debug|info|warning|error (or PP_NODE_LOG_LEVEL; default info)\n"
      << "  --debug               Verbose logging (--log-level debug)\n"
      << "  --help                Show this help\n"
      << "\n"
      << "Deploy env (see docs/ops/CONFIGURATION.md):\n"
      << "  PP_NODE_DATA_DIR, PP_NODE_AMP_UDP_PORT, PP_NODE_BOOTSTRAP_PEERS,\n"
      << "  PP_NODE_CAP_CIRCUIT_RELAY, PP_NODE_CAP_MEDIA_RELAY, PP_NODE_CAP_DHT, PP_NODE_DHT_K,\n"
      << "  PP_NODE_ADVERTISE_MULTIADDRS, PP_NODE_MESH_PUBLISH,\n"
      << "  PP_NODE_STATUS_ADDR, PP_NODE_STATUS_TOKEN, PP_NODE_LOG_LEVEL,\n"
      << "  PP_NODE_LEDGER_ROLE, PP_NODE_LEDGER_DIR\n"
      << "\n"
      << "Live status HTTP (long-running mode), e.g.:\n"
      << "  curl -sS http://127.0.0.1:18518/healthz\n"
      << "  curl -sS http://127.0.0.1:18518/status\n"
      << "  curl -sS http://127.0.0.1:18518/metrics   (Prometheus text)\n"
      << "\n"
      << "Drain before maintenance (loopback status bind only; the mesh directory sees the new state):\n"
      << "  curl -sS -X POST -d '{\"state\":\"draining\"}' http://127.0.0.1:18518/control/state\n";
}

/**
 * A server's log: what it runs, where it listens, its roles, its shutdown — INFO by default, unlike
 * the desktop app's quiet WARNING (DefaultRootLogLevel). The ledger role logs through it too.
 */
std::optional<pbr::logging::Level> NodeLogLevel(const std::string& name, const bool debug_mode) {
  if (debug_mode || name == "debug") {
    return pbr::logging::kLevelDebug;
  }
  if (name.empty() || name == "info") {
    return pbr::logging::Level::INFO;
  }
  if (name == "warning") {
    return pbr::logging::Level::WARNING;
  }
  if (name == "error") {
    return pbr::logging::Level::ERROR;
  }
  return std::nullopt;
}

/**
 * The node's pp-ledger role, if asked for (--ledger-role): it runs on the node's mesh runtime and stops
 * before the mesh. Without the ledger in this build (PP_BROWSER_WITH_LEDGER=OFF) asking for one fails.
 */
class LedgerRoleSlot {
public:
  /** Checks the flags before bootstrap; an error is a usage error. */
  pbr::Roe<void> Configure(const std::string& role, std::string dir) {
    if (role.empty()) {
      return {};
    }
#if defined(PP_BROWSER_HAS_LEDGER)
    role_ = pbr::NodeLedgerRole::RoleNamed(role);
    if (!role_) {
      return pbr::Error("--ledger-role must be relay or miner (a beacon runs standalone only)");
    }
    dir_ = std::move(dir);
    return {};
#else
    (void)dir;
    return pbr::Error("this pp-node is built without the ledger role (PP_BROWSER_WITH_LEDGER=OFF)");
#endif
  }

  pbr::Roe<void> Start(pbr::NodeBootstrapResult& boot) {
#if defined(PP_BROWSER_HAS_LEDGER)
    if (!role_) {
      return {};
    }
    pbr::NodeLedgerRole::Options options;
    options.role = *role_;
    options.work_dir = dir_.empty() ? boot.profile_data_dir + "/ledger" : dir_;
    return ledger_.Start(*boot.mesh, std::move(options));
#else
    (void)boot;
    return {};
#endif
  }

  /** Why the role ended on its own; none while it runs (or when there is none). */
  std::optional<std::string> Failure() const {
#if defined(PP_BROWSER_HAS_LEDGER)
    return ledger_.Failure();
#else
    return std::nullopt;
#endif
  }

  /** Before the mesh stops. */
  void Stop() {
#if defined(PP_BROWSER_HAS_LEDGER)
    ledger_.Stop();
#endif
  }

private:
#if defined(PP_BROWSER_HAS_LEDGER)
  std::optional<pbr::NodeLedgerRole::Role> role_;
  std::string dir_;
  pbr::NodeLedgerRole ledger_;
#endif
};

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

pbr::NodeHeartbeatDeps MakeHeartbeatDeps(pbr::NodeBootstrapResult& boot) {
  pbr::NodeHeartbeatDeps deps;
  deps.base_url = boot.config.registration.base_url;
  pbr::IdentityStore* identity = boot.identity.get();
  pbr::NodeBootstrapResult* node = &boot;
  deps.identity = [identity]() {
    pbr::HeartbeatIdentity out;
    if (auto loaded = identity->Get(); loaded && loaded->registered) {
      out.relay_user_id = loaded->relay_user_id;
      out.peer_id = loaded->peer_id;
    }
    return out;
  };
  deps.sign = [identity](const std::string& relay_user_id, const std::string& peer_id, const std::string& state,
                         int64_t timestamp) {
    return identity->SignBytes(pbr::BuildRelayHeartbeatSignBytes(relay_user_id, peer_id, state, timestamp));
  };
  deps.post = [](const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers,
                 long timeout_s) {
    return pbr::HttpClient::Post(url, body, headers, 64 * 1024,
                                 pbr::HttpTimeout{.total_s = timeout_s, .connect_s = std::min(timeout_s, 5L)});
  };
  deps.reregister = [node]() { return pbr::PublishOrRenewMeshNodeListing(node->config, *node->identity, ""); };
  deps.unix_ms = []() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  deps.steady_ms = []() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  return deps;
}

pbr::StatusHttpSnapshot MakeSnapshot(pbr::NodeBootstrapResult& boot) {
  pbr::StatusHttpSnapshot snap;
  snap.host_running = boot.mesh && boot.mesh->IsRunning();
  if (boot.mesh) {
    // Prefer ranked advertised front (global /ip6 or LAN) over raw wildcard bind.
    snap.listen_multiaddrs = boot.mesh->AdvertisedListenMultiaddrs();
    if (!snap.listen_multiaddrs.empty()) {
      snap.listen_multiaddr = snap.listen_multiaddrs.front();
    } else {
      snap.listen_multiaddr = boot.mesh->AmpListenMultiaddr();
    }
    if (boot.mesh->Amp()) {
      snap.peer_id = boot.mesh->Amp()->LocalPeerId();
    }
    snap.circuit_relay = boot.mesh->AmpCircuitServer() && boot.mesh->AmpCircuitServer()->IsStarted() &&
                         boot.mesh->AmpCircuitServer()->ServeInbound();
    snap.media_relay = boot.mesh->AmpMediaRelayServer() && boot.mesh->AmpMediaRelayServer()->IsStarted() &&
                       boot.mesh->AmpMediaRelayServer()->ServeInbound();
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
  std::string log_level = EnvOrEmpty("PP_NODE_LOG_LEVEL");
  std::string ledger_role = EnvOrEmpty("PP_NODE_LEDGER_ROLE");
  std::string ledger_dir = EnvOrEmpty("PP_NODE_LEDGER_DIR");

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
    } else if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
      log_level = argv[++i];
    } else if (std::strcmp(argv[i], "--ledger-role") == 0 && i + 1 < argc) {
      ledger_role = argv[++i];
    } else if (std::strcmp(argv[i], "--ledger-dir") == 0 && i + 1 < argc) {
      ledger_dir = argv[++i];
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

  const auto level = NodeLogLevel(log_level, debug_mode);
  if (!level) {
    std::cerr << "--log-level must be debug, info, warning or error\n";
    return 2;
  }
  LedgerRoleSlot ledger;
  if (auto configured = ledger.Configure(ledger_role, ledger_dir); !configured) {
    std::cerr << configured.error().message << "\n";
    return 2;
  }

  pbr::SetSandboxMode(pbr::ResolveSandboxModeFromLaunch(argc, argv));

  auto root = pbr::logging::getRootLogger();
  root.setLevel(*level);
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

  // Operator metrics for /metrics (projects/node-monitoring); goes before the mesh stops.
  pbr::ScopedMetricsCollector node_metrics = pbr::RegisterNodeMetrics(*boot);
  // Heartbeat runs only where this node publishes itself to a mesh directory.
  std::optional<pbr::NodeHeartbeat> heartbeat;
  if (boot->config.mesh.mesh_publish && !boot->config.registration.base_url.empty()) {
    heartbeat.emplace(MakeHeartbeatDeps(*boot));
  }
  pbr::StatusHttpServer status_http;
  if (auto bind = pbr::ParseStatusHttpBind(status_addr_spec)) {
    pbr::StatusHttpAuthConfig auth;
    auth.bearer_token = status_token;
    pbr::NodeBootstrapResult* boot_ptr = &(*boot);
    // Lab only (pp_local_test.sh --suite dht): GET /dht/find?peer_id=… starts a DHT lookup.
    pbr::StatusHttpActions actions;
    if (const char* lab_find = std::getenv("PP_NODE_LAB_DHT_FIND"); lab_find && std::string(lab_find) == "1") {
      actions.dht_find = [boot_ptr](const std::string& peer_id) {
        auto* dht = boot_ptr->mesh ? boot_ptr->mesh->AmpDht() : nullptr;
        if (!dht || !dht->IsStarted()) {
          return false;
        }
        dht->FindPeer(peer_id, [peer_id](pbr::mesh::AmpDhtProtocol::FindPeerRoe found) {
          auto log = pbr::logging::getLogger("pp-node");
          if (found) {
            log.info << "lab dht find " << peer_id << " found from_cache=" << found->from_cache;
          } else {
            log.info << "lab dht find " << peer_id << " failed: " << found.error().message;
          }
        });
        return true;
      };
    }
    // State control steers the node: loopback binds only.
    if (heartbeat && pbr::IsLoopbackStatusBindHost(bind->host)) {
      pbr::NodeHeartbeat* hb = &*heartbeat;
      actions.set_state = [hb](const std::string& state) {
        const auto parsed = pbr::ParseHeartbeatState(state);
        return parsed && hb->SetState(*parsed);
      };
    }
    if (auto started = status_http.Start(*bind, std::move(auth),
                                         [boot_ptr]() { return MakeSnapshot(*boot_ptr); }, std::move(actions));
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

  if (auto started = ledger.Start(*boot); !started) {
    root.error << "ledger role failed to start: " << started.error().message;
    status_http.Stop();
    node_metrics.Reset();
    ShutdownNode(*boot);
    return 1;
  }

  auto schedule_probe = [&]() {
    boot->mesh->StartReachabilityProbe(/*try_upnp_first=*/false);
  };
  schedule_probe();

  // Publish/renew mesh_node listing when advertise multiaddrs configured (N027); a failed first
  // publish is retried with backoff below until the node is registered.
  constexpr auto kPublishRetryInitial = std::chrono::seconds(10);
  constexpr auto kPublishRetryMax = std::chrono::minutes(5);
  std::chrono::steady_clock::duration publish_retry_delay = kPublishRetryInitial;
  auto publish_retry_due = std::chrono::steady_clock::now();
  bool publish_pending = false;
  if (auto published = pbr::PublishOrRenewMeshNodeListing(boot->config, *boot->identity, ""); !published) {
    root.warning << "mesh directory publish skipped/failed: " << published.error().message;
    publish_pending = true;
    publish_retry_due += publish_retry_delay;
  }

  root.info << "pp-node running (SIGINT/SIGTERM to stop)";
  auto last_probe = std::chrono::steady_clock::now();
  auto last_mesh_publish = std::chrono::steady_clock::now();
  // MeshPump drives the mesh (the only driver); this loop only schedules node chores.
  int exit_code = 0;
  while (!g_stop.load()) {
    if (const auto failed = ledger.Failure()) {
      root.error << "ledger role ended: " << *failed;
      exit_code = 1;
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_probe > std::chrono::seconds(60)) {
      last_probe = now;
      schedule_probe();
    }
    if (publish_pending && now >= publish_retry_due) {
      if (auto published = pbr::PublishOrRenewMeshNodeListing(boot->config, *boot->identity, ""); published) {
        publish_pending = false;
        last_mesh_publish = now;
      } else {
        root.warning << "mesh directory publish retry failed: " << published.error().message;
        publish_retry_delay = std::min<std::chrono::steady_clock::duration>(publish_retry_delay * 2, kPublishRetryMax);
        publish_retry_due = now + publish_retry_delay;
      }
    }
    if (heartbeat) {
      heartbeat->Tick();
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
  if (heartbeat) {
    heartbeat->SendFinal();  // before the mesh stops; bounded by a short timeout
  }
  ledger.Stop();  // its protocols withdrawn, its thread joined — before the mesh stops
  status_http.Stop();
  node_metrics.Reset();
  ShutdownNode(*boot);
  return exit_code;
}
