// pp-node-load: many clients against one pp-node, for the stress lab (docs/ops/STRESS_LAB.md).
//
// Each simulated client is its own Amp peer (its own identity and UDP socket), as a phone is. Three kinds:
//   park     phones parked on the node: associate, reserve, renew every --renew seconds (the app's round)
//   calls    group calls on the node's media_relay: --calls calls of --members, each member sending audio
//            (40 ms frames) and video (--fps, --video-kbps, in ~1100 B packets) and receiving everyone else's
//   bridges  relayed 1:1 calls through the node: a parked target and a dialer bridged to it, media both ways
//
// One JSON line per second on stdout (what the clients see), a summary line at the end. The node's own view is on
// its /metrics (pp_service_load, pp_service_pressure, …); scripts/test/pp_node_stress.py reads both.

#include "amp/L1/Clock.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "amp/link/PeerId.h"
#include "amp/link/Types.h"
#include "crypto/PeerId.h"
#include "foundation/crypto/MlDsa.h"
#include "mesh/l4/circuit/client/CircuitClientCoordinator.h"
#include "mesh/l4/media_relay/MediaRelayTypes.h"
#include "mesh/l4/media_relay/client/MediaRelayClientCoordinator.h"
#include "mesh/l4/shared/MediaFrame.h"
#include "mesh/l4/shared/ProductChannelPolicies.h"
#include "mesh/types/ServiceLoad.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using pp::mesh::MeshService;
using pp::mesh::ServiceLoad;

constexpr const char* kLoadBridgeProtocol = "/test/node-probe-bridge/1.0.0";
constexpr uint16_t kAudioChannel = 0x00;
constexpr uint16_t kVideoChannel = 0x11;
constexpr size_t kVideoPacketBytes = 1100;
/** The header every media payload starts with: send time (µs on the steady clock, shared by processes on a host). */
constexpr size_t kStampBytes = 8;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

struct Options {
  std::string hop_ma;
  std::string mode = "park";
  int clients = 100;
  double ramp_per_s = 50;
  int duration_s = 120;
  int threads = 8;
  int renew_s = 20;
  /** Park clients re-knock a node that said full every round (an old app, or abuse), instead of skipping it. */
  bool ignore_full = false;
  int calls = 4;
  int members = 4;
  int audio_kbps = 32;
  int video_kbps = 500;
  int fps = 15;
  int pairs = 20;
  /** Appended to call ids, so several load processes do not join each other's calls. */
  std::string tag = "load";
};

// --- what the clients see ---------------------------------------------------------------------

/** Counters every client adds to (any thread); the reporter reads them once a second. */
struct Stats {
  std::atomic<uint64_t> started{0};
  std::atomic<uint64_t> linked{0};
  std::atomic<uint64_t> link_fail{0};
  /** Links made again after one was lost (a node restart, a dead path): the storm's size. */
  std::atomic<uint64_t> relinked{0};
  /** Rounds a client skipped because the node said full (as N033 clients do). */
  std::atomic<uint64_t> skipped_full{0};
  std::atomic<uint64_t> parked{0};
  std::atomic<uint64_t> reserve_ok{0};
  std::atomic<uint64_t> reserve_fail{0};
  std::atomic<uint64_t> attached{0};
  std::atomic<uint64_t> attach_fail{0};
  std::atomic<uint64_t> bridged{0};
  std::atomic<uint64_t> bridge_fail{0};
  std::atomic<uint64_t> tx_frames{0};
  std::atomic<uint64_t> tx_bytes{0};
  std::atomic<uint64_t> rx_frames{0};
  std::atomic<uint64_t> rx_bytes{0};
  /** Refusals by the level the node said with them (Unknown: none said). */
  std::array<std::atomic<uint64_t>, 5> refused_by_level{};
  /** The last level the node said for each service. */
  std::array<std::atomic<int>, pp::mesh::kMeshServiceCount> last_level{};

  std::mutex latency_mu;
  std::vector<double> latency_ms; // this second's receive latencies (a sample of at most kSecondSamples)
  /** The whole run's latencies, for the summary line: 0.1 ms buckets up to kRunHistogramMs, then one overflow. */
  static constexpr size_t kSecondSamples = 20000;
  static constexpr int kRunHistogramMs = 60000;
  std::vector<uint64_t> run_histogram = std::vector<uint64_t>(kRunHistogramMs * 10 + 1);
  double run_max_ms = 0;

  void NoteLatency(const double ms) {
    std::lock_guard lock(latency_mu);
    if (latency_ms.size() < kSecondSamples) {
      latency_ms.push_back(ms);
    }
    const auto bucket = static_cast<size_t>(std::clamp(ms * 10.0, 0.0, static_cast<double>(kRunHistogramMs * 10)));
    ++run_histogram[bucket];
    run_max_ms = std::max(run_max_ms, ms);
  }
  /** The run's p-th latency (ms, to 0.1 ms) and its sample count. */
  std::pair<double, uint64_t> RunPercentile(const double p) {
    std::lock_guard lock(latency_mu);
    uint64_t total = 0;
    for (const uint64_t n : run_histogram) {
      total += n;
    }
    if (total == 0) {
      return {0.0, 0};
    }
    const auto rank = static_cast<uint64_t>(p * static_cast<double>(total - 1));
    uint64_t seen = 0;
    for (size_t b = 0; b < run_histogram.size(); ++b) {
      seen += run_histogram[b];
      if (seen > rank) {
        return {static_cast<double>(b) / 10.0, total};
      }
    }
    return {static_cast<double>(kRunHistogramMs), total};
  }
  std::vector<double> TakeLatencies() {
    std::lock_guard lock(latency_mu);
    std::vector<double> out;
    out.swap(latency_ms);
    return out;
  }
};

Stats g_stats;

void NoteLevel(const pp::mesh::PeerLoadBook& book, const MeshService service) {
  const ServiceLoad level = book.Load("hop", service, pp::mesh::PeerLoadBook::NowMs());
  if (level != ServiceLoad::Unknown) {
    g_stats.last_level[static_cast<size_t>(service)].store(static_cast<int>(level));
  }
}

void NoteRefusal(const pp::mesh::PeerLoadBook& book, const MeshService service) {
  const ServiceLoad level = book.Load("hop", service, pp::mesh::PeerLoadBook::NowMs());
  g_stats.refused_by_level[static_cast<size_t>(level)].fetch_add(1);
  NoteLevel(book, service);
}

std::vector<uint8_t> StampedPayload(const size_t bytes) {
  std::vector<uint8_t> payload(std::max(bytes, kStampBytes));
  const int64_t now = NowUs();
  std::memcpy(payload.data(), &now, kStampBytes);
  return payload;
}

void NoteReceived(const std::vector<uint8_t>& payload) {
  g_stats.rx_frames.fetch_add(1);
  g_stats.rx_bytes.fetch_add(payload.size());
  if (payload.size() >= kStampBytes) {
    int64_t sent = 0;
    std::memcpy(&sent, payload.data(), kStampBytes);
    g_stats.NoteLatency(static_cast<double>(NowUs() - sent) / 1000.0);
  }
}

// --- one Amp peer -----------------------------------------------------------------------------

struct Peer {
  std::shared_ptr<pp::adp::WallClock> clock;
  std::unique_ptr<pp::amp::AmpStack> stack;
  std::string peer_id;
  pp::mesh::PeerLoadBook book;

  void Pump() {
    stack->Pump();
    stack->Tick();
  }
  pp::amp::PeerLinkManager& Links() { return stack->Links(); }
};

pbr::Roe<std::unique_ptr<Peer>> MakePeer(const bool accept_inbound) {
  auto keys = pbr::MlDsa::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  auto peer_id = pp::PeerIdFromMlDsaPublicKey(keys->public_key);
  if (!peer_id) {
    return peer_id.error();
  }
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(0, 0, 0, 0, 0));
  if (!bound) {
    return bound.error();
  }
  auto peer = std::make_unique<Peer>();
  peer->clock = std::make_shared<pp::adp::WallClock>();
  peer->peer_id = *peer_id;
  pp::amp::AmpStack::Config cfg;
  cfg.identity.ml_dsa_secret_key = std::move(keys->secret_key);
  cfg.identity.ml_dsa_public_key = std::move(keys->public_key);
  cfg.local_peer_id = peer->peer_id;
  cfg.link_config.peer_id_from_identity = pp::amp::PeerIdFromIdentityKey;
  std::shared_ptr<pp::adp::DatagramIo> io = std::move(*bound);
  auto stack = pp::amp::AmpStack::Create(std::move(io), peer->clock, std::move(cfg));
  if (!stack) {
    return stack.error();
  }
  peer->stack = std::move(*stack);
  peer->stack->Start();
  peer->stack->GetEndpoint().SetAcceptEnabled(accept_inbound);
  return peer;
}

// --- the clients ------------------------------------------------------------------------------

/**
 * One simulated client (or a pair): stepped and pumped by its worker thread only. A completion never starts the next
 * operation itself — the mesh clients call back under their own locks — it defers it to the next step.
 */
class Unit {
public:
  virtual ~Unit() = default;
  /** Run what completions deferred, then advance: start what is due, send what is due. */
  void Advance(const Clock::time_point now) {
    std::vector<std::function<void()>> due;
    due.swap(deferred_);
    for (auto& fn : due) {
      fn();
    }
    Step(now);
  }
  virtual void Pump() = 0;

protected:
  virtual void Step(Clock::time_point now) = 0;
  /** From a completion: run `fn` at the next step, outside the client's callback. */
  void Later(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }

private:
  std::vector<std::function<void()>> deferred_;
};

/** Associates `peer` with the hop and calls `then(ok)` (on the worker's thread, from a Pump). */
void Associate(Peer& peer, const std::string& hop_ma, std::function<void(bool)> then) {
  if (auto reg = peer.Links().RegisterEndpoint("hop", hop_ma); !reg) {
    then(false);
    return;
  }
  peer.Links().EnsureAssociation(
      "hop", [then = std::move(then)](pp::amp::PeerLinkManager::LinkRoe assoc) { then(static_cast<bool>(assoc)); });
}

/**
 * A phone parked on the node, as the app's stay-reachable loop does it: reserve, then renew every --renew seconds
 * (the app's 20). A refusal is retried at the next round. A link that failed or died (a node restart) is dialed again
 * at the next round, as the app's next renew does — so a restart's clients come back spread over one round.
 */
/** How long a park's reservation is held: the app's (pp-cpp-mesh CircuitRendezvousCoordinator, 30 s). */
constexpr int kParkHoldMs = 30000;

class ParkUnit final : public Unit {
public:
  ParkUnit(std::unique_ptr<Peer> peer, const Options& opts) : peer_(std::move(peer)), opts_(opts) {
    circuit_ = std::make_unique<pp::mesh::CircuitClientCoordinator>(peer_->stack->Runtime());
    circuit_->SetPeerLoadBook(&peer_->book);
    circuit_->Start();
    Link();
  }
  ~ParkUnit() override {
    circuit_->Stop();
    peer_->stack->Stop();
  }

  void Step(const Clock::time_point now) override {
    if (in_flight_ || now < next_at_) {
      return;
    }
    if (!linked_) {
      Link();
      return;
    }
    // As the app's park does (N033): a relay that said full or draining, and still fresh, is skipped for the round —
    // unless this client holds a reservation there, whose renewal it takes.
    if (!opts_.ignore_full && !parked_ &&
        peer_->book.RefusesNew("hop", MeshService::Park, pp::mesh::PeerLoadBook::NowMs())) {
      g_stats.skipped_full.fetch_add(1);
      next_at_ = now + std::chrono::seconds(opts_.renew_s);
      return;
    }
    in_flight_ = true;
    circuit_->StartReserve(
        "hop",
        [this](pp::mesh::CircuitClientCoordinator::BridgeRoe result) {
          in_flight_ = false;
          next_at_ = Clock::now() + std::chrono::seconds(opts_.renew_s);
          if (result) {
            g_stats.reserve_ok.fetch_add(1);
            NoteLevel(peer_->book, MeshService::Park);
            if (!parked_) {
              parked_ = true;
              g_stats.parked.fetch_add(1);
            }
            return;
          }
          g_stats.reserve_fail.fetch_add(1);
          NoteRefusal(peer_->book, MeshService::Park);
          if (parked_) {
            parked_ = false;
            g_stats.parked.fetch_sub(1);
          }
          if (!peer_->Links().IsConnected("hop")) {
            linked_ = false; // the node went away: dial it again next round
          }
        },
        // The app's hold (pp-cpp-mesh CircuitRendezvousCoordinator's kParkReserveHoldMs, 30 s): longer than a round,
        // so a renewal lands before the last reservation lapses. 15 s under a 20 s round left every client unparked a
        // quarter of the time.
        kParkHoldMs);
  }
  void Pump() override { peer_->Pump(); }

private:
  void Link() {
    in_flight_ = true;
    Associate(*peer_, opts_.hop_ma, [this](const bool ok) {
      in_flight_ = false;
      if (ok) {
        g_stats.linked.fetch_add(1);
        if (ever_linked_) {
          g_stats.relinked.fetch_add(1);
        }
        ever_linked_ = true;
        linked_ = true;
        next_at_ = Clock::now();
        return;
      }
      g_stats.link_fail.fetch_add(1);
      next_at_ = Clock::now() + std::chrono::seconds(opts_.renew_s);
    });
  }

  std::unique_ptr<Peer> peer_;
  const Options& opts_;
  std::unique_ptr<pp::mesh::CircuitClientCoordinator> circuit_;
  bool linked_ = false;
  bool ever_linked_ = false;
  bool in_flight_ = false;
  bool parked_ = false;
  Clock::time_point next_at_ = Clock::time_point::max();
};

/** Sends audio every 40 ms and video at --fps, both stamped, through `send` (one call member, or a bridge end). */
class MediaPacer {
public:
  explicit MediaPacer(const Options& opts) : opts_(opts) {}

  template <typename Send>
  void Due(const Clock::time_point now, Send&& send) {
    if (now >= next_audio_) {
      next_audio_ = now + std::chrono::milliseconds(40);
      send(kAudioChannel, StampedPayload(static_cast<size_t>(opts_.audio_kbps) * 1000 / 8 * 40 / 1000), false);
    }
    if (opts_.video_kbps > 0 && opts_.fps > 0 && now >= next_video_) {
      next_video_ = now + std::chrono::microseconds(1'000'000 / opts_.fps);
      const size_t frame = static_cast<size_t>(opts_.video_kbps) * 1000 / 8 / static_cast<size_t>(opts_.fps);
      const bool key = frames_++ % static_cast<uint64_t>(opts_.fps * 2) == 0;
      for (size_t sent = 0; sent < frame; sent += kVideoPacketBytes) {
        send(kVideoChannel, StampedPayload(std::min(kVideoPacketBytes, frame - sent)), key);
      }
    }
  }

private:
  const Options& opts_;
  Clock::time_point next_audio_{};
  Clock::time_point next_video_{};
  uint64_t frames_ = 0;
};

/** One member of a group call on the node's media_relay. */
class CallMemberUnit final : public Unit {
public:
  CallMemberUnit(std::unique_ptr<Peer> peer, const Options& opts, std::string call_id, const int index)
      : peer_(std::move(peer)), opts_(opts), call_id_(std::move(call_id)), index_(index), pacer_(opts) {
    relay_ = std::make_unique<pp::mesh::MediaRelayClientCoordinator>(peer_->stack->Runtime());
    relay_->SetPeerLoadBook(&peer_->book);
    relay_->Start();
    Associate(*peer_, opts_.hop_ma, [this](const bool ok) {
      (ok ? g_stats.linked : g_stats.link_fail).fetch_add(1);
      if (ok) {
        Later([this] { Quote(); });
      }
    });
  }
  ~CallMemberUnit() override {
    relay_->Stop();
    peer_->stack->Stop();
  }

  void Step(const Clock::time_point now) override {
    if (!attached_) {
      return;
    }
    pacer_.Due(now, [this](const uint16_t channel, std::vector<uint8_t> payload, const bool key) {
      pp::mesh::MediaDataFrame frame;
      frame.stream_id = static_cast<uint32_t>(index_ + 1);
      frame.channel_id = channel;
      frame.channel_type =
          channel == kAudioChannel ? pp::mesh::MediaChannelType::Forward : pp::mesh::MediaChannelType::LatestLossy;
      frame.seq = ++seq_;
      frame.mark = key ? 1 : 0;
      const size_t bytes = payload.size();
      frame.payload = std::move(payload);
      if (relay_->SendFrame(frame)) {
        g_stats.tx_frames.fetch_add(1);
        g_stats.tx_bytes.fetch_add(bytes);
      }
    });
  }
  void Pump() override { peer_->Pump(); }

private:
  void Quote() {
    pp::mesh::MediaRelayQuoteRequest req;
    req.session_id = call_id_;
    req.participants = opts_.members;
    relay_->StartQuote(
        "hop", req,
        [this](pp::mesh::MediaRelayRoe<pp::mesh::MediaRelayQuote> quote) {
          if (!quote || !quote->ok) {
            g_stats.attach_fail.fetch_add(1);
            NoteRefusal(peer_->book, MeshService::MediaRelay);
            return;
          }
          Later([this, id = quote->quote_id] { Attach(id); });
        },
        10000);
  }

  void Attach(const std::string& quote_id) {
    relay_->StartAttach(
        "hop", quote_id, call_id_, call_id_, [](pp::mesh::MediaDataFrame frame) { NoteReceived(frame.payload); },
        [this](pp::mesh::MediaRelayRoe<pp::mesh::MediaRelayAttachResult> attach) {
          if (!attach || !attach->ok) {
            g_stats.attach_fail.fetch_add(1);
            NoteRefusal(peer_->book, MeshService::MediaRelay);
            return;
          }
          g_stats.attached.fetch_add(1);
          NoteLevel(peer_->book, MeshService::MediaRelay);
          Later([this] { Subscribe(); });
        },
        10000);
  }

  void Subscribe() {
    relay_->StartClientFrameReader();
    for (int other = 0; other < opts_.members; ++other) {
      if (other != index_) {
        (void)relay_->Subscribe(static_cast<uint32_t>(other + 1), kAudioChannel);
        (void)relay_->Subscribe(static_cast<uint32_t>(other + 1), kVideoChannel);
      }
    }
    attached_ = true;
  }

  std::unique_ptr<Peer> peer_;
  const Options& opts_;
  std::string call_id_;
  int index_ = 0;
  MediaPacer pacer_;
  std::unique_ptr<pp::mesh::MediaRelayClientCoordinator> relay_;
  bool attached_ = false;
  uint32_t seq_ = 0;
};

/**
 * A relayed 1:1 call through the node: the target parks there, the dialer bridges to it by PeerId (the node's
 * ServeDial), then both send media over the bridge.
 */
class BridgePairUnit final : public Unit {
public:
  BridgePairUnit(std::unique_ptr<Peer> target, std::unique_ptr<Peer> dialer, const Options& opts)
      : target_(std::move(target)), dialer_(std::move(dialer)), opts_(opts), target_pacer_(opts), dialer_pacer_(opts) {
    target_circuit_ = std::make_unique<pp::mesh::CircuitClientCoordinator>(target_->stack->Runtime());
    target_circuit_->SetPeerLoadBook(&target_->book);
    target_circuit_->Start();
    dialer_circuit_ = std::make_unique<pp::mesh::CircuitClientCoordinator>(dialer_->stack->Runtime());
    dialer_circuit_->SetPeerLoadBook(&dialer_->book);
    dialer_circuit_->Start();
    ArmTarget();
    Associate(*target_, opts_.hop_ma, [this](const bool ok) {
      (ok ? g_stats.linked : g_stats.link_fail).fetch_add(1);
      if (ok) {
        Later([this] { ParkTarget(); });
      }
    });
  }
  ~BridgePairUnit() override {
    dialer_circuit_->Stop();
    target_circuit_->Stop();
    dialer_->stack->Stop();
    target_->stack->Stop();
  }

  void Step(const Clock::time_point now) override {
    if (dialer_session_) {
      dialer_pacer_.Due(now, [this](uint16_t, std::vector<uint8_t> payload, bool) { Send(*dialer_session_, payload); });
    }
    if (target_session_) {
      target_pacer_.Due(now, [this](uint16_t, std::vector<uint8_t> payload, bool) { Send(*target_session_, payload); });
    }
  }
  void Pump() override {
    target_->Pump();
    dialer_->Pump();
  }

private:
  static void Send(pp::amp::ChannelSession& session, const std::vector<uint8_t>& payload) {
    if (session.EnqueueOutbound(payload)) {
      g_stats.tx_frames.fetch_add(1);
      g_stats.tx_bytes.fetch_add(payload.size());
    }
  }

  void ArmTarget() {
    target_->Links().SetProtocolHandler(
        kLoadBridgeProtocol, [this](pp::amp::LinkHandle handle, const std::string&, const uint32_t channel_id) {
          target_->Links().WithLiveLink(handle, [&](pp::amp::PeerLink& link) {
            if (!link.Mux()) {
              return;
            }
            auto session = std::make_shared<pp::amp::ChannelSession>();
            session->Bind(*link.Mux(), channel_id, pp::amp::CircuitTunnelChannelPolicy(),
                          [](pbr::Roe<std::vector<uint8_t>> frame) {
                            if (frame) {
                              NoteReceived(*frame);
                            }
                            return static_cast<bool>(frame);
                          });
            target_session_ = std::move(session);
          });
        });
  }

  void ParkTarget() {
    target_circuit_->StartReserve(
        "hop",
        [this](pp::mesh::CircuitClientCoordinator::BridgeRoe result) {
          if (!result) {
            g_stats.reserve_fail.fetch_add(1);
            NoteRefusal(target_->book, MeshService::Park);
            return;
          }
          g_stats.reserve_ok.fetch_add(1);
          Later([this] {
            Associate(*dialer_, opts_.hop_ma, [this](const bool ok) {
              (ok ? g_stats.linked : g_stats.link_fail).fetch_add(1);
              if (ok) {
                Later([this] { Bridge(); });
              }
            });
          });
        },
        15000);
  }

  void Bridge() {
    pp::mesh::CircuitBridgeTarget target;
    target.target_peer_id = target_->peer_id;
    target.target_protocol = kLoadBridgeProtocol;
    dialer_circuit_->StartBridge(
        "hop", target,
        [](pbr::Roe<std::vector<uint8_t>> frame) {
          if (frame) {
            NoteReceived(*frame);
          }
          return static_cast<bool>(frame);
        },
        {},
        [this](pp::mesh::CircuitClientCoordinator::BridgeRoe result) {
          if (!result || !result->session) {
            g_stats.bridge_fail.fetch_add(1);
            NoteRefusal(dialer_->book, MeshService::Relay);
            return;
          }
          g_stats.bridged.fetch_add(1);
          NoteLevel(dialer_->book, MeshService::Relay);
          dialer_session_ = result->session;
        },
        15000);
  }

  std::unique_ptr<Peer> target_;
  std::unique_ptr<Peer> dialer_;
  const Options& opts_;
  MediaPacer target_pacer_;
  MediaPacer dialer_pacer_;
  std::unique_ptr<pp::mesh::CircuitClientCoordinator> target_circuit_;
  std::unique_ptr<pp::mesh::CircuitClientCoordinator> dialer_circuit_;
  std::shared_ptr<pp::amp::ChannelSession> target_session_;
  std::shared_ptr<pp::amp::ChannelSession> dialer_session_;
};

// --- running them -----------------------------------------------------------------------------

/** Makes unit `index` of the run (a phone, a call member, or a bridge pair); null when its peers would not start. */
std::unique_ptr<Unit> MakeUnit(const Options& opts, const int index) {
  auto peer = MakePeer(opts.mode == "bridges");
  if (!peer) {
    std::cerr << "peer: " << peer.error().message << "\n";
    return nullptr;
  }
  if (opts.mode == "park") {
    return std::make_unique<ParkUnit>(std::move(*peer), opts);
  }
  if (opts.mode == "calls") {
    const std::string call_id = "call:" + opts.tag + "-" + std::to_string(index / opts.members);
    return std::make_unique<CallMemberUnit>(std::move(*peer), opts, call_id, index % opts.members);
  }
  auto dialer = MakePeer(false);
  if (!dialer) {
    std::cerr << "peer: " << dialer.error().message << "\n";
    return nullptr;
  }
  return std::make_unique<BridgePairUnit>(std::move(*peer), std::move(*dialer), opts);
}

int UnitCount(const Options& opts) {
  if (opts.mode == "calls") {
    return opts.calls * opts.members;
  }
  if (opts.mode == "bridges") {
    return opts.pairs;
  }
  return opts.clients;
}

/**
 * Worker `w` of `workers`: makes its share of the units as the ramp allows (unit i starts at i / ramp seconds), then
 * steps and pumps them until `stop`.
 */
void RunWorker(const Options& opts, const int w, const int workers, const Clock::time_point start,
               const std::atomic<bool>& stop) {
  std::vector<std::unique_ptr<Unit>> units;
  const int total = UnitCount(opts);
  int next = w;
  while (!stop.load()) {
    const Clock::time_point now = Clock::now();
    while (next < total && now - start >= std::chrono::duration<double>(next / opts.ramp_per_s)) {
      if (auto unit = MakeUnit(opts, next)) {
        units.push_back(std::move(unit));
        g_stats.started.fetch_add(1);
      }
      next += workers;
    }
    for (auto& unit : units) {
      unit->Advance(now);
      unit->Pump();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  units.clear();
}

std::string LevelName(const int level) {
  const char* word = pp::mesh::ServiceLoadWire(static_cast<ServiceLoad>(level));
  return *word ? word : "unknown";
}

/**
 * One JSON line: what the clients saw in the last second (and totals). The summary's latencies are the whole run's,
 * not its last partial second's.
 */
void Report(const double elapsed_s, const bool summary) {
  std::vector<double> lat = g_stats.TakeLatencies();
  std::sort(lat.begin(), lat.end());
  const auto second_pct = [&lat](const double p) {
    return lat.empty() ? 0.0 : lat[static_cast<size_t>(p * static_cast<double>(lat.size() - 1))];
  };
  const auto pct = [&](const double p) { return summary ? g_stats.RunPercentile(p).first : second_pct(p); };
  const uint64_t lat_n = summary ? g_stats.RunPercentile(0.5).second : lat.size();
  double lat_max = lat.empty() ? 0.0 : lat.back();
  if (summary) {
    std::lock_guard lock(g_stats.latency_mu);
    lat_max = g_stats.run_max_ms;
  }
  std::ostringstream out;
  out << "{\"t\":" << static_cast<int64_t>(elapsed_s) << (summary ? ",\"summary\":true" : "")
      << ",\"started\":" << g_stats.started << ",\"linked\":" << g_stats.linked
      << ",\"link_fail\":" << g_stats.link_fail << ",\"relinked\":" << g_stats.relinked
      << ",\"skipped_full\":" << g_stats.skipped_full << ",\"parked\":" << g_stats.parked
      << ",\"reserve_ok\":" << g_stats.reserve_ok << ",\"reserve_fail\":" << g_stats.reserve_fail
      << ",\"attached\":" << g_stats.attached << ",\"attach_fail\":" << g_stats.attach_fail
      << ",\"bridged\":" << g_stats.bridged << ",\"bridge_fail\":" << g_stats.bridge_fail
      << ",\"tx_frames\":" << g_stats.tx_frames << ",\"tx_bytes\":" << g_stats.tx_bytes
      << ",\"rx_frames\":" << g_stats.rx_frames << ",\"rx_bytes\":" << g_stats.rx_bytes
      << ",\"lat_p50_ms\":" << pct(0.5) << ",\"lat_p95_ms\":" << pct(0.95) << ",\"lat_p99_ms\":" << pct(0.99)
      << ",\"lat_max_ms\":" << lat_max << ",\"lat_n\":" << lat_n << ",\"refused\":{";
  for (int level = 0; level < 5; ++level) {
    out << (level ? "," : "") << "\"" << LevelName(level) << "\":" << g_stats.refused_by_level[level];
  }
  out << "},\"level\":{";
  for (size_t s = 0; s < pp::mesh::kMeshServiceCount; ++s) {
    out << (s ? "," : "") << "\"" << pp::mesh::MeshServiceName(static_cast<MeshService>(s)) << "\":\""
        << LevelName(g_stats.last_level[s].load()) << "\"";
  }
  out << "}}";
  std::cout << out.str() << std::endl;
}

void PrintUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " --hop <adp-multiaddr-with-p2p> [--mode park|calls|bridges] [--clients N] [--ramp N/s]\n"
               "       [--duration S] [--threads T] [--renew S] [--calls K --members M] [--pairs P]\n"
               "       [--audio-kbps A] [--video-kbps V] [--fps F] [--tag T]\n"
               "Many clients against one pp-node (docs/ops/STRESS_LAB.md). One JSON line a second.\n";
}

bool ParseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--hop") {
      o.hop_ma = next();
    } else if (a == "--mode") {
      o.mode = next();
    } else if (a == "--clients") {
      o.clients = std::atoi(next().c_str());
    } else if (a == "--ramp") {
      o.ramp_per_s = std::atof(next().c_str());
    } else if (a == "--duration") {
      o.duration_s = std::atoi(next().c_str());
    } else if (a == "--threads") {
      o.threads = std::max(1, std::atoi(next().c_str()));
    } else if (a == "--renew") {
      o.renew_s = std::max(1, std::atoi(next().c_str()));
    } else if (a == "--calls") {
      o.calls = std::atoi(next().c_str());
    } else if (a == "--members") {
      o.members = std::max(2, std::atoi(next().c_str()));
    } else if (a == "--pairs") {
      o.pairs = std::atoi(next().c_str());
    } else if (a == "--audio-kbps") {
      o.audio_kbps = std::atoi(next().c_str());
    } else if (a == "--video-kbps") {
      o.video_kbps = std::atoi(next().c_str());
    } else if (a == "--fps") {
      o.fps = std::atoi(next().c_str());
    } else if (a == "--ignore-full") {
      o.ignore_full = true;
    } else if (a == "--tag") {
      o.tag = next();
    } else {
      return false;
    }
  }
  return !o.hop_ma.empty() && o.ramp_per_s > 0 && (o.mode == "park" || o.mode == "calls" || o.mode == "bridges");
}

} // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseArgs(argc, argv, opts)) {
    PrintUsage(argv[0]);
    return 2;
  }
  std::atomic<bool> stop{false};
  const Clock::time_point start = Clock::now();
  std::vector<std::thread> workers;
  for (int w = 0; w < opts.threads; ++w) {
    workers.emplace_back([&opts, w, start, &stop] { RunWorker(opts, w, opts.threads, start, stop); });
  }
  const auto end = start + std::chrono::seconds(opts.duration_s);
  for (auto tick = start + std::chrono::seconds(1); tick <= end; tick += std::chrono::seconds(1)) {
    std::this_thread::sleep_until(tick);
    Report(std::chrono::duration<double>(tick - start).count(), false);
  }
  Report(std::chrono::duration<double>(Clock::now() - start).count(), true);
  stop.store(true);
  for (std::thread& t : workers) {
    t.join();
  }
  return 0;
}
