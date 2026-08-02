#include "libp2p/integration/host/CallMediaDirectService.h"

#include "libp2p/integration/host/CallMediaFrameCrypto.h"
#include "libp2p/integration/host/StreamJsonFrame.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace pbr {

namespace {

using libp2p::Bytes;
using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

constexpr size_t kMaxCallMediaFrameBytes = 16 * 1024;

Roe<std::vector<uint8_t>> ReadExactFrame(const std::shared_ptr<Stream>& stream) {
  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  if (!header_future.get()) {
    return Error("Failed to read call-media frame header");
  }
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | header[i];
  }
  if (payload_len > kMaxCallMediaFrameBytes) {
    return Error("call-media frame too large");
  }
  Bytes payload(payload_len);
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read call-media frame body");
  }
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

Roe<void> WriteExactBody(const std::shared_ptr<Stream>& stream, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> frame(8 + body.size());
  uint64_t len = body.size();
  for (int i = 7; i >= 0; --i) {
    frame[static_cast<size_t>(i)] = static_cast<uint8_t>(len & 0xff);
    len >>= 8;
  }
  std::memcpy(frame.data() + 8, body.data(), body.size());
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(stream, libp2p::Bytes(frame), [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write call-media frame");
  }
  return {};
}

Roe<void> WriteJson(const std::shared_ptr<Stream>& stream, const nlohmann::json& root) {
  auto encoded = EncodeStreamJsonFrame(root.dump());
  if (!encoded) {
    return encoded.error();
  }
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(stream, libp2p::Bytes(*encoded),
                [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write call-media json");
  }
  return {};
}

Roe<nlohmann::json> ReadJson(const std::shared_ptr<Stream>& stream) {
  auto frame = ReadExactFrame(stream);
  if (!frame) {
    return frame.error();
  }
  auto json_utf8 = DecodeStreamJsonFrame(*frame);
  if (!json_utf8) {
    return json_utf8.error();
  }
  nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("invalid call-media json");
  }
  return root;
}

CallMediaDirectConnectParams ParamsFromHello(const nlohmann::json& root, const ByteVector& media_key) {
  CallMediaDirectConnectParams params;
  params.call_id = root.value("call_id", "");
  params.media_epoch = root.value("media_epoch", 1u);
  params.media_key = media_key;
  params.offerer = root.value("role", "") == "offerer";
  return params;
}

} // namespace

struct CallMediaDirectService::Impl : std::enable_shared_from_this<Impl> {
  std::mutex mu;
  std::shared_ptr<Stream> stream;
  std::mutex write_mu;
  CallMediaDirectConnectParams active_params;
  CallMediaDirectCallbacks callbacks;
  std::atomic<bool> reader_running{false};
  std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> inbound_handler;

  void FailLocked(const std::string& message) {
    if (callbacks.on_failed) {
      callbacks.on_failed(message);
    }
    DetachLocked();
  }

  void DetachLocked() {
    reader_running.store(false);
    if (stream) {
      stream->close([](auto&&) {});
      stream.reset();
    }
    callbacks = {};
    active_params = {};
  }

  void StartReader(const std::shared_ptr<Impl>& self) {
    if (reader_running.exchange(true)) {
      return;
    }
    std::thread([self]() {
      while (self->reader_running.load()) {
        std::shared_ptr<Stream> stream;
        CallMediaDirectConnectParams params;
        CallMediaDirectCallbacks cbs;
        {
          std::lock_guard lock(self->mu);
          stream = self->stream;
          params = self->active_params;
          cbs = self->callbacks;
        }
        if (!stream) {
          break;
        }
        auto frame = ReadExactFrame(stream);
        if (!frame) {
          self->reader_running.store(false);
          if (cbs.on_failed) {
            cbs.on_failed(frame.error().message);
          }
          break;
        }
        if ((*frame)[0] == '{') {
          continue; // stray json — ignore
        }
        auto opus = DecryptCallMediaAudioFrame(params.media_key, params.call_id, params.media_epoch, *frame);
        if (!opus) {
          continue;
        }
        if (cbs.on_audio) {
          cbs.on_audio(*opus);
        }
      }
      std::lock_guard lock(self->mu);
      self->DetachLocked();
    }).detach();
  }

  void HandleInbound(libp2p::StreamAndProtocol stream_in) {
    auto stream = std::move(stream_in.stream);
    std::thread([self = shared_from_this(), stream = std::move(stream)]() mutable {
      auto hello = ReadJson(stream);
      if (!hello || hello->value("type", "") != "hello") {
        stream->close([](auto&&) {});
        return;
      }
      CallMediaDirectConnectParams params;
      params.call_id = hello->value("call_id", "");
      params.media_epoch = hello->value("media_epoch", 1u);
      params.offerer = hello->value("role", "") == "offerer";

      CallMediaDirectCallbacks cbs;
      {
        std::lock_guard lock(self->mu);
        if (self->inbound_handler) {
          self->inbound_handler(params, cbs);
        }
      }
      if (params.media_key.empty() || params.call_id.empty()) {
        (void)WriteJson(stream, {{"v", 1}, {"type", "hello_ack"}, {"ok", false}, {"error", "rejected"}});
        stream->close([](auto&&) {});
        return;
      }
      if (!(WriteJson(stream, {{"v", 1}, {"type", "hello_ack"}, {"ok", true}}))) {
        stream->close([](auto&&) {});
        return;
      }
      {
        std::lock_guard lock(self->mu);
        self->stream = stream;
        self->active_params = params;
        self->callbacks = cbs;
      }
      if (cbs.on_connected) {
        cbs.on_connected();
      }
      self->StartReader(self);
    }).detach();
  }
};

CallMediaDirectService::CallMediaDirectService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_shared<Impl>()), host_(host), sessions_(sessions) {}

CallMediaDirectService::~CallMediaDirectService() {
  Stop();
}

void CallMediaDirectService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  auto impl = impl_;
  host_.GetHost().setProtocolHandler({ProtocolName{kCallMediaDirectProtocolId}},
                                     [impl](libp2p::StreamAndProtocol stream) {
                                       impl->HandleInbound(std::move(stream));
                                     });
}

void CallMediaDirectService::Stop() {
  started_ = false;
  Detach();
}

void CallMediaDirectService::SetInboundHandler(
    std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler) {
  std::lock_guard lock(impl_->mu);
  impl_->inbound_handler = std::move(handler);
}

bool CallMediaDirectService::IsActive() const {
  std::lock_guard lock(impl_->mu);
  return impl_->stream != nullptr;
}

void CallMediaDirectService::Detach() {
  std::lock_guard lock(impl_->mu);
  impl_->DetachLocked();
}

Roe<void> CallMediaDirectService::Connect(const CallMediaDirectConnectParams& params,
                                          CallMediaDirectCallbacks callbacks, int timeout_ms) {
  if (!host_.IsRunning()) {
    return Error("call-media host not running");
  }
  if (params.peer_key.empty() || params.call_id.empty() || params.media_key.empty()) {
    return Error("call-media connect params incomplete");
  }
  if (!sessions_.IsDialable(params.peer_key)) {
    return Error("call-media peer not dialable");
  }

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();

  sessions_.OpenStream(params.peer_key, {ProtocolName{kCallMediaDirectProtocolId}},
                       [this, params, callbacks = std::move(callbacks), settled, result_promise,
                        timeout_ms](outcome::result<libp2p::StreamAndProtocol> stream_res) mutable {
                         if (settled->exchange(true)) {
                           return;
                         }
                         if (!stream_res) {
                           try {
                             result_promise->set_value(Error("call-media dial failed"));
                           } catch (const std::future_error&) {
                           }
                           return;
                         }
                         auto stream = std::move(stream_res.value().stream);
                         const std::string role = params.offerer ? "offerer" : "answerer";
                         if (!(WriteJson(stream, {{"v", 1},
                                                  {"type", "hello"},
                                                  {"call_id", params.call_id},
                                                  {"media_epoch", params.media_epoch},
                                                  {"role", role}}))) {
                           try {
                             result_promise->set_value(Error("call-media hello write failed"));
                           } catch (const std::future_error&) {
                           }
                           stream->close([](auto&&) {});
                           return;
                         }
                         auto ack = ReadJson(stream);
                         if (!ack || !ack->value("ok", false)) {
                           try {
                             result_promise->set_value(Error(ack ? ack->value("error", "hello rejected")
                                                                 : ack.error().message));
                           } catch (const std::future_error&) {
                           }
                           stream->close([](auto&&) {});
                           return;
                         }
                         {
                           std::lock_guard lock(impl_->mu);
                           impl_->stream = stream;
                           impl_->active_params = params;
                           impl_->callbacks = callbacks;
                         }
                         if (callbacks.on_connected) {
                           callbacks.on_connected();
                         }
                         impl_->StartReader(impl_);
                         try {
                           result_promise->set_value({});
                         } catch (const std::future_error&) {
                         }
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 15000) + 1000;
  if (result_future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
    settled->exchange(true);
    return Error("call-media connect timed out");
  }
  return result_future.get();
}

Roe<void> CallMediaDirectService::SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark) {
  std::shared_ptr<Stream> stream;
  CallMediaDirectConnectParams params;
  {
    std::lock_guard lock(impl_->mu);
    stream = impl_->stream;
    params = impl_->active_params;
  }
  if (!stream) {
    return Error("call-media not connected");
  }
  auto body = EncryptCallMediaAudioFrame(params.media_key, params.call_id, params.media_epoch, seq, mark,
                                         opus_payload);
  if (!body) {
    return body.error();
  }
  std::lock_guard wlock(impl_->write_mu);
  return WriteExactBody(stream, *body);
}

} // namespace pbr
