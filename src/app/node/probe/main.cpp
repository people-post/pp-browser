#include "base/p2p/CircuitRelayService.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/MediaRelayService.h"
#include "base/p2p/PeerSessionManager.h"
#include "base/p2p/StreamFrameIo.h"

#include "common/Logger.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using libp2p::peer::ProtocolName;

constexpr const char* kProbeBridgeProtocol = "/pp-browser/pp-node-probe-bridge/1.0.0";

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " --hop <multiaddr-with-p2p> [--advertise-host <ip>]\n"
      << "\n"
      << "L1 relay smoke against a live pp-node hop (image or binary).\n"
      << "Starts a local client + bridge target, then:\n"
      << "  1) media_relay RequestQuote on the hop\n"
      << "  2) circuit_relay RequestBridge through the hop to the local target\n"
      << "\n"
      << "When the hop runs in Docker, pass --advertise-host to an address the\n"
      << "container can dial (e.g. docker0 bridge 172.17.0.1), not 127.0.0.1.\n"
      << "\n"
      << "See packaging/pp-node/IMAGE_SMOKE.md\n";
}

template <typename Result>
Result RunOnWorker(pbr::Libp2pHost& host, std::function<Result()> work) {
  std::promise<Result> promise;
  auto future = promise.get_future();
  pbr::PostLibp2pWorker(host, pbr::WorkerLane::Normal, [&] { promise.set_value(work()); });
  return future.get();
}

std::string RewriteWildcardListenHost(std::string multiaddr) {
  const std::string from = "/ip4/0.0.0.0/";
  const std::string to = "/ip4/127.0.0.1/";
  const auto pos = multiaddr.find(from);
  if (pos != std::string::npos) {
    multiaddr.replace(pos, from.size(), to);
  }
  return multiaddr;
}

bool HasP2pSuffix(const std::string& ma) {
  return ma.find("/p2p/") != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
  std::string hop_ma;
  std::string advertise_host = "127.0.0.1";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--hop") == 0 && i + 1 < argc) {
      hop_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--advertise-host") == 0 && i + 1 < argc) {
      advertise_host = argv[++i];
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }
  if (hop_ma.empty()) {
    if (const char* env = std::getenv("PP_NODE_PROBE_HOP")) {
      hop_ma = env;
    }
  }
  if (const char* env = std::getenv("PP_NODE_PROBE_ADVERTISE_HOST")) {
    if (env[0] != '\0') {
      advertise_host = env;
    }
  }
  if (hop_ma.empty() || !HasP2pSuffix(hop_ma)) {
    std::cerr << "error: --hop multiaddr with /p2p/<PeerId> required\n";
    PrintUsage(argv[0]);
    return 2;
  }
  hop_ma = RewriteWildcardListenHost(std::move(hop_ma));

  auto root = pbr::logging::getRootLogger();
  root.setLevel(pbr::logging::Level::INFO);

  static std::atomic<int> port_base{47000};
  const int client_port = port_base.fetch_add(1);
  const int target_port = port_base.fetch_add(1);

  pbr::PeerSessionConfig sessions_cfg;
  sessions_cfg.dial_timeout = std::chrono::milliseconds(5000);
  sessions_cfg.dial_failure_backoff = std::chrono::milliseconds(100);

  pbr::Libp2pHost client_host;
  pbr::Libp2pHost target_host;
  {
    pbr::Libp2pHostConfig cfg;
    cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(client_port);
    if (auto started = client_host.Start(cfg); !started) {
      std::cerr << "error: client host start: " << started.error().message << "\n";
      return 1;
    }
  }
  {
    pbr::Libp2pHostConfig cfg;
    // Bind all interfaces so a Docker hop can reach us via advertise_host.
    cfg.listen_multiaddr = "/ip4/0.0.0.0/tcp/" + std::to_string(target_port);
    if (auto started = target_host.Start(cfg); !started) {
      std::cerr << "error: target host start: " << started.error().message << "\n";
      return 1;
    }
  }

  auto client_sessions = std::make_unique<pbr::PeerSessionManager>(client_host, sessions_cfg);
  auto client_circuit = std::make_unique<pbr::CircuitRelayService>(client_host, *client_sessions);
  auto client_media = std::make_unique<pbr::MediaRelayService>(client_host, *client_sessions);
  client_circuit->Start();

  auto target_id = target_host.LocalPeerIdBase58();
  if (!target_id) {
    std::cerr << "error: target peer id unavailable\n";
    return 1;
  }
  const std::string target_ma =
      "/ip4/" + advertise_host + "/tcp/" + std::to_string(target_port) + "/p2p/" + *target_id;

  std::mutex target_mu;
  std::condition_variable target_cv;
  bool target_got = false;
  std::vector<uint8_t> target_payload;

  target_host.GetHost().setProtocolHandler(
      {ProtocolName{kProbeBridgeProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        target_host.Post([&, stream = std::move(stream)]() mutable {
          auto reader = std::make_shared<pbr::AsyncLengthPrefixedReader>();
          reader->Start(
              stream,
              [&](pbr::Roe<std::vector<uint8_t>> frame) {
                if (!frame) {
                  return;
                }
                std::lock_guard lock(target_mu);
                target_payload = *frame;
                target_got = true;
                target_cv.notify_one();
              },
              [] { return false; });
        });
      });

  if (auto reg = client_sessions->RegisterEndpoint("hop", hop_ma); !reg) {
    std::cerr << "error: register hop: " << reg.error().message << "\n";
    return 1;
  }

  std::cout << "pp-node L1 probe hop=" << hop_ma << "\n";
  std::cout << "pp-node L1 probe target advertise=" << target_ma << "\n";

  // --- media_relay: quote against hop ---
  {
    pbr::MediaRelayQuoteRequest qreq;
    qreq.call_id = "pp-node-probe";
    qreq.participants = 1;
    auto quote = client_media->RequestQuote("hop", qreq, 8000);
    if (!quote) {
      std::cerr << "error: media_relay quote: " << quote.error().message << "\n";
      return 1;
    }
    if (!quote->ok) {
      std::cerr << "error: media_relay quote rejected: " << quote->error << "\n";
      return 1;
    }
    std::cout << "ok  media_relay RequestQuote quote_id=" << quote->quote_id
              << " pricing=" << quote->pricing_mode << "\n";
  }

  // --- circuit_relay: bridge to local target through hop ---
  {
    pbr::CircuitBridgeTarget target;
    target.target_multiaddr = target_ma;
    target.target_protocol = kProbeBridgeProtocol;
    auto bridged = client_circuit->RequestBridge("hop", target, 10000);
    if (!bridged) {
      std::cerr << "error: circuit_relay bridge: " << bridged.error().message << "\n";
      return 1;
    }
    if (!bridged->ok || !bridged->stream) {
      std::cerr << "error: circuit_relay bridge rejected: " << bridged->error << "\n";
      return 1;
    }
    const std::vector<uint8_t> payload = {'p', 'r', 'o', 'b', 'e'};
    auto write = RunOnWorker<pbr::Roe<void>>(client_host, [&] {
      return pbr::BlockingWriteLengthPrefixedFrame(bridged->stream, payload);
    });
    if (!write) {
      std::cerr << "error: circuit bridge write: " << write.error().message << "\n";
      return 1;
    }
    {
      std::unique_lock lock(target_mu);
      if (!target_cv.wait_for(lock, std::chrono::seconds(5), [&] { return target_got; })) {
        std::cerr << "error: circuit bridge payload not received by local target\n";
        return 1;
      }
      if (target_payload != payload) {
        std::cerr << "error: circuit bridge payload mismatch\n";
        return 1;
      }
    }
    std::cout << "ok  circuit_relay RequestBridge + payload\n";
  }

  client_media.reset();
  client_circuit.reset();
  client_sessions.reset();
  client_host.Stop();
  target_host.Stop();

  std::cout << "pp-node L1 probe PASSED\n";
  return 0;
}
