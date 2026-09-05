#include "feature/conversations/AmpPeerAnnounceService.h"

#include "amp/link/PeerLink.h"

#include "domain/messaging/PeerAnnounceCodec.h"
#include "domain/messaging/PeerAnnounceRpcCodec.h"

#include "common/chat/IDirectMessageClient.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpPeerAnnounceService::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

} // namespace

struct AmpPeerAnnounceService::Impl {
  IChatPeerLinks* links = nullptr;
  PeerAnnounceFeed* feed = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  std::mutex feed_mutex;
  std::mutex resolver_mutex;
  ResolvePublisherKey resolve_key;
  OnTipIngested on_tip_ingested;
  std::atomic<bool> stopped{false};

  void IoPumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      }
    }
  }

  std::optional<std::vector<uint8_t>> ResolveKey(const std::string& peer_id) {
    ResolvePublisherKey resolver;
    {
      std::lock_guard lock(resolver_mutex);
      resolver = resolve_key;
    }
    if (!resolver) {
      return std::nullopt;
    }
    return resolver(peer_id);
  }

  PeerAnnounceTipAck IngestTip(const PeerAnnounceTip& tip) {
    PeerAnnounceTipAck ack;
    ack.seq = tip.seq;
    ack.epoch = tip.epoch;
    auto pk = ResolveKey(tip.peer_id);
    if (!pk || pk->empty()) {
      ack.ok = false;
      ack.error = "unknown publisher key";
      return ack;
    }
    if (auto verified = VerifyPeerAnnounceTip(tip, *pk); !verified) {
      ack.ok = false;
      ack.error = verified.error().message;
      return ack;
    }
    std::lock_guard lock(feed_mutex);
    feed->SetTrustedPublisherKey(*pk);
    if (auto ingested = feed->Ingest(tip); !ingested) {
      ack.ok = false;
      ack.error = ingested.error().message;
      return ack;
    }
    OnTipIngested cb;
    {
      std::lock_guard lock(resolver_mutex);
      cb = on_tip_ingested;
    }
    if (cb) {
      cb(tip);
    }
    ack.ok = true;
    return ack;
  }

  void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links || !feed) {
      return;
    }
    auto session = std::make_shared<pp::amp::ChannelSession>();
    auto policy = pp::amp::ControlJsonChannelPolicy();
    session->Bind(*link.Mux(), channel_id, policy, [this, session](Roe<std::vector<uint8_t>> frame) {
      if (!frame || stopped.load(std::memory_order_acquire)) {
        return false;
      }
      auto body = std::move(*frame);
      RunWorker(post_worker, [this, session, body = std::move(body)]() mutable {
        if (stopped.load(std::memory_order_acquire)) {
          return;
        }
        const std::string json_utf8(body.begin(), body.end());
        auto decoded = DecodePeerAnnounceRpcJson(json_utf8);
        PeerAnnounceTipAck ack;
        if (!decoded) {
          ack.ok = false;
          ack.error = decoded.error().message;
        } else if (!std::holds_alternative<PeerAnnounceTipPush>(*decoded)) {
          ack.ok = false;
          ack.error = "expected tip_push";
        } else {
          ack = IngestTip(std::get<PeerAnnounceTipPush>(*decoded).tip);
        }
        auto ack_json = EncodePeerAnnounceTipAck(ack);
        if (!ack_json) {
          return;
        }
        if (!session->EnqueueOutbound(JsonToBody(*ack_json))) {
          return;
        }
        if (io_pump) {
          io_pump();
        }
      });
      return false;
    });
  }
};

AmpPeerAnnounceService::AmpPeerAnnounceService(IChatPeerLinks& links, PeerAnnounceFeed& feed, IoPump io_pump,
                                               WorkerPost post_worker, ResolvePublisherKey resolve_key)
    : impl_(std::make_unique<Impl>()), links_(links), feed_(feed), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->feed = &feed_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
  impl_->resolve_key = std::move(resolve_key);
}

AmpPeerAnnounceService::~AmpPeerAnnounceService() {
  Stop();
}

void AmpPeerAnnounceService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kRpcPeerAnnounceProtocolId,
                            [impl = impl_.get()](pp::amp::PeerLink& link, const uint32_t channel_id) {
                              impl->HandleInboundChannel(link, channel_id);
                            });
}

void AmpPeerAnnounceService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kRpcPeerAnnounceProtocolId);
}

void AmpPeerAnnounceService::SetPublisherKeyResolver(ResolvePublisherKey resolve_key) {
  std::lock_guard lock(impl_->resolver_mutex);
  impl_->resolve_key = std::move(resolve_key);
}

void AmpPeerAnnounceService::SetOnTipIngested(OnTipIngested cb) {
  std::lock_guard lock(impl_->resolver_mutex);
  impl_->on_tip_ingested = std::move(cb);
}


bool AmpPeerAnnounceService::IsPeerReachable(const std::string& peer_identity_value) const {
  return links_.GetLinkSnapshot(peer_identity_value).has_endpoint || links_.IsConnected(peer_identity_value);
}

Roe<PeerAnnounceTipAck> AmpPeerAnnounceService::PushTip(const std::string& peer_key, const PeerAnnounceTip& tip) {
  if (!started_) {
    return Error("amp peer-announce service not started");
  }
  if (!IsPeerReachable(peer_key)) {
    return Error("Peer-direct endpoint not registered")
        .WithUser("No usable peer address — add a dialable multiaddr on the contact.");
  }

  auto push_json = EncodePeerAnnounceTipPush(tip);
  if (!push_json) {
    return push_json.error();
  }

  constexpr auto kSendTimeout = std::chrono::milliseconds(4000);
  const auto deadline = Clock::now() + kSendTimeout;

  auto result_promise = std::make_shared<std::promise<Roe<PeerAnnounceTipAck>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto session = std::make_shared<pp::amp::ChannelSession>();

  auto finish = [settled, result_promise, session](Roe<PeerAnnounceTipAck> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    session->Close();
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  links_.OpenChannel(peer_key, kRpcPeerAnnounceProtocolId, pp::amp::ControlJsonChannelPolicy(),
                     [this, peer_key, push_json = *push_json, finish, settled, session,
                      deadline](IChatPeerLinks::ChannelRoe channel) mutable {
                       if (!channel) {
                         finish(Error(channel.error().message));
                         return;
                       }
                       impl_->IoPumpUntil(
                           [&] {
                             auto* link = links_.FindLink(peer_key);
                             return link && link->Mux() &&
                                    link->Mux()->State(*channel) == pp::amp::ChannelState::Open;
                           },
                           deadline);
                       auto* link = links_.FindLink(peer_key);
                       if (!link || !link->Mux() || link->Mux()->State(*channel) != pp::amp::ChannelState::Open) {
                         finish(Error("amp peer-announce: channel open failed")
                                    .WithUser("Direct tip push didn't confirm."));
                         return;
                       }

                       session->Bind(*link->Mux(), *channel, pp::amp::ControlJsonChannelPolicy(),
                                     [finish](Roe<std::vector<uint8_t>> ack_frame) {
                                       if (!ack_frame) {
                                         finish(Error("Failed to read peer-announce tip_ack")
                                                    .WithUser("Direct tip push didn't confirm."));
                                         return false;
                                       }
                                       const std::string json(ack_frame->begin(), ack_frame->end());
                                       auto decoded = DecodePeerAnnounceRpcJson(json);
                                       if (!decoded) {
                                         finish(decoded.error());
                                         return false;
                                       }
                                       if (!std::holds_alternative<PeerAnnounceTipAck>(*decoded)) {
                                         finish(Error("peer-announce response was not tip_ack"));
                                         return false;
                                       }
                                       finish(std::get<PeerAnnounceTipAck>(*decoded));
                                       return false;
                                     });

                       if (!session->EnqueueOutbound(JsonToBody(push_json))) {
                         finish(Error("Failed to send peer-announce tip_push")
                                    .WithUser("Direct tip push didn't confirm."));
                         return;
                       }

                       impl_->IoPumpUntil([settled] { return settled->load(std::memory_order_acquire); }, deadline);
                       if (!settled->load(std::memory_order_acquire)) {
                         finish(Error("amp peer-announce send timed out").WithUser("Direct tip push timed out."));
                       }
                     });

  impl_->IoPumpUntil([&] { return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
                     deadline);

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    finish(Error("amp peer-announce send timed out").WithUser("Direct tip push timed out."));
    return Error("amp peer-announce send timed out").WithUser("Direct tip push timed out.");
  }
  return result_future.get();
}

} // namespace pbr
