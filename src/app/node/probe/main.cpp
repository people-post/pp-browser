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

enum class ProbeMode { L1, MediaFanout, MediaCap };

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0
      << " --hop <multiaddr-with-p2p> [--mode l1|media-fanout|media-cap]\n"
      << "       [--advertise-host <ip>] [--attachers N]\n"
      << "\n"
      << "Modes:\n"
      << "  l1 (default)         media_relay RequestQuote + circuit_relay bridge payload\n"
      << "  media-fanout (N-FANOUT) quote → AcceptAndAttach ×2 → subscribe → frame fan-out\n"
      << "  media-cap (N-CAP-MEDIA) attach N clients; print success curve (soft SLO)\n"
      << "\n"
      << "When the hop runs in Docker, pass --advertise-host for L1 circuit dial-back\n"
      << "(e.g. docker0 bridge 172.17.0.1), not 127.0.0.1.\n"
      << "\n"
      << "See packaging/pp-node/IMAGE_SMOKE.md and docs/ops/TEST_STRATEGY.md\n";
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

void SendUntilReceived(pbr::MediaRelayService& sender, pbr::MediaDataFrame frame, std::mutex& mu,
                       std::condition_variable& cv, bool& got,
                       const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  uint32_t seq = frame.seq == 0 ? 1 : frame.seq;
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard lock(mu);
      if (got) {
        return;
      }
    }
    frame.seq = seq++;
    if (!sender.SendFrame(frame)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    std::unique_lock lock(mu);
    if (cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return got; })) {
      return;
    }
  }
}

int RunL1(const std::string& hop_ma, const std::string& advertise_host) {
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

/** N-FANOUT: two client hosts in this process against a live (often container) hop. */
int RunMediaFanout(const std::string& hop_ma) {
  static std::atomic<int> port_base{48000};
  const int a_port = port_base.fetch_add(1);
  const int b_port = port_base.fetch_add(1);

  pbr::PeerSessionConfig sessions_cfg;
  sessions_cfg.dial_timeout = std::chrono::milliseconds(5000);
  sessions_cfg.dial_failure_backoff = std::chrono::milliseconds(100);

  pbr::Libp2pHost a_host;
  pbr::Libp2pHost b_host;
  {
    pbr::Libp2pHostConfig cfg;
    cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(a_port);
    if (auto started = a_host.Start(cfg); !started) {
      std::cerr << "error: client-a host start: " << started.error().message << "\n";
      return 1;
    }
  }
  {
    pbr::Libp2pHostConfig cfg;
    cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(b_port);
    if (auto started = b_host.Start(cfg); !started) {
      std::cerr << "error: client-b host start: " << started.error().message << "\n";
      return 1;
    }
  }

  auto a_sessions = std::make_unique<pbr::PeerSessionManager>(a_host, sessions_cfg);
  auto b_sessions = std::make_unique<pbr::PeerSessionManager>(b_host, sessions_cfg);
  auto a_relay = std::make_unique<pbr::MediaRelayService>(a_host, *a_sessions);
  auto b_relay = std::make_unique<pbr::MediaRelayService>(b_host, *b_sessions);

  if (auto reg = a_sessions->RegisterEndpoint("hop", hop_ma); !reg) {
    std::cerr << "error: client-a register hop: " << reg.error().message << "\n";
    return 1;
  }
  if (auto reg = b_sessions->RegisterEndpoint("hop", hop_ma); !reg) {
    std::cerr << "error: client-b register hop: " << reg.error().message << "\n";
    return 1;
  }

  const std::string call_id = "pp-node-fanout";
  pbr::MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;

  std::cout << "pp-node N-FANOUT probe hop=" << hop_ma << "\n";

  auto qa = a_relay->RequestQuote("hop", qreq, 8000);
  if (!qa) {
    std::cerr << "error: client-a quote: " << qa.error().message << "\n";
    return 1;
  }
  if (!qa->ok) {
    std::cerr << "error: client-a quote rejected: " << qa->error << "\n";
    return 1;
  }
  auto qb = b_relay->RequestQuote("hop", qreq, 8000);
  if (!qb) {
    std::cerr << "error: client-b quote: " << qb.error().message << "\n";
    return 1;
  }
  if (!qb->ok) {
    std::cerr << "error: client-b quote rejected: " << qb->error << "\n";
    return 1;
  }
  std::cout << "ok  quotes a=" << qa->quote_id << " b=" << qb->quote_id << "\n";

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  pbr::MediaDataFrame received;

  auto attach_a =
      a_relay->AcceptAndAttach("hop", qa->quote_id, call_id, call_id, [](pbr::MediaDataFrame) {}, 8000);
  if (!attach_a) {
    std::cerr << "error: client-a attach: " << attach_a.error().message << "\n";
    return 1;
  }
  if (!attach_a->ok) {
    std::cerr << "error: client-a attach rejected: " << attach_a->error << "\n";
    return 1;
  }
  auto attach_b = b_relay->AcceptAndAttach(
      "hop", qb->quote_id, call_id, call_id,
      [&](pbr::MediaDataFrame frame) {
        std::lock_guard lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      8000);
  if (!attach_b) {
    std::cerr << "error: client-b attach: " << attach_b.error().message << "\n";
    return 1;
  }
  if (!attach_b->ok) {
    std::cerr << "error: client-b attach rejected: " << attach_b->error << "\n";
    return 1;
  }
  std::cout << "ok  AcceptAndAttach ×2\n";

  a_relay->StartClientFrameReader();
  b_relay->StartClientFrameReader();

  if (auto sub = b_relay->Subscribe(1, 0); !sub) {
    std::cerr << "error: client-b subscribe: " << sub.error().message << "\n";
    return 1;
  }

  pbr::MediaDataFrame sent;
  sent.stream_id = 1;
  sent.channel_id = 0;
  sent.channel_type = pbr::MediaChannelType::LatestLossy;
  sent.seq = 1;
  sent.payload = {'f', 'a', 'n', 'o', 'u', 't'};
  SendUntilReceived(*a_relay, sent, mu, cv, got);
  if (!got) {
    std::cerr << "error: fan-out frame not received by client-b\n";
    return 1;
  }
  if (received.stream_id != 1u || received.payload != sent.payload) {
    std::cerr << "error: fan-out frame mismatch\n";
    return 1;
  }
  std::cout << "ok  fan-out frame delivered to client-b\n";

  a_relay->Detach();
  b_relay->Detach();
  a_relay.reset();
  b_relay.reset();
  a_sessions.reset();
  b_sessions.reset();
  a_host.Stop();
  b_host.Stop();

  std::cout << "pp-node N-FANOUT probe PASSED\n";
  return 0;
}

struct CapClient {
  pbr::Libp2pHost host;
  std::unique_ptr<pbr::PeerSessionManager> sessions;
  std::unique_ptr<pbr::MediaRelayService> relay;
  int port = 0;
};

/**
 * N-CAP-MEDIA soft start: attach N clients to a live hop; report success count.
 * Soft pass: all attachers succeed for N ≤ default (no hard latency SLO yet).
 */
int RunMediaCap(const std::string& hop_ma, int attachers) {
  if (attachers < 1 || attachers > 32) {
    std::cerr << "error: --attachers must be 1..32 (got " << attachers << ")\n";
    return 2;
  }

  static std::atomic<int> port_base{49000};
  pbr::PeerSessionConfig sessions_cfg;
  sessions_cfg.dial_timeout = std::chrono::milliseconds(5000);
  sessions_cfg.dial_failure_backoff = std::chrono::milliseconds(100);

  const std::string call_id = "pp-node-cap";
  std::vector<std::unique_ptr<CapClient>> clients;
  clients.reserve(static_cast<size_t>(attachers));

  std::cout << "pp-node N-CAP-MEDIA probe hop=" << hop_ma << " attachers=" << attachers << "\n";

  int attached = 0;
  for (int i = 0; i < attachers; ++i) {
    auto c = std::make_unique<CapClient>();
    c->port = port_base.fetch_add(1);
    pbr::Libp2pHostConfig cfg;
    cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(c->port);
    if (auto started = c->host.Start(cfg); !started) {
      std::cerr << "error: client-" << i << " host start: " << started.error().message << "\n";
      std::cout << "cap_curve n=" << attachers << " attached=" << attached
                << " success_rate=" << (attachers > 0 ? (100.0 * attached / attachers) : 0.0) << "\n";
      return 1;
    }
    c->sessions = std::make_unique<pbr::PeerSessionManager>(c->host, sessions_cfg);
    c->relay = std::make_unique<pbr::MediaRelayService>(c->host, *c->sessions);
    if (auto reg = c->sessions->RegisterEndpoint("hop", hop_ma); !reg) {
      std::cerr << "error: client-" << i << " register hop: " << reg.error().message << "\n";
      std::cout << "cap_curve n=" << attachers << " attached=" << attached
                << " success_rate=" << (100.0 * attached / attachers) << "\n";
      return 1;
    }

    pbr::MediaRelayQuoteRequest qreq;
    qreq.call_id = call_id;
    qreq.participants = attachers;
    auto quote = c->relay->RequestQuote("hop", qreq, 8000);
    if (!quote || !quote->ok) {
      std::cerr << "warn: client-" << i << " quote failed: "
                << (quote ? quote->error : quote.error().message) << "\n";
      clients.push_back(std::move(c));
      continue;
    }
    auto attach = c->relay->AcceptAndAttach("hop", quote->quote_id, call_id, call_id,
                                            [](pbr::MediaDataFrame) {}, 8000);
    if (!attach || !attach->ok) {
      std::cerr << "warn: client-" << i << " attach failed: "
                << (attach ? attach->error : attach.error().message) << "\n";
      clients.push_back(std::move(c));
      continue;
    }
    ++attached;
    clients.push_back(std::move(c));
  }

  const double rate = 100.0 * attached / attachers;
  std::cout << "cap_curve n=" << attachers << " attached=" << attached << " success_rate=" << rate
            << "\n";

  for (auto& c : clients) {
    if (c && c->relay) {
      c->relay->Detach();
    }
  }
  for (auto& c : clients) {
    if (!c) {
      continue;
    }
    c->relay.reset();
    c->sessions.reset();
    c->host.Stop();
  }

  // Soft SLO (Gate B): require 100% attach success for N≤8; larger N is informational.
  if (attachers <= 8 && attached < attachers) {
    std::cerr << "error: soft SLO failed (need all attachers for N<=8)\n";
    return 1;
  }
  std::cout << "pp-node N-CAP-MEDIA probe PASSED\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::string hop_ma;
  std::string advertise_host = "127.0.0.1";
  ProbeMode mode = ProbeMode::L1;
  int attachers = 4;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--hop") == 0 && i + 1 < argc) {
      hop_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--advertise-host") == 0 && i + 1 < argc) {
      advertise_host = argv[++i];
    } else if (std::strcmp(argv[i], "--attachers") == 0 && i + 1 < argc) {
      attachers = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      ++i;
      if (std::strcmp(argv[i], "l1") == 0) {
        mode = ProbeMode::L1;
      } else if (std::strcmp(argv[i], "media-fanout") == 0) {
        mode = ProbeMode::MediaFanout;
      } else if (std::strcmp(argv[i], "media-cap") == 0) {
        mode = ProbeMode::MediaCap;
      } else {
        std::cerr << "Unknown --mode: " << argv[i] << "\n";
        PrintUsage(argv[0]);
        return 2;
      }
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
  if (const char* env = std::getenv("PP_NODE_PROBE_MODE")) {
    if (std::strcmp(env, "media-fanout") == 0) {
      mode = ProbeMode::MediaFanout;
    } else if (std::strcmp(env, "media-cap") == 0) {
      mode = ProbeMode::MediaCap;
    } else if (std::strcmp(env, "l1") == 0) {
      mode = ProbeMode::L1;
    }
  }
  if (const char* env = std::getenv("PP_NODE_PROBE_ATTACHERS")) {
    if (env[0] != '\0') {
      attachers = std::atoi(env);
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

  if (mode == ProbeMode::MediaFanout) {
    return RunMediaFanout(hop_ma);
  }
  if (mode == ProbeMode::MediaCap) {
    return RunMediaCap(hop_ma, attachers);
  }
  return RunL1(hop_ma, advertise_host);
}
