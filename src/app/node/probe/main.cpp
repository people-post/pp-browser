#include "base/adp/Clock.h"
#include "base/adp/OsUdpDatagramIo.h"
#include "base/adp/Types.h"
#include "base/crypto/MlDsa.h"
#include "base/p2p/ProductChannelPolicies.h"
#include "base/mesh/channel/ChannelSession.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/AmpStack.h"
#include "base/mesh/link/Types.h"
#include "base/p2p/AmpMediaRelayCoordinator.h"
#include "base/p2p/CircuitTunnelCoordinator.h"
#include "base/p2p/MediaRelayTypes.h"
#include "base/p2p/PeerIdUtil.h"

#include "common/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kProbeBridgeProtocol = "/pp-browser/pp-node-probe-bridge/1.0.0";

enum class ProbeMode { L1, MediaFanout, MediaCap, CircuitCap, MediaSoak };

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0
      << " --hop <adp-multiaddr-with-p2p> [--mode l1|media-fanout|media-cap|circuit-cap|media-soak]\n"
      << "       [--advertise-host <ip>] [--attachers N|N,N,...] [--sweep A:B:S]\n"
      << "       [--bridges M|M,M,...] [--duration SEC] [--churn N]\n"
      << "\n"
      << "Amp thin client against a live hop (pp-node Amp listen MA).\n"
      << "Hop example: /ip4/127.0.0.1/udp/4001/adp/1.0.0/p2p/<PeerId>\n"
      << "\n"
      << "Modes:\n"
      << "  l1 (default)            Amp media-relay RequestQuote + Amp circuit StartBridge payload\n"
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

bool HasP2pSuffix(const std::string& ma) {
  return ma.find("/p2p/") != std::string::npos;
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

std::string RewriteListenHost(std::string multiaddr, const std::string& host) {
  const std::string from0 = "/ip4/0.0.0.0/";
  const std::string from1 = "/ip4/127.0.0.1/";
  const std::string to = "/ip4/" + host + "/";
  auto pos = multiaddr.find(from0);
  if (pos != std::string::npos) {
    multiaddr.replace(pos, from0.size(), to);
    return multiaddr;
  }
  pos = multiaddr.find(from1);
  if (pos != std::string::npos && host != "127.0.0.1") {
    multiaddr.replace(pos, from1.size(), to);
  }
  return multiaddr;
}

pbr::amp::PeerLinkConfig MakeProbeLinkConfig() {
  pbr::amp::PeerLinkConfig config;
  config.peer_id_from_identity = [](const pbr::ByteVector& identity_public_key) -> std::string {
    auto peer_id = pbr::PeerIdFromMlDsaPublicKey(identity_public_key);
    if (!peer_id) {
      return {};
    }
    return *peer_id;
  };
  return config;
}

struct AmpPeer {
  std::shared_ptr<pbr::adp::WallClock> clock;
  std::unique_ptr<pbr::amp::AmpStack> stack;
  std::string peer_id;
  std::string listen_ma;

  void Pump() {
    if (stack) {
      stack->Pump();
      stack->Tick();
    }
  }

  pbr::amp::MeshRuntime& Runtime() { return stack->Runtime(); }
  pbr::amp::PeerLinkManager& Links() { return stack->Links(); }
};

void PumpPeers(const std::vector<AmpPeer*>& peers) {
  for (auto* p : peers) {
    if (p) {
      p->Pump();
    }
  }
}

template <typename Pred>
bool PumpUntil(const std::vector<AmpPeer*>& peers, Pred&& done, const int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) {
      return true;
    }
    PumpPeers(peers);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

pbr::Roe<std::unique_ptr<AmpPeer>> MakeAmpPeer(const pbr::adp::IpEndpoint& bind_ep,
                                               const bool accept_inbound) {
  auto keys = pbr::MlDsa::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  auto peer_id = pbr::PeerIdFromMlDsaPublicKey(keys->public_key);
  if (!peer_id) {
    return peer_id.error();
  }

  auto bound = pbr::adp::OsUdpDatagramIo::Bind(bind_ep);
  if (!bound) {
    return bound.error();
  }

  auto peer = std::make_unique<AmpPeer>();
  peer->clock = std::make_shared<pbr::adp::WallClock>();
  peer->peer_id = *peer_id;

  pbr::amp::MshIdentity identity;
  identity.ml_dsa_secret_key = std::move(keys->secret_key);
  identity.ml_dsa_public_key = std::move(keys->public_key);

  pbr::amp::AmpStack::Config cfg;
  cfg.identity = std::move(identity);
  cfg.local_peer_id = peer->peer_id;
  cfg.link_config = MakeProbeLinkConfig();

  std::shared_ptr<pbr::adp::DatagramIo> io = std::move(*bound);
  auto stack = pbr::amp::AmpStack::Create(std::move(io), peer->clock, std::move(cfg));
  if (!stack) {
    return stack.error();
  }
  peer->stack = std::move(*stack);
  peer->stack->Start();
  peer->stack->GetEndpoint().SetAcceptEnabled(accept_inbound);

  auto listen = pbr::amp::FormatAdpMultiaddr(peer->stack->LocalEndpoint(), peer->peer_id);
  if (!listen) {
    return listen.error();
  }
  peer->listen_ma = *listen;
  peer->Links().SetLocalListenMultiaddrs({peer->listen_ma});
  return peer;
}

pbr::Roe<std::unique_ptr<AmpPeer>> MakeLocalClient() {
  return MakeAmpPeer(pbr::adp::IpEndpoint::V4(127, 0, 0, 1, 0), false);
}

pbr::Roe<std::unique_ptr<AmpPeer>> MakeAdvertisableTarget() {
  return MakeAmpPeer(pbr::adp::IpEndpoint::V4(0, 0, 0, 0, 0), true);
}

template <typename Result>
struct AsyncWait {
  std::atomic<bool> done{false};
  pbr::Roe<Result> result = pbr::Error("pending");

  std::function<void(pbr::Roe<Result>)> Fn() {
    return [this](pbr::Roe<Result> r) {
      result = std::move(r);
      done.store(true, std::memory_order_release);
    };
  }

  bool PumpUntilDone(const std::vector<AmpPeer*>& peers, const int timeout_ms = 10000) {
    return PumpUntil(peers, [this] { return done.load(std::memory_order_acquire); }, timeout_ms);
  }
};

void ArmProbeBridgeTarget(AmpPeer& target, std::mutex& mu, bool& got, std::vector<uint8_t>& payload) {
  target.Links().SetProtocolHandler(
      kProbeBridgeProtocol, [&](pbr::amp::PeerLink& link, const uint32_t channel_id) {
        auto session = std::make_shared<pbr::amp::ChannelSession>();
        session->Bind(*link.Mux(), channel_id, pbr::amp::CircuitTunnelChannelPolicy(),
                      [&, session](pbr::Roe<std::vector<uint8_t>> frame) {
                        if (!frame) {
                          return false;
                        }
                        std::lock_guard lock(mu);
                        payload = *frame;
                        got = true;
                        return true;
                      });
      });
}

int RunL1(const std::string& hop_ma, const std::string& advertise_host) {
  auto client = MakeLocalClient();
  if (!client) {
    std::cerr << "error: client amp start: " << client.error().message << "\n";
    return 1;
  }
  auto target = MakeAdvertisableTarget();
  if (!target) {
    std::cerr << "error: target amp start: " << target.error().message << "\n";
    return 1;
  }

  std::string target_ma = RewriteListenHost((*target)->listen_ma, advertise_host);

  std::mutex target_mu;
  bool target_got = false;
  std::vector<uint8_t> target_payload;
  ArmProbeBridgeTarget(**target, target_mu, target_got, target_payload);

  if (auto reg = (*client)->Links().RegisterEndpoint("hop", hop_ma); !reg) {
    std::cerr << "error: register hop: " << reg.error().message << "\n";
    return 1;
  }

  auto media = std::make_unique<pbr::AmpMediaRelayCoordinator>((*client)->Runtime());
  auto circuit = std::make_unique<pbr::CircuitTunnelCoordinator>((*client)->Runtime());
  media->Start();
  media->SetServeInbound(false);
  circuit->Start();
  circuit->SetServeInbound(false);

  const std::vector<AmpPeer*> pumps = {client->get(), target->get()};

  std::cout << "pp-node L1 probe hop=" << hop_ma << "\n";
  std::cout << "pp-node L1 probe target advertise=" << target_ma << "\n";

  {
    pbr::MediaRelayQuoteRequest qreq;
    qreq.call_id = "pp-node-probe";
    qreq.participants = 1;
    AsyncWait<pbr::MediaRelayQuote> wait;
    if (!media->StartQuote("hop", qreq, wait.Fn(), 8000)) {
      std::cerr << "error: media_relay quote start failed\n";
      return 1;
    }
    if (!wait.PumpUntilDone(pumps) || !wait.result) {
      std::cerr << "error: media_relay quote: "
                << (wait.result ? wait.result->error : wait.result.error().message) << "\n";
      return 1;
    }
    if (!wait.result->ok) {
      std::cerr << "error: media_relay quote rejected: " << wait.result->error << "\n";
      return 1;
    }
    std::cout << "ok  media_relay RequestQuote quote_id=" << wait.result->quote_id
              << " pricing=" << wait.result->pricing_mode << "\n";
  }

  {
    pbr::CircuitBridgeTarget bridge_target;
    bridge_target.target_peer_id = (*target)->peer_id;
    bridge_target.target_multiaddr = target_ma;
    bridge_target.target_protocol = kProbeBridgeProtocol;

    AsyncWait<pbr::CircuitTunnelBridgeResult> wait;
    auto tunnel_id = circuit->StartBridge("hop", bridge_target, {}, {}, wait.Fn(), 10000);
    if (!tunnel_id) {
      std::cerr << "error: circuit_relay bridge start failed\n";
      return 1;
    }
    if (!wait.PumpUntilDone(pumps, 12000) || !wait.result) {
      std::cerr << "error: circuit_relay bridge: "
                << (wait.result ? wait.result->error : wait.result.error().message) << "\n";
      return 1;
    }
    if (!wait.result->ok || !wait.result->session) {
      std::cerr << "error: circuit_relay bridge rejected: " << wait.result->error << "\n";
      return 1;
    }

    const std::vector<uint8_t> payload = {'p', 'r', 'o', 'b', 'e'};
    if (!wait.result->session->EnqueueOutbound(payload)) {
      std::cerr << "error: circuit bridge write failed\n";
      return 1;
    }
    if (!PumpUntil(
            pumps,
            [&] {
              std::lock_guard lock(target_mu);
              return target_got;
            },
            8000)) {
      std::cerr << "error: circuit bridge payload not received by local target\n";
      return 1;
    }
    {
      std::lock_guard lock(target_mu);
      if (target_payload != payload) {
        std::cerr << "error: circuit bridge payload mismatch\n";
        return 1;
      }
    }
    std::cout << "ok  circuit_relay StartBridge + payload\n";
  }

  media->Stop();
  circuit->Stop();
  (*client)->stack->Stop();
  (*target)->stack->Stop();

  std::cout << "pp-node L1 probe PASSED\n";
  return 0;
}

int RunMediaFanout(const std::string& hop_ma) {
  auto a = MakeLocalClient();
  if (!a) {
    std::cerr << "error: client-a amp start: " << a.error().message << "\n";
    return 1;
  }
  auto b = MakeLocalClient();
  if (!b) {
    std::cerr << "error: client-b amp start: " << b.error().message << "\n";
    return 1;
  }

  if (auto reg = (*a)->Links().RegisterEndpoint("hop", hop_ma); !reg) {
    std::cerr << "error: client-a register hop: " << reg.error().message << "\n";
    return 1;
  }
  if (auto reg = (*b)->Links().RegisterEndpoint("hop", hop_ma); !reg) {
    std::cerr << "error: client-b register hop: " << reg.error().message << "\n";
    return 1;
  }

  auto a_relay = std::make_unique<pbr::AmpMediaRelayCoordinator>((*a)->Runtime());
  auto b_relay = std::make_unique<pbr::AmpMediaRelayCoordinator>((*b)->Runtime());
  a_relay->Start();
  b_relay->Start();
  a_relay->SetServeInbound(false);
  b_relay->SetServeInbound(false);

  const std::vector<AmpPeer*> pumps = {a->get(), b->get()};
  const std::string call_id = "pp-node-fanout";
  pbr::MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;

  std::cout << "pp-node N-FANOUT probe hop=" << hop_ma << "\n";

  AsyncWait<pbr::MediaRelayQuote> qa;
  AsyncWait<pbr::MediaRelayQuote> qb;
  if (!a_relay->StartQuote("hop", qreq, qa.Fn(), 8000) || !b_relay->StartQuote("hop", qreq, qb.Fn(), 8000)) {
    std::cerr << "error: quote start failed\n";
    return 1;
  }
  if (!qa.PumpUntilDone(pumps) || !qa.result || !qa.result->ok) {
    std::cerr << "error: client-a quote: "
              << (qa.result ? qa.result->error : qa.result.error().message) << "\n";
    return 1;
  }
  if (!qb.PumpUntilDone(pumps) || !qb.result || !qb.result->ok) {
    std::cerr << "error: client-b quote: "
              << (qb.result ? qb.result->error : qb.result.error().message) << "\n";
    return 1;
  }
  std::cout << "ok  quotes a=" << qa.result->quote_id << " b=" << qb.result->quote_id << "\n";

  std::atomic<bool> got{false};
  pbr::MediaDataFrame received;

  AsyncWait<pbr::MediaRelayAttachResult> attach_a;
  AsyncWait<pbr::MediaRelayAttachResult> attach_b;
  if (!a_relay->StartAttach("hop", qa.result->quote_id, call_id, call_id, [](pbr::MediaDataFrame) {},
                            attach_a.Fn(), 8000)) {
    std::cerr << "error: client-a attach start failed\n";
    return 1;
  }
  if (!b_relay->StartAttach(
          "hop", qb.result->quote_id, call_id, call_id,
          [&](pbr::MediaDataFrame frame) {
            received = std::move(frame);
            got.store(true, std::memory_order_release);
          },
          attach_b.Fn(), 8000)) {
    std::cerr << "error: client-b attach start failed\n";
    return 1;
  }
  if (!attach_a.PumpUntilDone(pumps) || !attach_a.result || !attach_a.result->ok) {
    std::cerr << "error: client-a attach: "
              << (attach_a.result ? attach_a.result->error : attach_a.result.error().message) << "\n";
    return 1;
  }
  if (!attach_b.PumpUntilDone(pumps) || !attach_b.result || !attach_b.result->ok) {
    std::cerr << "error: client-b attach: "
              << (attach_b.result ? attach_b.result->error : attach_b.result.error().message) << "\n";
    return 1;
  }
  std::cout << "ok  AcceptAndAttach ×2\n";

  a_relay->StartClientFrameReader();
  b_relay->StartClientFrameReader();

  if (auto sub = b_relay->Subscribe(1, 0); !sub) {
    std::cerr << "error: client-b subscribe: " << sub.error().message << "\n";
    return 1;
  }
  for (int i = 0; i < 40; ++i) {
    PumpPeers(pumps);
  }

  pbr::MediaDataFrame sent;
  sent.stream_id = 1;
  sent.channel_id = 0;
  sent.channel_type = pbr::MediaChannelType::LatestLossy;
  sent.payload = {'f', 'a', 'n', 'o', 'u', 't'};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  uint32_t seq = 1;
  while (std::chrono::steady_clock::now() < deadline && !got.load(std::memory_order_acquire)) {
    sent.seq = seq++;
    (void)a_relay->SendFrame(sent);
    PumpPeers(pumps);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!got.load(std::memory_order_acquire)) {
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
  a_relay->Stop();
  b_relay->Stop();
  (*a)->stack->Stop();
  (*b)->stack->Stop();

  std::cout << "pp-node N-FANOUT probe PASSED\n";
  return 0;
}

struct CapClient {
  std::unique_ptr<AmpPeer> peer;
  std::unique_ptr<pbr::AmpMediaRelayCoordinator> relay;
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

MediaCapResult RunMediaCapOnce(const std::string& hop_ma, int attachers) {
  MediaCapResult result;
  result.n = attachers;

  const std::string call_id = "pp-node-cap-" + std::to_string(attachers);
  std::vector<std::unique_ptr<CapClient>> clients;
  clients.reserve(static_cast<size_t>(attachers));
  std::vector<double> attach_ms;
  std::vector<AmpPeer*> pumps;

  std::cout << "pp-node N-CAP-MEDIA probe hop=" << hop_ma << " attachers=" << attachers << "\n";

  int attached = 0;
  int transport_fails = 0;
  for (int i = 0; i < attachers; ++i) {
    auto c = std::make_unique<CapClient>();
    auto peer = MakeLocalClient();
    if (!peer) {
      std::cerr << "error: client-" << i << " amp start: " << peer.error().message << "\n";
      result.hop_died = true;
      result.attached = attached;
      result.success_rate = attachers > 0 ? (100.0 * attached / attachers) : 0.0;
      result.p50_ms = PercentileMs(attach_ms, 50.0);
      result.p95_ms = PercentileMs(attach_ms, 95.0);
      PrintCapCurve(result);
      return result;
    }
    c->peer = std::move(*peer);
    pumps.push_back(c->peer.get());
    c->relay = std::make_unique<pbr::AmpMediaRelayCoordinator>(c->peer->Runtime());
    c->relay->Start();
    c->relay->SetServeInbound(false);
    if (auto reg = c->peer->Links().RegisterEndpoint("hop", hop_ma); !reg) {
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

    AsyncWait<pbr::MediaRelayQuote> quote_wait;
    if (!c->relay->StartQuote("hop", qreq, quote_wait.Fn(), 8000)) {
      ++transport_fails;
      clients.push_back(std::move(c));
      continue;
    }
    if (!quote_wait.PumpUntilDone(pumps) || !quote_wait.result || !quote_wait.result->ok) {
      const std::string err =
          quote_wait.result ? quote_wait.result->error : quote_wait.result.error().message;
      std::cerr << "warn: client-" << i << " quote failed: " << err << "\n";
      if (!quote_wait.result) {
        ++transport_fails;
      }
      clients.push_back(std::move(c));
      continue;
    }

    AsyncWait<pbr::MediaRelayAttachResult> attach_wait;
    if (!c->relay->StartAttach("hop", quote_wait.result->quote_id, call_id, call_id,
                               [](pbr::MediaDataFrame) {}, attach_wait.Fn(), 8000)) {
      ++transport_fails;
      clients.push_back(std::move(c));
      continue;
    }
    if (!attach_wait.PumpUntilDone(pumps) || !attach_wait.result || !attach_wait.result->ok) {
      std::cerr << "warn: client-" << i << " attach failed: "
                << (attach_wait.result ? attach_wait.result->error
                                       : attach_wait.result.error().message)
                << "\n";
      if (!attach_wait.result) {
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
      c->relay->Stop();
    }
    if (c && c->peer && c->peer->stack) {
      c->peer->stack->Stop();
    }
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
  std::unique_ptr<AmpPeer> peer;
  std::string advertise_ma;
  std::mutex mu;
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

  std::cout << "pp-node N-CAP-CIRCUIT probe hop=" << hop_ma << " bridges=" << bridges << "\n";

  auto client = MakeLocalClient();
  if (!client) {
    std::cerr << "error: circuit-cap client amp start: " << client.error().message << "\n";
    return 1;
  }
  auto circuit = std::make_unique<pbr::CircuitTunnelCoordinator>((*client)->Runtime());
  circuit->Start();
  circuit->SetServeInbound(false);
  if (auto reg = (*client)->Links().RegisterEndpoint("hop", hop_ma); !reg) {
    std::cerr << "error: register hop: " << reg.error().message << "\n";
    return 1;
  }

  std::vector<std::unique_ptr<CircuitTarget>> targets;
  targets.reserve(static_cast<size_t>(bridges));
  std::vector<AmpPeer*> pumps = {client->get()};

  for (int i = 0; i < bridges; ++i) {
    auto t = std::make_unique<CircuitTarget>();
    auto peer = MakeAdvertisableTarget();
    if (!peer) {
      std::cerr << "error: target-" << i << " amp start: " << peer.error().message << "\n";
      std::cout << "circuit_curve m=" << bridges << " ok=0 success_rate=0\n";
      return 1;
    }
    t->peer = std::move(*peer);
    t->advertise_ma = RewriteListenHost(t->peer->listen_ma, advertise_host);
    ArmProbeBridgeTarget(*t->peer, t->mu, t->got, t->payload);
    pumps.push_back(t->peer.get());
    targets.push_back(std::move(t));
  }

  int ok = 0;
  for (int i = 0; i < bridges; ++i) {
    pbr::CircuitBridgeTarget bridge_target;
    bridge_target.target_peer_id = targets[static_cast<size_t>(i)]->peer->peer_id;
    bridge_target.target_multiaddr = targets[static_cast<size_t>(i)]->advertise_ma;
    bridge_target.target_protocol = kProbeBridgeProtocol;

    AsyncWait<pbr::CircuitTunnelBridgeResult> wait;
    auto tunnel_id = circuit->StartBridge("hop", bridge_target, {}, {}, wait.Fn(), 10000);
    if (!tunnel_id || !wait.PumpUntilDone(pumps, 12000) || !wait.result || !wait.result->ok ||
        !wait.result->session) {
      std::cerr << "warn: bridge-" << i << " failed: "
                << (wait.result ? wait.result->error : wait.result.error().message) << "\n";
      continue;
    }

    const std::vector<uint8_t> payload = {'c', 'a', 'p', static_cast<uint8_t>('0' + (i % 10))};
    if (!wait.result->session->EnqueueOutbound(payload)) {
      std::cerr << "warn: bridge-" << i << " write failed\n";
      continue;
    }
    auto& tgt = *targets[static_cast<size_t>(i)];
    if (!PumpUntil(
            pumps,
            [&] {
              std::lock_guard lock(tgt.mu);
              return tgt.got;
            },
            8000)) {
      std::cerr << "warn: bridge-" << i << " payload not received\n";
      continue;
    }
    {
      std::lock_guard lock(tgt.mu);
      if (tgt.payload != payload) {
        std::cerr << "warn: bridge-" << i << " payload mismatch\n";
        continue;
      }
    }
    ++ok;
  }

  out_ok = ok;
  const double rate = 100.0 * ok / bridges;
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "circuit_curve m=" << bridges << " ok=" << ok << " success_rate=" << rate << "\n";
  std::cout.unsetf(std::ios::floatfield);

  circuit->Stop();
  (*client)->stack->Stop();
  for (auto& t : targets) {
    if (t && t->peer && t->peer->stack) {
      t->peer->stack->Stop();
    }
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
    std::cerr << "error: --hop Amp multiaddr with /p2p/<PeerId> required\n";
    PrintUsage(argv[0]);
    return 2;
  }
  if (hop_ma.find("/adp/") == std::string::npos) {
    std::cerr << "error: --hop must be an Amp ADP multiaddr (.../udp/.../adp/1.0.0/p2p/...)\n";
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
