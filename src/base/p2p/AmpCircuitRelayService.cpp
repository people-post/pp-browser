#include "base/p2p/AmpCircuitRelayService.h"

#include "base/mesh/channel/ChannelBridge.h"
#include "base/mesh/channel/ChannelPolicy.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/PeerLink.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::string BodyToJson(const std::vector<uint8_t>& body) {
  return std::string(body.begin(), body.end());
}

/** Resolve dial key + multiaddr for AMP circuit target (no libp2p host). */
Roe<std::pair<std::string, std::string>> NormalizeAmpCircuitTarget(amp::PeerLinkManager& links,
                                                                   const CircuitBridgeTarget& target) {
  if (target.target_multiaddr.empty() && target.target_peer_id.empty()) {
    return Error("missing circuit bridge target");
  }
  if (!target.target_multiaddr.empty()) {
    auto parsed = amp::ParseAdpMultiaddr(target.target_multiaddr);
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
  // Peer-id only: require an already-registered endpoint under that key.
  auto snap = links.GetLinkSnapshot(target.target_peer_id);
  if (!snap.has_endpoint || snap.multiaddr.empty()) {
    return Error("circuit target peer endpoint not registered");
  }
  return std::make_pair(target.target_peer_id, snap.multiaddr);
}

} // namespace

struct AmpCircuitRelayService::Impl {
  amp::MeshRuntime* runtime = nullptr;
  IoPump io_pump;
  std::mutex mu;
  CircuitRelayAdmissionPolicy admission;
  std::atomic<bool> stopped{true};
  std::atomic<bool> aborting{false};
  std::vector<std::shared_ptr<std::atomic<bool>>> inflight;
  std::vector<std::shared_ptr<amp::ChannelBridge>> active_bridges;

  void IoPumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline && !stopped.load(std::memory_order_acquire) &&
           !aborting.load(std::memory_order_acquire)) {
      if (io_pump) {
        io_pump();
      } else if (runtime) {
        runtime->Pump();
        runtime->Tick();
      }
    }
  }

  void CancelBridgesLocked() {
    for (auto& bridge : active_bridges) {
      if (bridge) {
        bridge->Stop();
      }
    }
    active_bridges.clear();
  }

  void AbortInflightLocked() {
    aborting.store(true, std::memory_order_release);
    for (auto& flag : inflight) {
      if (flag) {
        flag->store(true, std::memory_order_release);
      }
    }
  }

  void HandleInboundChannel(amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || !link.Mux()) {
      return;
    }
    auto client = std::make_shared<amp::ChannelSession>();
    auto self = this;
    client->Bind(*link.Mux(), channel_id, amp::CircuitTunnelChannelPolicy(),
                 [self, client, remote = link.RemotePeerId()](Roe<std::vector<uint8_t>> frame) {
                   if (!frame || self->stopped.load(std::memory_order_acquire)) {
                     return false;
                   }
                   auto root = TryParseObject(BodyToJson(*frame));
                   if (!root) {
                     Object err;
                     err.set("v", int64_t{1});
                     err.set("ok", false);
                     err.set("error", "invalid circuit-relay json");
                     client->EnqueueOutbound(JsonToBody(DumpJson(err)));
                     return false;
                   }
                   self->ServeBridgeRequest(remote, client, *root);
                   return true; // stay open for tunnel forward (or Close from ServeBridge)
                 });
  }

  void ServeBridgeRequest(const std::string& dialer_peer_id, std::shared_ptr<amp::ChannelSession> client,
                          const Object& root) {
    auto fail = [client](const std::string& message) {
      Object err;
      err.set("v", int64_t{1});
      err.set("ok", false);
      err.set("error", message);
      client->EnqueueOutbound(JsonToBody(DumpJson(err)));
      client->Close();
    };

    if (root.getString("op").value_or("") != "bridge") {
      fail("unsupported op");
      return;
    }
    if (!runtime || stopped.load(std::memory_order_acquire)) {
      fail("circuit-relay service not ready");
      return;
    }

    CircuitRelayAdmissionPolicy policy;
    {
      std::lock_guard lock(mu);
      policy = admission;
    }
    if (!RelayAdmissionAllowsDialer(policy.serve_scope_mask, dialer_peer_id, policy.contact_peer_ids)) {
      fail("relay scope: stranger refused");
      return;
    }

    CircuitBridgeTarget target;
    target.target_peer_id = root.getString("target_peer_id").value_or("");
    target.target_multiaddr = root.getString("target_multiaddr").value_or("");
    target.target_protocol = root.getString("target_protocol").value_or("");
    if (target.target_protocol.empty()) {
      target.target_protocol = kCircuitRelayProtocolId;
    }

    auto normalized = NormalizeAmpCircuitTarget(runtime->Links(), target);
    if (!normalized) {
      fail(normalized.error().message);
      return;
    }
    const std::string target_key = normalized->first;
    const std::string resolved_multiaddr = normalized->second;
    const int timeout_ms = static_cast<int>(root.getNonNegInt("timeout_ms").value_or(8000));
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);

    auto assoc_done = std::make_shared<std::atomic<bool>>(false);
    auto assoc_result = std::make_shared<Roe<void>>(Error("pending"));
    runtime->Links().EnsureAssociation(target_key, [assoc_done, assoc_result](Roe<void> r) {
      *assoc_result = std::move(r);
      assoc_done->store(true, std::memory_order_release);
    });
    IoPumpUntil([&] { return assoc_done->load(std::memory_order_acquire); }, deadline);
    if (!assoc_done->load(std::memory_order_acquire)) {
      fail(aborting.load(std::memory_order_acquire) ? "circuit-relay aborted" : "relay dial timed out");
      return;
    }
    if (!*assoc_result) {
      fail(assoc_result->error().message);
      return;
    }

    auto channel_done = std::make_shared<std::atomic<bool>>(false);
    auto channel_result = std::make_shared<Roe<uint32_t>>(Error("pending"));
    runtime->Links().OpenChannel(target_key, target.target_protocol, amp::CircuitTunnelChannelPolicy(),
                                 [channel_done, channel_result](Roe<uint32_t> r) {
                                   *channel_result = std::move(r);
                                   channel_done->store(true, std::memory_order_release);
                                 });
    IoPumpUntil(
        [&] {
          if (!channel_done->load(std::memory_order_acquire)) {
            return false;
          }
          if (!*channel_result) {
            return true;
          }
          auto* link = runtime->Links().FindLink(target_key);
          return link && link->Mux() && link->Mux()->State(**channel_result) == amp::ChannelState::Open;
        },
        deadline);
    if (!channel_done->load(std::memory_order_acquire) || !*channel_result) {
      fail(aborting.load(std::memory_order_acquire) ? "circuit-relay aborted"
                                                    : (!channel_done->load(std::memory_order_acquire)
                                                           ? "relay target stream timed out"
                                                           : channel_result->error().message));
      return;
    }
    auto* target_link = runtime->Links().FindLink(target_key);
    if (!target_link || !target_link->Mux() ||
        target_link->Mux()->State(**channel_result) != amp::ChannelState::Open) {
      fail("relay target stream timed out");
      return;
    }

    auto target_session = std::make_shared<amp::ChannelSession>();
    target_session->Bind(*target_link->Mux(), **channel_result, amp::CircuitTunnelChannelPolicy(),
                         [](Roe<std::vector<uint8_t>>) { return true; });

    Object response;
    response.set("v", int64_t{1});
    response.set("ok", true);
    response.set("resolved_multiaddr", resolved_multiaddr);
    if (!client->EnqueueOutbound(JsonToBody(DumpJson(response)))) {
      fail("failed to ack bridge");
      return;
    }

    auto bridge = std::make_shared<amp::ChannelBridge>();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard lock(mu);
      active_bridges.push_back(bridge);
    }
    auto self = this;
    bridge->Attach(
        client, target_session, [cancelled]() { return cancelled->load(std::memory_order_acquire); },
        [self, bridge, cancelled]() {
          cancelled->store(true, std::memory_order_release);
          std::lock_guard lock(self->mu);
          self->active_bridges.erase(
              std::remove_if(self->active_bridges.begin(), self->active_bridges.end(),
                             [&](const std::shared_ptr<amp::ChannelBridge>& e) { return e.get() == bridge.get(); }),
              self->active_bridges.end());
        });
  }
};

AmpCircuitRelayService::AmpCircuitRelayService(amp::MeshRuntime& runtime, IoPump io_pump)
    : impl_(std::make_unique<Impl>()), runtime_(runtime), io_pump_(std::move(io_pump)) {
  impl_->runtime = &runtime_;
  impl_->io_pump = io_pump_;
}

AmpCircuitRelayService::~AmpCircuitRelayService() {
  Stop();
}

void AmpCircuitRelayService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  impl_->aborting.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(kCircuitRelayProtocolId,
                                      [impl = impl_.get()](amp::PeerLink& link, const uint32_t channel_id) {
                                        impl->HandleInboundChannel(link, channel_id);
                                      });
}

void AmpCircuitRelayService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  AbortInflightRequests();
  runtime_.Links().RemoveProtocolHandler(kCircuitRelayProtocolId);
  std::lock_guard lock(impl_->mu);
  impl_->CancelBridgesLocked();
}

void AmpCircuitRelayService::SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy) {
  std::lock_guard lock(impl_->mu);
  impl_->admission = std::move(policy);
}

void AmpCircuitRelayService::AbortInflightRequests() {
  std::lock_guard lock(impl_->mu);
  impl_->AbortInflightLocked();
}

Roe<AmpCircuitRelayBridgeResult> AmpCircuitRelayService::RequestBridge(
    const std::string& relay_peer_key, const CircuitBridgeTarget& target_in, FrameHandler on_payload,
    ClosedCallback on_closed, int timeout_ms) {
  AmpCircuitRelayBridgeResult out;
  if (!started_) {
    return Error("amp circuit-relay service not started");
  }
  CircuitBridgeTarget target = target_in;
  if (target.target_multiaddr.empty() && target.target_peer_id.empty()) {
    return Error("missing circuit bridge target");
  }
  if (target.target_protocol.empty()) {
    target.target_protocol = kCircuitRelayProtocolId;
  }

  Object request;
  request.set("v", int64_t{1});
  request.set("op", "bridge");
  request.set("timeout_ms", int64_t{timeout_ms > 0 ? timeout_ms : 8000});
  if (!target.target_peer_id.empty()) {
    request.set("target_peer_id", target.target_peer_id);
  }
  if (!target.target_multiaddr.empty()) {
    request.set("target_multiaddr", target.target_multiaddr);
  }
  if (!target.target_protocol.empty()) {
    request.set("target_protocol", target.target_protocol);
  }

  const auto wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto abort_flag = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard lock(impl_->mu);
    impl_->inflight.push_back(abort_flag);
  }
  auto result_promise = std::make_shared<std::promise<Roe<AmpCircuitRelayBridgeResult>>>();
  auto result_future = result_promise->get_future();
  auto session = std::make_shared<amp::ChannelSession>();

  auto finish = [settled, result_promise, abort_flag, impl = impl_.get()](Roe<AmpCircuitRelayBridgeResult> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    {
      std::lock_guard lock(impl->mu);
      impl->inflight.erase(std::remove_if(impl->inflight.begin(), impl->inflight.end(),
                                          [&](const std::shared_ptr<std::atomic<bool>>& e) {
                                            return e.get() == abort_flag.get();
                                          }),
                           impl->inflight.end());
    }
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  runtime_.Links().OpenChannel(relay_peer_key, kCircuitRelayProtocolId, amp::CircuitTunnelChannelPolicy(),
                               [this, relay_peer_key, request, session, finish, settled, abort_flag, deadline,
                                on_payload = std::move(on_payload),
                                on_closed = std::move(on_closed)](Roe<uint32_t> channel) mutable {
                                 if (abort_flag->load(std::memory_order_acquire)) {
                                   finish(Error("circuit-relay aborted"));
                                   return;
                                 }
                                 if (!channel) {
                                   finish(channel.error());
                                   return;
                                 }
                                 impl_->IoPumpUntil(
                                     [&] {
                                       if (abort_flag->load(std::memory_order_acquire)) {
                                         return true;
                                       }
                                       auto* link = runtime_.Links().FindLink(relay_peer_key);
                                       return link && link->Mux() &&
                                              link->Mux()->State(*channel) == amp::ChannelState::Open;
                                     },
                                     deadline);
                                 if (abort_flag->load(std::memory_order_acquire)) {
                                   finish(Error("circuit-relay aborted"));
                                   return;
                                 }
                                 auto* link = runtime_.Links().FindLink(relay_peer_key);
                                 if (!link || !link->Mux() ||
                                     link->Mux()->State(*channel) != amp::ChannelState::Open) {
                                   finish(Error("amp circuit-relay: channel open failed"));
                                   return;
                                 }

                                 session->Bind(
                                     *link->Mux(), *channel, amp::CircuitTunnelChannelPolicy(),
                                     [finish, session, on_payload = std::move(on_payload),
                                      on_closed = std::move(on_closed)](Roe<std::vector<uint8_t>> frame) mutable {
                                       if (!frame) {
                                         finish(Error("circuit-relay bridge failed"));
                                         return false;
                                       }
                                       auto root = TryParseObject(BodyToJson(*frame));
                                       if (!root) {
                                         finish(Error("invalid circuit-relay ack"));
                                         return false;
                                       }
                                       AmpCircuitRelayBridgeResult result;
                                       result.ok = root->getBool("ok").value_or(false);
                                       result.error = root->getString("error").value_or("");
                                       result.resolved_multiaddr =
                                           root->getString("resolved_multiaddr").value_or("");
                                       if (!result.ok) {
                                         finish(Error(result.error.empty() ? "circuit-relay bridge refused"
                                                                           : result.error));
                                         return false;
                                       }
                                       result.session = session;
                                       if (on_payload) {
                                         session->SetFrameHandler(std::move(on_payload));
                                       } else {
                                         session->SetFrameHandler([](Roe<std::vector<uint8_t>>) { return true; });
                                       }
                                       if (on_closed) {
                                         session->SetClosedCallback(std::move(on_closed));
                                       }
                                       finish(std::move(result));
                                       return true;
                                     });

                                 if (!session->EnqueueOutbound(JsonToBody(DumpJson(request)))) {
                                   finish(Error("failed to send circuit-relay bridge request"));
                                   return;
                                 }

                                 impl_->IoPumpUntil(
                                     [settled, abort_flag] {
                                       return settled->load(std::memory_order_acquire) ||
                                              abort_flag->load(std::memory_order_acquire);
                                     },
                                     deadline);
                                 if (!settled->load(std::memory_order_acquire)) {
                                   if (abort_flag->load(std::memory_order_acquire)) {
                                     finish(Error("circuit-relay aborted"));
                                   } else {
                                     finish(Error("circuit-relay bridge timed out"));
                                   }
                                 }
                               });

  impl_->IoPumpUntil(
      [&] {
        return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready ||
               abort_flag->load(std::memory_order_acquire);
      },
      deadline);

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    abort_flag->store(true, std::memory_order_release);
    session->CloseQuiet();
    finish(Error("circuit-relay bridge timed out"));
  }
  return result_future.get();
}

} // namespace pbr
