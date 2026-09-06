#include "feature/conversations/AmpBroadcastService.h"

#include "amp/link/PeerLink.h"

#include "common/chat/IDirectMessageClient.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpBroadcastService::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

std::string MakeProgramKey(const std::string& program_id, const std::string& join_handle) {
  return program_id + '\n' + join_handle;
}

int64_t DefaultNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

struct AmpBroadcastService::Impl {
  IChatPeerLinks* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  std::mutex mutex;
  ResolvePublisherKey resolve_key;
  ResolvePublisherSecret resolve_secret;
  ResolveViewerPairwiseKey resolve_pairwise;
  ResolveNowMs resolve_now;
  ResolveHopAttachContext resolve_attach;
  ResolveHopSlotWinContext resolve_slot_win;
  std::unordered_map<std::string, LiveProgramKey> live_keys;
  std::atomic<bool> stopped{false};

  void IoPumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      }
    }
  }

  int64_t NowMs() {
    ResolveNowMs resolver;
    {
      std::lock_guard lock(mutex);
      resolver = resolve_now;
    }
    return resolver ? resolver() : DefaultNowMs();
  }

  std::optional<ByteVector> ResolveKey(const std::string& peer_id) {
    ResolvePublisherKey resolver;
    {
      std::lock_guard lock(mutex);
      resolver = resolve_key;
    }
    if (!resolver) {
      return std::nullopt;
    }
    return resolver(peer_id);
  }

  std::optional<ByteVector> ResolveSecret() {
    ResolvePublisherSecret resolver;
    {
      std::lock_guard lock(mutex);
      resolver = resolve_secret;
    }
    if (!resolver) {
      return std::nullopt;
    }
    return resolver();
  }

  std::optional<ByteVector> ResolvePairwise(const std::string& viewer_peer_id) {
    ResolveViewerPairwiseKey resolver;
    {
      std::lock_guard lock(mutex);
      resolver = resolve_pairwise;
    }
    if (!resolver) {
      return std::nullopt;
    }
    return resolver(viewer_peer_id);
  }

  std::optional<LiveProgramKey> LookupLiveKey(const std::string& program_id, const std::string& join_handle) {
    std::lock_guard lock(mutex);
    const auto it = live_keys.find(MakeProgramKey(program_id, join_handle));
    if (it == live_keys.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  HopAttachContext LookupAttach(const std::string& program_id, const std::string& join_handle) {
    ResolveHopAttachContext resolver;
    {
      std::lock_guard lock(mutex);
      resolver = resolve_attach;
    }
    return resolver ? resolver(program_id, join_handle) : HopAttachContext{};
  }

  HopSlotWinContext LookupSlotWin(const std::string& program_id, const std::string& join_handle,
                              const std::string& relay_peer_id) {
    ResolveHopSlotWinContext resolver;
    {
      std::lock_guard lock(mutex);
      resolver = resolve_slot_win;
    }
    return resolver ? resolver(program_id, join_handle, relay_peer_id) : HopSlotWinContext{};
  }

  BroadcastTicketResponse HandleTicketRequest(const BroadcastTicketRequest& req) {
    BroadcastTicketResponse resp;
    if (req.program_id.empty() || req.join_handle.empty() || req.viewer_peer_id.empty()) {
      resp.ok = false;
      resp.error = "ticket_request missing fields";
      return resp;
    }
    auto live = LookupLiveKey(req.program_id, req.join_handle);
    if (!live || live->media_key_bytes.empty()) {
      resp.ok = false;
      resp.error = "unknown live program key";
      return resp;
    }
    auto secret = ResolveSecret();
    if (!secret || secret->empty()) {
      resp.ok = false;
      resp.error = "publisher secret unavailable";
      return resp;
    }

    BroadcastJoinTicketDraft draft;
    draft.publisher_peer_id = live->publisher_peer_id;
    draft.program_id = req.program_id;
    draft.join_handle = req.join_handle;
    draft.viewer_peer_id = req.viewer_peer_id;
    draft.media_epoch = live->media_epoch;
    draft.media_key_id = live->media_key_id;
    draft.hop_peer_id = live->hop_peer_id;
    draft.expires_at_ms = live->expires_at_ms;
    if (draft.expires_at_ms <= 0) {
      const int64_t ttl = live->ticket_ttl_ms > 0 ? live->ticket_ttl_ms : (24LL * 60 * 60 * 1000);
      draft.expires_at_ms = NowMs() + ttl;
    }

    ByteVector pairwise_storage;
    const ByteVector* pairwise_ptr = nullptr;
    if (auto pairwise = ResolvePairwise(req.viewer_peer_id); pairwise && !pairwise->empty()) {
      pairwise_storage = std::move(*pairwise);
      pairwise_ptr = &pairwise_storage;
    }

    auto ticket = MintBroadcastJoinTicket(std::move(draft), live->media_key_bytes, *secret, pairwise_ptr);
    if (!ticket) {
      resp.ok = false;
      resp.error = ticket.error().message;
      return resp;
    }
    resp.ok = true;
    resp.ticket = std::move(*ticket);
    return resp;
  }

  BroadcastViewerAttachResult HandleViewerAttach(const BroadcastViewerAttachRequest& req) {
    BroadcastViewerAttachResult result;
    result.action = BroadcastLadderViewerAction::Refuse;
    if (req.program_id.empty() || req.join_handle.empty() || req.viewer_peer_id.empty() ||
        req.ticket_json.empty()) {
      result.refuse_reason = "viewer_attach missing fields";
      return result;
    }
    auto ticket = DecodeBroadcastJoinTicketJson(req.ticket_json);
    if (!ticket) {
      result.refuse_reason = ticket.error().message;
      return result;
    }
    if (ticket->program_id != req.program_id || ticket->join_handle != req.join_handle) {
      result.refuse_reason = "ticket program/join mismatch";
      return result;
    }
    if (ticket->viewer_peer_id != req.viewer_peer_id) {
      result.refuse_reason = "ticket viewer mismatch";
      return result;
    }
    auto pk = ResolveKey(ticket->publisher_peer_id);
    if (!pk || pk->empty()) {
      result.refuse_reason = "unknown publisher key";
      return result;
    }
    if (auto verified = VerifyBroadcastJoinTicket(*ticket, *pk, NowMs(), req.viewer_peer_id); !verified) {
      result.refuse_reason = verified.error().message;
      return result;
    }

    const auto hop = LookupAttach(req.program_id, req.join_handle);
    BroadcastLadderViewerInput in;
    in.free_viewer_slots = hop.free_viewer_slots;
    in.whitelist_online_children = hop.whitelist_online_children;
    in.redirect_budget = req.redirect_budget;
    in.path_stamp = req.path_stamp;
    in.self_peer_id = hop.self_peer_id;
    in.max_redirect_hints = hop.max_redirect_hints;
    in.jitter_unit = hop.jitter_unit;
    return BroadcastViewerAttachResultFromDecision(DecideBroadcastViewerAdmit(in), hop.self_peer_id);
  }

  BroadcastRelaySlotWinResult HandleRelaySlotWin(const BroadcastRelaySlotWinRequest& req) {
    BroadcastRelaySlotWinResult result;
    result.action = BroadcastLadderSlotWinAction::Refuse;
    if (req.program_id.empty() || req.join_handle.empty() || req.relay_peer_id.empty()) {
      result.refuse_reason = "relay_slot_win missing fields";
      return result;
    }
    const auto hop = LookupSlotWin(req.program_id, req.join_handle, req.relay_peer_id);
    BroadcastLadderSlotWinInput in;
    in.free_child_slots = hop.free_child_slots;
    in.candidate_on_whitelist = hop.candidate_on_whitelist;
    in.slot_win_rate_limited = hop.slot_win_rate_limited;
    in.demotable_viewer_peer_ids = hop.demotable_viewer_peer_ids;
    in.new_relay_peer_id = req.relay_peer_id;
    in.max_demotions = hop.max_demotions;
    return BroadcastRelaySlotWinResultFromDecision(DecideBroadcastSlotWin(in));
  }

  Roe<std::string> EncodeResponseForRequest(const BroadcastRpcMessage& decoded) {
    if (std::holds_alternative<BroadcastTicketRequest>(decoded)) {
      return EncodeBroadcastTicketResponse(HandleTicketRequest(std::get<BroadcastTicketRequest>(decoded)));
    }
    if (std::holds_alternative<BroadcastViewerAttachRequest>(decoded)) {
      return EncodeBroadcastViewerAttachResult(HandleViewerAttach(std::get<BroadcastViewerAttachRequest>(decoded)));
    }
    if (std::holds_alternative<BroadcastRelaySlotWinRequest>(decoded)) {
      return EncodeBroadcastRelaySlotWinResult(HandleRelaySlotWin(std::get<BroadcastRelaySlotWinRequest>(decoded)));
    }
    return Error("expected broadcast request op");
  }

  void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links) {
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
        auto decoded = DecodeBroadcastRpcJson(json_utf8);
        Roe<std::string> response_json = [&]() -> Roe<std::string> {
          if (!decoded) {
            BroadcastTicketResponse err;
            err.ok = false;
            err.error = decoded.error().message;
            return EncodeBroadcastTicketResponse(err);
          }
          return EncodeResponseForRequest(*decoded);
        }();
        if (!response_json) {
          return;
        }
        if (!session->EnqueueOutbound(JsonToBody(*response_json))) {
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



AmpBroadcastService::AmpBroadcastService(IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
}

AmpBroadcastService::~AmpBroadcastService() { Stop(); }

void AmpBroadcastService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kRpcBroadcastProtocolId,
                            [impl = impl_.get()](pp::amp::PeerLink& link, const uint32_t channel_id) {
                              impl->HandleInboundChannel(link, channel_id);
                            });
}

void AmpBroadcastService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kRpcBroadcastProtocolId);
}

void AmpBroadcastService::SetPublisherKeyResolver(ResolvePublisherKey resolve_key) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_key = std::move(resolve_key);
}

void AmpBroadcastService::SetPublisherSecretResolver(ResolvePublisherSecret resolve_secret) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_secret = std::move(resolve_secret);
}

void AmpBroadcastService::SetViewerPairwiseKeyResolver(ResolveViewerPairwiseKey resolve_pairwise) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_pairwise = std::move(resolve_pairwise);
}

void AmpBroadcastService::SetNowMsResolver(ResolveNowMs resolve_now) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_now = std::move(resolve_now);
}

void AmpBroadcastService::SetHopAttachResolver(ResolveHopAttachContext resolve_attach) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_attach = std::move(resolve_attach);
}

void AmpBroadcastService::SetHopSlotWinResolver(ResolveHopSlotWinContext resolve_slot_win) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_slot_win = std::move(resolve_slot_win);
}

void AmpBroadcastService::PutLiveProgramKey(const std::string& program_id, const std::string& join_handle,
                                            LiveProgramKey key) {
  std::lock_guard lock(impl_->mutex);
  impl_->live_keys[MakeProgramKey(program_id, join_handle)] = std::move(key);
}

void AmpBroadcastService::ClearLiveProgramKey(const std::string& program_id, const std::string& join_handle) {
  std::lock_guard lock(impl_->mutex);
  impl_->live_keys.erase(MakeProgramKey(program_id, join_handle));
}

bool AmpBroadcastService::IsPeerReachable(const std::string& peer_identity_value) const {
  return links_.GetLinkSnapshot(peer_identity_value).has_endpoint || links_.IsConnected(peer_identity_value);
}


template <typename ResponseT>
Roe<ResponseT> AmpBroadcastService::RoundTrip(const std::string& peer_key, const std::string& request_json,
                                              const char* expect_label,
                                              std::function<bool(const BroadcastRpcMessage&)> is_response,
                                              std::function<ResponseT(BroadcastRpcMessage&&)> take_response) {
  if (!started_) {
    return Error("amp broadcast service not started");
  }
  if (!IsPeerReachable(peer_key)) {
    return Error("Peer-direct endpoint not registered")
        .WithUser("No usable peer address — add a dialable multiaddr on the contact.");
  }

  constexpr auto kSendTimeout = std::chrono::milliseconds(4000);
  const auto deadline = Clock::now() + kSendTimeout;

  auto result_promise = std::make_shared<std::promise<Roe<ResponseT>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto session = std::make_shared<pp::amp::ChannelSession>();

  auto finish = [settled, result_promise, session](Roe<ResponseT> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    session->Close();
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  links_.OpenChannel(peer_key, kRpcBroadcastProtocolId, pp::amp::ControlJsonChannelPolicy(),
                     [this, peer_key, request_json, expect_label, finish, settled, session,
                      deadline, is_response = std::move(is_response),
                      take_response = std::move(take_response)](IChatPeerLinks::ChannelRoe channel) mutable {
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
                         finish(Error("amp broadcast: channel open failed")
                                    .WithUser("Broadcast control request didn't confirm."));
                         return;
                       }

                       session->Bind(*link->Mux(), *channel, pp::amp::ControlJsonChannelPolicy(),
                                     [finish, expect_label, is_response = std::move(is_response),
                                      take_response = std::move(take_response)](Roe<std::vector<uint8_t>> frame) {
                                       if (!frame) {
                                         finish(Error("Failed to read broadcast response")
                                                    .WithUser("Broadcast control request didn't confirm."));
                                         return false;
                                       }
                                       const std::string json(frame->begin(), frame->end());
                                       auto decoded = DecodeBroadcastRpcJson(json);
                                       if (!decoded) {
                                         finish(decoded.error());
                                         return false;
                                       }
                                       if (!is_response(*decoded)) {
                                         finish(Error(std::string("broadcast response was not ") + expect_label));
                                         return false;
                                       }
                                       finish(take_response(std::move(*decoded)));
                                       return false;
                                     });

                       if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                         finish(Error("Failed to send broadcast request")
                                    .WithUser("Broadcast control request didn't confirm."));
                         return;
                       }

                       impl_->IoPumpUntil([settled] { return settled->load(std::memory_order_acquire); }, deadline);
                       if (!settled->load(std::memory_order_acquire)) {
                         finish(Error("amp broadcast send timed out").WithUser("Broadcast control timed out."));
                       }
                     });

  impl_->IoPumpUntil([&] { return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
                     deadline);

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    finish(Error("amp broadcast send timed out").WithUser("Broadcast control timed out."));
    return Error("amp broadcast send timed out").WithUser("Broadcast control timed out.");
  }
  return result_future.get();
}

Roe<BroadcastTicketResponse> AmpBroadcastService::RequestTicket(const std::string& peer_key,
                                                                const BroadcastTicketRequest& req) {
  auto json = EncodeBroadcastTicketRequest(req);
  if (!json) {
    return json.error();
  }
  return RoundTrip<BroadcastTicketResponse>(
      peer_key, *json, "ticket_response",
      [](const BroadcastRpcMessage& msg) { return std::holds_alternative<BroadcastTicketResponse>(msg); },
      [](BroadcastRpcMessage&& msg) { return std::get<BroadcastTicketResponse>(std::move(msg)); });
}

Roe<BroadcastViewerAttachResult> AmpBroadcastService::RequestViewerAttach(
    const std::string& peer_key, const BroadcastViewerAttachRequest& req) {
  auto json = EncodeBroadcastViewerAttachRequest(req);
  if (!json) {
    return json.error();
  }
  return RoundTrip<BroadcastViewerAttachResult>(
      peer_key, *json, "viewer_attach_result",
      [](const BroadcastRpcMessage& msg) { return std::holds_alternative<BroadcastViewerAttachResult>(msg); },
      [](BroadcastRpcMessage&& msg) { return std::get<BroadcastViewerAttachResult>(std::move(msg)); });
}

Roe<BroadcastRelaySlotWinResult> AmpBroadcastService::RequestRelaySlotWin(
    const std::string& peer_key, const BroadcastRelaySlotWinRequest& req) {
  auto json = EncodeBroadcastRelaySlotWinRequest(req);
  if (!json) {
    return json.error();
  }
  return RoundTrip<BroadcastRelaySlotWinResult>(
      peer_key, *json, "relay_slot_win_result",
      [](const BroadcastRpcMessage& msg) { return std::holds_alternative<BroadcastRelaySlotWinResult>(msg); },
      [](BroadcastRpcMessage&& msg) { return std::get<BroadcastRelaySlotWinResult>(std::move(msg)); });
}

} // namespace pbr
