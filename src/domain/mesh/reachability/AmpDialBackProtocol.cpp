#include "domain/mesh/reachability/AmpDialBackProtocol.h"

#include "domain/mesh/l4/shared/InboundReply.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"
#include "domain/mesh/shared/AmpChannelOpen.h"
#include "domain/mesh/shared/AmpParkUntil.h"
#include "foundation/runtime/DeferredSelf.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpDialBackProtocol::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

std::chrono::milliseconds RemainingTimeout(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

DialBackProbeResult DialAmpTargets(pp::amp::PeerLinkManager& links, AmpDialBackProtocol::IoPump io_pump,
                                   const std::vector<std::string>& targets, int timeout_ms) {
  DialBackProbeResult out;
  if (targets.empty()) {
    out.error = "no target_multiaddrs";
    return out;
  }
  const int timeout = timeout_ms > 0 ? timeout_ms : 8000;
  for (size_t i = 0; i < targets.size(); ++i) {
    const std::string& ma = targets[i];
    if (ma.empty()) {
      continue;
    }
    if (!pp::amp::ParseAdpMultiaddr(ma)) {
      out.error = "target is not an ADP multiaddr";
      out.dialed = ma;
      continue;
    }
    const std::string key = "dialback:probe:" + std::to_string(i);
    if (auto registered = links.RegisterEndpoint(key, ma); !registered) {
      out.error = registered.error().message;
      out.dialed = ma;
      continue;
    }

    SettledWait<void> wait;
    links.EnsureAssociation(key, [wait](pp::amp::PeerLinkManager::LinkRoe result) {
      if (result) {
        wait.Finish(Roe<void>());
      } else {
        wait.Finish(Roe<void>(Error(AmpDialBackProtocol::WrapLinkFailure(result.error()).message)));
      }
    });
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump);
    auto dialed = wait.Wait(std::chrono::milliseconds(1), Error("dial-back timed out"));
    if (dialed) {
      out.ok = true;
      out.dialed = ma;
      out.error.clear();
      return out;
    }
    out.error = dialed.error().message;
    out.dialed = ma;
  }
  return out;
}

} // namespace

AmpDialBackProtocol::Failure AmpDialBackProtocol::WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child) {
  switch (child.GetCode()) {
    case pp::amp::PeerLinkManager::Err::EndpointNotRegistered:
      return Failure::Of(Err::EndpointNotRegistered,
                         detail::AppendFrom("dial-back: endpoint not registered", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialTimeout:
      return Failure::Of(Err::Timeout, detail::AppendFrom("dial-back: dial timed out", "link", child.message));
    case pp::amp::PeerLinkManager::Err::ChannelOpenFailed:
      return Failure::Of(Err::ChannelFailed,
                         detail::AppendFrom("dial-back: channel open failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialInBackoff:
    case pp::amp::PeerLinkManager::Err::TooManyConcurrentDials:
    case pp::amp::PeerLinkManager::Err::MaxLinksReached:
    case pp::amp::PeerLinkManager::Err::AssociationNotReady:
    case pp::amp::PeerLinkManager::Err::LinkNotFound:
    case pp::amp::PeerLinkManager::Err::NestedCarrierIncomplete:
    case pp::amp::PeerLinkManager::Err::HandshakeFailed:
    case pp::amp::PeerLinkManager::Err::TransportFailed:
    case pp::amp::PeerLinkManager::Err::DualDialLost:
      return Failure::Of(Err::LinkFailed, detail::AppendFrom("dial-back: link failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::Ok:
    case pp::amp::PeerLinkManager::Err::Generic:
    default:
      return Failure::Of(Err::Generic, detail::AppendFrom("dial-back: link error", "link", child.message));
  }
}

struct AmpDialBackProtocol::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § DeferredSelf. */
  DeferredSelf deferred;

  pp::amp::PeerLinkManager& Links() { return runtime->Links(); }
  /** IO lane for InboundReply (MeshHost::Stop joins MeshControl before freeing the runtime). */
  InboundReply::IoPost IoPost() {
    return [rt = runtime](std::function<void()> task) { rt->PostToIo(std::move(task)); };
  }

  void ScheduleWhenChannelOpen(const std::string& peer_key, const uint32_t channel_id,
                               const Clock::time_point deadline, std::function<void(bool open)> done) {
    if (!runtime || peer_key.empty()) {
      done(false);
      return;
    }
    AmpWhenChannelOpen(Links(), peer_key, channel_id, deadline, std::move(done));
  }

  /**
   * B26: the seed's view of the client's Amp UDP endpoint on this association. Dialing the
   * client's LAN advertise addrs often fails cross-NAT; the observed reflexive address is what
   * peers need to dial. IO strand only (link state is IO-affine).
   */
  std::string ObservedMultiaddrOnIo(const std::string& remote_peer_id) {
    if (auto* link = Links().FindLink(remote_peer_id)) {
      if (auto* conn = link->ConnectionOrNull()) {
        const auto ep = conn->PeerEndpoint();
        if (ep.port != 0) {
          if (auto ma = pp::amp::FormatAdpMultiaddr(ep, remote_peer_id)) {
            return *ma;
          }
        }
      }
    }
    return {};
  }

  void ServeProbe(std::shared_ptr<InboundReply> reply, std::string observed, std::vector<uint8_t> body) {
    RunWorker(post_worker, [this, reply, observed = std::move(observed), body = std::move(body)]() mutable {
      if (stopped.load(std::memory_order_acquire) || !runtime) {
        return;
      }
      DialBackProbeResult result;
      result.observed = std::move(observed);
      const std::string json_utf8(body.begin(), body.end());
      auto root = TryParseObject(json_utf8);
      if (!root) {
        result.error = "invalid dial-back json";
      } else {
        const std::string op = root->getString("op").value_or("");
        if (op != "probe") {
          result.error = "unsupported op";
        } else {
          std::vector<std::string> targets;
          if (const Array* addrs = root->getArray("target_multiaddrs")) {
            for (const auto& item : addrs->elements) {
              if (auto s = asString(item)) {
                targets.push_back(*s);
              }
            }
          }
          const int timeout_ms = static_cast<int>(root->getNonNegInt("timeout_ms").value_or(8000));
          auto dialed = DialAmpTargets(Links(), io_pump, targets, timeout_ms);
          result.ok = dialed.ok;
          result.dialed = std::move(dialed.dialed);
          result.error = std::move(dialed.error);
        }
      }
      Object response;
      response.set("v", int64_t{1});
      response.set("ok", result.ok);
      response.set("dialed", result.dialed);
      response.set("observed", result.observed);
      response.set("error", result.error);
      reply->Send(JsonToBody(DumpJson(response)));
    });
  }

  void HandleInboundOnLink(pp::amp::LinkHandle /*handle*/, const std::string& remote_peer_id,
                           const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || remote_peer_id.empty()) {
      return;
    }
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    *session_holder = Links().BindChannel(
        remote_peer_id, channel_id, InboundReplyPolicy(pp::amp::ControlJsonChannelPolicy()),
        [this, session_holder, remote_peer_id](Roe<std::vector<uint8_t>> frame) {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          // Keep the channel open for the worker's reply (InboundReply.h); `reply` closes it.
          ServeProbe(MakeInboundReply(session, IoPost()), ObservedMultiaddrOnIo(remote_peer_id),
                     std::move(*frame));
          return true;
        });
  }
};

AmpDialBackProtocol::AmpDialBackProtocol(pp::amp::MeshRuntime& runtime, IoPump io_pump, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), runtime_(runtime), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->runtime = &runtime_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
}

AmpDialBackProtocol::~AmpDialBackProtocol() { Stop(); }

void AmpDialBackProtocol::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(
      kDialBackProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle,
                                                const std::string& remote_peer_id,
                                                const uint32_t channel_id) {
        impl->HandleInboundOnLink(handle, remote_peer_id, channel_id);
      }));
}

void AmpDialBackProtocol::Stop() {
  // Idempotent: the destructor Stops again, possibly after MeshHost::Stop freed the runtime.
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.Links().RemoveProtocolHandler(kDialBackProtocolId);
  impl_->deferred.Invalidate();
}

void AmpDialBackProtocol::ProbeAsync(const std::string& seed_peer_key,
                                    const std::vector<std::string>& target_multiaddrs,
                                    std::function<void(ProbeRoe)> on_done, int timeout_ms) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(ProbeRoe)>>();
  *finish_once = [on_done = std::move(on_done), settled](ProbeRoe value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (!started_) {
    (*finish_once)(ProbeRoe::error(Failure::Of(Err::NotStarted, "amp dial-back service not started")));
    return;
  }
  if (!runtime_.Links().GetLinkSnapshot(seed_peer_key).has_endpoint) {
    (*finish_once)(ProbeRoe::error(Failure::Of(Err::EndpointNotRegistered, "seed peer endpoint not registered")));
    return;
  }
  if (target_multiaddrs.empty()) {
    (*finish_once)(ProbeRoe::error(Failure::Of(Err::InvalidRequest, "no target_multiaddrs")));
    return;
  }

  Object request;
  request.set("v", int64_t{1});
  request.set("op", "probe");
  std::vector<Value> addrs;
  addrs.reserve(target_multiaddrs.size());
  for (const auto& ma : target_multiaddrs) {
    addrs.emplace_back(ma);
  }
  request.set("target_multiaddrs", ArrayValue(std::move(addrs)));
  request.set("timeout_ms", int64_t{timeout_ms > 0 ? timeout_ms : 8000});
  const std::string request_json = DumpJson(request);

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  auto session = std::make_shared<pp::amp::ChannelSession>();

  // Shared so ChannelSession / PostToIo callbacks can invoke from const contexts.
  auto finish = std::make_shared<std::function<void(ProbeRoe)>>();
  *finish = [finish_once, session](ProbeRoe value) {
    session->Close();
    (*finish_once)(std::move(value));
  };

  const auto read_timeout = RemainingTimeout(deadline);
  runtime_.Links().EnsureAssociation(seed_peer_key, [this, seed_peer_key, request_json, finish, session, deadline,
                                           read_timeout, settled](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
    if (!assoc) {
      (*finish)(ProbeRoe::error(WrapLinkFailure(assoc.error())));
      return;
    }
    runtime_.Links().OpenChannel(seed_peer_key, kDialBackProtocolId, pp::amp::ControlJsonChannelPolicy(read_timeout),
                       [this, seed_peer_key, request_json, finish, session, deadline, read_timeout,
                        settled](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                         if (!channel) {
                           (*finish)(ProbeRoe::error(WrapLinkFailure(channel.error())));
                           return;
                         }
                         impl_->ScheduleWhenChannelOpen(
                             seed_peer_key, *channel, deadline,
                             [this, seed_peer_key, channel_id = *channel, request_json, finish, session, deadline,
                              read_timeout, settled](bool open) mutable {
                               if (!open) {
                                 (*finish)(ProbeRoe::error(
                                     Failure::Of(Err::ChannelFailed, "amp dial-back: channel open failed")));
                                 return;
                               }
                               auto* link = runtime_.Links().FindLink(seed_peer_key);
                               if (!link || !link->Mux() ||
                                   link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
                                 (*finish)(ProbeRoe::error(
                                     Failure::Of(Err::ChannelFailed, "amp dial-back: channel open failed")));
                                 return;
                               }
                               session->Bind(*link->Mux(), channel_id, pp::amp::ControlJsonChannelPolicy(read_timeout),
                                             [finish](Roe<std::vector<uint8_t>> frame) {
                                               if (!frame) {
                                                 (*finish)(ProbeRoe::error(Failure::Of(
                                                     Err::ProtocolError, "Failed to read dial-back response")));
                                                 return false;
                                               }
                                               auto root =
                                                   TryParseObject(std::string(frame->begin(), frame->end()));
                                               if (!root) {
                                                 (*finish)(ProbeRoe::error(Failure::Of(
                                                     Err::ProtocolError, "invalid dial-back response")));
                                                 return false;
                                               }
                                               DialBackProbeResult parsed;
                                               parsed.ok = root->getIf<bool>("ok").value_or(false);
                                               parsed.dialed = root->getString("dialed").value_or("");
                                               parsed.observed = root->getString("observed").value_or("");
                                               parsed.error = root->getString("error").value_or("");
                                               (*finish)(parsed);
                                               return false;
                                             });
                               if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                                 (*finish)(ProbeRoe::error(
                                     Failure::Of(Err::ProtocolError, "Failed to send dial-back probe")));
                                 return;
                               }
                               const auto attempt_ms =
                                   std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                               if (attempt_ms.count() > 0) {
                                 runtime_.PostAfter(attempt_ms, [finish, settled]() {
                                   if (!settled->load(std::memory_order_acquire)) {
                                     (*finish)(ProbeRoe::error(
                                         Failure::Of(Err::Timeout, "dial-back probe timed out")));
                                   }
                                 });
                               }
                             });
                       });
  });
}

AmpDialBackProtocol::ProbeRoe AmpDialBackProtocol::Probe(const std::string& seed_peer_key,
                                                         const std::vector<std::string>& target_multiaddrs,
                                                         int timeout_ms) {
  SettledWait<DialBackProbeResult, Failure> wait;
  ProbeAsync(seed_peer_key, target_multiaddrs, [wait](ProbeRoe value) { wait.Finish(std::move(value)); },
             timeout_ms);

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "dial-back probe timed out"));
}

} // namespace pbr
