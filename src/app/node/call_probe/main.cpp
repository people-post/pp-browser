#include "amp/L1/Clock.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "foundation/crypto/MlDsa.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "amp/link/Types.h"
#include "common/chat/RelayEnvelope.h"
#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/call_media/CallMediaLegCoordinator.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/mesh/reachability/AmpPunchCoordinator.h"
#include "feature/calls/AmpCircuitHopReach.h"
#include "foundation/identity/PeerIdUtil.h"
#include "feature/conversations/AmpDirectChatTransport.h"
#include "common/chat/IDirectMessageClient.h"

#include "common/Logger.h"

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --role answerer --listen <adp-ma|/ip4/0.0.0.0/udp/PORT/adp/1.0.0>\n"
      << "                 [--advertise-host <ip>] [--call-id ID] [--ready-file PATH]\n"
      << "                 [--warm-hop <adp-ma-with-p2p>] [--min-rx-frames N]\n"
      << "                 [--no-auto-detach] [--with-chat]\n"
      << "  " << argv0 << " --role offerer --peer <adp-ma-with-p2p> [--call-id ID] [--cycles K]\n"
      << "                 [--via-hop <adp-ma-with-p2p>] [--peer-id-only]\n"
      << "                 [--hold-ms N] [--timeout-ms N]\n"
      << "                 [--expect ok|busy] [--with-chat]\n"
      << "\n"
      << "Amp thin-client B-CALL-DIRECT / B-CALL-HOP / B-CONFLICT / B-MSG+CALL\n"
      << "(docs/ops/TEST_STRATEGY.md).\n"
      << "  Listen/peer example: /ip4/127.0.0.1/udp/47100/adp/1.0.0[/p2p/<PeerId>]\n"
      << "  --expect busy   Connect failure is success (second inbound while MediaReady).\n"
      << "  --hold-ms N     Stay MediaReady after audio before detach (conflict holder).\n"
      << "  --with-chat     AmpDirectChatTransport ping during and after the call.\n"
      << "                  With --via-hop, chat rides the nested Amp circuit link.\n"
      << "  --warm-hop     Answerer dials hop first (NAT return-path / hop peer-book).\n"
      << "  --peer-id-only Offerer StartBridge omits target multiaddr (hop book only).\n"
      << "  --min-rx-frames Answerer fails if fewer audio frames received (duplex gate).\n"
      << "  --reach product  Offerer: punch→circuit via AmpCircuitHopReach (no StartBridge shortcut).\n"
      << "                  Requires --via-hop as seed/introducer; do not register private peer MA.\n";
}

std::optional<std::string> PeerIdFromMultiaddr(const std::string& ma) {
  const auto pos = ma.rfind("/p2p/");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  auto id = ma.substr(pos + 5);
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == '/')) {
    id.pop_back();
  }
  if (id.empty()) {
    return std::nullopt;
  }
  return id;
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

bool HasP2pSuffix(const std::string& ma) {
  return ma.find("/p2p/") != std::string::npos;
}

std::string RewriteWildcardListenHost(std::string multiaddr) {
  const std::string from4 = "/ip4/0.0.0.0/";
  const std::string to4 = "/ip4/127.0.0.1/";
  auto pos = multiaddr.find(from4);
  if (pos != std::string::npos) {
    multiaddr.replace(pos, from4.size(), to4);
    return multiaddr;
  }
  // Dual-stack Amp bind uses `/ip6/::/`; rewrite to loopback for local dial/register.
  const std::string from6 = "/ip6/::/";
  const std::string to6 = "/ip6/::1/";
  pos = multiaddr.find(from6);
  if (pos != std::string::npos) {
    multiaddr.replace(pos, from6.size(), to6);
  }
  return multiaddr;
}

/** Parse `/ip4|ip6/H/udp/P/adp/1.0.0` with optional `/p2p/...` for answerer bind. */
std::optional<pp::adp::IpEndpoint> ParseListenEndpoint(const std::string& ma) {
  if (auto full = pp::amp::ParseAdpMultiaddr(ma)) {
    return full->endpoint;
  }
  // Without /p2p/: append a dummy peer so amp ParseAdpMultiaddr accepts /ip4|/ip6/...
  if (ma.find("/p2p/") == std::string::npos && ma.find("/adp/") != std::string::npos) {
    if (auto full = pp::amp::ParseAdpMultiaddr(ma + "/p2p/_")) {
      return full->endpoint;
    }
  }
  return std::nullopt;
}

pp::amp::PeerLinkConfig MakeProbeLinkConfig() {
  pp::amp::PeerLinkConfig config;
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
  std::shared_ptr<pp::adp::WallClock> clock;
  std::unique_ptr<pp::amp::AmpStack> stack;
  std::string peer_id;
  std::string listen_ma;

  void Pump() {
    if (stack) {
      stack->Pump();
      stack->Tick();
    }
  }

  pp::amp::MeshRuntime& Runtime() { return stack->Runtime(); }
  pp::amp::PeerLinkManager& Links() { return stack->Links(); }
};

template <typename Pred>
bool PumpUntil(AmpPeer& peer, Pred&& done, const int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) {
      return true;
    }
    peer.Pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

pbr::Roe<std::unique_ptr<AmpPeer>> MakeAmpPeer(const pp::adp::IpEndpoint& bind_ep,
                                               const bool accept_inbound) {
  auto keys = pbr::MlDsa::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  auto peer_id = pbr::PeerIdFromMlDsaPublicKey(keys->public_key);
  if (!peer_id) {
    return peer_id.error();
  }

  auto bound = pp::adp::OsUdpDatagramIo::Bind(bind_ep);
  if (!bound) {
    return bound.error();
  }

  auto peer = std::make_unique<AmpPeer>();
  peer->clock = std::make_shared<pp::adp::WallClock>();
  peer->peer_id = *peer_id;

  pp::amp::MshIdentity identity;
  identity.ml_dsa_secret_key = std::move(keys->secret_key);
  identity.ml_dsa_public_key = std::move(keys->public_key);

  pp::amp::AmpStack::Config cfg;
  cfg.identity = std::move(identity);
  cfg.local_peer_id = peer->peer_id;
  cfg.link_config = MakeProbeLinkConfig();

  std::shared_ptr<pp::adp::DatagramIo> io = std::move(*bound);
  auto stack = pp::amp::AmpStack::Create(std::move(io), peer->clock, std::move(cfg));
  if (!stack) {
    return stack.error();
  }
  peer->stack = std::move(*stack);
  peer->stack->Start();
  peer->stack->GetEndpoint().SetAcceptEnabled(accept_inbound);

  auto listen = pp::amp::FormatAdpMultiaddr(peer->stack->LocalEndpoint(), peer->peer_id);
  if (!listen) {
    return listen.error();
  }
  peer->listen_ma = *listen;
  peer->Links().SetLocalListenMultiaddrs({peer->listen_ma});
  peer->Links().EnableNestedCarrierAccept(true);
  return peer;
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

  pp::amp::PeerLinkManager::LinkCb LinkFn() {
    return [this](pp::amp::PeerLinkManager::LinkRoe r) {
      if (r) {
        result = pbr::Roe<Result>();
      } else {
        result = pbr::Error(r.error().message);
      }
      done.store(true, std::memory_order_release);
    };
  }

  bool PumpUntilDone(AmpPeer& peer, const int timeout_ms = 10000) {
    return PumpUntil(peer, [this] { return done.load(std::memory_order_acquire); }, timeout_ms);
  }
};

pbr::RelayEnvelope MakeProbeChatEnvelope(const int cycle) {
  pbr::RelayEnvelope env;
  env.message_id = "pp-call-probe-chat-" + std::to_string(cycle);
  env.sender_relay_id = "probe";
  env.sender_contact_id = "probe";
  env.body.e2e.payload_b64 = "cGluZw=="; // "ping"
  env.sender_seq = static_cast<uint64_t>(cycle + 1);
  env.order_key = env.sender_seq;
  env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  return env;
}

bool SendProbeChat(pbr::AmpDirectChatTransport& chat, const std::string& peer_key, const int cycle) {
  auto sent = chat.SendEnvelope(peer_key, MakeProbeChatEnvelope(cycle));
  if (!sent) {
    std::cerr << "error: chat send: " << sent.error().message << "\n";
    return false;
  }
  return true;
}

pbr::Roe<void> EstablishNestedViaHop(AmpPeer& peer, pbr::CircuitTunnelCoordinator& circuit,
                                     pbr::AmpCircuitHopRegistry& hops, const std::string& hop_key,
                                     const std::string& peer_id, const std::string& peer_ma) {
  pbr::CircuitBridgeTarget target;
  target.target_peer_id = peer_id;
  // Empty multiaddr = peer-id-only: hop must already know a dialable path (warm-hop / book).
  target.target_multiaddr = peer_ma;
  target.target_protocol = pp::amp::kAmpCircuitCarrierProtocolId;

  AsyncWait<pbr::CircuitTunnelBridgeResult> bridge_wait;
  auto tunnel_id = circuit.StartBridge(hop_key, target, {}, {}, bridge_wait.Fn(), 10000);
  if (!tunnel_id) {
    return pbr::Error("start bridge failed");
  }
  if (!bridge_wait.PumpUntilDone(peer, 12000) || !bridge_wait.result) {
    return bridge_wait.result ? pbr::Error(bridge_wait.result->error) : bridge_wait.result.error();
  }
  if (!bridge_wait.result->ok || !bridge_wait.result->session) {
    return pbr::Error(bridge_wait.result->error.empty() ? "bridge refused" : bridge_wait.result->error);
  }

  AsyncWait<void> nested_wait;
  peer.Links().EstablishNestedOverCarrier(peer_id, bridge_wait.result->session, true, nested_wait.LinkFn());
  if (!nested_wait.PumpUntilDone(peer, 12000) || !nested_wait.result) {
    return nested_wait.result ? pbr::Error("nested failed") : nested_wait.result.error();
  }
  if (!peer.Links().IsConnected(peer_id)) {
    return pbr::Error("nested link not connected");
  }
  (void)hops.Install(peer_id, hop_key, pp::amp::kAmpCircuitCarrierProtocolId, bridge_wait.result->session,
                     tunnel_id);
  return {};
}


std::unique_ptr<pbr::AmpPunchCoordinator> StartProbePunch(AmpPeer& peer,
                                                          const std::vector<std::string>& candidates) {
  auto punch = std::make_unique<pbr::AmpPunchCoordinator>(
      peer.Links(), [&peer]() { peer.Pump(); }, pbr::AmpPunchCoordinator::WorkerPost{},
      pbr::AmpPunchCoordinator::IoPost{});
  punch->SetLocalCandidateAddrs(candidates);
  punch->Start();
  return punch;
}

pbr::Roe<void> WarmHopAssociation(AmpPeer& peer, const std::string& hop_key, const std::string& hop_ma) {
  if (auto reg = peer.Links().RegisterEndpoint(hop_key, RewriteWildcardListenHost(hop_ma)); !reg) {
    return pbr::Error(reg.error().message);
  }
  AsyncWait<void> warm_wait;
  peer.Links().EnsureAssociation(hop_key, warm_wait.LinkFn());
  if (!warm_wait.PumpUntilDone(peer, 15000) || !warm_wait.result) {
    return warm_wait.result ? pbr::Error("warm-hop associate failed") : warm_wait.result.error();
  }
  for (int i = 0; i < 50; ++i) {
    peer.Pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return {};
}

/** Product NAT path: punch (via hop introducer) → circuit nested Session — mirrors CallStack Ensure. */
pbr::Roe<void> EnsureProductCallMediaReach(AmpPeer& peer, pbr::CircuitTunnelCoordinator& circuit,
                                           pbr::AmpCircuitHopRegistry& hops, pbr::IChatPeerLinks& links,
                                           pbr::AmpPunchCoordinator& punch, const std::string& hop_peer_id,
                                           const std::string& hop_ma, const std::string& target_peer_id,
                                           const int timeout_ms) {
  // Drive punch at the top-level PumpUntilDone — AmpCircuitHopReach's punch callback nests
  // AmpParkUntil/AmpScheduleUntilSettled inside channel handlers and can drop the hop assoc
  // under dual-NAT (hard-w5 Phase-2).
  if (!hop_peer_id.empty()) {
    AsyncWait<void> punch_wait;
    auto punch_done = punch_wait.Fn();
    punch.TryColdPunchAsync(
        hop_peer_id, target_peer_id, punch.LocalCandidateAddrs(),
        [punch_done](pbr::AmpPunchCoordinator::PunchRoe punched) mutable {
          if (!punched) {
            punch_done(pbr::Error(punched.error().message));
            return;
          }
          if (!punched->ok) {
            punch_done(pbr::Error(punched->error.empty() ? "punch failed" : punched->error));
            return;
          }
          punch_done({});
        },
        2000);
    if (!punch_wait.PumpUntilDone(peer, 12000) || !punch_wait.result) {
      const std::string err = punch_wait.done.load(std::memory_order_acquire)
                                  ? punch_wait.result.error().message
                                  : "punch timed out";
      std::cerr << "pp-call-probe punch miss target=" << target_peer_id << " err=" << err
                << " (fall through to circuit)\n";
    } else {
      std::cout << "ok  punch connected target=" << target_peer_id << "\n";
    }
  }

  if (peer.Links().IsConnected(target_peer_id)) {
    return {};
  }

  // Punch sync registers private advertise MAs; re-warm hop so circuit StartBridge still has a live assoc.
  if (auto warm = WarmHopAssociation(peer, hop_peer_id, hop_ma); !warm) {
    return pbr::Error(std::string("re-warm hop after punch: ") + warm.error().message);
  }
  (void)peer.Links().RegisterEndpoint("hop", RewriteWildcardListenHost(hop_ma));

  auto io_pump = [&peer]() { peer.Pump(); };
  pbr::AmpCircuitHopReach reach(
      circuit, hops, links, io_pump,
      [hop_peer_id](const std::string& exclude) {
        std::vector<std::string> out;
        if (!hop_peer_id.empty() && hop_peer_id != exclude) {
          out.push_back(hop_peer_id);
        }
        return out;
      },
      // Punch already attempted above; circuit-only Ensure (peer-id-only nested).
      pbr::AmpCircuitHopReach::TryPunchAsync{},
      [&punch](const std::string& intro, const std::string& target,
               std::function<void(pbr::Roe<void>)> on_done) {
        punch.TryUpgradePunchAsync(
            intro, target, punch.LocalCandidateAddrs(),
            [on_done = std::move(on_done)](pbr::AmpPunchCoordinator::PunchRoe punched) mutable {
              if (!punched) {
                on_done(pbr::Error(punched.error().message));
                return;
              }
              if (!punched->ok) {
                on_done(pbr::Error(punched->error.empty() ? "upgrade punch failed" : punched->error));
                return;
              }
              on_done(pbr::Roe<void>());
            },
            2000);
      });

  // If punch left a private endpoint, PreferAssociation would burn the dial budget — go straight
  // to nested circuit via EstablishNestedViaHop (same as Phase-1 peer-id-only).
  if (links.GetLinkSnapshot(target_peer_id).has_endpoint && !peer.Links().IsConnected(target_peer_id)) {
    if (auto nested = EstablishNestedViaHop(peer, circuit, hops, hop_peer_id, target_peer_id, std::string());
        !nested) {
      return nested;
    }
    return {};
  }

  AsyncWait<void> ensure_wait;
  reach.TryEnsureCallMediaReachableAsync(target_peer_id, ensure_wait.Fn());
  if (!ensure_wait.PumpUntilDone(peer, timeout_ms) || !ensure_wait.result) {
    return ensure_wait.result ? pbr::Error("product ensure failed") : ensure_wait.result.error();
  }
  if (!peer.Links().IsConnected(target_peer_id)) {
    return pbr::Error("product ensure: peer not connected after punch/circuit");
  }
  return {};
}

int RunAnswerer(const std::string& listen_ma, const std::string& call_id, const std::string& ready_file,
                int hold_seconds, const std::string& advertise_host, bool no_auto_detach, bool with_chat,
                const std::string& warm_hop_ma, int min_rx_frames) {
  if (sodium_init() < 0) {
    std::cerr << "error: sodium_init failed\n";
    return 1;
  }

  auto bind_ep = ParseListenEndpoint(listen_ma);
  if (!bind_ep) {
    std::cerr << "error: --listen must be Amp ADP multiaddr "
                 "(/ip4/.../udp/.../adp/1.0.0[/p2p/...])\n";
    return 2;
  }

  auto peer = MakeAmpPeer(*bind_ep, true);
  if (!peer) {
    std::cerr << "error: answerer amp start: " << peer.error().message << "\n";
    return 1;
  }

  std::string advertise = (*peer)->listen_ma;
  if (!advertise_host.empty()) {
    advertise = RewriteListenHost(std::move(advertise), advertise_host);
  } else {
    advertise = RewriteWildcardListenHost(std::move(advertise));
  }
  // Publish advertise MA on ch0 so a warm hop can learn PeerId→addr (even if RFC1918).
  (*peer)->Links().SetLocalListenMultiaddrs({advertise});

  if (!warm_hop_ma.empty()) {
    if (!HasP2pSuffix(warm_hop_ma) || warm_hop_ma.find("/adp/") == std::string::npos) {
      std::cerr << "error: --warm-hop must be Amp ADP multiaddr with /p2p/<PeerId>\n";
      return 2;
    }
    if (auto reg = (*peer)->Links().RegisterEndpoint("hop", RewriteWildcardListenHost(warm_hop_ma));
        !reg) {
      std::cerr << "error: register warm-hop: " << reg.error().message << "\n";
      return 1;
    }
    if (auto hop_pid = PeerIdFromMultiaddr(warm_hop_ma)) {
      (void)(*peer)->Links().RegisterEndpoint(*hop_pid, RewriteWildcardListenHost(warm_hop_ma));
    }
    AsyncWait<void> warm_wait;
    (*peer)->Links().EnsureAssociation("hop", warm_wait.LinkFn());
    if (!warm_wait.PumpUntilDone(**peer, 15000) || !warm_wait.result) {
      std::cerr << "error: warm-hop associate: "
                << (warm_wait.result ? "failed" : warm_wait.result.error().message) << "\n";
      return 1;
    }
    // Allow hop peer-book ingest after association.
    for (int i = 0; i < 50; ++i) {
      (*peer)->Pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cout << "ok  warm-hop associated hop=" << warm_hop_ma << "\n";
  }

  // Punch responder for Phase-2 product reach (harmless for Phase-1 via-hop).
  std::unique_ptr<pbr::AmpPunchCoordinator> punch;
  {
    std::vector<std::string> cands = {advertise};
    if (!(*peer)->listen_ma.empty()) {
      cands.push_back((*peer)->listen_ma);
    }
    punch = StartProbePunch(**peer, cands);
    std::cout << "ok  punch coordinator started (answerer)\n";
  }

  auto media = std::make_unique<pbr::CallMediaLegCoordinator>((*peer)->Runtime());
  media->Start();

  auto circuit = std::make_unique<pbr::CircuitTunnelCoordinator>((*peer)->Runtime());
  circuit->Start();
  circuit->SetServeInbound(false);

  std::unique_ptr<pbr::IChatPeerLinks> chat_links;
  std::unique_ptr<pbr::AmpDirectChatTransport> chat;
  std::atomic<int> chat_received{0};
  if (with_chat) {
    auto pump = [p = peer->get()]() { p->Pump(); };
    chat_links = pbr::NewAmpChatPeerLinks((*peer)->Links());
    chat = std::make_unique<pbr::AmpDirectChatTransport>(
        *chat_links, pbr::AmpDirectChatTransport::IoPump{pump});
    chat->Start();
    chat->SetInboundHandler([&](pbr::RelayEnvelope) {
      chat_received.fetch_add(1, std::memory_order_acq_rel);
    });
  }

  const pbr::ByteVector media_key(32, 0x42);
  std::mutex mu;
  int cycles_done = 0;
  int audio_in_session = 0;
  std::atomic<bool> detach_after_audio{false};

  media->SetInboundHandler([&](pbr::CallMediaDirectConnectParams& params, pbr::CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] {
      audio_in_session = 0;
      std::lock_guard lock(mu);
    };
    cbs.on_audio = [&](const std::vector<uint8_t>&) {
      std::lock_guard lock(mu);
      ++cycles_done;
      ++audio_in_session;
      if (audio_in_session >= (with_chat ? 2 : 1)) {
        detach_after_audio.store(true, std::memory_order_release);
      }
    };
  });

  if (!ready_file.empty()) {
    FILE* f = std::fopen(ready_file.c_str(), "w");
    if (!f) {
      std::cerr << "error: cannot write ready-file " << ready_file << "\n";
      return 1;
    }
    std::fprintf(f, "%s\n", advertise.c_str());
    std::fclose(f);
  }
  std::cout << "pp-call-probe answerer ready peer=" << advertise << "\n";
  std::cout.flush();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(hold_seconds);
  bool min_rx_met = false;
  while (std::chrono::steady_clock::now() < deadline) {
    (*peer)->Pump();
    if (!no_auto_detach && detach_after_audio.exchange(false, std::memory_order_acq_rel)) {
      media->Detach();
    }
    if (min_rx_frames > 0) {
      std::lock_guard lock(mu);
      if (cycles_done >= min_rx_frames) {
        min_rx_met = true;
      }
    }
    // NAT/hard-lab: once duplex RX gate is satisfied, exit so the driver need not SIGTERM us.
    if (min_rx_met && !no_auto_detach) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (chat) {
    chat->Stop();
  }
  media->Stop();
  circuit->Stop();
  (*peer)->stack->Stop();
    std::cout << "pp-call-probe answerer exit audio_frames=" << cycles_done
            << " chat_frames=" << chat_received.load() << "\n";
  if (min_rx_frames > 0 && cycles_done < min_rx_frames) {
    std::cerr << "error: answerer rx frames=" << cycles_done << " < min-rx-frames="
              << min_rx_frames << " (duplex/NAT path gate)\n";
    return 1;
  }
  return 0;
}

int RunOfferer(const std::string& peer_ma, const std::string& call_id, int cycles,
               const std::string& hop_ma, int hold_ms, int timeout_ms, bool expect_busy,
               bool with_chat, bool peer_id_only, bool reach_product) {
  if (cycles < 1 || cycles > 100) {
    std::cerr << "error: --cycles must be 1..100\n";
    return 2;
  }
  if (!HasP2pSuffix(peer_ma) || peer_ma.find("/adp/") == std::string::npos) {
    std::cerr << "error: --peer must be Amp ADP multiaddr with /p2p/<PeerId>\n";
    return 2;
  }
  const bool via_hop = !hop_ma.empty();
  if (via_hop && (!HasP2pSuffix(hop_ma) || hop_ma.find("/adp/") == std::string::npos)) {
    std::cerr << "error: --via-hop must be Amp ADP multiaddr with /p2p/<PeerId>\n";
    return 2;
  }
  if (peer_id_only && !via_hop) {
    std::cerr << "error: --peer-id-only requires --via-hop\n";
    return 2;
  }
  if (reach_product && !via_hop) {
    std::cerr << "error: --reach product requires --via-hop (seed/introducer)\n";
    return 2;
  }
  if (reach_product && expect_busy) {
    std::cerr << "error: --reach product does not support --expect busy\n";
    return 2;
  }
  auto peer_id = PeerIdFromMultiaddr(peer_ma);
  if (!peer_id) {
    std::cerr << "error: cannot parse peer id from --peer\n";
    return 2;
  }
  if (sodium_init() < 0) {
    std::cerr << "error: sodium_init failed\n";
    return 1;
  }

  // Bind all interfaces so Docker/netns offerers can dial a hop on a bridge IP
  // (127.0.0.1-bound UDP cannot sendto non-loopback destinations).
  // Product reach: accept inbound so coordinated punch can complete.
  auto offerer = MakeAmpPeer(pp::adp::IpEndpoint::V4(0, 0, 0, 0, 0), reach_product);
  if (!offerer) {
    std::cerr << "error: offerer amp start: " << offerer.error().message << "\n";
    return 1;
  }

  auto hops = std::make_unique<pbr::AmpCircuitHopRegistry>();
  auto circuit = std::make_unique<pbr::CircuitTunnelCoordinator>((*offerer)->Runtime());
  circuit->Start();
  circuit->SetServeInbound(false);

  auto media = std::make_unique<pbr::CallMediaLegCoordinator>((*offerer)->Runtime());
  media->Start();

  auto pump = [p = offerer->get()]() { p->Pump(); };
  std::unique_ptr<pbr::IChatPeerLinks> chat_links;
  std::unique_ptr<pbr::AmpDirectChatTransport> chat;
  if (with_chat) {
    chat_links = pbr::NewAmpChatPeerLinks((*offerer)->Links());
    chat = std::make_unique<pbr::AmpDirectChatTransport>(
        *chat_links, pbr::AmpDirectChatTransport::IoPump{pump});
    chat->Start();
  }

  const std::string dial_peer_ma = RewriteWildcardListenHost(peer_ma);
  const std::string peer_key = *peer_id;
  std::unique_ptr<pbr::IChatPeerLinks> reach_links;
  std::unique_ptr<pbr::AmpPunchCoordinator> punch;
  std::string hop_peer_id;
  if (via_hop) {
    auto hop_id = PeerIdFromMultiaddr(hop_ma);
    if (!hop_id) {
      std::cerr << "error: cannot parse hop peer id from --via-hop\n";
      return 2;
    }
    hop_peer_id = *hop_id;
    // Product Ensure looks up relays by PeerId; Phase-1 StartBridge uses endpoint key "hop".
    const std::string warm_key = reach_product ? hop_peer_id : std::string("hop");
    if (auto warm = WarmHopAssociation(**offerer, warm_key, hop_ma); !warm) {
      std::cerr << "error: warm hop: " << warm.error().message << "\n";
      return 1;
    }
    // Also alias the other key so punch/circuit/StartBridge share one association.
    const std::string alias_key = reach_product ? std::string("hop") : hop_peer_id;
    if (alias_key != warm_key) {
      (void)(*offerer)->Links().RegisterEndpoint(alias_key, RewriteWildcardListenHost(hop_ma));
    }
    std::cout << "ok  offerer warm-hop associated key=" << warm_key << " hop=" << hop_ma << "\n";
    if (reach_product) {
      // Do NOT register private advertise MA — PreferredMultiaddr would poison circuit dial.
      reach_links = pbr::NewAmpChatPeerLinks((*offerer)->Links());
      std::vector<std::string> cands = {(*offerer)->listen_ma};
      punch = StartProbePunch(**offerer, cands);
      std::cout << "pp-call-probe offerer reach=product seed-hop=" << hop_ma << " peer=" << peer_key
                << "\n";
    } else {
      std::cout << "pp-call-probe offerer via-hop=" << hop_ma << " peer=" << peer_ma
                << " peer-id-only=" << (peer_id_only ? 1 : 0) << "\n";
    }
  } else if (auto reg = (*offerer)->Links().RegisterEndpoint(peer_key, dial_peer_ma); !reg) {
    std::cerr << "error: register peer: " << reg.error().message << "\n";
    return 1;
  }

  const pbr::ByteVector media_key(32, 0x42);
  for (int cycle = 0; cycle < cycles; ++cycle) {
    if (via_hop && reach_product) {
      auto ensured = EnsureProductCallMediaReach(**offerer, *circuit, *hops, *reach_links, *punch,
                                                 hop_peer_id, hop_ma, peer_key, timeout_ms + 15000);
      if (!ensured) {
        std::cerr << "error: product reach cycle " << cycle << ": " << ensured.error().message << "\n";
        return 1;
      }
      std::cout << "ok  product punch/circuit reach cycle " << cycle << "\n";
    } else if (via_hop) {
      auto nested = EstablishNestedViaHop(**offerer, *circuit, *hops, "hop", *peer_id,
                                          (peer_id_only ? std::string() : dial_peer_ma));
      if (!nested) {
        std::cerr << "error: nested circuit cycle " << cycle << ": " << nested.error().message << "\n";
        return 1;
      }
    }

    std::atomic<bool> connected{false};
    pbr::CallMediaDirectConnectParams params;
    params.peer_key = peer_key;
    params.call_id = call_id;
    params.media_key = media_key;
    params.media_epoch = 1;
    params.offerer = true;

    pbr::CallMediaDirectCallbacks cbs;
    cbs.on_connected = [&] { connected.store(true, std::memory_order_release); };

    AsyncWait<void> leg_done;
    const pbr::CallMediaLegId leg_id =
        media->StartLeg(params, std::move(cbs), leg_done.Fn(), timeout_ms);
    if (!leg_id) {
      if (expect_busy) {
        std::cout << "ok  busy cycle " << cycle << " start rejected\n";
        continue;
      }
      std::cerr << "error: start leg cycle " << cycle << "\n";
      return 1;
    }

    if (!leg_done.PumpUntilDone(**offerer, timeout_ms + 2000) || !leg_done.result) {
      if (expect_busy) {
        std::cout << "ok  busy cycle " << cycle << " connect rejected: "
                  << (leg_done.result ? "failed" : leg_done.result.error().message) << "\n";
        media->DetachLeg(leg_id);
        continue;
      }
      std::cerr << "error: connect cycle " << cycle << ": "
                << (leg_done.result ? "failed" : leg_done.result.error().message) << "\n";
      return 1;
    }

    if (!PumpUntil(**offerer, [&] { return connected.load(std::memory_order_acquire); },
                   timeout_ms + 1000)) {
      if (expect_busy) {
        std::cout << "ok  busy cycle " << cycle << " connect timeout\n";
        media->DetachLeg(leg_id);
        continue;
      }
      std::cerr << "error: connect timeout cycle " << cycle << "\n";
      return 1;
    }
    if (expect_busy) {
      std::cerr << "error: expected busy (second inbound rejected) but connect succeeded\n";
      media->DetachLeg(leg_id);
      return 1;
    }

    const std::vector<uint8_t> opus = {static_cast<uint8_t>(0x10 + cycle)};
    if (auto sent = media->SendAudio(leg_id, opus, 1, 0); !sent) {
      std::cerr << "error: send audio cycle " << cycle << ": " << sent.error().message << "\n";
      return 1;
    }
    const auto audio_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < audio_deadline) {
      (*offerer)->Pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (with_chat && chat) {
      if (!SendProbeChat(*chat, peer_key, cycle)) {
        std::cerr << "error: chat during call cycle " << cycle << "\n";
        return 1;
      }
      std::cout << "ok  chat during call cycle " << cycle << (via_hop ? " via-hop" : "") << "\n";
      if (auto sent = media->SendAudio(leg_id, {static_cast<uint8_t>(0x20 + cycle)}, 2, 0); !sent) {
        std::cerr << "error: audio after chat cycle " << cycle << "\n";
        return 1;
      }
      const auto after_chat = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
      while (std::chrono::steady_clock::now() < after_chat) {
        (*offerer)->Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    if (hold_ms > 0) {
      const auto hold_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms);
      while (std::chrono::steady_clock::now() < hold_deadline) {
        (*offerer)->Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    media->DetachLeg(leg_id);
    if (via_hop) {
      hops->Clear(*peer_id, pp::amp::kAmpCircuitCarrierProtocolId);
      const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
      while (std::chrono::steady_clock::now() < settle) {
        (*offerer)->Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    if (with_chat && chat) {
      if (via_hop) {
        auto nested = EstablishNestedViaHop(**offerer, *circuit, *hops, "hop", *peer_id, (peer_id_only ? std::string() : dial_peer_ma));
        if (!nested) {
          std::cerr << "error: chat hop after leave cycle " << cycle << ": " << nested.error().message
                    << "\n";
          return 1;
        }
      }
      if (!SendProbeChat(*chat, peer_key, cycle + 100)) {
        std::cerr << "error: chat after leave cycle " << cycle << "\n";
        return 1;
      }
      std::cout << "ok  chat after leave cycle " << cycle << (via_hop ? " via-hop" : "") << "\n";
      if (via_hop) {
        hops->Clear(*peer_id, pp::amp::kAmpCircuitCarrierProtocolId);
      }
    }

    std::cout << "ok  cycle " << cycle << " connect+audio+detach" << (via_hop ? " via-hop" : "")
              << "\n";
  }

  if (chat) {
    chat->Stop();
  }
  media->Stop();
  circuit->Stop();
  (*offerer)->stack->Stop();
  std::cout << "pp-call-probe offerer PASSED cycles=" << cycles
            << (reach_product ? " reach=product" : (via_hop ? " via-hop" : ""))
            << (with_chat ? " with-chat" : "") << "\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::string role;
  std::string listen_ma = "/ip4/127.0.0.1/udp/47100/adp/1.0.0";
  std::string peer_ma;
  std::string hop_ma;
  std::string advertise_host;
  std::string call_id = "pp-call-direct";
  std::string ready_file;
  int cycles = 1;
  int hold_seconds = 30;
  int hold_ms = 0;
  int timeout_ms = 8000;
  bool expect_busy = false;
  bool with_chat = false;
  bool no_auto_detach = false;
  bool peer_id_only = false;
  bool reach_product = false;
  std::string warm_hop_ma;
  int min_rx_frames = 0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
      role = argv[++i];
    } else if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
      listen_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
      peer_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--via-hop") == 0 && i + 1 < argc) {
      hop_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--advertise-host") == 0 && i + 1 < argc) {
      advertise_host = argv[++i];
    } else if (std::strcmp(argv[i], "--call-id") == 0 && i + 1 < argc) {
      call_id = argv[++i];
    } else if (std::strcmp(argv[i], "--ready-file") == 0 && i + 1 < argc) {
      ready_file = argv[++i];
    } else if (std::strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
      cycles = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--hold-seconds") == 0 && i + 1 < argc) {
      hold_seconds = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
      hold_ms = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
      timeout_ms = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
      ++i;
      if (std::strcmp(argv[i], "busy") == 0) {
        expect_busy = true;
      } else if (std::strcmp(argv[i], "ok") == 0) {
        expect_busy = false;
      } else {
        std::cerr << "error: --expect ok|busy\n";
        return 2;
      }
    } else if (std::strcmp(argv[i], "--with-chat") == 0) {
      with_chat = true;
    } else if (std::strcmp(argv[i], "--no-auto-detach") == 0) {
      no_auto_detach = true;
    } else if (std::strcmp(argv[i], "--warm-hop") == 0 && i + 1 < argc) {
      warm_hop_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--min-rx-frames") == 0 && i + 1 < argc) {
      min_rx_frames = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--peer-id-only") == 0) {
      peer_id_only = true;
    } else if (std::strcmp(argv[i], "--reach") == 0 && i + 1 < argc) {
      ++i;
      if (std::strcmp(argv[i], "product") == 0) {
        reach_product = true;
      } else {
        std::cerr << "error: --reach product\n";
        return 2;
      }
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }

  auto root = pbr::logging::getRootLogger();
  root.setLevel(pbr::logging::Level::INFO);

  if (const char* env = std::getenv("PP_CALL_PROBE_VIA_HOP")) {
    if (hop_ma.empty() && env[0] != '\0') {
      hop_ma = env;
    }
  }
  if (const char* env = std::getenv("PP_NODE_PROBE_ADVERTISE_HOST")) {
    if (advertise_host.empty() && env[0] != '\0') {
      advertise_host = env;
    }
  }

  if (role == "answerer") {
    return RunAnswerer(listen_ma, call_id, ready_file, hold_seconds, advertise_host, no_auto_detach,
                       with_chat, warm_hop_ma, min_rx_frames);
  }
  if (role == "offerer") {
    if (peer_ma.empty()) {
      std::cerr << "error: --peer required for offerer\n";
      return 2;
    }
    return RunOfferer(peer_ma, call_id, cycles, hop_ma, hold_ms, timeout_ms, expect_busy, with_chat,
                       peer_id_only, reach_product);
  }
  std::cerr << "error: --role answerer|offerer required\n";
  PrintUsage(argv[0]);
  return 2;
}
