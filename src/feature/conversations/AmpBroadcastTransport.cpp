#include "feature/conversations/AmpBroadcastTransport.h"

#include "domain/mesh/l4/shared/InboundReply.h"

#include "amp/link/LinkIdentity.h"

#include "common/chat/IDirectMessageClient.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "common/PbrCompat.h"
#include "foundation/runtime/DeferredSelf.h"

namespace pbr {
namespace {


std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpBroadcastTransport::WorkerPost& post_worker, std::function<void()> task) {
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

struct AmpBroadcastTransport::Impl {
  IChatPeerLinks* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  IoPost post_io;
  std::mutex mutex;
  ResolvePublisherKey resolve_key;
  ResolvePublisherSecret resolve_secret;
  ResolveViewerPairwiseKey resolve_pairwise;
  ResolveNowMs resolve_now;
  ResolveHopAttachContext resolve_attach;
  ResolveHopSlotWinContext resolve_slot_win;
  std::unordered_map<std::string, LiveProgramKey> live_keys;
  std::atomic<bool> stopped{false};

  /** Guards protocol-handler raw Impl* past Stop — OWNERSHIP.md § DeferredSelf. */
  DeferredSelf deferred;

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

  void HandleInboundChannel(const std::string& remote_peer_id, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links || remote_peer_id.empty()) {
      return;
    }
    // The frame handler lives in the session and captures this holder: the holder must drop its
    // reference once used (first frame → `reply` owns the session) or when the channel closes, or
    // session → handler → holder → session leaks every served request (LeakSanitizer).
    auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
    auto policy = InboundReplyPolicy(pp::amp::ControlJsonChannelPolicy());
    *session_holder = links->BindChannel(
        remote_peer_id, channel_id, policy,
        [this, session_holder](Roe<std::vector<uint8_t>> frame) {
      auto session = std::exchange(*session_holder, nullptr);
      if (!session || !frame || stopped.load(std::memory_order_acquire)) {
        return false;
      }
      auto body = std::move(*frame);
      // Keep the channel open for the worker's reply (InboundReply.h); `reply` closes it.
      auto reply = MakeInboundReply(session, post_io);
      RunWorker(post_worker, [this, reply, body = std::move(body)]() mutable {
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
        reply->Send(JsonToBody(*response_json));
      });
      return true;
    },
        [session_holder](const char* /*reason*/) { session_holder->reset(); });
  }

};



AmpBroadcastTransport::AmpBroadcastTransport(IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker, IoPost post_io,
                                             IoAfter post_after)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)), post_io_(std::move(post_io)),
      post_after_(std::move(post_after)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
  impl_->post_io = post_io_;
}

AmpBroadcastTransport::~AmpBroadcastTransport() { Stop(); }

void AmpBroadcastTransport::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(
      kRpcBroadcastProtocolId,
      impl_->deferred.Bind([impl = impl_.get()](pp::amp::LinkHandle /*handle*/,
                                                const std::string& remote_peer_id,
                                                const uint32_t channel_id) {
        impl->HandleInboundChannel(remote_peer_id, channel_id);
      }));
}

void AmpBroadcastTransport::Stop() {
  // Idempotent: the destructor Stops again, possibly after MeshHost::Stop freed the runtime.
  if (!started_) {
    return;
  }
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kRpcBroadcastProtocolId);
  impl_->deferred.Invalidate();
}

void AmpBroadcastTransport::SetPublisherKeyResolver(ResolvePublisherKey resolve_key) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_key = std::move(resolve_key);
}

void AmpBroadcastTransport::SetPublisherSecretResolver(ResolvePublisherSecret resolve_secret) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_secret = std::move(resolve_secret);
}

void AmpBroadcastTransport::SetViewerPairwiseKeyResolver(ResolveViewerPairwiseKey resolve_pairwise) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_pairwise = std::move(resolve_pairwise);
}

void AmpBroadcastTransport::SetNowMsResolver(ResolveNowMs resolve_now) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_now = std::move(resolve_now);
}

void AmpBroadcastTransport::SetHopAttachResolver(ResolveHopAttachContext resolve_attach) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_attach = std::move(resolve_attach);
}

void AmpBroadcastTransport::SetHopSlotWinResolver(ResolveHopSlotWinContext resolve_slot_win) {
  std::lock_guard lock(impl_->mutex);
  impl_->resolve_slot_win = std::move(resolve_slot_win);
}

void AmpBroadcastTransport::PutLiveProgramKey(const std::string& program_id, const std::string& join_handle,
                                            LiveProgramKey key) {
  std::lock_guard lock(impl_->mutex);
  impl_->live_keys[MakeProgramKey(program_id, join_handle)] = std::move(key);
}

void AmpBroadcastTransport::ClearLiveProgramKey(const std::string& program_id, const std::string& join_handle) {
  std::lock_guard lock(impl_->mutex);
  impl_->live_keys.erase(MakeProgramKey(program_id, join_handle));
}

bool AmpBroadcastTransport::IsPeerReachable(const std::string& peer_identity_value) const {
  return links_.GetLinkSnapshot(peer_identity_value).has_endpoint || links_.IsConnected(peer_identity_value);
}


} // namespace pbr
