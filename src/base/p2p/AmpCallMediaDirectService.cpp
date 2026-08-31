#include "base/p2p/AmpCallMediaDirectService.h"

#include "base/mesh/channel/ChannelPolicy.h"
#include "base/mesh/channel/ChannelSession.h"
#include "base/p2p/CallMediaFrameCrypto.h"
#include "base/p2p/StreamFrameIo.h"

#include "common/ValueJson.h"

#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> Utf8Body(const std::string& utf8) {
  return std::vector<uint8_t>(utf8.begin(), utf8.end());
}

Roe<Object> ParseJsonObject(const std::string& json_utf8) {
  auto root = TryParseObject(json_utf8);
  if (!root) {
    return Error("invalid call-media json");
  }
  return *root;
}

std::string BuildHelloJson(const CallMediaDirectConnectParams& params) {
  Object hello;
  hello.set("v", int64_t{1});
  hello.set("type", "hello");
  hello.set("call_id", params.call_id);
  hello.setJsonUInt("media_epoch", params.media_epoch);
  hello.set("role", params.offerer ? "offerer" : "answerer");
  return DumpJson(hello);
}

std::string BuildHelloAckJson(const bool ok, const char* error = nullptr) {
  Object ack;
  ack.set("v", int64_t{1});
  ack.set("type", "hello_ack");
  ack.set("ok", ok);
  if (!ok && error) {
    ack.set("error", error);
  }
  return DumpJson(ack);
}

enum class AmpCallLeg {
  None,
  ControlHello,
  AwaitingMedia,
  Media,
};

void RunWorker(const AmpCallMediaDirectService::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

} // namespace

struct AmpCallMediaDirectService::Impl {
  amp::PeerLinkManager* links = nullptr;
  IoPump io_pump;
  AmpCallMediaDirectService::WorkerPost post_worker;
  std::mutex mu;
  InboundHandler inbound;
  std::atomic<bool> stopped{false};
  std::atomic<CallMediaSessionPhase> phase{CallMediaSessionPhase::Idle};
  std::atomic<AmpCallLeg> leg{AmpCallLeg::None};

  CallMediaDirectConnectParams active_params;
  CallMediaDirectCallbacks callbacks;
  amp::PeerLink* active_link = nullptr;
  std::shared_ptr<amp::ChannelSession> control_session;
  std::shared_ptr<amp::ChannelSession> media_session;
  std::shared_ptr<std::promise<Roe<void>>> connect_promise;
  std::shared_ptr<std::atomic<bool>> connect_settled;
  std::atomic<bool> control_ready{false};
  std::atomic<bool> media_bound{false};
  Clock::time_point connect_deadline{};

  void PumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      }
    }
  }

  void ResetFieldsUnlocked() {
    active_link = nullptr;
    callbacks = {};
    active_params = {};
    leg.store(AmpCallLeg::None, std::memory_order_release);
    phase.store(CallMediaSessionPhase::Idle, std::memory_order_release);
    control_ready.store(false, std::memory_order_release);
    media_bound.store(false, std::memory_order_release);
    connect_promise.reset();
    connect_settled.reset();
  }

  void ResetLocked() {
    control_session.reset();
    media_session.reset();
    ResetFieldsUnlocked();
  }

  void OnChannelClosed() {
    std::lock_guard lock(mu);
    if (phase.load(std::memory_order_acquire) == CallMediaSessionPhase::Idle) {
      return;
    }
    control_session.reset();
    media_session.reset();
    ResetFieldsUnlocked();
  }

  void CompleteConnect(Roe<void> result) {
    if (connect_settled && !connect_settled->exchange(true, std::memory_order_acq_rel) && connect_promise) {
      try {
        connect_promise->set_value(std::move(result));
      } catch (const std::future_error&) {
      }
    }
  }

  void EnterMediaReady() {
    leg.store(AmpCallLeg::Media, std::memory_order_release);
    phase.store(CallMediaSessionPhase::MediaReady, std::memory_order_release);
    CallMediaDirectCallbacks cbs;
    {
      std::lock_guard lock(mu);
      cbs = callbacks;
    }
    if (cbs.on_connected) {
      cbs.on_connected();
    }
    CompleteConnect({});
  }

  void TryEnterMediaReady() {
    if (!control_ready.load(std::memory_order_acquire) || !media_bound.load(std::memory_order_acquire)) {
      return;
    }
    if (leg.load(std::memory_order_acquire) == AmpCallLeg::Media) {
      return;
    }
    EnterMediaReady();
  }

  void OpenMediaChannelOnActiveLink(const Clock::time_point deadline) {
    amp::PeerLink* link = nullptr;
    {
      std::lock_guard lock(mu);
      link = active_link;
    }
    if (!link || !link->Mux()) {
      CompleteConnect(Error("amp call-media: no link for media channel"));
      return;
    }
    auto channel_id = link->Mux()->OpenOutbound(kCallMediaDirectProtocolId, amp::CallMediaChannelPolicy());
    if (!channel_id) {
      CompleteConnect(channel_id.error());
      return;
    }
    PumpUntil(
        [&] {
          return link->Mux()->State(*channel_id) == amp::ChannelState::Open;
        },
        deadline);
    if (link->Mux()->State(*channel_id) != amp::ChannelState::Open) {
      CompleteConnect(Error("amp call-media: media channel open failed"));
      return;
    }
    BindMediaChannel(*link, *channel_id);
    TryEnterMediaReady();
  }

  bool HandleControlJson(const std::string& json_utf8, const std::shared_ptr<amp::ChannelSession>& channel_session,
                         const Clock::time_point deadline) {
    auto parsed = ParseJsonObject(json_utf8);
    if (!parsed) {
      return false;
    }
    const auto type = parsed->getString("type").value_or("");
    if (type == "hello") {
      phase.store(CallMediaSessionPhase::HelloInbound, std::memory_order_release);
      CallMediaDirectConnectParams params;
      params.call_id = parsed->getString("call_id").value_or("");
      params.media_epoch = static_cast<uint32_t>(parsed->getNonNegInt("media_epoch").value_or(1));
      params.offerer = parsed->getString("role").value_or("") == "offerer";

      CallMediaDirectConnectParams answer_params = params;
      CallMediaDirectCallbacks answer_cbs;
      InboundHandler handler;
      {
        std::lock_guard lock(mu);
        handler = inbound;
      }
      if (!handler) {
        if (!channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "no handler")))) {
          return false;
        }
        if (io_pump) {
          io_pump();
        }
        CompleteConnect(Error("amp call-media: inbound rejected"));
        return false;
      }
      RunWorker(post_worker, [this, channel_session, answer_params, answer_cbs = std::move(answer_cbs),
                              handler = std::move(handler)]() mutable {
        handler(answer_params, answer_cbs);
        if (!channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(true)))) {
          CompleteConnect(Error("amp call-media: hello ack failed"));
          return;
        }
        if (io_pump) {
          io_pump();
        }
        {
          std::lock_guard lock(mu);
          active_params = answer_params;
          callbacks = std::move(answer_cbs);
          leg.store(AmpCallLeg::AwaitingMedia, std::memory_order_release);
        }
        control_ready.store(true, std::memory_order_release);
        TryEnterMediaReady();
      });
      return true;
    }
    if (type == "hello_ack") {
      if (!parsed->getIf<bool>("ok").value_or(false)) {
        CompleteConnect(Error("amp call-media: hello rejected"));
        return false;
      }
      control_ready.store(true, std::memory_order_release);
      leg.store(AmpCallLeg::AwaitingMedia, std::memory_order_release);
      OpenMediaChannelOnActiveLink(deadline);
      return true;
    }
    return false;
  }

  bool HandleMediaBody(const std::vector<uint8_t>& frame) {
    CallMediaDirectCallbacks cbs;
    CallMediaDirectConnectParams params;
    {
      std::lock_guard lock(mu);
      if (leg.load(std::memory_order_acquire) != AmpCallLeg::Media) {
        return true;
      }
      cbs = callbacks;
      params = active_params;
    }
    if (frame.size() < 8) {
      return true;
    }
    const uint64_t len = DecodeLengthPrefixedHeader(std::vector<uint8_t>(frame.begin(), frame.begin() + 8));
    if (len + 8 != frame.size()) {
      return true;
    }
    std::vector<uint8_t> body(frame.begin() + static_cast<std::ptrdiff_t>(8), frame.end());
    auto decoded = DecryptCallMediaFrame(params.media_key, params.call_id, params.media_epoch, body);
    if (!decoded) {
      return true;
    }
    if (cbs.on_media) {
      cbs.on_media(decoded->channel, decoded->payload);
    } else if (decoded->channel == kCallMediaChannelAudio && cbs.on_audio) {
      cbs.on_audio(decoded->payload);
    }
    return true;
  }

  void BindControlChannel(amp::PeerLink& link, const uint32_t channel_id) {
    {
      std::lock_guard lock(mu);
      if (phase.load(std::memory_order_acquire) == CallMediaSessionPhase::MediaReady) {
        return;
      }
      active_link = &link;
    }
    auto channel_session = std::make_shared<amp::ChannelSession>();
    const auto deadline = connect_deadline.time_since_epoch().count() != 0
                              ? connect_deadline
                              : Clock::now() + std::chrono::seconds(15);
    channel_session->Bind(*link.Mux(), channel_id, amp::CallMediaControlChannelPolicy(),
                          [this, channel_session, deadline](Roe<std::vector<uint8_t>> frame) {
                            if (!frame) {
                              return false;
                            }
                            const std::string json_utf8(frame->begin(), frame->end());
                            (void)HandleControlJson(json_utf8, channel_session, deadline);
                            return true;
                          },
                          [this](const char*) { OnChannelClosed(); });
    std::lock_guard lock(mu);
    control_session = std::move(channel_session);
  }

  void BindMediaChannel(amp::PeerLink& link, const uint32_t channel_id) {
    {
      std::lock_guard lock(mu);
      active_link = &link;
    }
    auto channel_session = std::make_shared<amp::ChannelSession>();
    channel_session->Bind(*link.Mux(), channel_id, amp::CallMediaChannelPolicy(),
                          [this](Roe<std::vector<uint8_t>> frame) {
                            if (!frame) {
                              return false;
                            }
                            return HandleMediaBody(*frame);
                          },
                          [this](const char*) { OnChannelClosed(); });
    {
      std::lock_guard lock(mu);
      media_session = std::move(channel_session);
    }
    media_bound.store(true, std::memory_order_release);
    TryEnterMediaReady();
  }

  void HandleInboundChannel(amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire)) {
      return;
    }
    if (!link.Mux()) {
      return;
    }
    const auto cls = link.Mux()->Class(channel_id);
    if (cls == amp::ChannelClass::RealtimeControl) {
      BindControlChannel(link, channel_id);
      return;
    }
    if (cls == amp::ChannelClass::Realtime) {
      BindMediaChannel(link, channel_id);
    }
  }
};

AmpCallMediaDirectService::AmpCallMediaDirectService(amp::PeerLinkManager& links, IoPump io_pump,
                                                     WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
}

AmpCallMediaDirectService::~AmpCallMediaDirectService() {
  Stop();
}

void AmpCallMediaDirectService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kCallMediaDirectProtocolId, [impl = impl_.get()](amp::PeerLink& link, const uint32_t ch) {
    impl->HandleInboundChannel(link, ch);
  });
}

void AmpCallMediaDirectService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kCallMediaDirectProtocolId);
  Detach();
  ClearInboundHandler();
}

void AmpCallMediaDirectService::SetInboundHandler(InboundHandler handler) {
  std::lock_guard lock(impl_->mu);
  impl_->inbound = std::move(handler);
}

void AmpCallMediaDirectService::ClearInboundHandler() {
  std::lock_guard lock(impl_->mu);
  impl_->inbound = nullptr;
}

bool AmpCallMediaDirectService::IsActive() const {
  return impl_->phase.load(std::memory_order_acquire) == CallMediaSessionPhase::MediaReady;
}

CallMediaDirectConnectParams AmpCallMediaDirectService::ActiveParams() const {
  std::lock_guard lock(impl_->mu);
  return impl_->active_params;
}

CallMediaSessionPhase AmpCallMediaDirectService::Phase() const {
  return impl_->phase.load(std::memory_order_acquire);
}

void AmpCallMediaDirectService::Detach() {
  std::shared_ptr<amp::ChannelSession> control;
  std::shared_ptr<amp::ChannelSession> media;
  {
    std::lock_guard lock(impl_->mu);
    const bool connect_in_flight =
        impl_->connect_settled && !impl_->connect_settled->load(std::memory_order_acquire);
    if (connect_in_flight) {
      impl_->CompleteConnect(Error("call-media aborted"));
    }
    control = std::move(impl_->control_session);
    media = std::move(impl_->media_session);
    impl_->ResetFieldsUnlocked();
  }
  if (control) {
    control->Close();
  }
  if (media) {
    media->Close();
  }
}

Roe<void> AmpCallMediaDirectService::Connect(const CallMediaDirectConnectParams& params,
                                             CallMediaDirectCallbacks callbacks, const int timeout_ms) {
  if (!started_) {
    return Error("amp call-media service not started");
  }
  if (params.peer_key.empty() || params.call_id.empty() || params.media_key.empty()) {
    return Error("amp call-media: invalid connect params");
  }
  if (!links_.GetLinkSnapshot(params.peer_key).has_endpoint) {
    return Error("amp call-media: peer endpoint not registered");
  }

  Detach();

  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
  auto connect_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = connect_promise->get_future();
  auto connect_settled = std::make_shared<std::atomic<bool>>(false);

  const std::string peer_key = params.peer_key;
  {
    std::lock_guard lock(impl_->mu);
    impl_->active_params = params;
    impl_->callbacks = callbacks;
    impl_->leg.store(AmpCallLeg::ControlHello, std::memory_order_release);
    impl_->phase.store(CallMediaSessionPhase::HelloOutbound, std::memory_order_release);
    impl_->connect_promise = connect_promise;
    impl_->connect_settled = connect_settled;
    impl_->connect_deadline = deadline;
  }

  links_.OpenChannel(peer_key, kCallMediaDirectProtocolId, amp::CallMediaControlChannelPolicy(),
                     [this, peer_key, params, deadline](Roe<uint32_t> channel) mutable {
                       if (!channel) {
                         impl_->CompleteConnect(channel.error());
                         return;
                       }
                       impl_->PumpUntil(
                           [&] {
                             auto* link = links_.FindLink(peer_key);
                             return link && link->Mux() &&
                                    link->Mux()->State(*channel) == amp::ChannelState::Open;
                           },
                           deadline);
                       auto* link = links_.FindLink(peer_key);
                       if (!link || !link->Mux() ||
                           link->Mux()->State(*channel) != amp::ChannelState::Open) {
                         impl_->CompleteConnect(Error("amp call-media: channel open failed"));
                         return;
                       }
                       impl_->BindControlChannel(*link, *channel);
                       std::shared_ptr<amp::ChannelSession> session;
                       {
                         std::lock_guard lock(impl_->mu);
                         session = impl_->control_session;
                       }
                       if (!session || !session->EnqueueOutbound(Utf8Body(BuildHelloJson(params)))) {
                         impl_->CompleteConnect(Error("amp call-media: hello write failed"));
                       }
                     });

  impl_->PumpUntil(
      [&] {
        if (result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
          return true;
        }
        return impl_->phase.load(std::memory_order_acquire) == CallMediaSessionPhase::MediaReady;
      },
      deadline);

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    impl_->CompleteConnect(Error("amp call-media connect timed out"));
    Detach();
    return Error("amp call-media connect timed out");
  }
  auto result = result_future.get();
  if (!result) {
    Detach();
    return result.error();
  }
  if (impl_->phase.load(std::memory_order_acquire) != CallMediaSessionPhase::MediaReady) {
    Detach();
    return Error("amp call-media connect incomplete");
  }
  return Roe<void>();
}

Roe<void> AmpCallMediaDirectService::SendMedia(const uint8_t channel, const std::vector<uint8_t>& payload,
                                               const uint32_t seq, const uint8_t mark) {
  std::shared_ptr<amp::ChannelSession> session;
  CallMediaDirectConnectParams params;
  {
    std::lock_guard lock(impl_->mu);
    if (impl_->leg.load(std::memory_order_acquire) != AmpCallLeg::Media || !impl_->media_session) {
      return Error("amp call-media: not in media ready");
    }
    session = impl_->media_session;
    params = impl_->active_params;
  }
  auto encrypted =
      EncryptCallMediaFrame(params.media_key, params.call_id, params.media_epoch, seq, mark, channel, payload);
  if (!encrypted) {
    return encrypted.error();
  }
  auto framed = EncodeLengthPrefixedFrame(*encrypted);
  if (!session->EnqueueOutbound(std::move(framed))) {
    return Error("amp call-media: send queue full");
  }
  if (io_pump_) {
    io_pump_();
  }
  return Roe<void>();
}

Roe<void> AmpCallMediaDirectService::SendAudio(const std::vector<uint8_t>& opus_payload, const uint32_t seq,
                                               const uint8_t mark) {
  return SendMedia(kCallMediaChannelAudio, opus_payload, seq, mark);
}

} // namespace pbr
