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
  AwaitingHelloAck,
  Media,
};

} // namespace

struct AmpCallMediaDirectService::Impl {
  amp::PeerLinkManager* links = nullptr;
  IoPump io_pump;
  std::mutex mu;
  InboundHandler inbound;
  std::atomic<bool> stopped{false};
  std::atomic<CallMediaSessionPhase> phase{CallMediaSessionPhase::Idle};
  std::atomic<AmpCallLeg> leg{AmpCallLeg::None};

  CallMediaDirectConnectParams active_params;
  CallMediaDirectCallbacks callbacks;
  std::shared_ptr<amp::ChannelSession> session;
  std::shared_ptr<std::promise<Roe<void>>> connect_promise;
  std::shared_ptr<std::atomic<bool>> connect_settled;

  void PumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      }
    }
  }

  void ResetLocked() {
    if (session) {
      session->Close();
    }
    session.reset();
    callbacks = {};
    active_params = {};
    leg.store(AmpCallLeg::None, std::memory_order_release);
    phase.store(CallMediaSessionPhase::Idle, std::memory_order_release);
    connect_promise.reset();
    connect_settled.reset();
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

  bool HandleJsonFrame(const std::string& json_utf8, amp::ChannelSession& channel_session) {
    auto parsed = ParseJsonObject(json_utf8);
    if (!parsed) {
      return false;
    }
    const auto type = parsed->getString("type").value_or("");
    if (type == "hello") {
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
      if (handler) {
        handler(answer_params, answer_cbs);
      }
      if (!channel_session.EnqueueOutbound(Utf8Body(BuildHelloAckJson(true)))) {
        return false;
      }
      if (io_pump) {
        io_pump();
      }
      {
        std::lock_guard lock(mu);
        active_params = answer_params;
        callbacks = std::move(answer_cbs);
      }
      EnterMediaReady();
      return true;
    }
    if (type == "hello_ack") {
      if (!parsed->getIf<bool>("ok").value_or(false)) {
        CompleteConnect(Error("amp call-media: hello rejected"));
        return false;
      }
      EnterMediaReady();
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

  void BindCallChannel(amp::PeerLink& link, const uint32_t channel_id) {
    auto channel_session = std::make_shared<amp::ChannelSession>();
    channel_session->Bind(*link.Mux(), channel_id, amp::CallMediaChannelPolicy(),
                          [this, channel_session](Roe<std::vector<uint8_t>> frame) {
                            if (!frame) {
                              return false;
                            }
                            const auto leg_now = leg.load(std::memory_order_acquire);
                            if (leg_now != AmpCallLeg::Media) {
                              const std::string json_utf8(frame->begin(), frame->end());
                              (void)HandleJsonFrame(json_utf8, *channel_session);
                              return true;
                            }
                            return HandleMediaBody(*frame);
                          });
    std::lock_guard lock(mu);
    session = std::move(channel_session);
  }

  void HandleInboundChannel(amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire)) {
      return;
    }
    BindCallChannel(link, channel_id);
  }
};

AmpCallMediaDirectService::AmpCallMediaDirectService(amp::PeerLinkManager& links, IoPump io_pump)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
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
  std::lock_guard lock(impl_->mu);
  impl_->ResetLocked();
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
    impl_->leg.store(AmpCallLeg::AwaitingHelloAck, std::memory_order_release);
    impl_->connect_promise = connect_promise;
    impl_->connect_settled = connect_settled;
  }

  links_.OpenChannel(peer_key, kCallMediaDirectProtocolId, amp::CallMediaChannelPolicy(),
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
                       impl_->BindCallChannel(*link, *channel);
                       std::shared_ptr<amp::ChannelSession> session;
                       {
                         std::lock_guard lock(impl_->mu);
                         session = impl_->session;
                       }
                       if (!session || !session->EnqueueOutbound(Utf8Body(BuildHelloJson(params)))) {
                         impl_->CompleteConnect(Error("amp call-media: hello write failed"));
                       }
                     });

  impl_->PumpUntil(
      [&] {
        return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
               impl_->phase.load(std::memory_order_acquire) == CallMediaSessionPhase::MediaReady;
      },
      deadline);

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready ||
      impl_->phase.load(std::memory_order_acquire) != CallMediaSessionPhase::MediaReady) {
    impl_->CompleteConnect(Error("amp call-media connect timed out"));
    return Error("amp call-media connect timed out");
  }
  return result_future.get();
}

Roe<void> AmpCallMediaDirectService::SendMedia(const uint8_t channel, const std::vector<uint8_t>& payload,
                                               const uint32_t seq, const uint8_t mark) {
  std::shared_ptr<amp::ChannelSession> session;
  CallMediaDirectConnectParams params;
  {
    std::lock_guard lock(impl_->mu);
    if (impl_->leg.load(std::memory_order_acquire) != AmpCallLeg::Media || !impl_->session) {
      return Error("amp call-media: not in media ready");
    }
    session = impl_->session;
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
