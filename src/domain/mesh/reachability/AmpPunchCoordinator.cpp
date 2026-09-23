#include "domain/mesh/reachability/AmpPunchCoordinator.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "domain/mesh/reachability/PunchBurst.h"
#include "domain/mesh/reachability/PunchLogic.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return {json_utf8.begin(), json_utf8.end()};
}

std::chrono::milliseconds RemainingTimeout(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

pp::amp::ChannelPolicy PunchJsonChannelPolicy(std::chrono::milliseconds read_timeout) {
  auto policy = pp::amp::ControlJsonChannelPolicy(read_timeout);
  policy.read_once = false;
  return policy;
}

std::string MakeEpochId(const std::string& local_peer_id) {
  const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
                         Clock::now().time_since_epoch())
                         .count();
  return "ep-" + local_peer_id.substr(0, std::min<size_t>(local_peer_id.size(), 8)) + "-" +
         std::to_string(ticks);
}

void PublishIfPunchConnected(pp::amp::PeerLinkManager& links, const std::string& known_peer_id,
                             PunchBurstResult& burst) {
  std::string peer_id = known_peer_id;
  if (peer_id.empty() && !burst.dialed.empty()) {
    if (auto parsed = pp::amp::ParseAdpMultiaddr(burst.dialed)) {
      peer_id = parsed->peer_id;
    }
  }
  if (!burst.ok && PeerAlreadyConnectedDirect(links, peer_id)) {
    burst.ok = true;
  }
  if (!burst.ok) {
    return;
  }
  std::string winner = burst.dialed;
  if (winner.empty() && !peer_id.empty()) {
    if (auto ma = links.PreferredMultiaddr(peer_id)) {
      winner = *ma;
    }
  }
  // Last resort: any endpoint record whose peer_id matches the authenticated PeerId.
  if (winner.empty() && !peer_id.empty()) {
    if (auto* link = links.FindLinkByPeerId(peer_id)) {
      if (auto ma = links.PreferredMultiaddr(link->PeerKey())) {
        winner = *ma;
      }
    }
  }
  if (winner.empty()) {
    return;
  }
  PublishPunchWinnerAddrs(links, peer_id, winner);
  burst.dialed = winner;
}


PunchBurstResult ToPunchBurst(pp::amp::BurstDialResult r) {
  PunchBurstResult out;
  out.ok = r.ok;
  out.dialed = std::move(r.dialed);
  out.error = std::move(r.error);
  return out;
}

std::string ResolvePeerKey(pp::amp::PeerLinkManager& links, const std::string& peer_id) {
  if (peer_id.empty()) {
    return {};
  }
  if (auto* link = links.FindLinkByPeerId(peer_id)) {
    return link->PeerKey();
  }
  if (links.GetLinkSnapshot(peer_id).has_endpoint) {
    return peer_id;
  }
  if (auto ma = links.PreferredMultiaddr(peer_id)) {
    (void)links.RegisterEndpoint(peer_id, *ma);
    return peer_id;
  }
  return {};
}

} // namespace

AmpPunchCoordinator::Failure AmpPunchCoordinator::WrapLinkFailure(
    const pp::amp::PeerLinkManager::Failure& child) {
  switch (child.GetCode()) {
    case pp::amp::PeerLinkManager::Err::EndpointNotRegistered:
      return Failure::Of(Err::EndpointNotRegistered,
                         detail::AppendFrom("punch: endpoint not registered", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialTimeout:
      return Failure::Of(Err::Timeout, detail::AppendFrom("punch: dial timed out", "link", child.message));
    case pp::amp::PeerLinkManager::Err::ChannelOpenFailed:
      return Failure::Of(Err::ChannelFailed,
                         detail::AppendFrom("punch: channel open failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DualDialLost:
      return Failure::Of(Err::PunchFailed, detail::AppendFrom("punch: dual-dial lost", "link", child.message));
    case pp::amp::PeerLinkManager::Err::DialInBackoff:
    case pp::amp::PeerLinkManager::Err::TooManyConcurrentDials:
    case pp::amp::PeerLinkManager::Err::MaxLinksReached:
    case pp::amp::PeerLinkManager::Err::AssociationNotReady:
    case pp::amp::PeerLinkManager::Err::LinkNotFound:
    case pp::amp::PeerLinkManager::Err::NestedCarrierIncomplete:
    case pp::amp::PeerLinkManager::Err::HandshakeFailed:
    case pp::amp::PeerLinkManager::Err::TransportFailed:
      return Failure::Of(Err::LinkFailed, detail::AppendFrom("punch: link failed", "link", child.message));
    case pp::amp::PeerLinkManager::Err::Ok:
    case pp::amp::PeerLinkManager::Err::Generic:
    default:
      return Failure::Of(Err::Generic, detail::AppendFrom("punch: link error", "link", child.message));
  }
}

struct AmpPunchCoordinator::Impl {
  pp::amp::MeshRuntime* runtime = nullptr;
  IoPump io_pump; // AmpParkUntil only — never call from SM / PostToIo work
  AmpPunchCoordinator::ProbeInbound probe_inbound;
  std::atomic<bool> stopped{false};
  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § DeferredSelf. */
  DeferredSelf deferred;
  std::vector<std::string>* local_addrs = nullptr;

  pp::amp::PeerLinkManager& Links() { return runtime->Links(); }

  std::function<void(std::function<void()>)> MakePostIo() {
    return [this](std::function<void()> fn) { PostStrand(std::move(fn)); };
  }

  /** Non-teardown SM work on MeshRuntime IO strand (connect/offer continuations). */
  void PostStrand(std::function<void()> fn) {
    if (!fn) {
      return;
    }
    if (runtime) {
      runtime->PostToIo(std::move(fn));
      return;
    }
    fn();
  }

  /** Teardown lane — Abort / Close / on_done (MeshRuntime::PostDeferred). */
  void PostDeferred(std::function<void()> fn) {
    if (!fn) {
      return;
    }
    if (runtime) {
      runtime->PostDeferred(std::move(fn));
      return;
    }
    fn();
  }

  void FailSession(const std::shared_ptr<pp::amp::ChannelSession>& session, const std::string& epoch_id,
                   const std::string& error) {
    PunchResult result;
    result.epoch_id = epoch_id;
    result.ok = false;
    result.error = error;
    (void)session->EnqueueOutbound(JsonToBody(EncodePunchResult(result)));
    PostDeferred([session]() {
      if (session) {
        session->Close();
      }
    });
  }

  void SendSyncPair(const std::shared_ptr<pp::amp::ChannelSession>& initiator_session,
                    const std::shared_ptr<pp::amp::ChannelSession>& target_session,
                    const std::string& epoch_id, int window_ms, const PunchConnectRequest& req,
                    const PunchCandidates& candidates) {
    PunchSync sync_to_initiator;
    sync_to_initiator.epoch_id = epoch_id;
    sync_to_initiator.peer_addrs = SanitizePunchAddrs(candidates.addrs);
    sync_to_initiator.window_ms = window_ms;

    PunchSync sync_to_target;
    sync_to_target.epoch_id = epoch_id;
    sync_to_target.peer_addrs = SanitizePunchAddrs(req.addrs);
    sync_to_target.window_ms = window_ms;

    if (!initiator_session->EnqueueOutbound(JsonToBody(EncodePunchSync(sync_to_initiator)))) {
      FailSession(initiator_session, epoch_id, "punch: failed to send sync to initiator");
      return;
    }
    if (target_session && !target_session->IsClosed()) {
      if (!target_session->EnqueueOutbound(JsonToBody(EncodePunchSync(sync_to_target)))) {
        FailSession(initiator_session, epoch_id, "punch: failed to send sync to target");
        return;
      }
    }
    // Do not call IoPump/Tick here — next exclusive Drive flushes outbound.
  }

  /**
   * Introducer side of ACP: open punch channel to target, offer, then sync both ends.
   * Fully async — no AmpParkUntil. Continuations run on the IO strand via callbacks / PostStrand.
   */
  void RunIntroducerConnect(const std::shared_ptr<pp::amp::ChannelSession>& initiator_session,
                            const std::string& initiator_peer_id, PunchConnectRequest req) {
    if (stopped.load(std::memory_order_acquire) || !runtime) {
      return;
    }
    const int window_ms = req.window_ms > 0 ? req.window_ms : 2000;
    const std::string epoch_id = MakeEpochId(Links().LocalPeerId());
    const auto deadline = Clock::now() + std::chrono::milliseconds(window_ms + 3000);

    const std::string target_key = ResolvePeerKey(Links(), req.target_peer_id);
    if (target_key.empty()) {
      FailSession(initiator_session, epoch_id, "punch: target endpoint unknown to introducer");
      return;
    }

    auto target_session = std::make_shared<pp::amp::ChannelSession>();
    auto target_settled = std::make_shared<std::atomic<bool>>(false);
    auto finish_candidates =
        [this, target_settled, target_session, initiator_session, epoch_id, window_ms,
         req](CodedRoe<PunchCandidates, Err> value) {
          if (target_settled->exchange(true, std::memory_order_acq_rel)) {
            return;
          }
          // Stay on strand: candidate frames may arrive under mux — continue via PostStrand.
          PostStrand([this, target_session, initiator_session, epoch_id, window_ms, req,
                      value = std::move(value)]() mutable {
            if (stopped.load(std::memory_order_acquire) || !runtime) {
              target_session->Close();
              return;
            }
            if (!value) {
              target_session->Close();
              FailSession(initiator_session, epoch_id, value.error().message);
              return;
            }
            if (SanitizePunchAddrs(value->addrs).empty()) {
              target_session->Close();
              FailSession(initiator_session, epoch_id, "punch: target returned no candidates");
              return;
            }
            SendSyncPair(initiator_session, target_session, epoch_id, window_ms, req, *value);
          });
        };

    const auto read_timeout = RemainingTimeout(deadline);
    Links().EnsureAssociation(
        target_key, [this, target_key, initiator_peer_id, req, epoch_id, window_ms, finish_candidates,
                     target_session, deadline, read_timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
          if (!assoc) {
            finish_candidates(CodedRoe<PunchCandidates, Err>::error(WrapLinkFailure(assoc.error())));
            return;
          }
          Links().OpenChannel(
              target_key, kAmpPunchProtocolId, PunchJsonChannelPolicy(read_timeout),
              [this, target_key, initiator_peer_id, req, epoch_id, window_ms, finish_candidates, target_session,
               deadline, read_timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                if (!channel) {
                  finish_candidates(CodedRoe<PunchCandidates, Err>::error(WrapLinkFailure(channel.error())));
                  return;
                }
                AmpScheduleWhenChannelOpen(
                    MakePostIo(), io_pump,
                    [this, target_key, channel_id = *channel]() {
                      auto* link = Links().FindLink(target_key);
                      return link && link->Mux() &&
                             link->Mux()->State(channel_id) == pp::amp::ChannelState::Open;
                    },
                    deadline,
                    [this, target_key, channel_id = *channel, initiator_peer_id, req, epoch_id, window_ms,
                     finish_candidates, target_session, read_timeout](bool open) mutable {
                      if (!open) {
                        finish_candidates(CodedRoe<PunchCandidates, Err>::error(
                            Failure::Of(Err::ChannelFailed, "punch: target channel open failed")));
                        return;
                      }
                      auto* link = Links().FindLink(target_key);
                      if (!link || !link->Mux() ||
                          link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
                        finish_candidates(CodedRoe<PunchCandidates, Err>::error(
                            Failure::Of(Err::ChannelFailed, "punch: target channel open failed")));
                        return;
                      }
                      target_session->Bind(
                          *link->Mux(), channel_id, PunchJsonChannelPolicy(read_timeout),
                          [finish_candidates](Roe<std::vector<uint8_t>> frame) {
                            if (!frame) {
                              finish_candidates(CodedRoe<PunchCandidates, Err>::error(
                                  Failure::Of(Err::ProtocolError, "punch: failed to read target candidates")));
                              return false;
                            }
                            auto root = TryParseObject(std::string(frame->begin(), frame->end()));
                            if (!root) {
                              finish_candidates(CodedRoe<PunchCandidates, Err>::error(
                                  Failure::Of(Err::ProtocolError, "punch: invalid target candidates json")));
                              return false;
                            }
                            auto decoded = DecodePunchCandidates(*root);
                            if (!decoded) {
                              finish_candidates(CodedRoe<PunchCandidates, Err>::error(
                                  Failure::Of(Err::ProtocolError, "punch: target candidates decode failed")));
                              return false;
                            }
                            finish_candidates(*decoded);
                            return true; // keep open for sync
                          });

                      PunchOffer offer;
                      offer.initiator_peer_id = initiator_peer_id;
                      offer.addrs = SanitizePunchAddrs(req.addrs);
                      offer.epoch_id = epoch_id;
                      offer.window_ms = window_ms;
                      if (!target_session->EnqueueOutbound(JsonToBody(EncodePunchOffer(offer)))) {
                        finish_candidates(CodedRoe<PunchCandidates, Err>::error(
                            Failure::Of(Err::ProtocolError, "punch: failed to send offer")));
                        return;
                      }
                    },
                    [this]() { return stopped.load(std::memory_order_acquire); });
              });
        });
  }

  void RunBurstAndReply(const std::shared_ptr<pp::amp::ChannelSession>& session, PunchSync sync,
                        std::string remote_peer_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime) {
      return;
    }
    for (const std::string& ma : SanitizePunchAddrs(sync.peer_addrs)) {
      if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
        if (!parsed->peer_id.empty()) {
          (void)Links().RegisterEndpoint(parsed->peer_id, ma);
        }
      }
    }
    auto complete = std::make_shared<std::function<void(PunchBurstResult)>>();
    *complete = [this, session, sync, remote_peer_id](PunchBurstResult burst) mutable {
      if (stopped.load(std::memory_order_acquire) || !runtime || !session) {
        return;
      }
      std::string remote_id = remote_peer_id;
      if (remote_id.empty()) {
        for (const std::string& ma : sync.peer_addrs) {
          if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
            remote_id = parsed->peer_id;
            break;
          }
        }
      }
      PublishIfPunchConnected(Links(), remote_id, burst);
      PunchResult result;
      result.epoch_id = sync.epoch_id;
      result.ok = burst.ok;
      result.winner_multiaddr = burst.dialed;
      result.error = burst.ok ? "" : burst.error;
      (void)session->EnqueueOutbound(JsonToBody(EncodePunchResult(result)));
      PostDeferred([session]() {
        if (session) {
          session->Close();
        }
      });
    };
    const int window = sync.window_ms > 0 ? sync.window_ms : 2000;
    runtime->BurstDial(sync.peer_addrs, std::chrono::milliseconds(window),
                       [complete](pp::amp::BurstDialResult r) { (*complete)(ToPunchBurst(std::move(r))); });
  }

  void HandleInboundFrame(const std::shared_ptr<pp::amp::ChannelSession>& session,
                          const std::string& remote_peer_id, const std::shared_ptr<std::string>& phase,
                          const std::shared_ptr<std::string>& punch_remote_peer_id,
                          const std::string& json_utf8) {
    if (stopped.load(std::memory_order_acquire) || !runtime || !session) {
      return;
    }
    auto root = TryParseObject(json_utf8);
    if (!root) {
      return;
    }
    const std::string op = PunchOp(*root).value_or("");
    if (op == "probe" && *phase == "await_first") {
      if (probe_inbound) {
        *phase = "probe";
        probe_inbound(session, std::vector<uint8_t>(json_utf8.begin(), json_utf8.end()));
      }
      return;
    }
    if (op == "connect" && *phase == "await_first") {
      auto req = DecodePunchConnect(*root);
      if (!req) {
        FailSession(session, "", "punch: invalid connect");
        return;
      }
      *phase = "introducing";
      RunIntroducerConnect(session, remote_peer_id, *req);
      return;
    }
    if (op == "offer" && *phase == "await_first") {
      auto offer = DecodePunchOffer(*root);
      if (!offer) {
        FailSession(session, "", "punch: invalid offer");
        return;
      }
      *phase = "await_sync";
      *punch_remote_peer_id = offer->initiator_peer_id;
      PunchCandidates reply;
      reply.peer_id = Links().LocalPeerId();
      reply.addrs = local_addrs ? SanitizePunchAddrs(*local_addrs) : std::vector<std::string>{};
      reply.nonce = offer->epoch_id;
      if (!session->EnqueueOutbound(JsonToBody(EncodePunchCandidates(reply)))) {
        FailSession(session, offer->epoch_id, "punch: failed to send candidates");
        return;
      }
      return;
    }
    if (op == "sync" && *phase == "await_sync") {
      auto sync = DecodePunchSync(*root);
      if (!sync) {
        FailSession(session, "", "punch: invalid sync");
        return;
      }
      *phase = "bursting";
      RunBurstAndReply(session, *sync, *punch_remote_peer_id);
      return;
    }
  }

  void HandleInboundOnLink(pp::amp::LinkHandle /*handle*/, const std::string& remote_peer_id_in,
                           uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !runtime || remote_peer_id_in.empty()) {
      return;
    }
    const std::string remote_peer_id = remote_peer_id_in;
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    auto phase = std::make_shared<std::string>("await_first");
    auto punch_remote_peer_id = std::make_shared<std::string>();
    *session_holder = Links().BindChannel(
        remote_peer_id, channel_id, PunchJsonChannelPolicy(std::chrono::milliseconds{8000}),
        [this, session_holder, remote_peer_id, phase, punch_remote_peer_id](Roe<std::vector<uint8_t>> frame) {
          auto session = *session_holder;
          if (!session || !frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          const std::string json_utf8(frame->begin(), frame->end());
          // Leave the mux stack before any dial/park/burst (Windows nested Drive UAF).
          PostStrand([this, session, remote_peer_id, phase, punch_remote_peer_id, json_utf8]() {
            HandleInboundFrame(session, remote_peer_id, phase, punch_remote_peer_id, json_utf8);
          });
          return true; // keep open across connect/offer → sync
        });
  }
};

AmpPunchCoordinator::AmpPunchCoordinator(pp::amp::MeshRuntime& runtime, IoPump io_pump)
    : impl_(std::make_unique<Impl>()), runtime_(runtime), io_pump_(std::move(io_pump)) {
  impl_->runtime = &runtime_;
  impl_->io_pump = io_pump_;
  impl_->local_addrs = &local_addrs_;
}

AmpPunchCoordinator::~AmpPunchCoordinator() { Stop(); }

void AmpPunchCoordinator::SetLocalCandidateAddrs(std::vector<std::string> addrs) {
  local_addrs_ = SanitizePunchAddrs(std::move(addrs));
}

void AmpPunchCoordinator::SetProbeInbound(ProbeInbound handler) {
  impl_->probe_inbound = std::move(handler);
}

void AmpPunchCoordinator::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.Links().SetProtocolHandler(
      kAmpPunchProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle handle,
                                                const std::string& remote_peer_id, uint32_t channel_id) {
        impl->HandleInboundOnLink(handle, remote_peer_id, channel_id);
      }));
}

void AmpPunchCoordinator::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.Links().RemoveProtocolHandler(kAmpPunchProtocolId);
  impl_->deferred.Invalidate();
}

void AmpPunchCoordinator::TryColdPunchAsync(const std::string& introducer_peer_key,
                                            const std::string& target_peer_id,
                                            const std::vector<std::string>& my_addrs,
                                            std::function<void(PunchRoe)> on_done, int window_ms) {
  RunPunchAsync(introducer_peer_key, target_peer_id, my_addrs, window_ms, "cold", std::move(on_done));
}

void AmpPunchCoordinator::TryUpgradePunchAsync(const std::string& introducer_peer_key,
                                               const std::string& target_peer_id,
                                               const std::vector<std::string>& my_addrs,
                                               std::function<void(PunchRoe)> on_done, int window_ms) {
  RunPunchAsync(introducer_peer_key, target_peer_id, my_addrs, window_ms, "upgrade", std::move(on_done));
}

void AmpPunchCoordinator::TrySignalingPunchBurstAsync(const std::vector<std::string>& peer_addrs,
                                                      std::function<void(PunchRoe)> on_done, int window_ms) {
  if (!on_done) {
    return;
  }
  if (!started_ || impl_->stopped.load(std::memory_order_acquire)) {
    on_done(PunchRoe::error(Failure::Of(Err::NotStarted, "punch: not started")));
    return;
  }
  const auto sanitized = SanitizePunchAddrs(peer_addrs);
  if (sanitized.empty()) {
    on_done(PunchRoe::error(Failure::Of(Err::InvalidRequest, "punch: no peer candidates")));
    return;
  }
  std::string remote_peer_id;
  for (const std::string& ma : sanitized) {
    if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
      if (!parsed->peer_id.empty()) {
        (void)runtime_.Links().RegisterEndpoint(parsed->peer_id, ma);
        if (remote_peer_id.empty()) {
          remote_peer_id = parsed->peer_id;
        }
      }
    }
  }
  const int burst_window = window_ms > 0 ? window_ms : 2000;
  runtime_.BurstDial(
      sanitized, std::chrono::milliseconds(burst_window),
      [this, on_done = std::move(on_done), remote_peer_id](pp::amp::BurstDialResult r) mutable {
        PunchBurstResult burst = ToPunchBurst(std::move(r));
        PublishIfPunchConnected(runtime_.Links(), remote_peer_id, burst);
        PunchResult result;
        result.epoch_id = "signaling";
        result.ok = burst.ok;
        result.winner_multiaddr = burst.dialed;
        result.error = burst.ok ? "" : burst.error;
        if (burst.ok) {
          on_done(result);
        } else {
          on_done(PunchRoe::error(
              Failure::Of(Err::PunchFailed, burst.error.empty() ? "punch burst failed" : burst.error)));
        }
      });
}

AmpPunchCoordinator::PunchRoe AmpPunchCoordinator::TryColdPunch(const std::string& introducer_peer_key,
                                                                const std::string& target_peer_id,
                                                                const std::vector<std::string>& my_addrs,
                                                                int window_ms) {
  return RunPunch(introducer_peer_key, target_peer_id, my_addrs, window_ms, "cold");
}

AmpPunchCoordinator::PunchRoe AmpPunchCoordinator::TryUpgradePunch(const std::string& introducer_peer_key,
                                                                   const std::string& target_peer_id,
                                                                   const std::vector<std::string>& my_addrs,
                                                                   int window_ms) {
  return RunPunch(introducer_peer_key, target_peer_id, my_addrs, window_ms, "upgrade");
}

AmpPunchCoordinator::PunchRoe AmpPunchCoordinator::RunPunch(const std::string& introducer_peer_key,
                                                            const std::string& target_peer_id,
                                                            const std::vector<std::string>& my_addrs,
                                                            int window_ms, const std::string& reason) {
  SettledWait<PunchResult, Failure> wait;
  const int window = window_ms > 0 ? window_ms : 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(window + 4000);
  RunPunchAsync(introducer_peer_key, target_peer_id, my_addrs, window_ms, reason,
                [wait](PunchRoe value) { wait.Finish(std::move(value)); });
  // Park outside mux — caller IoPump must drain PostToIo (MeshRuntime::Pump / harness PumpAll).
  AmpParkUntil([&] { return wait.IsSettled(); }, deadline, io_pump_);
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "punch: cold punch timed out"));
}

void AmpPunchCoordinator::RunPunchAsync(const std::string& introducer_peer_key,
                                        const std::string& target_peer_id,
                                        const std::vector<std::string>& my_addrs, int window_ms,
                                        const std::string& reason, std::function<void(PunchRoe)> on_done) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto finish_once = std::make_shared<std::function<void(PunchRoe)>>();
  *finish_once = [on_done = std::move(on_done), settled](PunchRoe value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };

  if (!started_) {
    (*finish_once)(PunchRoe::error(Failure::Of(Err::NotStarted, "amp punch coordinator not started")));
    return;
  }
  if (!runtime_.Links().GetLinkSnapshot(introducer_peer_key).has_endpoint) {
    (*finish_once)(
        PunchRoe::error(Failure::Of(Err::EndpointNotRegistered, "introducer endpoint not registered")));
    return;
  }
  if (target_peer_id.empty()) {
    (*finish_once)(PunchRoe::error(Failure::Of(Err::InvalidRequest, "empty target_peer_id")));
    return;
  }
  const auto sanitized = SanitizePunchAddrs(my_addrs);
  if (sanitized.empty()) {
    (*finish_once)(PunchRoe::error(Failure::Of(Err::InvalidRequest, "no my_addrs")));
    return;
  }
  const int window = window_ms > 0 ? window_ms : 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(window + 4000);

  PunchConnectRequest req;
  req.target_peer_id = target_peer_id;
  req.addrs = sanitized;
  req.window_ms = window;
  req.reason = reason.empty() ? "cold" : reason;
  const std::string request_json = EncodePunchConnect(req);

  auto session = std::make_shared<pp::amp::ChannelSession>();
  auto finish = std::make_shared<std::function<void(PunchRoe)>>();
  *finish = [this, finish_once, session](PunchRoe value) {
    auto deliver = [finish_once, session, value = std::move(value)]() mutable {
      if (session) {
        session->Close();
      }
      (*finish_once)(std::move(value));
    };
    impl_->PostDeferred(std::move(deliver));
  };

  const auto read_timeout = RemainingTimeout(deadline);
  runtime_.Links().EnsureAssociation(
      introducer_peer_key, [this, introducer_peer_key, target_peer_id, request_json, finish, session, deadline,
                            read_timeout, settled](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
        if (!assoc) {
          (*finish)(PunchRoe::error(WrapLinkFailure(assoc.error())));
          return;
        }
        runtime_.Links().OpenChannel(
            introducer_peer_key, kAmpPunchProtocolId, PunchJsonChannelPolicy(read_timeout),
            [this, introducer_peer_key, target_peer_id, request_json, finish, session, deadline, read_timeout,
             settled](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
              if (!channel) {
                (*finish)(PunchRoe::error(WrapLinkFailure(channel.error())));
                return;
              }
              AmpScheduleWhenChannelOpen(
                  impl_->MakePostIo(), io_pump_,
                  [this, introducer_peer_key, channel_id = *channel]() {
                    auto* link = runtime_.Links().FindLink(introducer_peer_key);
                    return link && link->Mux() &&
                           link->Mux()->State(channel_id) == pp::amp::ChannelState::Open;
                  },
                  deadline,
                  [this, introducer_peer_key, channel_id = *channel, target_peer_id, request_json, finish, session,
                   deadline, read_timeout, settled](bool open) mutable {
                    if (!open) {
                      (*finish)(PunchRoe::error(
                          Failure::Of(Err::ChannelFailed, "punch: introducer channel open failed")));
                      return;
                    }
                    auto* link = runtime_.Links().FindLink(introducer_peer_key);
                    if (!link || !link->Mux() ||
                        link->Mux()->State(channel_id) != pp::amp::ChannelState::Open) {
                      (*finish)(PunchRoe::error(
                          Failure::Of(Err::ChannelFailed, "punch: introducer channel open failed")));
                      return;
                    }
                    session->Bind(
                        *link->Mux(), channel_id, PunchJsonChannelPolicy(read_timeout),
                        [this, finish, target_peer_id](Roe<std::vector<uint8_t>> frame) {
                          if (!frame) {
                            (*finish)(PunchRoe::error(
                                Failure::Of(Err::ProtocolError, "punch: failed to read introducer frame")));
                            return false;
                          }
                          auto root = TryParseObject(std::string(frame->begin(), frame->end()));
                          if (!root) {
                            (*finish)(PunchRoe::error(
                                Failure::Of(Err::ProtocolError, "punch: invalid introducer frame")));
                            return false;
                          }
                          const std::string op = PunchOp(*root).value_or("");
                          if (op == "result") {
                            auto result = DecodePunchResult(*root);
                            if (!result) {
                              (*finish)(PunchRoe::error(
                                  Failure::Of(Err::ProtocolError, "punch: invalid result frame")));
                              return false;
                            }
                            if (result->ok) {
                              PublishPunchWinnerAddrs(runtime_.Links(), target_peer_id, result->winner_multiaddr);
                              (*finish)(*result);
                            } else {
                              (*finish)(PunchRoe::error(Failure::Of(
                                  Err::PunchFailed, result->error.empty() ? "punch failed" : result->error)));
                            }
                            return false;
                          }
                          if (op == "sync") {
                            auto sync = DecodePunchSync(*root);
                            if (!sync) {
                              (*finish)(PunchRoe::error(
                                  Failure::Of(Err::ProtocolError, "punch: invalid sync frame")));
                              return false;
                            }
                            auto run_burst = [this, finish, target_peer_id, sync = *sync]() mutable {
                              for (const std::string& ma : SanitizePunchAddrs(sync.peer_addrs)) {
                                if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
                                  if (!parsed->peer_id.empty()) {
                                    (void)runtime_.Links().RegisterEndpoint(parsed->peer_id, ma);
                                  }
                                }
                              }
                              auto apply = [this, finish, target_peer_id,
                                            epoch = sync.epoch_id](PunchBurstResult burst) {
                                PublishIfPunchConnected(runtime_.Links(), target_peer_id, burst);
                                PunchResult result;
                                result.epoch_id = epoch;
                                result.ok = burst.ok;
                                result.winner_multiaddr = burst.dialed;
                                result.error = burst.ok ? "" : burst.error;
                                if (burst.ok) {
                                  (*finish)(result);
                                } else {
                                  (*finish)(PunchRoe::error(Failure::Of(
                                      Err::PunchFailed,
                                      burst.error.empty() ? "punch burst failed" : burst.error)));
                                }
                              };
                              const int burst_window = sync.window_ms > 0 ? sync.window_ms : 2000;
                              runtime_.BurstDial(
                                  sync.peer_addrs, std::chrono::milliseconds(burst_window),
                                  [apply = std::move(apply)](pp::amp::BurstDialResult r) mutable {
                                    apply(ToPunchBurst(std::move(r)));
                                  });
                            };
                            // Leave mux before dial/burst start.
                            impl_->PostStrand(std::move(run_burst));
                            return true;
                          }
                          return true;
                        });
                    if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                      (*finish)(PunchRoe::error(Failure::Of(Err::ProtocolError, "punch: failed to send connect")));
                      return;
                    }
                    // Overall attempt deadline on Amp clock.
                    const auto attempt_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                    if (attempt_ms.count() > 0) {
                      runtime_.PostAfter(attempt_ms, [finish, settled]() {
                        if (!settled->load(std::memory_order_acquire)) {
                          (*finish)(PunchRoe::error(
                              Failure::Of(Err::Timeout, "punch: cold punch timed out")));
                        }
                      });
                    }
                  },
                  [this]() { return impl_->stopped.load(std::memory_order_acquire); });
            });
      });
}

} // namespace pbr
