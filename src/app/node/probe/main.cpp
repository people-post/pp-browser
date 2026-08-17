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

#include <algorithm>
#include <memory>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using libp2p::peer::ProtocolName;

constexpr const char* kProbeBridgeProtocol = "/pp-browser/pp-node-probe-bridge/1.0.0";

enum class ProbeMode { L1, MediaFanout, MediaCap, CircuitCap, MediaSoak };

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0
      << " --hop <multiaddr-with-p2p> [--mode l1|media-fanout|media-cap|circuit-cap|media-soak]\n"
      << "       [--advertise-host <ip>] [--attachers N|N,N,...] [--sweep A:B:S]\n"
      << "       [--bridges M|M,M,...] [--duration SEC] [--churn N]\n"
      << "\n"
      << "Modes:\n"
      << "  l1 (default)            media_relay RequestQuote + circuit_relay bridge payload\n"
      << "  media-fanout (N-FANOUT) quote → AcceptAndAttach ×2 → subscribe → frame fan-out\n"
      << "  media-cap (N-CAP-MEDIA) attach N clients; print n attached success_rate p50_ms p95_ms\n"
      << "  circuit-cap (N-CAP-CIRCUIT) M concurrent bridges + payload; print circuit_curve\n"
      << "  media-soak (N-SOAK)     attach/detach/fan-out loop for --duration (default 120s)\n"
      << "\n"
      << "When the hop runs in Docker, pass --advertise-host for circuit dial-back\n"
      << "(e.g. docker0 bridge 172.17.0.1), not 127.0.0.1.\n"
      << "\n"
      << "See packaging/pp-node/IMAGE_SMOKE.md and docs/ops/TEST_STRATEGY.md\n";
}

double PercentileMs(std::vector<double> samples, double pct) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  if (samples.size() == 1) {
    return samples.front();
  }
  const double idx = (pct / 100.0) * static_cast<double>(samples.size() - 1);
  const auto lo = static_cast<size_t>(std::floor(idx));
  const auto hi = static_cast<size_t>(std::ceil(idx));
  if (lo == hi) {
    return samples[lo];
  }
  const double w = idx - static_cast<double>(lo);
  return samples[lo] * (1.0 - w) + samples[hi] * w;
}

std::optional<std::vector<int>> ParseIntList(const std::string& spec, int min_v, int max_v,
                                             const char* what) {
  std::vector<int> out;
  if (spec.empty()) {
    std::cerr << "error: empty " << what << "\n";
    return std::nullopt;
  }
  const auto colon = spec.find(':');
  if (colon != std::string::npos) {
    std::stringstream ss(spec);
    int start = 0;
    int end = 0;
    int step = 0;
    char c1 = 0;
    char c2 = 0;
    if (!(ss >> start >> c1 >> end >> c2 >> step) || c1 != ':' || c2 != ':') {
      std::cerr << "error: --sweep must be start:end:step (got " << spec << ")\n";
      return std::nullopt;
    }
    if (step <= 0 || start < min_v || end > max_v || start > end) {
      std::cerr << "error: invalid sweep " << spec << " (each value " << min_v << ".." << max_v
                << ")\n";
      return std::nullopt;
    }
    for (int n = start; n <= end; n += step) {
      out.push_back(n);
    }
    return out;
  }
  std::stringstream ss(spec);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (tok.empty()) {
      continue;
    }
    const int n = std::atoi(tok.c_str());
    if (n < min_v || n > max_v) {
      std::cerr << "error: " << what << " value " << n << " out of " << min_v << ".." << max_v
                << "\n";
      return std::nullopt;
    }
    out.push_back(n);
  }
  if (out.empty()) {
    std::cerr << "error: empty " << what << "\n";
    return std::nullopt;
  }
  return out;
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

struct MediaCapResult {
  int n = 0;
  int attached = 0;
  double success_rate = 0.0;
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  bool hop_died = false;
};

void PrintCapCurve(const MediaCapResult& r) {
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "cap_curve n=" << r.n << " attached=" << r.attached
            << " success_rate=" << r.success_rate << " p50_ms=" << r.p50_ms
            << " p95_ms=" << r.p95_ms << "\n";
  std::cout.unsetf(std::ios::floatfield);
}

/**
 * N-CAP-MEDIA: attach N clients to a live hop; report success + attach latency.
 * Soft pass: all attachers succeed for N ≤ 8; larger N is informational unless hop dies.
 */
MediaCapResult RunMediaCapOnce(const std::string& hop_ma, int attachers) {
  MediaCapResult result;
  result.n = attachers;

  static std::atomic<int> port_base{49000};
  pbr::PeerSessionConfig sessions_cfg;
  sessions_cfg.dial_timeout = std::chrono::milliseconds(5000);
  sessions_cfg.dial_failure_backoff = std::chrono::milliseconds(100);

  const std::string call_id = "pp-node-cap-" + std::to_string(attachers);
  std::vector<std::unique_ptr<CapClient>> clients;
  clients.reserve(static_cast<size_t>(attachers));
  std::vector<double> attach_ms;

  std::cout << "pp-node N-CAP-MEDIA probe hop=" << hop_ma << " attachers=" << attachers << "\n";

  int attached = 0;
  int transport_fails = 0;
  for (int i = 0; i < attachers; ++i) {
    auto c = std::make_unique<CapClient>();
    c->port = port_base.fetch_add(1);
    pbr::Libp2pHostConfig cfg;
    cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(c->port);
    if (auto started = c->host.Start(cfg); !started) {
      std::cerr << "error: client-" << i << " host start: " << started.error().message << "\n";
      result.hop_died = true;
      result.attached = attached;
      result.success_rate = attachers > 0 ? (100.0 * attached / attachers) : 0.0;
      result.p50_ms = PercentileMs(attach_ms, 50.0);
      result.p95_ms = PercentileMs(attach_ms, 95.0);
      PrintCapCurve(result);
      return result;
    }
    c->sessions = std::make_unique<pbr::PeerSessionManager>(c->host, sessions_cfg);
    c->relay = std::make_unique<pbr::MediaRelayService>(c->host, *c->sessions);
    if (auto reg = c->sessions->RegisterEndpoint("hop", hop_ma); !reg) {
      std::cerr << "error: client-" << i << " register hop: " << reg.error().message << "\n";
      result.hop_died = true;
      result.attached = attached;
      result.success_rate = 100.0 * attached / attachers;
      result.p50_ms = PercentileMs(attach_ms, 50.0);
      result.p95_ms = PercentileMs(attach_ms, 95.0);
      PrintCapCurve(result);
      return result;
    }

    const auto t0 = std::chrono::steady_clock::now();
    pbr::MediaRelayQuoteRequest qreq;
    qreq.call_id = call_id;
    qreq.participants = attachers;
    auto quote = c->relay->RequestQuote("hop", qreq, 8000);
    if (!quote || !quote->ok) {
      const std::string err = quote ? quote->error : quote.error().message;
      std::cerr << "warn: client-" << i << " quote failed: " << err << "\n";
      if (!quote) {
        ++transport_fails;
      }
      clients.push_back(std::move(c));
      continue;
    }
    auto attach = c->relay->AcceptAndAttach("hop", quote->quote_id, call_id, call_id,
                                            [](pbr::MediaDataFrame) {}, 8000);
    if (!attach || !attach->ok) {
      std::cerr << "warn: client-" << i << " attach failed: "
                << (attach ? attach->error : attach.error().message) << "\n";
      if (!attach) {
        ++transport_fails;
      }
      clients.push_back(std::move(c));
      continue;
    }
    const auto dt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0);
    attach_ms.push_back(dt.count());
    ++attached;
    clients.push_back(std::move(c));
  }

  result.attached = attached;
  result.success_rate = 100.0 * attached / attachers;
  result.p50_ms = PercentileMs(attach_ms, 50.0);
  result.p95_ms = PercentileMs(attach_ms, 95.0);
  result.hop_died = (attached == 0 && transport_fails == attachers);
  PrintCapCurve(result);

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
  return result;
}

int RunMediaCap(const std::string& hop_ma, const std::vector<int>& ns) {
  bool slo_fail = false;
  bool hop_died = false;
  for (int n : ns) {
    auto r = RunMediaCapOnce(hop_ma, n);
    if (r.hop_died) {
      hop_died = true;
      break;
    }
    if (n <= 8 && r.attached < n) {
      slo_fail = true;
    }
  }
  if (hop_died) {
    std::cerr << "error: hop died during N-CAP-MEDIA sweep\n";
    return 1;
  }
  if (slo_fail) {
    std::cerr << "error: soft SLO failed (need all attachers for N<=8)\n";
    return 1;
  }
  std::cout << "pp-node N-CAP-MEDIA probe PASSED\n";
  return 0;
}

struct CircuitTarget {
  pbr::Libp2pHost host;
  int port = 0;
  std::string peer_id;
  std::string advertise_ma;
  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  std::vector<uint8_t> payload;
};

int RunCircuitCapOnce(const std::string& hop_ma, const std::string& advertise_host, int bridges,
                      int& out_ok) {
  out_ok = 0;
  if (bridges < 1 || bridges > 16) {
    std::cerr << "error: --bridges must be 1..16 (got " << bridges << ")\n";
    return 2;
  }

  static std::atomic<int> port_base{50000};
  pbr::PeerSessionConfig sessions_cfg;
  sessions_cfg.dial_timeout = std::chrono::milliseconds(5000);
  sessions_cfg.dial_failure_backoff = std::chrono::milliseconds(100);

  std::cout << "pp-node N-CAP-CIRCUIT probe hop=" << hop_ma << " bridges=" << bridges << "\n";

  pbr::Libp2pHost client_host;
  {
    pbr::Libp2pHostConfig cfg;
    cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(port_base.fetch_add(1));
    if (auto started = client_host.Start(cfg); !started) {
      std::cerr << "error: circuit-cap client host start: " << started.error().message << "\n";
      return 1;
    }
  }
  auto client_sessions = std::make_unique<pbr::PeerSessionManager>(client_host, sessions_cfg);
  auto client_circuit = std::make_unique<pbr::CircuitRelayService>(client_host, *client_sessions);
  client_circuit->Start();
  if (auto reg = client_sessions->RegisterEndpoint("hop", hop_ma); !reg) {
    std::cerr << "error: register hop: " << reg.error().message << "\n";
    return 1;
  }

  std::vector<std::unique_ptr<CircuitTarget>> targets;
  targets.reserve(static_cast<size_t>(bridges));
  for (int i = 0; i < bridges; ++i) {
    auto t = std::make_unique<CircuitTarget>();
    t->port = port_base.fetch_add(1);
    pbr::Libp2pHostConfig cfg;
    cfg.listen_multiaddr = "/ip4/0.0.0.0/tcp/" + std::to_string(t->port);
    if (auto started = t->host.Start(cfg); !started) {
      std::cerr << "error: target-" << i << " host start: " << started.error().message << "\n";
      std::cout << "circuit_curve m=" << bridges << " ok=0 success_rate=0\n";
      return 1;
    }
    auto id = t->host.LocalPeerIdBase58();
    if (!id) {
      std::cerr << "error: target-" << i << " peer id unavailable\n";
      return 1;
    }
    t->peer_id = *id;
    t->advertise_ma = "/ip4/" + advertise_host + "/tcp/" + std::to_string(t->port) + "/p2p/" + *id;
    auto* raw = t.get();
    t->host.GetHost().setProtocolHandler(
        {ProtocolName{kProbeBridgeProtocol}},
        [raw](libp2p::StreamAndProtocol stream_in) {
          auto stream = std::move(stream_in.stream);
          raw->host.Post([raw, stream = std::move(stream)]() mutable {
            auto reader = std::make_shared<pbr::AsyncLengthPrefixedReader>();
            reader->Start(
                stream,
                [raw](pbr::Roe<std::vector<uint8_t>> frame) {
                  if (!frame) {
                    return;
                  }
                  std::lock_guard lock(raw->mu);
                  raw->payload = *frame;
                  raw->got = true;
                  raw->cv.notify_one();
                },
                [] { return false; });
          });
        });
    targets.push_back(std::move(t));
  }

  int ok = 0;
  std::vector<std::shared_ptr<libp2p::connection::Stream>> live;
  for (int i = 0; i < bridges; ++i) {
    pbr::CircuitBridgeTarget target;
    target.target_multiaddr = targets[static_cast<size_t>(i)]->advertise_ma;
    target.target_protocol = kProbeBridgeProtocol;
    auto bridged = client_circuit->RequestBridge("hop", target, 10000);
    if (!bridged || !bridged->ok || !bridged->stream) {
      std::cerr << "warn: bridge-" << i << " failed: "
                << (bridged ? bridged->error : bridged.error().message) << "\n";
      continue;
    }
    const std::vector<uint8_t> payload = {'c', 'a', 'p', static_cast<uint8_t>('0' + (i % 10))};
    auto write = RunOnWorker<pbr::Roe<void>>(client_host, [&] {
      return pbr::BlockingWriteLengthPrefixedFrame(bridged->stream, payload);
    });
    if (!write) {
      std::cerr << "warn: bridge-" << i << " write: " << write.error().message << "\n";
      continue;
    }
    auto& tgt = *targets[static_cast<size_t>(i)];
    {
      std::unique_lock lock(tgt.mu);
      if (!tgt.cv.wait_for(lock, std::chrono::seconds(5), [&] { return tgt.got; })) {
        std::cerr << "warn: bridge-" << i << " payload not received\n";
        continue;
      }
      if (tgt.payload != payload) {
        std::cerr << "warn: bridge-" << i << " payload mismatch\n";
        continue;
      }
    }
    live.push_back(bridged->stream);
    ++ok;
  }

  out_ok = ok;
  const double rate = 100.0 * ok / bridges;
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "circuit_curve m=" << bridges << " ok=" << ok << " success_rate=" << rate << "\n";
  std::cout.unsetf(std::ios::floatfield);

  live.clear();
  client_circuit.reset();
  client_sessions.reset();
  client_host.Stop();
  for (auto& t : targets) {
    t->host.Stop();
  }
  return 0;
}

int RunCircuitCap(const std::string& hop_ma, const std::string& advertise_host,
                  const std::vector<int>& ms) {
  bool slo_fail = false;
  for (int m : ms) {
    int ok = 0;
    const int rc = RunCircuitCapOnce(hop_ma, advertise_host, m, ok);
    if (rc != 0) {
      return rc;
    }
    if (m <= 4 && ok < m) {
      slo_fail = true;
    }
  }
  if (slo_fail) {
    std::cerr << "error: soft SLO failed (need all bridges for M<=4)\n";
    return 1;
  }
  std::cout << "pp-node N-CAP-CIRCUIT probe PASSED\n";
  return 0;
}

int RunMediaSoak(const std::string& hop_ma, int duration_sec, int churn) {
  if (duration_sec < 5 || duration_sec > 86400) {
    std::cerr << "error: --duration must be 5..86400 seconds (got " << duration_sec << ")\n";
    return 2;
  }
  if (churn < 2 || churn > 16) {
    std::cerr << "error: --churn must be 2..16 (got " << churn << ")\n";
    return 2;
  }

  std::cout << "pp-node N-SOAK probe hop=" << hop_ma << " duration=" << duration_sec
            << " churn=" << churn << "\n";

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
  int rounds = 0;
  int attached_total = 0;
  int consecutive_fail = 0;
  constexpr int kMaxConsecutive = 3;

  while (std::chrono::steady_clock::now() < deadline) {
    auto r = RunMediaCapOnce(hop_ma, churn);
    ++rounds;
    attached_total += r.attached;
    if (r.hop_died) {
      std::cerr << "error: hop died during N-SOAK round " << rounds << "\n";
      return 1;
    }
    if (r.attached < churn) {
      ++consecutive_fail;
      if (consecutive_fail >= kMaxConsecutive) {
        std::cerr << "error: " << kMaxConsecutive << " consecutive attach failures in N-SOAK\n";
        return 1;
      }
    } else {
      consecutive_fail = 0;
    }
  }

  std::cout << "soak_summary rounds=" << rounds << " attached_total=" << attached_total << "\n";
  std::cout << "pp-node N-SOAK probe PASSED rounds=" << rounds << " attached_total=" << attached_total
            << "\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::string hop_ma;
  std::string advertise_host = "127.0.0.1";
  ProbeMode mode = ProbeMode::L1;
  std::string attachers_spec = "4";
  std::string bridges_spec = "4";
  int duration_sec = 120;
  int churn = 4;

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
      attachers_spec = argv[++i];
    } else if (std::strcmp(argv[i], "--sweep") == 0 && i + 1 < argc) {
      attachers_spec = argv[++i];
    } else if (std::strcmp(argv[i], "--bridges") == 0 && i + 1 < argc) {
      bridges_spec = argv[++i];
    } else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
      duration_sec = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--churn") == 0 && i + 1 < argc) {
      churn = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      ++i;
      if (std::strcmp(argv[i], "l1") == 0) {
        mode = ProbeMode::L1;
      } else if (std::strcmp(argv[i], "media-fanout") == 0) {
        mode = ProbeMode::MediaFanout;
      } else if (std::strcmp(argv[i], "media-cap") == 0) {
        mode = ProbeMode::MediaCap;
      } else if (std::strcmp(argv[i], "circuit-cap") == 0) {
        mode = ProbeMode::CircuitCap;
      } else if (std::strcmp(argv[i], "media-soak") == 0) {
        mode = ProbeMode::MediaSoak;
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
    } else if (std::strcmp(env, "circuit-cap") == 0) {
      mode = ProbeMode::CircuitCap;
    } else if (std::strcmp(env, "media-soak") == 0) {
      mode = ProbeMode::MediaSoak;
    } else if (std::strcmp(env, "l1") == 0) {
      mode = ProbeMode::L1;
    }
  }
  if (const char* env = std::getenv("PP_NODE_PROBE_ATTACHERS")) {
    if (env[0] != '\0') {
      attachers_spec = env;
    }
  }
  if (const char* env = std::getenv("PP_NODE_PROBE_BRIDGES")) {
    if (env[0] != '\0') {
      bridges_spec = env;
    }
  }
  if (const char* env = std::getenv("PP_NODE_SOAK_SEC")) {
    if (env[0] != '\0') {
      duration_sec = std::atoi(env);
    }
  }
  if (const char* env = std::getenv("PP_NODE_PROBE_CHURN")) {
    if (env[0] != '\0') {
      churn = std::atoi(env);
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
    auto ns = ParseIntList(attachers_spec, 1, 32, "--attachers");
    if (!ns) {
      return 2;
    }
    return RunMediaCap(hop_ma, *ns);
  }
  if (mode == ProbeMode::CircuitCap) {
    auto ms = ParseIntList(bridges_spec, 1, 16, "--bridges");
    if (!ms) {
      return 2;
    }
    return RunCircuitCap(hop_ma, advertise_host, *ms);
  }
  if (mode == ProbeMode::MediaSoak) {
    return RunMediaSoak(hop_ma, duration_sec, churn);
  }
  return RunL1(hop_ma, advertise_host);
}
