#include "feature/broadcast/AmpBroadcastRpcClient.h"

#include "common/chat/IDirectMessageClient.h"
#include "domain/mesh/shared/AmpParkUntil.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"

#include <atomic>
#include <future>
#include <utility>
#include <variant>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct AmpBroadcastRpcClient::State {
  IChatPeerLinks* links = nullptr;
  IoPump io_pump;
  IoPost post_io;
  IoAfter post_after;
  std::atomic<bool> stopped{false};
};

namespace {

using Clock = std::chrono::steady_clock;
using State = AmpBroadcastRpcClient::State;
constexpr auto kRoundTripTimeout = AmpBroadcastRpcClient::kDefaultTimeout;

int64_t SteadyDeadlineMs(const Clock::time_point deadline) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
}

bool IsPeerReachable(const State& state, const std::string& peer_key) {
  return state.links->GetLinkSnapshot(peer_key).has_endpoint || state.links->IsConnected(peer_key);
}

template <typename ResponseT>
ResponseT TakeAs(BroadcastRpcMessage&& msg) {
  return std::get<ResponseT>(std::move(msg));
}

/** Once the channel is open: bind, send, and wait (bounded) for the one response frame. */
template <typename ResponseT>
void SendAndAwait(const std::shared_ptr<State>& state, const std::string& peer_key, uint32_t channel_id,
                  const std::string& request_json, const char* expect_label, Clock::time_point deadline,
                  const std::shared_ptr<std::atomic<bool>>& settled,
                  const std::function<void(Roe<ResponseT>)>& finish,
                  const std::shared_ptr<std::shared_ptr<pp::amp::ChannelSession>>& session_holder) {
  *session_holder = state->links->BindChannel(
      peer_key, channel_id, pp::amp::ControlJsonChannelPolicy(),
      [finish, expect_label](Roe<std::vector<uint8_t>> frame) {
        if (!frame) {
          finish(Error("Failed to read broadcast response").WithUser("Broadcast control request didn't confirm."));
          return false;
        }
        auto decoded = DecodeBroadcastRpcJson(std::string(frame->begin(), frame->end()));
        if (!decoded) {
          finish(decoded.error());
          return false;
        }
        if (!std::holds_alternative<ResponseT>(*decoded)) {
          finish(Error(std::string("broadcast response was not ") + expect_label));
          return false;
        }
        finish(TakeAs<ResponseT>(std::move(*decoded)));
        return false;
      });
  if (!*session_holder) {
    finish(Error("amp broadcast: channel open failed").WithUser("Broadcast control request didn't confirm."));
    return;
  }
  if (!(*session_holder)->EnqueueOutbound(std::vector<uint8_t>(request_json.begin(), request_json.end()))) {
    finish(Error("Failed to send broadcast request").WithUser("Broadcast control request didn't confirm."));
    return;
  }
  AmpScheduleUntilSettled(
      state->post_io, state->io_pump, settled, deadline,
      [finish]() { finish(Error("amp broadcast send timed out").WithUser("Broadcast control timed out.")); },
      state->post_after);
}

template <typename ResponseT>
void RoundTripAsync(const std::shared_ptr<State>& state, const std::string& peer_key, Roe<std::string> request_json,
                    const char* expect_label, std::function<void(Roe<ResponseT>)> on_done,
                    std::chrono::milliseconds timeout = kRoundTripTimeout) {
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto session_holder = std::make_shared<std::shared_ptr<pp::amp::ChannelSession>>();
  std::function<void(Roe<ResponseT>)> finish = [on_done = std::move(on_done), settled,
                                                 session_holder](Roe<ResponseT> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    // Take the session out of the holder: the session's frame handler captures this lambda, so
    // a holder left owning it would keep session → handler → holder → session alive forever.
    if (auto session = std::exchange(*session_holder, nullptr)) {
      session->Close();
    }
    if (on_done) {
      on_done(std::move(value));
    }
  };
  if (!request_json) {
    finish(request_json.error());
    return;
  }
  if (state->stopped.load(std::memory_order_acquire)) {
    finish(Error("amp broadcast client stopped"));
    return;
  }
  if (!IsPeerReachable(*state, peer_key)) {
    finish(Error("Peer-direct endpoint not registered")
               .WithUser("No usable peer address — add a dialable multiaddr on the contact."));
    return;
  }
  const auto deadline = Clock::now() + timeout;
  state->links->OpenChannel(
      peer_key, kRpcBroadcastProtocolId, pp::amp::ControlJsonChannelPolicy(),
      [state, peer_key, json = std::move(*request_json), expect_label, deadline, settled, finish,
       session_holder](IChatPeerLinks::ChannelRoe channel) {
        if (!channel) {
          finish(Error(channel.error().message));
          return;
        }
        if (state->stopped.load(std::memory_order_acquire)) {
          finish(Error("amp broadcast client stopped"));
          return;
        }
        const uint32_t channel_id = *channel;
        state->links->WhenChannelOpen(
            peer_key, channel_id, SteadyDeadlineMs(deadline),
            [state, peer_key, channel_id, json, expect_label, deadline, settled, finish, session_holder](bool open) {
              if (state->stopped.load(std::memory_order_acquire)) {
                finish(Error("amp broadcast client stopped"));
                return;
              }
              if (!open) {
                finish(Error("amp broadcast: channel open failed")
                           .WithUser("Broadcast control request didn't confirm."));
                return;
              }
              SendAndAwait<ResponseT>(state, peer_key, channel_id, json, expect_label, deadline, settled, finish,
                                      session_holder);
            });
      });
}

template <typename ResponseT>
Roe<ResponseT> RoundTrip(const std::shared_ptr<State>& state, const std::string& peer_key,
                         Roe<std::string> request_json, const char* expect_label) {
  auto promise = std::make_shared<std::promise<Roe<ResponseT>>>();
  auto future = promise->get_future();
  const auto deadline = Clock::now() + kRoundTripTimeout;
  RoundTripAsync<ResponseT>(state, peer_key, std::move(request_json), expect_label,
                            [promise](Roe<ResponseT> value) {
                              try {
                                promise->set_value(std::move(value));
                              } catch (const std::future_error&) {
                              }
                            });
  AmpParkUntil([&] { return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, deadline,
               state->io_pump);
  if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return Error("amp broadcast send timed out").WithUser("Broadcast control timed out.");
  }
  return future.get();
}

} // namespace

AmpBroadcastRpcClient::AmpBroadcastRpcClient(IChatPeerLinks& links, IoPump io_pump, IoPost post_io, IoAfter post_after)
    : state_(std::make_shared<State>()) {
  state_->links = &links;
  state_->io_pump = std::move(io_pump);
  state_->post_io = std::move(post_io);
  state_->post_after = std::move(post_after);
}

AmpBroadcastRpcClient::~AmpBroadcastRpcClient() {
  Stop();
}

void AmpBroadcastRpcClient::Stop() {
  state_->stopped.store(true, std::memory_order_release);
}

void AmpBroadcastRpcClient::RequestTicketAsync(const std::string& peer_key, const BroadcastTicketRequest& req,
                                               std::function<void(Roe<BroadcastTicketResponse>)> on_done) {
  RoundTripAsync<BroadcastTicketResponse>(state_, peer_key, EncodeBroadcastTicketRequest(req), "ticket_response",
                                          std::move(on_done));
}

void AmpBroadcastRpcClient::RequestViewerAttachAsync(const std::string& peer_key,
                                                     const BroadcastViewerAttachRequest& req,
                                                     std::function<void(Roe<BroadcastViewerAttachResult>)> on_done,
                                                     std::chrono::milliseconds timeout) {
  RoundTripAsync<BroadcastViewerAttachResult>(state_, peer_key, EncodeBroadcastViewerAttachRequest(req),
                                              "viewer_attach_result", std::move(on_done), timeout);
}

void AmpBroadcastRpcClient::RequestRelaySlotWinAsync(const std::string& peer_key,
                                                     const BroadcastRelaySlotWinRequest& req,
                                                     std::function<void(Roe<BroadcastRelaySlotWinResult>)> on_done) {
  RoundTripAsync<BroadcastRelaySlotWinResult>(state_, peer_key, EncodeBroadcastRelaySlotWinRequest(req),
                                              "relay_slot_win_result", std::move(on_done));
}

Roe<BroadcastTicketResponse> AmpBroadcastRpcClient::RequestTicket(const std::string& peer_key,
                                                                  const BroadcastTicketRequest& req) {
  return RoundTrip<BroadcastTicketResponse>(state_, peer_key, EncodeBroadcastTicketRequest(req), "ticket_response");
}

Roe<BroadcastViewerAttachResult> AmpBroadcastRpcClient::RequestViewerAttach(const std::string& peer_key,
                                                                            const BroadcastViewerAttachRequest& req) {
  return RoundTrip<BroadcastViewerAttachResult>(state_, peer_key, EncodeBroadcastViewerAttachRequest(req),
                                                "viewer_attach_result");
}

Roe<BroadcastRelaySlotWinResult> AmpBroadcastRpcClient::RequestRelaySlotWin(const std::string& peer_key,
                                                                            const BroadcastRelaySlotWinRequest& req) {
  return RoundTrip<BroadcastRelaySlotWinResult>(state_, peer_key, EncodeBroadcastRelaySlotWinRequest(req),
                                                "relay_slot_win_result");
}

} // namespace pbr
