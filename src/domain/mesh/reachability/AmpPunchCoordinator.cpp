#include "domain/mesh/reachability/AmpPunchCoordinator.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "domain/mesh/reachability/PunchLogic.h"
#include "common/ValueJson.h"

#include <atomic>

namespace pbr {
namespace {

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
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};
  std::vector<std::string>* local_addrs = nullptr;

  void HandleInboundOnLink(pp::amp::PeerLink& link, uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links) {
      return;
    }
    auto session = std::make_shared<pp::amp::ChannelSession>();
    session->Bind(*link.Mux(), channel_id, pp::amp::ControlJsonChannelPolicy(),
                  [this, session](Roe<std::vector<uint8_t>> frame) {
                    if (!frame || stopped.load(std::memory_order_acquire)) {
                      return false;
                    }
                    const std::string json_utf8(frame->begin(), frame->end());
                    RunWorker(post_worker, [this, session, json_utf8]() {
                      if (stopped.load(std::memory_order_acquire) || !links) {
                        return;
                      }
                      auto root = TryParseObject(json_utf8);
                      if (!root) {
                        return;
                      }
                      const std::string op = root->getString("op").value_or("");
                      if (op == "connect" || op == "offer") {
                        PunchCandidates reply;
                        reply.peer_id = links->LocalPeerId();
                        reply.addrs =
                            local_addrs ? SanitizePunchAddrs(*local_addrs) : std::vector<std::string>{};
                        reply.nonce = root->getString("epoch_id").value_or("");
                        (void)session->EnqueueOutbound(JsonToBody(EncodePunchCandidates(reply)));

                        PunchResult result;
                        result.epoch_id = reply.nonce;
                        result.ok = false;
                        result.error = "punch: introducer orchestration not yet enabled (L3.25a stub)";
                        (void)session->EnqueueOutbound(JsonToBody(EncodePunchResult(result)));
                        session->Close();
                      }
                    });
                    return false;
                  });
  }
};

AmpPunchCoordinator::AmpPunchCoordinator(pp::amp::PeerLinkManager& links, IoPump io_pump,
                                         WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
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
                                                                int /*window_ms*/) {
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
  if (SanitizePunchAddrs(my_addrs).empty()) {
    return PunchRoe::error(Failure::Of(Err::InvalidRequest, "no my_addrs"));
  }
  return PunchRoe::error(
      Failure::Of(Err::PunchFailed, "punch: cold punch orchestration not yet enabled (L3.25a stub)"));
}

} // namespace pbr
