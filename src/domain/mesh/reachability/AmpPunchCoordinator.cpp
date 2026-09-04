#include "domain/mesh/reachability/AmpPunchCoordinator.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "domain/mesh/reachability/PunchLogic.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return {json_utf8.begin(), json_utf8.end()};
}

void RunWorker(const AmpPunchCoordinator::WorkerPost& post_worker, std::function<void()> task) {
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

struct BurstDialResult {
  bool ok = false;
  std::string dialed;
  std::string error;
};

bool PeerAlreadyConnectedDirect(pp::amp::PeerLinkManager& links, const std::string& peer_id) {
  if (peer_id.empty()) {
    return false;
  }
  // Nested/circuit carrier links must not short-circuit punch (L3.25c upgrade-from-circuit).
  if (auto* link = links.FindLinkByPeerId(peer_id)) {
    return link->Phase() == pp::amp::PeerLinkPhase::Connected && !link->IsCarrierBacked();
  }
  return false;
}

BurstDialResult BurstDialCandidates(pp::amp::PeerLinkManager& links, AmpPunchCoordinator::IoPump io_pump,
                                    const std::vector<std::string>& targets, int window_ms) {
  BurstDialResult out;
  const auto addrs = SanitizePunchAddrs(targets);
  if (addrs.empty()) {
    out.error = "no peer_addrs";
    return out;
  }
  const int window = window_ms > 0 ? window_ms : 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(window);

  for (size_t i = 0; i < addrs.size(); ++i) {
    if (Clock::now() >= deadline) {
      break;
    }
    const std::string& ma = addrs[i];
    auto parsed = pp::amp::ParseAdpMultiaddr(ma);
    if (!parsed) {
      out.error = "peer addr is not an ADP multiaddr";
      out.dialed = ma;
      continue;
    }
    const std::string peer_id = parsed->peer_id;

    if (PeerAlreadyConnectedDirect(links, peer_id)) {
      out.ok = true;
      out.dialed = ma;
      out.error.clear();
      return out;
    }

    // Unique dial alias (not peer_id) so inbound adopt can own the peer_id key under A026.
    const std::string key = "punch:burst:" + std::to_string(i) + ":" +
                            peer_id.substr(0, std::min<size_t>(peer_id.size(), 12));

    if (auto registered = links.RegisterEndpoint(key, ma); !registered) {
      if (PeerAlreadyConnectedDirect(links, peer_id)) {
        out.ok = true;
        out.dialed = ma;
        out.error.clear();
        return out;
      }
      out.error = registered.error().message;
      out.dialed = ma;
      continue;
    }

    // Pump before Open so VirtualClock harnesses mint a fresh ADP assoc id.
    if (io_pump) {
      io_pump();
      io_pump();
    }

    SettledWait<void> wait;
    links.EnsureAssociation(key, [wait](pp::amp::PeerLinkManager::LinkRoe result) {
      if (result) {
        wait.Finish(Roe<void>());
      } else {
        wait.Finish(Roe<void>(Error(AmpPunchCoordinator::WrapLinkFailure(result.error()).message)));
      }
    });

    while (Clock::now() < deadline && !wait.IsSettled()) {
      if (PeerAlreadyConnectedDirect(links, peer_id)) {
        out.ok = true;
        out.dialed = ma;
        out.error.clear();
        return out;
      }
      if (io_pump) {
        io_pump();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    auto dialed = wait.Wait(std::chrono::milliseconds(1), Error("punch burst dial timed out"));
    // Require a non-carrier PeerLink. EnsureAssociation must not count a nested/circuit
    // carrier Session as a punch win (L3.25c upgrade-from-circuit / A024 coexist).
    if (PeerAlreadyConnectedDirect(links, peer_id)) {
      out.ok = true;
      out.dialed = ma;
      out.error.clear();
      return out;
    }
    if (dialed) {
      if (auto* link = links.FindLink(key)) {
        if (link->Phase() == pp::amp::PeerLinkPhase::Connected && !link->IsCarrierBacked()) {
          out.ok = true;
          out.dialed = ma;
          out.error.clear();
          return out;
        }
      }
      out.error = "punch burst associated without a direct PeerLink";
      out.dialed = ma;
      continue;
    }

    // Dual-dial race: local ADP may report "assoc already open" while inbound wins A026.
    const std::string err = dialed.error().message;
    if (err.find("already open") != std::string::npos) {
      const auto race_deadline = std::min(deadline, Clock::now() + std::chrono::milliseconds(500));
      while (Clock::now() < race_deadline && !PeerAlreadyConnectedDirect(links, peer_id)) {
        if (io_pump) {
          io_pump();
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      if (PeerAlreadyConnectedDirect(links, peer_id)) {
        out.ok = true;
        out.dialed = ma;
        out.error.clear();
        return out;
      }
    }
    out.error = err;
    out.dialed = ma;
  }
  if (!out.ok && out.error.empty()) {
    out.error = "punch burst window expired";
  }
  return out;
}


void PublishIfPunchConnected(pp::amp::PeerLinkManager& links, const std::string& known_peer_id,
                             BurstDialResult& burst) {
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
  pp::amp::PeerLinkManager* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};
  std::vector<std::string>* local_addrs = nullptr;

  void IoPumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  void FailSession(const std::shared_ptr<pp::amp::ChannelSession>& session, const std::string& epoch_id,
                   const std::string& error) {
    PunchResult result;
    result.epoch_id = epoch_id;
    result.ok = false;
    result.error = error;
    (void)session->EnqueueOutbound(JsonToBody(EncodePunchResult(result)));
    if (io_pump) {
      io_pump();
    }
    session->Close();
  }

  void RunIntroducerConnect(const std::shared_ptr<pp::amp::ChannelSession>& initiator_session,
                            const std::string& initiator_peer_id, PunchConnectRequest req) {
    if (stopped.load(std::memory_order_acquire) || !links) {
      return;
    }
    const auto my_addrs =
        local_addrs ? SanitizePunchAddrs(*local_addrs) : std::vector<std::string>{};
    const int window_ms = req.window_ms > 0 ? req.window_ms : 2000;
    const std::string epoch_id = MakeEpochId(links->LocalPeerId());
    const auto deadline = Clock::now() + std::chrono::milliseconds(window_ms + 3000);

    const std::string target_key = ResolvePeerKey(*links, req.target_peer_id);
    if (target_key.empty()) {
      FailSession(initiator_session, epoch_id, "punch: target endpoint unknown to introducer");
      return;
    }

    SettledWait<PunchCandidates, Failure> candidates_wait;
    auto target_session = std::make_shared<pp::amp::ChannelSession>();
    auto target_settled = std::make_shared<std::atomic<bool>>(false);
    auto finish_candidates = [target_settled, candidates_wait, target_session](CodedRoe<PunchCandidates, Err> value) {
      if (target_settled->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      target_session->Close();
      candidates_wait.Finish(std::move(value));
    };

    const auto read_timeout = RemainingTimeout(deadline);
    links->EnsureAssociation(
        target_key, [this, target_key, initiator_peer_id, req, epoch_id, window_ms, finish_candidates,
                     target_session, deadline, read_timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
          if (!assoc) {
            finish_candidates(CodedRoe<PunchCandidates, Err>::error(WrapLinkFailure(assoc.error())));
            return;
          }
          links->OpenChannel(
              target_key, kAmpPunchProtocolId, PunchJsonChannelPolicy(read_timeout),
              [this, target_key, initiator_peer_id, req, epoch_id, window_ms, finish_candidates, target_session,
               deadline, read_timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                if (!channel) {
                  finish_candidates(CodedRoe<PunchCandidates, Err>::error(WrapLinkFailure(channel.error())));
                  return;
                }
                IoPumpUntil(
                    [&] {
                      auto* link = links->FindLink(target_key);
                      return link && link->Mux() &&
                             link->Mux()->State(*channel) == pp::amp::ChannelState::Open;
                    },
                    deadline);
                auto* link = links->FindLink(target_key);
                if (!link || !link->Mux() ||
                    link->Mux()->State(*channel) != pp::amp::ChannelState::Open) {
                  finish_candidates(CodedRoe<PunchCandidates, Err>::error(
                      Failure::Of(Err::ChannelFailed, "punch: target channel open failed")));
                  return;
                }
                target_session->Bind(
                    *link->Mux(), *channel, PunchJsonChannelPolicy(read_timeout),
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
                if (io_pump) {
                  io_pump();
                }
              });
        });

    while (Clock::now() < deadline && !candidates_wait.IsSettled()) {
      if (io_pump) {
        io_pump();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    auto candidates = candidates_wait.Wait(
        std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "punch: waiting for target candidates timed out"));
    if (!candidates) {
      FailSession(initiator_session, epoch_id, candidates.error().message);
      return;
    }
    if (SanitizePunchAddrs(candidates->addrs).empty()) {
      FailSession(initiator_session, epoch_id, "punch: target returned no candidates");
      return;
    }

    PunchSync sync_to_initiator;
    sync_to_initiator.epoch_id = epoch_id;
    sync_to_initiator.peer_addrs = SanitizePunchAddrs(candidates->addrs);
    sync_to_initiator.window_ms = window_ms;

    PunchSync sync_to_target;
    sync_to_target.epoch_id = epoch_id;
    sync_to_target.peer_addrs = SanitizePunchAddrs(req.addrs);
    sync_to_target.window_ms = window_ms;

    if (!initiator_session->EnqueueOutbound(JsonToBody(EncodePunchSync(sync_to_initiator)))) {
      FailSession(initiator_session, epoch_id, "punch: failed to send sync to initiator");
      return;
    }
    if (!target_session->IsClosed()) {
      if (!target_session->EnqueueOutbound(JsonToBody(EncodePunchSync(sync_to_target)))) {
        FailSession(initiator_session, epoch_id, "punch: failed to send sync to target");
        return;
      }
    }
    // Flush sync frames; do not Close here — initiator/target own burst + session lifetime
    // (closing + extended pumping nested under TryColdPunch was dropping PeerLinks).
    if (io_pump) {
      io_pump();
      io_pump();
      io_pump();
    }
  }

  void HandleInboundOnLink(pp::amp::PeerLink& link, uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links) {
      return;
    }
    const std::string remote_peer_id = link.RemotePeerId();
    auto session = std::make_shared<pp::amp::ChannelSession>();
    auto phase = std::make_shared<std::string>("await_first");
    auto punch_remote_peer_id = std::make_shared<std::string>();
    session->Bind(*link.Mux(), channel_id, PunchJsonChannelPolicy(std::chrono::milliseconds{8000}),
                  [this, session, remote_peer_id, phase, punch_remote_peer_id](Roe<std::vector<uint8_t>> frame) {
                    if (!frame || stopped.load(std::memory_order_acquire)) {
                      return false;
                    }
                    const std::string json_utf8(frame->begin(), frame->end());
                    RunWorker(post_worker, [this, session, remote_peer_id, phase, punch_remote_peer_id, json_utf8]() {
                      if (stopped.load(std::memory_order_acquire) || !links) {
                        return;
                      }
                      auto root = TryParseObject(json_utf8);
                      if (!root) {
                        return;
                      }
                      const std::string op = PunchOp(*root).value_or("");
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
                        PunchCandidates reply;
                        reply.peer_id = links->LocalPeerId();
                        reply.addrs =
                            local_addrs ? SanitizePunchAddrs(*local_addrs) : std::vector<std::string>{};
                        reply.nonce = offer->epoch_id;
                        if (!session->EnqueueOutbound(JsonToBody(EncodePunchCandidates(reply)))) {
                          FailSession(session, offer->epoch_id, "punch: failed to send candidates");
                          return;
                        }
                        if (io_pump) {
                          io_pump();
                        }
                        *punch_remote_peer_id = offer->initiator_peer_id;
                        *phase = "await_sync";
                        return;
                      }
                      if (op == "sync" && *phase == "await_sync") {
                        auto sync = DecodePunchSync(*root);
                        if (!sync) {
                          FailSession(session, "", "punch: invalid sync");
                          return;
                        }
                        *phase = "bursting";
                        // L3.25b: book remote candidate addrs under PeerId before/without burst win.
                        for (const std::string& ma : SanitizePunchAddrs(sync->peer_addrs)) {
                          if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
                            if (!parsed->peer_id.empty()) {
                              (void)links->RegisterEndpoint(parsed->peer_id, ma);
                            }
                          }
                        }
                        auto burst = BurstDialCandidates(*links, io_pump, sync->peer_addrs, sync->window_ms);
                        std::string remote_id = *punch_remote_peer_id;
                        if (remote_id.empty()) {
                          for (const std::string& ma : sync->peer_addrs) {
                            if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
                              remote_id = parsed->peer_id;
                              break;
                            }
                          }
                        }
                        PublishIfPunchConnected(*links, remote_id, burst);
                        PunchResult result;
                        result.epoch_id = sync->epoch_id;
                        result.ok = burst.ok;
                        result.winner_multiaddr = burst.dialed;
                        result.error = burst.ok ? "" : burst.error;
                        (void)session->EnqueueOutbound(JsonToBody(EncodePunchResult(result)));
                        if (io_pump) {
                          io_pump();
                        }
                        session->Close();
                        return;
                      }
                    });
                    return true; // keep open across connect/offer → sync
                  });
  }
};

AmpPunchCoordinator::AmpPunchCoordinator(pp::amp::PeerLinkManager& links, IoPump io_pump,
                                         WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
  impl_->local_addrs = &local_addrs_;
}

AmpPunchCoordinator::~AmpPunchCoordinator() { Stop(); }

void AmpPunchCoordinator::SetLocalCandidateAddrs(std::vector<std::string> addrs) {
  local_addrs_ = SanitizePunchAddrs(std::move(addrs));
}

void AmpPunchCoordinator::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kAmpPunchProtocolId,
                            [impl = impl_.get()](pp::amp::PeerLink& link, uint32_t channel_id) {
                              impl->HandleInboundOnLink(link, channel_id);
                            });
}

void AmpPunchCoordinator::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kAmpPunchProtocolId);
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
  if (!started_) {
    return PunchRoe::error(Failure::Of(Err::NotStarted, "amp punch coordinator not started"));
  }
  if (!links_.GetLinkSnapshot(introducer_peer_key).has_endpoint) {
    return PunchRoe::error(
        Failure::Of(Err::EndpointNotRegistered, "introducer endpoint not registered"));
  }
  if (target_peer_id.empty()) {
    return PunchRoe::error(Failure::Of(Err::InvalidRequest, "empty target_peer_id"));
  }
  const auto sanitized = SanitizePunchAddrs(my_addrs);
  if (sanitized.empty()) {
    return PunchRoe::error(Failure::Of(Err::InvalidRequest, "no my_addrs"));
  }
  const int window = window_ms > 0 ? window_ms : 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(window + 4000);

  PunchConnectRequest req;
  req.target_peer_id = target_peer_id;
  req.addrs = sanitized;
  req.window_ms = window;
  req.reason = reason.empty() ? "cold" : reason;
  const std::string request_json = EncodePunchConnect(req);

  SettledWait<PunchResult, Failure> wait;
  auto session = std::make_shared<pp::amp::ChannelSession>();
  auto settled = std::make_shared<std::atomic<bool>>(false);

  auto finish = [settled, wait, session](PunchRoe value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    session->Close();
    wait.Finish(std::move(value));
  };

  const auto read_timeout = RemainingTimeout(deadline);
  links_.EnsureAssociation(
      introducer_peer_key, [this, introducer_peer_key, target_peer_id, request_json, finish, session, deadline,
                            read_timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
        if (!assoc) {
          finish(PunchRoe::error(WrapLinkFailure(assoc.error())));
          return;
        }
        links_.OpenChannel(
            introducer_peer_key, kAmpPunchProtocolId, PunchJsonChannelPolicy(read_timeout),
            [this, introducer_peer_key, target_peer_id, request_json, finish, session, deadline,
             read_timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
              if (!channel) {
                finish(PunchRoe::error(WrapLinkFailure(channel.error())));
                return;
              }
              impl_->IoPumpUntil(
                  [&] {
                    auto* link = links_.FindLink(introducer_peer_key);
                    return link && link->Mux() &&
                           link->Mux()->State(*channel) == pp::amp::ChannelState::Open;
                  },
                  deadline);
              auto* link = links_.FindLink(introducer_peer_key);
              if (!link || !link->Mux() ||
                  link->Mux()->State(*channel) != pp::amp::ChannelState::Open) {
                finish(PunchRoe::error(
                    Failure::Of(Err::ChannelFailed, "punch: introducer channel open failed")));
                return;
              }
              session->Bind(
                  *link->Mux(), *channel, PunchJsonChannelPolicy(read_timeout),
                  [this, finish, target_peer_id](Roe<std::vector<uint8_t>> frame) {
                    if (!frame) {
                      finish(PunchRoe::error(
                          Failure::Of(Err::ProtocolError, "punch: failed to read introducer frame")));
                      return false;
                    }
                    auto root = TryParseObject(std::string(frame->begin(), frame->end()));
                    if (!root) {
                      finish(PunchRoe::error(
                          Failure::Of(Err::ProtocolError, "punch: invalid introducer frame")));
                      return false;
                    }
                    const std::string op = PunchOp(*root).value_or("");
                    if (op == "result") {
                      auto result = DecodePunchResult(*root);
                      if (!result) {
                        finish(PunchRoe::error(
                            Failure::Of(Err::ProtocolError, "punch: invalid result frame")));
                        return false;
                      }
                      if (result->ok) {
                        PublishPunchWinnerAddrs(links_, target_peer_id, result->winner_multiaddr);
                        finish(*result);
                      } else {
                        finish(PunchRoe::error(Failure::Of(
                            Err::PunchFailed, result->error.empty() ? "punch failed" : result->error)));
                      }
                      return false;
                    }
                    if (op == "sync") {
                      auto sync = DecodePunchSync(*root);
                      if (!sync) {
                        finish(PunchRoe::error(
                            Failure::Of(Err::ProtocolError, "punch: invalid sync frame")));
                        return false;
                      }
                      for (const std::string& ma : SanitizePunchAddrs(sync->peer_addrs)) {
                        if (auto parsed = pp::amp::ParseAdpMultiaddr(ma)) {
                          if (!parsed->peer_id.empty()) {
                            (void)links_.RegisterEndpoint(parsed->peer_id, ma);
                          }
                        }
                      }
                      auto burst =
                          BurstDialCandidates(links_, io_pump_, sync->peer_addrs, sync->window_ms);
                      PublishIfPunchConnected(links_, target_peer_id, burst);
                      PunchResult result;
                      result.epoch_id = sync->epoch_id;
                      result.ok = burst.ok;
                      result.winner_multiaddr = burst.dialed;
                      result.error = burst.ok ? "" : burst.error;
                      if (burst.ok) {
                        finish(result);
                      } else {
                        finish(PunchRoe::error(Failure::Of(
                            Err::PunchFailed, burst.error.empty() ? "punch burst failed" : burst.error)));
                      }
                      return false;
                    }
                    return true;
                  });
              if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                finish(PunchRoe::error(Failure::Of(Err::ProtocolError, "punch: failed to send connect")));
                return;
              }
              if (io_pump_) {
                io_pump_();
              }
            });
      });

  while (Clock::now() < deadline && !wait.IsSettled()) {
    if (io_pump_) {
      io_pump_();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return wait.Wait(std::chrono::milliseconds(1), Failure::Of(Err::Timeout, "punch: cold punch timed out"));
}

} // namespace pbr
