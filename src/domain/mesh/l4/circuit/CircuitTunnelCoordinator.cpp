#include "domain/mesh/l4/circuit/CircuitRelayTypes.h"
#include "domain/mesh/l4/circuit/CircuitTunnelCoordinator.h"
#include "domain/mesh/l4/circuit/CircuitServeDialPolicy.h"

#include "amp/L3/ChannelBridge.h"
#include "domain/mesh/l4/shared/ChannelSessionSlot.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/PeerLink.h"
#include "amp/link/Types.h"
#include "common/ValueJson.h"
#include "domain/mesh/shared/AmpChannelOpen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

pp::amp::ChannelPolicy PolicyForCircuitTarget(const std::string& target_protocol) {
  if (target_protocol == pp::amp::kAmpCircuitCarrierProtocolId) {
    return pp::amp::CircuitCarrierChannelPolicy();
  }
  return pp::amp::CircuitTunnelChannelPolicy();
}

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::string BodyToJson(const std::vector<uint8_t>& body) {
  return std::string(body.begin(), body.end());
}

Roe<std::pair<std::string, std::string>> NormalizeAmpCircuitTarget(pp::amp::PeerLinkManager& links,
                                                                   const CircuitBridgeTarget& target) {
  if (target.target_multiaddr.empty() && target.target_peer_id.empty()) {
    return Error("missing circuit bridge target");
  }
  if (!target.target_multiaddr.empty()) {
    auto parsed = pp::amp::ParseAdpMultiaddr(target.target_multiaddr);
    if (!parsed) {
      return parsed.error();
    }
    const std::string peer_id =
        !target.target_peer_id.empty() ? target.target_peer_id : parsed->peer_id;
    if (peer_id.empty()) {
      return Error("circuit target multiaddr missing peer id");
    }
    if (auto registered = links.RegisterEndpoint(peer_id, target.target_multiaddr); !registered) {
      return registered.error();
    }
    return std::make_pair(peer_id, target.target_multiaddr);
  }
  // Peer-id-only (nested call-media): H010 — live Connected only (CircuitServeDialPolicy).
  if (CircuitPeerIdOnlyHasLiveFarLeg(links.CountConnectedLinksForPeerId(target.target_peer_id))) {
    return std::make_pair(target.target_peer_id, std::string{});
  }
  return Error(std::string(kCircuitTargetPeerNotRegistered));
}

} // namespace

struct CircuitTunnelCoordinator::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  std::mutex mu;
  CircuitRelayAdmissionPolicy admission;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{true};
  std::atomic<bool> serve_inbound{true};
  std::atomic<uint64_t> next_id{1};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;
  pp::amp::PeerLinkManager::PeerConnectedListenerId peer_connected_listener_id_ = 0;

  struct Tunnel {
    CircuitTunnelId id;
    CircuitTunnelRole role = CircuitTunnelRole::Client;
    CircuitTunnelPhase phase = CircuitTunnelPhase::Idle;
    Clock::time_point deadline{};
    CircuitBridgeTarget target;
    std::string relay_peer_key;
    std::string dialer_peer_id;
    std::string resolved_multiaddr;
    std::shared_ptr<pp::amp::ChannelSession> near_session;   // client↔relay circuit channel
    std::shared_ptr<pp::amp::ChannelSession> far_session;    // relay↔target protocol channel
    std::shared_ptr<pp::amp::ChannelBridge> bridge;
    FrameHandler on_payload;
    ClosedCallback on_closed;
    BridgeFinished on_finished;
    bool finished = false;
    bool local_cancel = false;
    bool is_reserve = false;
    /** Absolute steady ms; 0 = not waiting for far PeerLink (event-driven ServeDial). */
    int64_t serve_far_wait_deadline_ms = 0;
  };

  struct Reservation {
    std::shared_ptr<pp::amp::ChannelSession> session;
    Clock::time_point deadline{};
  };

  std::unordered_map<uint64_t, std::unique_ptr<Tunnel>> tunnels;
  /** Relay: PeerId → parked inbound circuit channel from answerer (op=reserve). */
  std::unordered_map<std::string, Reservation> reservations;
  /** PeerId → ServeDial tunnels waiting for Connected / reserve (H010 event wait). */
  std::unordered_map<std::string, std::vector<CircuitTunnelId>> far_leg_waiters_;

  void PostIo(std::function<void()> task) {
    if (!runtime || !task) {
      return;
    }
    runtime->PostToIo(std::move(task));
  }

  Tunnel* Find(const CircuitTunnelId id) {
    auto it = tunnels.find(id.value);
    return it == tunnels.end() ? nullptr : it->second.get();
  }

  const Tunnel* Find(const CircuitTunnelId id) const {
    auto it = tunnels.find(id.value);
    return it == tunnels.end() ? nullptr : it->second.get();
  }

  void ClearFarLegWait(Tunnel& tunnel) {
    tunnel.serve_far_wait_deadline_ms = 0;
    const std::string& peer_id = tunnel.target.target_peer_id;
    if (peer_id.empty()) {
      return;
    }
    auto it = far_leg_waiters_.find(peer_id);
    if (it == far_leg_waiters_.end()) {
      return;
    }
    auto& ids = it->second;
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](const CircuitTunnelId id) { return id.value == tunnel.id.value; }),
              ids.end());
    if (ids.empty()) {
      far_leg_waiters_.erase(it);
    }
  }

  void ArmFarLegWait(Tunnel& tunnel) {
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    const int64_t tunnel_deadline_ms =
        tunnel.deadline.time_since_epoch().count() == 0
            ? 0
            : std::chrono::duration_cast<std::chrono::milliseconds>(tunnel.deadline.time_since_epoch())
                  .count();
    tunnel.serve_far_wait_deadline_ms =
        CircuitServeDialArmFarLegWaitDeadlineMs(now_ms, tunnel_deadline_ms);
    tunnel.phase = CircuitTunnelPhase::ServeDial;
    const std::string& peer_id = tunnel.target.target_peer_id;
    if (peer_id.empty()) {
      return;
    }
    auto& ids = far_leg_waiters_[peer_id];
    for (const auto id : ids) {
      if (id.value == tunnel.id.value) {
        return;
      }
    }
    ids.push_back(tunnel.id);
  }

  /** PeerConnected / reserve event — resume ServeDial waiters for this PeerId. */
  void NotifyFarLegReady(const std::string& peer_id) {
    if (peer_id.empty() || !runtime) {
      return;
    }
    std::vector<CircuitTunnelId> ids;
    {
      std::lock_guard lock(mu);
      auto it = far_leg_waiters_.find(peer_id);
      if (it == far_leg_waiters_.end()) {
        return;
      }
      ids = it->second;
    }
    for (const auto id : ids) {
      PostIo([this, id]() {
        Tunnel* tunnel = nullptr;
        {
          std::lock_guard lock(mu);
          tunnel = Find(id);
          if (!tunnel || tunnel->finished || tunnel->phase != CircuitTunnelPhase::ServeDial ||
              tunnel->serve_far_wait_deadline_ms == 0) {
            return;
          }
        }
        BeginServe(*tunnel);
      });
    }
  }

  void ScheduleWhenChannelOpen(std::string peer_key, const uint32_t channel_id, const Clock::time_point deadline,
                               std::function<void(bool open)> done) {
    if (!done) {
      return;
    }
    if (stopped.load(std::memory_order_acquire) || !runtime || peer_key.empty()) {
      done(false);
      return;
    }
    AmpWhenChannelOpen(runtime->Links(), peer_key, channel_id, deadline, std::move(done));
  }

  pp::amp::PeerLink* ResolveLink(const std::string& peer_key) const {
    if (!runtime || peer_key.empty()) {
      return nullptr;
    }
    return runtime->Links().FindLink(peer_key);
  }

  /**
   * PeerLink drop leaves ChannelSession mux_ dangling — tear down before CloseQuiet.
   * Do not treat FindLink(key)==null alone as lost: ADP links are often under inbound:…
   * or dial aliases while the tunnel key is PeerId ([A024] / dogfood dual-NAT). Match
   * CallMediaLeg grace while a dialable endpoint remains during Open/WaitAck/ServeDial.
   */
  bool PeerLinkMissing(const Tunnel& tunnel) const {
    if (!runtime) {
      return false;
    }
    auto adp_gone = [this](const std::string& peer_id) {
      if (peer_id.empty()) {
        return false;
      }
      // Prefer PeerId connectivity (inbound alias / dial alias) over exact dial-key FindLink.
      if (runtime->Links().CountConnectedLinksForPeerId(peer_id) > 0) {
        return false;
      }
      return runtime->Links().FindLink(peer_id) == nullptr;
    };
    auto dialing_grace = [this](const CircuitTunnelPhase phase, const std::string& peer_id) {
      if (peer_id.empty()) {
        return false;
      }
      if (phase != CircuitTunnelPhase::OutboundOpen && phase != CircuitTunnelPhase::WaitAck &&
          phase != CircuitTunnelPhase::ServeDial) {
        return false;
      }
      return runtime->Links().GetLinkSnapshot(peer_id).has_endpoint;
    };

    if (tunnel.near_session) {
      const std::string& near_key =
          !tunnel.relay_peer_key.empty() ? tunnel.relay_peer_key : tunnel.dialer_peer_id;
      if (!near_key.empty() && adp_gone(near_key) && !dialing_grace(tunnel.phase, near_key)) {
        return true;
      }
    }
    if (tunnel.far_session && !tunnel.target.target_peer_id.empty() &&
        adp_gone(tunnel.target.target_peer_id) &&
        !dialing_grace(tunnel.phase, tunnel.target.target_peer_id)) {
      return true;
    }
    return false;
  }

  void TickDeadlines() {
    const auto now = Clock::now();
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::vector<CircuitTunnelId> timed_out;
    std::vector<CircuitTunnelId> link_lost;
    std::vector<CircuitTunnelId> far_wait_expired;
    std::vector<std::string> expired_reserves;
    {
      std::lock_guard lock(mu);
      for (auto& [_, tunnel] : tunnels) {
        if (!tunnel || tunnel->finished || tunnel->phase == CircuitTunnelPhase::Closing) {
          continue;
        }
        if (PeerLinkMissing(*tunnel)) {
          link_lost.push_back(tunnel->id);
          continue;
        }
        // Deadline event for event-driven ServeDial far-leg wait (not a poll-resume).
        if (tunnel->phase == CircuitTunnelPhase::ServeDial && tunnel->serve_far_wait_deadline_ms != 0 &&
            now_ms >= tunnel->serve_far_wait_deadline_ms) {
          far_wait_expired.push_back(tunnel->id);
          continue;
        }
        if (tunnel->deadline.time_since_epoch().count() == 0) {
          continue;
        }
        if (now >= tunnel->deadline &&
            (tunnel->phase == CircuitTunnelPhase::Reserved ||
             (CircuitTunnelPhaseIsActive(tunnel->phase) && tunnel->phase != CircuitTunnelPhase::Bridging))) {
          timed_out.push_back(tunnel->id);
        }
      }
      for (auto& [peer_id, res] : reservations) {
        if (now >= res.deadline || !res.session || res.session->IsClosed()) {
          expired_reserves.push_back(peer_id);
        }
      }
    }
    for (const auto id : link_lost) {
      std::lock_guard lock(mu);
      if (auto* tunnel = Find(id)) {
        if (tunnel->phase == CircuitTunnelPhase::Closing) {
          continue;
        }
        TearDown(*tunnel, false, false, "circuit-relay peer link lost");
      }
    }
    for (const auto id : far_wait_expired) {
      std::lock_guard lock(mu);
      if (auto* tunnel = Find(id)) {
        if (tunnel->finished || tunnel->phase != CircuitTunnelPhase::ServeDial) {
          continue;
        }
        if (tunnel->near_session) {
          Object err;
          err.set("v", int64_t{1});
          err.set("ok", false);
          err.set("error", std::string(kCircuitTargetPeerNotRegistered));
          tunnel->near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
        }
        TearDown(*tunnel, false, false, std::string(kCircuitTargetPeerNotRegistered));
      }
    }
    for (const auto id : timed_out) {
      std::lock_guard lock(mu);
      if (auto* tunnel = Find(id)) {
        const bool far_wait =
            tunnel->phase == CircuitTunnelPhase::ServeDial && tunnel->serve_far_wait_deadline_ms != 0;
        if (far_wait && tunnel->near_session) {
          Object err;
          err.set("v", int64_t{1});
          err.set("ok", false);
          err.set("error", std::string(kCircuitTargetPeerNotRegistered));
          tunnel->near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
        }
        TearDown(*tunnel, false, false,
                 far_wait ? std::string(kCircuitTargetPeerNotRegistered)
                          : (tunnel->is_reserve ? "circuit-relay reserve timed out"
                                                : "circuit-relay bridge timed out"));
      }
    }
    for (const auto& peer_id : expired_reserves) {
      std::lock_guard lock(mu);
      auto it = reservations.find(peer_id);
      if (it == reservations.end()) {
        continue;
      }
      CloseQuietSlot(it->second.session, ResolveLink(peer_id));
      reservations.erase(it);
    }
  }

  void Finish(Tunnel& tunnel, Roe<CircuitTunnelBridgeResult> result) {
    if (tunnel.finished) {
      return;
    }
    tunnel.finished = true;
    auto cb = std::move(tunnel.on_finished);
    tunnel.on_finished = nullptr;
    if (!cb) {
      return;
    }
    // Callers hold Impl::mu. try_relay → StartBridge → OpenChannel must not re-enter under lock
    // (dogfood hop give-up / 130521). Deliver on the IO queue after TearDown returns.
    PostIo([cb = std::move(cb), result = std::move(result)]() mutable { cb(std::move(result)); });
  }

  void TearDown(Tunnel& tunnel, const bool suppress_notify, const bool local_cancel, const std::string& error) {
    if (tunnel.finished && !tunnel.bridge) {
      ClearFarLegWait(tunnel);
      tunnels.erase(tunnel.id.value);
      return;
    }
    ClearFarLegWait(tunnel);
    tunnel.local_cancel = local_cancel || tunnel.local_cancel;
    tunnel.phase = CircuitTunnelPhase::Closing;
    if (tunnel.bridge) {
      auto bridge = std::move(tunnel.bridge);
      bridge->Stop();
    }
    if (tunnel.near_session) {
      CloseQuietSlot(tunnel.near_session, ResolveLink(tunnel.relay_peer_key));
    }
    if (tunnel.far_session) {
      CloseQuietSlot(tunnel.far_session, ResolveLink(tunnel.target.target_peer_id));
    }
    if (!tunnel.finished) {
      if (suppress_notify || local_cancel) {
        Finish(tunnel, Error(error.empty() ? "circuit-relay aborted" : error));
      } else {
        CircuitTunnelBridgeResult fail;
        fail.ok = false;
        fail.error = error;
        Finish(tunnel, Error(error));
      }
    }
    tunnels.erase(tunnel.id.value);
  }

  void ArmBridge(Tunnel& tunnel) {
    if (!tunnel.near_session || !tunnel.far_session) {
      TearDown(tunnel, false, false, "circuit-relay bridge missing sessions");
      return;
    }
    tunnel.phase = CircuitTunnelPhase::Bridging;
    tunnel.bridge = std::make_shared<pp::amp::ChannelBridge>();
    const CircuitTunnelId id = tunnel.id;
    tunnel.bridge->Attach(
        tunnel.near_session, tunnel.far_session, {}, [this, id]() {
          PostIo([this, id]() {
            std::lock_guard lock(mu);
            if (auto* tunnel = Find(id)) {
              const auto decision = DecideCircuitTunnelClose(CircuitTunnelCloseContext{
                  .phase = tunnel->phase,
                  .local_cancel = tunnel->local_cancel,
                  .remote_terminal = true,
                  .finished = tunnel->finished,
              });
              if (decision == CircuitTunnelCloseDecision::Ignore) {
                return;
              }
              TearDown(*tunnel, decision == CircuitTunnelCloseDecision::SuppressNotify, tunnel->local_cancel,
                       "circuit-relay tunnel closed");
            }
          });
        });

    if (tunnel.role == CircuitTunnelRole::Client) {
      CircuitTunnelBridgeResult ok;
      ok.ok = true;
      ok.resolved_multiaddr = tunnel.resolved_multiaddr;
      ok.session = tunnel.near_session;
      Finish(tunnel, std::move(ok));
    }
  }

  void BindClientNear(Tunnel& tunnel, pp::amp::PeerLink& link, const uint32_t channel_id) {
    tunnel.near_session = std::make_shared<pp::amp::ChannelSession>();
    const CircuitTunnelId id = tunnel.id;
    tunnel.near_session->Bind(
        *link.Mux(), channel_id, PolicyForCircuitTarget(tunnel.target.target_protocol),
        [this, id](Roe<std::vector<uint8_t>> frame) {
          std::lock_guard lock(mu);
          auto* tunnel = Find(id);
          if (!tunnel) {
            return false;
          }
          if (!frame) {
            TearDown(*tunnel, false, false, "circuit-relay bridge failed");
            return false;
          }
          if (tunnel->phase == CircuitTunnelPhase::WaitAck) {
            auto root = TryParseObject(BodyToJson(*frame));
            if (!root) {
              TearDown(*tunnel, false, false, "invalid circuit-relay ack");
              return false;
            }
            const bool ack_ok = root->getIf<bool>("ok").value_or(false);
            const auto decision = DecideCircuitBridgeAck(
                CircuitBridgeAckContext{.phase = tunnel->phase, .ack_ok = ack_ok});
            if (decision == CircuitBridgeAckDecision::IgnoreStale) {
              return true;
            }
            if (decision == CircuitBridgeAckDecision::Fail) {
              TearDown(*tunnel, false, false,
                       root->getString("error").value_or("circuit-relay bridge refused"));
              return false;
            }
            tunnel->resolved_multiaddr = root->getString("resolved_multiaddr").value_or("");
            if (tunnel->is_reserve) {
              tunnel->phase = CircuitTunnelPhase::Reserved;
              CircuitTunnelBridgeResult ok;
              ok.ok = true;
              ok.session = tunnel->near_session;
              // Notify once; keep tunnel + session until Cancel / TTL (do not mark finished).
              if (tunnel->on_finished) {
                auto cb = std::move(tunnel->on_finished);
                tunnel->on_finished = nullptr;
                PostIo([cb = std::move(cb), ok = std::move(ok)]() mutable { cb(std::move(ok)); });
              }
              return true;
            }
            // Client does not open far channel — relay splices. Treat near_session as both ends of local view:
            // payload handler stays on near_session; bridge is armed only on relay. Client enters Bridging with
            // near_session only (no ChannelBridge locally).
            tunnel->phase = CircuitTunnelPhase::Bridging;
            CircuitTunnelBridgeResult ok;
            ok.ok = true;
            ok.resolved_multiaddr = tunnel->resolved_multiaddr;
            ok.session = tunnel->near_session;
            Finish(*tunnel, std::move(ok));
            return true;
          }
          if (tunnel->phase == CircuitTunnelPhase::Bridging && tunnel->on_payload) {
            return tunnel->on_payload(std::move(frame));
          }
          return true;
        },
        [this, id](const char* reason) {
          std::lock_guard lock(mu);
          auto* tunnel = Find(id);
          if (!tunnel) {
            return;
          }
          if (tunnel->on_closed) {
            tunnel->on_closed(reason);
          }
          const auto decision = DecideCircuitTunnelClose(CircuitTunnelCloseContext{
              .phase = tunnel->phase,
              .local_cancel = tunnel->local_cancel,
              .remote_terminal = true,
              .finished = tunnel->finished,
          });
          if (decision == CircuitTunnelCloseDecision::Ignore) {
            return;
          }
          TearDown(*tunnel, decision == CircuitTunnelCloseDecision::SuppressNotify, tunnel->local_cancel,
                   "circuit-relay channel closed");
        });
  }

  void BeginOutbound(Tunnel& tunnel) {
    tunnel.phase = CircuitTunnelPhase::OutboundOpen;
    const CircuitTunnelId id = tunnel.id;
    const std::string relay_key = tunnel.relay_peer_key;
    const auto deadline = tunnel.deadline;
    Object request;
    request.set("v", int64_t{1});
    request.set("op", tunnel.is_reserve ? "reserve" : "bridge");
    request.set("timeout_ms",
                int64_t{std::max<int64_t>(
                    1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count())});
    if (!tunnel.is_reserve) {
      if (!tunnel.target.target_peer_id.empty()) {
        request.set("target_peer_id", tunnel.target.target_peer_id);
      }
      if (!tunnel.target.target_multiaddr.empty()) {
        request.set("target_multiaddr", tunnel.target.target_multiaddr);
      }
      request.set("target_protocol", tunnel.target.target_protocol);
    }
    const std::string request_json = DumpJson(request);

    // Must not hold mu across OpenChannel — callback may run synchronously.
    runtime->Links().OpenChannel(
        relay_key, kCircuitRelayProtocolId,
        tunnel.is_reserve ? pp::amp::CircuitTunnelChannelPolicy()
                          : PolicyForCircuitTarget(tunnel.target.target_protocol),
        [this, id, relay_key, deadline, request_json](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
          uint32_t channel_id = 0;
          {
            std::lock_guard lock(mu);
            auto* tunnel = Find(id);
            if (!tunnel) {
              return;
            }
            if (!channel) {
              TearDown(*tunnel, false, false, channel.error().message);
              return;
            }
            if (!runtime->Links().FindLink(relay_key)) {
              TearDown(*tunnel, false, false, "amp circuit-relay: channel open failed");
              return;
            }
            channel_id = *channel;
          }
          ScheduleWhenChannelOpen(relay_key, channel_id, deadline,
                                  [this, id, relay_key, channel_id, request_json](const bool open) {
                                    std::lock_guard lock(mu);
                                    auto* tunnel = Find(id);
                                    if (!tunnel) {
                                      return;
                                    }
                                    if (!open) {
                                      TearDown(*tunnel, false, false, "amp circuit-relay: channel open failed");
                                      return;
                                    }
                                    auto* link = runtime->Links().FindLink(relay_key);
                                    if (!link || !link->Mux()) {
                                      TearDown(*tunnel, false, false, "amp circuit-relay: channel open failed");
                                      return;
                                    }
                                    BindClientNear(*tunnel, *link, channel_id);
                                    tunnel->phase = CircuitTunnelPhase::WaitAck;
                                    if (!tunnel->near_session->EnqueueOutbound(JsonToBody(request_json))) {
                                      TearDown(*tunnel, false, false,
                                               "failed to send circuit-relay bridge request");
                                    }
                                  });
        });
  }

  void ContinueServeAfterTargetOpen(Tunnel& tunnel, pp::amp::PeerLink& target_link, const uint32_t channel_id) {
    tunnel.far_session = std::make_shared<pp::amp::ChannelSession>();
    tunnel.far_session->Bind(*target_link.Mux(), channel_id, PolicyForCircuitTarget(tunnel.target.target_protocol),
                     [](Roe<std::vector<uint8_t>>) { return true; });

    Object response;
    response.set("v", int64_t{1});
    response.set("ok", true);
    response.set("resolved_multiaddr", tunnel.resolved_multiaddr);
    if (!tunnel.near_session->EnqueueOutbound(JsonToBody(DumpJson(response)))) {
      TearDown(tunnel, false, false, "failed to ack bridge");
      return;
    }
    ArmBridge(tunnel);
  }

  void BeginServe(Tunnel& tunnel) {
    tunnel.phase = CircuitTunnelPhase::ServeDial;
    CircuitAdmitContext admit;
    admit.service_started = started.load(std::memory_order_acquire) &&
                            serve_inbound.load(std::memory_order_acquire);
    admit.stopping = stopped.load(std::memory_order_acquire);
    admit.dialer_peer_id = tunnel.dialer_peer_id;
    admit.op = "bridge";
    {
      std::lock_guard lock(mu);
      admit.serve_scope_mask = admission.serve_scope_mask;
      admit.contact_peer_ids = admission.contact_peer_ids;
    }
    const auto decision = DecideCircuitAdmit(admit);
    auto fail_near = [&](const std::string& message) {
      Object err;
      err.set("v", int64_t{1});
      err.set("ok", false);
      err.set("error", message);
      if (tunnel.near_session) {
        tunnel.near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
      }
      TearDown(tunnel, false, false, message);
    };
    if (decision == CircuitAdmitDecision::RefuseStranger) {
      fail_near("relay scope: stranger refused");
      return;
    }
    if (decision != CircuitAdmitDecision::Allow) {
      fail_near(decision == CircuitAdmitDecision::RefuseBadOp ? "unsupported op"
                                                              : "circuit-relay service not ready");
      return;
    }

    // Live op=reserve → peer-id-only Connected path (H010 CircuitServeDialPolicy).
    {
      std::lock_guard lock(mu);
      const std::string& tid = tunnel.target.target_peer_id;
      if (!tid.empty()) {
        auto it = reservations.find(tid);
        const bool live_res =
            it != reservations.end() && it->second.session && !it->second.session->IsClosed();
        if (CircuitServeDialClearTargetMaWhenReserved(live_res, !tunnel.target.target_multiaddr.empty())) {
          tunnel.target.target_multiaddr.clear();
        }
      }
    }

    // Peer-id-only: wait for answerer Connected (PeerConnected / reserve events), not Tick poll.
    const bool peer_id_only =
        tunnel.target.target_multiaddr.empty() && !tunnel.target.target_peer_id.empty();
    if (peer_id_only) {
      const bool has_live = CircuitPeerIdOnlyHasLiveFarLeg(
          runtime->Links().CountConnectedLinksForPeerId(tunnel.target.target_peer_id));
      if (!has_live) {
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
                .count();
        if (tunnel.serve_far_wait_deadline_ms != 0 && now_ms >= tunnel.serve_far_wait_deadline_ms) {
          fail_near(std::string(kCircuitTargetPeerNotRegistered));
          return;
        }
        ArmFarLegWait(tunnel);
        return;
      }
      ClearFarLegWait(tunnel);
    }

    auto normalized = NormalizeAmpCircuitTarget(runtime->Links(), tunnel.target);
    if (!normalized) {
      fail_near(normalized.error().message);
      return;
    }
    tunnel.resolved_multiaddr = normalized->second;
    const std::string target_key = normalized->first;
    const CircuitTunnelId id = tunnel.id;
    const auto deadline = tunnel.deadline;
    const std::string target_protocol = tunnel.target.target_protocol;

    // Peer-id-only → OpenChannelOnLink on live far leg (H010); never EnsureAssociation→Preferred.
    if (CircuitServeDialOpenOnLiveLink(normalized->second.empty())) {
      auto* live = runtime->Links().FindLinkByPeerId(target_key);
      if (!live || live->Phase() != pp::amp::PeerLinkPhase::Connected || !live->Mux()) {
        fail_near(std::string(kCircuitTargetPeerNotRegistered));
        return;
      }
      const std::string dial_key = live->PeerKey();
      runtime->Links().OpenChannelOnLink(
          *live, target_protocol, PolicyForCircuitTarget(target_protocol),
          [this, id, dial_key, deadline](pp::amp::PeerLinkManager::ChannelRoe channel) {
            uint32_t channel_id = 0;
            {
              std::lock_guard lock(mu);
              auto* tunnel = Find(id);
              if (!tunnel) {
                return;
              }
              if (!channel) {
                Object err;
                err.set("v", int64_t{1});
                err.set("ok", false);
                err.set("error", channel.error().message);
                if (tunnel->near_session) {
                  tunnel->near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
                }
                TearDown(*tunnel, false, false, channel.error().message);
                return;
              }
              if (!runtime->Links().FindLink(dial_key)) {
                TearDown(*tunnel, false, false, "relay target stream timed out");
                return;
              }
              channel_id = *channel;
            }
            ScheduleWhenChannelOpen(
                dial_key, channel_id, deadline, [this, id, dial_key, channel_id](const bool open) {
                  std::lock_guard lock(mu);
                  auto* tunnel = Find(id);
                  if (!tunnel) {
                    return;
                  }
                  if (!open) {
                    Object err;
                    err.set("v", int64_t{1});
                    err.set("ok", false);
                    err.set("error", "relay target stream timed out");
                    if (tunnel->near_session) {
                      tunnel->near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
                    }
                    TearDown(*tunnel, false, false, "relay target stream timed out");
                    return;
                  }
                  auto* link = runtime->Links().FindLink(dial_key);
                  if (!link || !link->Mux()) {
                    TearDown(*tunnel, false, false, "relay target stream timed out");
                    return;
                  }
                  ContinueServeAfterTargetOpen(*tunnel, *link, channel_id);
                });
          });
      return;
    }

    runtime->Links().EnsureAssociation(target_key, [this, id, target_key, deadline, target_protocol](
                                               pp::amp::PeerLinkManager::LinkRoe assoc) {
      {
        std::lock_guard lock(mu);
        auto* tunnel = Find(id);
        if (!tunnel) {
          return;
        }
        if (!assoc) {
          Object err;
          err.set("v", int64_t{1});
          err.set("ok", false);
          err.set("error", assoc.error().message);
          if (tunnel->near_session) {
            tunnel->near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
          }
          TearDown(*tunnel, false, false, assoc.error().message);
          return;
        }
      }
      runtime->Links().OpenChannel(
          target_key, target_protocol, PolicyForCircuitTarget(target_protocol),
          [this, id, target_key, deadline](pp::amp::PeerLinkManager::ChannelRoe channel) {
            uint32_t channel_id = 0;
            {
              std::lock_guard lock(mu);
              auto* tunnel = Find(id);
              if (!tunnel) {
                return;
              }
              if (!channel) {
                Object err;
                err.set("v", int64_t{1});
                err.set("ok", false);
                err.set("error", channel.error().message);
                if (tunnel->near_session) {
                  tunnel->near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
                }
                TearDown(*tunnel, false, false, channel.error().message);
                return;
              }
              if (!runtime->Links().FindLink(target_key)) {
                TearDown(*tunnel, false, false, "relay target stream timed out");
                return;
              }
              channel_id = *channel;
            }
            ScheduleWhenChannelOpen(target_key, channel_id, deadline, [this, id, target_key, channel_id](const bool open) {
              std::lock_guard lock(mu);
              auto* tunnel = Find(id);
              if (!tunnel) {
                return;
              }
              if (!open) {
                Object err;
                err.set("v", int64_t{1});
                err.set("ok", false);
                err.set("error", "relay target stream timed out");
                if (tunnel->near_session) {
                  tunnel->near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
                }
                TearDown(*tunnel, false, false, "relay target stream timed out");
                return;
              }
              auto* link = runtime->Links().FindLink(target_key);
              if (!link || !link->Mux()) {
                TearDown(*tunnel, false, false, "relay target stream timed out");
                return;
              }
              ContinueServeAfterTargetOpen(*tunnel, *link, channel_id);
            });
          });
    });
  }

  void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || !link.Mux()) {
      return;
    }
    auto near_session = std::make_shared<pp::amp::ChannelSession>();
    auto started_req = std::make_shared<std::atomic<bool>>(false);
    const std::string remote = link.RemotePeerId();
    near_session->Bind(*link.Mux(), channel_id, pp::amp::CircuitTunnelChannelPolicy(),
               [this, near_session, started_req, remote](Roe<std::vector<uint8_t>> frame) {
                 if (!frame || stopped.load(std::memory_order_acquire)) {
                   return false;
                 }
                 if (started_req->exchange(true, std::memory_order_acq_rel)) {
                   return true; // superseded once tunnel Bind/Bridge takes over
                 }
                 auto root = TryParseObject(BodyToJson(*frame));
                 if (!root) {
                   Object err;
                   err.set("v", int64_t{1});
                   err.set("ok", false);
                   err.set("error", "invalid circuit-relay json");
                   near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
                   return false;
                 }
                 PostIo([this, near_session, remote, root = *root]() mutable {
                   const std::string op = root.getString("op").value_or("");
                   CircuitAdmitContext admit;
                   admit.service_started = started.load(std::memory_order_acquire) &&
                                          serve_inbound.load(std::memory_order_acquire);
                   admit.stopping = stopped.load(std::memory_order_acquire);
                   admit.dialer_peer_id = remote;
                   admit.op = op;
                   {
                     std::lock_guard lock(mu);
                     admit.serve_scope_mask = admission.serve_scope_mask;
                     admit.contact_peer_ids = admission.contact_peer_ids;
                   }
                   const auto decision = DecideCircuitAdmit(admit);
                   auto refuse = [&](const std::string& message) {
                     Object err;
                     err.set("v", int64_t{1});
                     err.set("ok", false);
                     err.set("error", message);
                     near_session->EnqueueOutbound(JsonToBody(DumpJson(err)));
                     near_session->Close();
                   };
                   if (decision != CircuitAdmitDecision::Allow) {
                     refuse(decision == CircuitAdmitDecision::RefuseStranger
                                ? "relay scope: stranger refused"
                                : (decision == CircuitAdmitDecision::RefuseBadOp
                                       ? "unsupported op"
                                       : "circuit-relay service not ready"));
                     return;
                   }

                   if (op == "reserve") {
                     const int timeout_ms =
                         static_cast<int>(root.getNonNegInt("timeout_ms").value_or(30000));
                     {
                       std::lock_guard lock(mu);
                       Reservation res;
                       res.session = near_session;
                       res.deadline =
                           Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 30000);
                       reservations[remote] = std::move(res);
                     }
                     Object ack;
                     ack.set("v", int64_t{1});
                     ack.set("ok", true);
                     ack.set("op", "reserve");
                     near_session->EnqueueOutbound(JsonToBody(DumpJson(ack)));
                     // Answerer park may complete after dialer already armed ServeDial wait.
                     NotifyFarLegReady(remote);
                     return;
                   }

                   CircuitTunnelId id{};
                   {
                     std::lock_guard lock(mu);
                     auto tunnel = std::make_unique<Tunnel>();
                     tunnel->id = CircuitTunnelId{next_id.fetch_add(1, std::memory_order_relaxed)};
                     tunnel->role = CircuitTunnelRole::RelayServe;
                     tunnel->dialer_peer_id = remote;
                     // Near leg is dialer↔relay; key for PeerLinkMissing / CloseQuietSlot ResolveLink.
                     tunnel->relay_peer_key = remote;
                     tunnel->near_session = near_session;
                     tunnel->target.target_peer_id = root.getString("target_peer_id").value_or("");
                     tunnel->target.target_multiaddr = root.getString("target_multiaddr").value_or("");
                     tunnel->target.target_protocol = root.getString("target_protocol").value_or("");
                     if (tunnel->target.target_protocol.empty()) {
                       tunnel->target.target_protocol = kCircuitRelayProtocolId;
                     }
                     const int timeout_ms = static_cast<int>(root.getNonNegInt("timeout_ms").value_or(8000));
                     tunnel->deadline =
                         Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
                     id = tunnel->id;
                     tunnels[tunnel->id.value] = std::move(tunnel);
                   }
                   if (auto* tunnel = Find(id)) {
                     BeginServe(*tunnel);
                   }
                 });
                 return true;
               });
  }
};

CircuitTunnelCoordinator::CircuitTunnelCoordinator(pp::amp::MeshRuntime& runtime)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
}

CircuitTunnelCoordinator::~CircuitTunnelCoordinator() {
  Stop();
}

void CircuitTunnelCoordinator::Start() {
  if (impl_->started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  impl_->stopped.store(false, std::memory_order_release);
  impl_->io_tick_id = runtime_.AddIoTick([impl = impl_.get()] { impl->TickDeadlines(); });
  impl_->peer_connected_listener_id_ = runtime_.Links().AddPeerConnectedListener(
      [impl = impl_.get()](const std::string& peer_id) {
        if (impl) {
          impl->NotifyFarLegReady(peer_id);
        }
      });
  runtime_.Links().SetProtocolHandler(
      kCircuitRelayProtocolId,
      [impl = impl_.get()](pp::amp::LinkHandle handle, const std::string& /*remote_peer_id*/,
                           const uint32_t ch) {
        if (!impl->runtime) {
          return;
        }
        impl->runtime->Links().WithLiveLink(handle, [&](pp::amp::PeerLink& link) {
          impl->HandleInboundChannel(link, ch);
        });
      });
}

void CircuitTunnelCoordinator::Stop() {
  if (!impl_->started.load(std::memory_order_acquire) && impl_->stopped.load(std::memory_order_acquire)) {
    return;
  }
  impl_->started.store(false, std::memory_order_release);
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.RemoveIoTick(impl_->io_tick_id);
  impl_->io_tick_id = 0;
  if (impl_->peer_connected_listener_id_ != 0) {
    runtime_.Links().RemovePeerConnectedListener(impl_->peer_connected_listener_id_);
    impl_->peer_connected_listener_id_ = 0;
  }
  runtime_.Links().RemoveProtocolHandler(kCircuitRelayProtocolId);
  AbortInflight();
}

bool CircuitTunnelCoordinator::IsStarted() const {
  return impl_->started.load(std::memory_order_acquire);
}

void CircuitTunnelCoordinator::SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy) {
  std::lock_guard lock(impl_->mu);
  impl_->admission = std::move(policy);
}

void CircuitTunnelCoordinator::SetServeInbound(const bool serve) {
  impl_->serve_inbound.store(serve, std::memory_order_release);
}

bool CircuitTunnelCoordinator::ServeInbound() const {
  return impl_->serve_inbound.load(std::memory_order_acquire);
}

void CircuitTunnelCoordinator::AbortInflight() {
  impl_->PostIo([impl = impl_.get()] {
    std::lock_guard lock(impl->mu);
    std::vector<uint64_t> ids;
    for (auto& [id, _] : impl->tunnels) {
      ids.push_back(id);
    }
    for (const auto id : ids) {
      if (auto* tunnel = impl->Find(CircuitTunnelId{id})) {
        // Teardown / mesh-stop: do not Finish into CallMediaPlane / AmpCircuitHopReach
        // lambdas that may already be destroyed (hard-w5 offerer SIGSEGV after Leave).
        // Reach uses AbortPending gen; reserve cbs use CallMediaPlane async_gen_.
        tunnel->on_finished = nullptr;
        impl->TearDown(*tunnel, true, true, "circuit-relay aborted");
      }
    }
    for (auto& [peer_id, res] : impl->reservations) {
      CloseQuietSlot(res.session, impl->ResolveLink(peer_id));
    }
    impl->reservations.clear();
  });
}

CircuitTunnelId CircuitTunnelCoordinator::StartBridge(const std::string& relay_peer_key,
                                                      const CircuitBridgeTarget& target_in,
                                                      FrameHandler on_payload, ClosedCallback on_closed,
                                                      BridgeFinished on_finished, const int timeout_ms) {
  if (!impl_->started.load(std::memory_order_acquire)) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("amp circuit-relay service not started"));
      });
    }
    return {};
  }
  CircuitBridgeTarget target = target_in;
  if (target.target_multiaddr.empty() && target.target_peer_id.empty()) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("missing circuit bridge target"));
      });
    }
    return {};
  }
  if (target.target_protocol.empty()) {
    target.target_protocol = kCircuitRelayProtocolId;
  }
  if (!runtime_.Links().GetLinkSnapshot(relay_peer_key).has_endpoint) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("relay peer endpoint not registered"));
      });
    }
    return {};
  }

  const CircuitTunnelId id{impl_->next_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_.get(), id, relay_peer_key, target = std::move(target),
                 on_payload = std::move(on_payload), on_closed = std::move(on_closed),
                 on_finished = std::move(on_finished), timeout_ms]() mutable {
    CircuitTunnelCoordinator::Impl::Tunnel* raw = nullptr;
    {
      std::lock_guard lock(impl->mu);
      auto tunnel = std::make_unique<CircuitTunnelCoordinator::Impl::Tunnel>();
      tunnel->id = id;
      tunnel->role = CircuitTunnelRole::Client;
      tunnel->relay_peer_key = relay_peer_key;
      tunnel->target = std::move(target);
      tunnel->on_payload = std::move(on_payload);
      tunnel->on_closed = std::move(on_closed);
      tunnel->on_finished = std::move(on_finished);
      tunnel->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
      raw = tunnel.get();
      impl->tunnels[id.value] = std::move(tunnel);
    }
    impl->BeginOutbound(*raw);
  });
  return id;
}

CircuitTunnelId CircuitTunnelCoordinator::StartReserve(const std::string& relay_peer_key,
                                                       BridgeFinished on_finished, const int timeout_ms) {
  if (!impl_->started.load(std::memory_order_acquire)) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("amp circuit-relay service not started"));
      });
    }
    return {};
  }
  if (!runtime_.Links().GetLinkSnapshot(relay_peer_key).has_endpoint) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("relay peer endpoint not registered"));
      });
    }
    return {};
  }

  const CircuitTunnelId id{impl_->next_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_.get(), id, relay_peer_key, on_finished = std::move(on_finished),
                 timeout_ms]() mutable {
    CircuitTunnelCoordinator::Impl::Tunnel* raw = nullptr;
    {
      std::lock_guard lock(impl->mu);
      auto tunnel = std::make_unique<CircuitTunnelCoordinator::Impl::Tunnel>();
      tunnel->id = id;
      tunnel->role = CircuitTunnelRole::Client;
      tunnel->is_reserve = true;
      tunnel->relay_peer_key = relay_peer_key;
      tunnel->on_finished = std::move(on_finished);
      tunnel->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 30000);
      raw = tunnel.get();
      impl->tunnels[id.value] = std::move(tunnel);
    }
    impl->BeginOutbound(*raw);
  });
  return id;
}

void CircuitTunnelCoordinator::CancelTunnel(const CircuitTunnelId id) {
  if (!id) {
    return;
  }
  impl_->PostIo([impl = impl_.get(), id] {
    std::lock_guard lock(impl->mu);
    if (auto* tunnel = impl->Find(id)) {
      impl->TearDown(*tunnel, true, true, "circuit-relay aborted");
    }
  });
}

CircuitTunnelPhase CircuitTunnelCoordinator::Phase(const CircuitTunnelId id) const {
  std::lock_guard lock(impl_->mu);
  if (const auto* tunnel = impl_->Find(id)) {
    return tunnel->phase;
  }
  return CircuitTunnelPhase::Idle;
}

bool CircuitTunnelCoordinator::IsTunnelActive(const CircuitTunnelId id) const {
  return CircuitTunnelPhaseIsActive(Phase(id));
}

std::shared_ptr<pp::amp::ChannelSession> CircuitTunnelCoordinator::Session(const CircuitTunnelId id) const {
  std::lock_guard lock(impl_->mu);
  if (const auto* tunnel = impl_->Find(id)) {
    return tunnel->near_session;
  }
  return nullptr;
}

} // namespace pbr
