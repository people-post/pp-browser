#include "feature/calls/CallMediaDoorway.h"

#include "common/runtime/AppRuntime.h"
#include "common/media/MediaSessionsOwner.h"

#include <type_traits>
#include <utility>
#include <variant>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Inbound hello waits this long for the epoch key (offerer dials before the relay delivers it). */
constexpr int kKeyWaitMs = 8000;
/** Re-ask for the key this often while a hello is parked. */
constexpr int kKeyPollMs = 250;

} // namespace

CallMediaDoorway::CallMediaDoorway(ICallMediaTransport& transport) : transport_(transport), key_wait_ms_(kKeyWaitMs) {
  redirectLogger("CallMediaDoorway");
}

CallMediaDoorway::~CallMediaDoorway() {
  shut_down_.store(true, std::memory_order_release);
  RejectPending("doorway gone");
  // Do not ClearInboundHandler here: a replacement may already have installed its own. Ours holds no
  // pointer to us — a late hello reaches a dropped outbox and is NACKed (the offerer retries).
}

void CallMediaDoorway::SetOutbox(OwnerOutbox<DoorwayEvent> outbox) {
  outbox_ = std::move(outbox);
  InstallHandler();
}

void CallMediaDoorway::SetPorts(CallMediaInboundPorts ports) {
  ports_ = std::move(ports);
  ports_set_ = true;
  InstallHandler();
}

void CallMediaDoorway::SetKeyWaitMsForTest(const int wait_ms) {
  key_wait_ms_ = wait_ms > 0 ? wait_ms : kKeyWaitMs;
}

void CallMediaDoorway::Handle(DoorwayEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, doorway_event::InboundHello>) {
          OnHello(std::move(e.hello));
        } else if constexpr (std::is_same_v<E, doorway_event::KeyAvailable>) {
          RecheckPending(/*ask_again=*/false);
        } else if constexpr (std::is_same_v<E, doorway_event::KeyPollTick>) {
          if (key_poll_timer_id_ == 0) {
            return; // cancelled since
          }
          key_poll_timer_id_ = 0;
          RecheckPending(/*ask_again=*/true);
          ArmKeyPoll();
        } else {
          static_assert(!sizeof(E), "handle every DoorwayEvent");
        }
      },
      event);
}

void CallMediaDoorway::Shutdown() {
  shut_down_.store(true, std::memory_order_release);
  RejectPending("shutting down");
  if (installed_) {
    transport_.ClearInboundHandler();
    installed_ = false;
  }
}

void CallMediaDoorway::InstallHandler() {
  if (!ports_set_ || !outbox_.IsBound() || shut_down_.load(std::memory_order_acquire)) {
    return; // both ports and the outbox are needed; the later of the two installs
  }
  // Called on the transport's IO hop: hand the hello over as an event and return. The handler holds
  // only the outbox — never this doorway.
  transport_.SetInboundHandler([outbox = outbox_](CallMediaDirectConnectParams params, CallMediaInboundAnswer answer) {
    outbox.Emit(
        doorway_event::InboundHello{std::make_shared<CallMediaInboundReply>(std::move(params), std::move(answer))});
  });
  installed_ = true;
}

void CallMediaDoorway::NotifyKeyAvailable() {
  outbox_.Emit(doorway_event::KeyAvailable{});
}

void CallMediaDoorway::OnHello(std::shared_ptr<CallMediaInboundReply> hello) {
  PP_ASSERT_ON_OWNER(kMediaSessionsOwner);
  CallMediaDirectConnectParams& params = hello->Params();
  log().info << "Inbound hello call_id=" << params.call_id << " epoch=" << params.media_epoch
             << " peer=" << (params.peer_key.empty() ? "(empty)" : params.peer_key);
  if (shut_down_.load(std::memory_order_acquire) || !ports_.session_open || !ports_.session_open(params.call_id)) {
    log().warning << "Inbound rejected: no open session call_id=" << params.call_id;
    hello->Reject();
    return;
  }
  if (TryLoadKey(params)) {
    Accept(*hello);
    return;
  }
  if (ports_.request_key) {
    ports_.request_key(params.call_id);
  }
  pending_.push_back(
      PendingHello{std::move(hello), std::chrono::steady_clock::now() + std::chrono::milliseconds(key_wait_ms_)});
  ArmKeyPoll();
}

bool CallMediaDoorway::TryLoadKey(CallMediaDirectConnectParams& params) const {
  if (!ports_.load_key) {
    return false;
  }
  if (auto key = ports_.load_key(params.call_id, params.media_epoch)) {
    params.media_key = std::move(*key);
    return true;
  }
  return false;
}

void CallMediaDoorway::Accept(CallMediaInboundReply& reply) {
  CallMediaDirectCallbacks cbs;
  if (ports_.on_accepted) {
    const CallMediaDirectConnectParams& params = reply.Params();
    CallMediaInboundHello hello;
    hello.call_id = params.call_id;
    hello.media_epoch = params.media_epoch;
    hello.peer_id = params.peer_key;
    cbs = ports_.on_accepted(hello);
  }
  reply.Accept(std::move(cbs));
}

void CallMediaDoorway::RecheckPending(const bool ask_again) {
  PP_ASSERT_ON_OWNER(kMediaSessionsOwner);
  const auto now = std::chrono::steady_clock::now();
  std::vector<PendingHello> still;
  auto parked = std::move(pending_);
  pending_.clear();
  for (auto& hello : parked) {
    CallMediaDirectConnectParams& params = hello.reply->Params();
    if (!ports_.session_open || !ports_.session_open(params.call_id)) {
      hello.reply->Reject(); // ended while parked
      continue;
    }
    if (TryLoadKey(params)) {
      Accept(*hello.reply);
      continue;
    }
    if (now >= hello.deadline) {
      // No key in time: the transport NACKs the hello; the offerer retries.
      log().info << "Inbound rejected: media key not ready call_id=" << params.call_id;
      hello.reply->Reject();
      continue;
    }
    if (ask_again && ports_.request_key) {
      ports_.request_key(params.call_id);
    }
    still.push_back(std::move(hello));
  }
  // Appended, not assigned: an answer runs inline and could, in principle, park another hello.
  for (auto& hello : still) {
    pending_.push_back(std::move(hello));
  }
}

void CallMediaDoorway::ArmKeyPoll() {
  if (key_poll_timer_id_ != 0 || pending_.empty()) {
    return;
  }
  key_poll_timer_id_ = outbox_.After(std::chrono::milliseconds(kKeyPollMs), doorway_event::KeyPollTick{});
}

void CallMediaDoorway::RejectPending(const char* why) {
  outbox_.Cancel(key_poll_timer_id_);
  auto parked = std::move(pending_);
  pending_.clear();
  for (auto& hello : parked) {
    log().info << "Inbound rejected (" << why << ") call_id=" << hello.reply->Params().call_id;
    hello.reply->Reject();
  }
}

} // namespace pbr
