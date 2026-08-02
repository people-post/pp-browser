#include "libp2p/integration/host/CallMediaDirectService.h"

#include "common/Logger.h"
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
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

namespace pbr {

namespace {

using libp2p::Bytes;
using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

constexpr size_t kMaxCallMediaFrameBytes = 16 * 1024;
constexpr size_t kMaxOutboundFrames = 64;

std::vector<uint8_t> MakeLengthPrefixedFrame(const std::vector<uint8_t>& body) {
  std::vector<uint8_t> frame(8 + body.size());
  uint64_t len = body.size();
  for (int i = 7; i >= 0; --i) {
    frame[static_cast<size_t>(i)] = static_cast<uint8_t>(len & 0xff);
    len >>= 8;
  }
  std::memcpy(frame.data() + 8, body.data(), body.size());
  return frame;
}

Roe<std::vector<uint8_t>> ReadExactFrame(const std::shared_ptr<Stream>& stream) {
  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  auto header_res = header_future.get();
  if (!header_res) {
    return Error(std::string("Failed to read call-media frame header: ") + header_res.error().message());
  }
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | header[i];
  }
  if (payload_len == 0) {
    return Error("call-media frame empty");
  }
  if (payload_len > kMaxCallMediaFrameBytes) {
    return Error("call-media frame too large len=" + std::to_string(payload_len));
  }
  Bytes payload(static_cast<size_t>(payload_len));
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  auto body_res = body_future.get();
  if (!body_res) {
    return Error(std::string("Failed to read call-media frame body len=") + std::to_string(payload_len) +
                 ": " + body_res.error().message());
  }
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

Roe<void> WriteJson(const std::shared_ptr<Stream>& stream, const nlohmann::json& root) {
  auto encoded = EncodeStreamJsonFrame(root.dump());
  if (!encoded) {
    return encoded.error();
  }
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(stream, *encoded, [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write call-media json");
  }
  return {};
}

Roe<nlohmann::json> ReadJson(const std::shared_ptr<Stream>& stream) {
  // ReadExactFrame already strips the 8-byte length prefix — payload is raw UTF-8 JSON.
  auto payload = ReadExactFrame(stream);
  if (!payload) {
    return payload.error();
  }
  nlohmann::json root =
      nlohmann::json::parse(std::string(payload->begin(), payload->end()), nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("invalid call-media json");
  }
  return root;
}

} // namespace

struct CallMediaDirectService::Impl : std::enable_shared_from_this<Impl> {
  Libp2pHost* host = nullptr;

  std::mutex mu;
  std::shared_ptr<Stream> stream;
  CallMediaDirectConnectParams active_params;
  CallMediaDirectCallbacks callbacks;
  std::atomic<bool> pump_running{false};
  std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> inbound_handler;

  std::mutex outbound_mu;
  std::deque<std::vector<uint8_t>> outbound_;
  std::atomic<bool> kick_pending{false};
  std::atomic<uint32_t> decrypt_fail_log_{0};
  std::atomic<uint32_t> drop_log_{0};

  // IO-thread-only media pump state (Yamux/Noise are not cross-thread safe).
  enum class ReadPhase { Idle, Header, Body };
  ReadPhase read_phase = ReadPhase::Idle;
  bool write_inflight = false;
  Bytes header_buf;
  Bytes payload_buf;

  void Fail(const std::string& message) {
    CallMediaDirectCallbacks cbs;
    {
      std::lock_guard lock(mu);
      cbs = callbacks;
      DetachLocked();
    }
    if (cbs.on_failed) {
      cbs.on_failed(message);
    }
  }

  void DetachLocked() {
    pump_running.store(false);
    {
      std::lock_guard ol(outbound_mu);
      outbound_.clear();
    }
    read_phase = ReadPhase::Idle;
    write_inflight = false;
    if (stream) {
      stream->close([](auto&&) {});
      stream.reset();
    }
    callbacks = {};
    active_params = {};
  }

  bool EnqueueOutbound(std::vector<uint8_t> body) {
    {
      std::lock_guard ol(outbound_mu);
      if (!pump_running.load()) {
        return false;
      }
      if (outbound_.size() >= kMaxOutboundFrames) {
        outbound_.pop_front();
        if ((drop_log_.fetch_add(1) % 50) == 0) {
          logging::getLogger("CallMediaDirect").warning << "Call-media outbound queue full; dropping oldest";
        }
      }
      outbound_.push_back(std::move(body));
    }
    Kick();
    return true;
  }

  void Kick() {
    if (!host || !pump_running.load()) {
      return;
    }
    bool expected = false;
    if (!kick_pending.compare_exchange_strong(expected, true)) {
      return;
    }
    auto self = shared_from_this();
    host->Post([self]() {
      self->kick_pending.store(false);
      if (!self->pump_running.load()) {
        return;
      }
      self->PumpIo();
    });
  }

  void StartIoPump() {
    if (pump_running.exchange(true)) {
      Kick();
      return;
    }
    Kick();
  }

  /** Runs only on host io_context thread. */
  void PumpIo() {
    TryWriteIo();
    TryReadIo();
  }

  void TryWriteIo() {
    if (!pump_running.load() || write_inflight) {
      return;
    }
    std::shared_ptr<Stream> s;
    {
      std::lock_guard lock(mu);
      s = stream;
    }
    if (!s) {
      return;
    }

    std::vector<uint8_t> body;
    {
      std::lock_guard ol(outbound_mu);
      if (outbound_.empty()) {
        return;
      }
      body = std::move(outbound_.front());
      outbound_.pop_front();
    }

    auto frame = std::make_shared<std::vector<uint8_t>>(MakeLengthPrefixedFrame(body));
    write_inflight = true;
    auto self = shared_from_this();
    libp2p::write(s, *frame, [self, frame](outcome::result<void> result) {
      self->write_inflight = false;
      if (!self->pump_running.load()) {
        return;
      }
      if (!result) {
        self->Fail("Failed to write call-media frame");
        return;
      }
      self->PumpIo();
    });
  }

  void TryReadIo() {
    if (!pump_running.load() || read_phase != ReadPhase::Idle) {
      return;
    }
    std::shared_ptr<Stream> s;
    {
      std::lock_guard lock(mu);
      s = stream;
    }
    if (!s) {
      return;
    }

    header_buf.assign(8, 0);
    read_phase = ReadPhase::Header;
    auto self = shared_from_this();
    libp2p::read(s, header_buf, [self](outcome::result<void> result) {
      if (!self->pump_running.load()) {
        self->read_phase = ReadPhase::Idle;
        return;
      }
      if (!result) {
        self->read_phase = ReadPhase::Idle;
        self->Fail(std::string("Failed to read call-media frame header: ") + result.error().message());
        return;
      }

      uint64_t payload_len = 0;
      for (size_t i = 0; i < 8; ++i) {
        payload_len = (payload_len << 8) | self->header_buf[i];
      }
      if (payload_len == 0 || payload_len > kMaxCallMediaFrameBytes) {
        self->read_phase = ReadPhase::Idle;
        self->Fail(payload_len == 0 ? "call-media frame empty"
                                    : "call-media frame too large len=" + std::to_string(payload_len));
        return;
      }

      std::shared_ptr<Stream> stream2;
      {
        std::lock_guard lock(self->mu);
        stream2 = self->stream;
      }
      if (!stream2) {
        self->read_phase = ReadPhase::Idle;
        return;
      }

      // Prefer draining uplink before waiting on body (keeps duplex moving).
      self->TryWriteIo();

      self->payload_buf.resize(static_cast<size_t>(payload_len));
      self->read_phase = ReadPhase::Body;
      libp2p::read(stream2, self->payload_buf, [self](outcome::result<void> body_res) {
        self->read_phase = ReadPhase::Idle;
        if (!self->pump_running.load()) {
          return;
        }
        if (!body_res) {
          self->Fail(std::string("Failed to read call-media frame body: ") + body_res.error().message());
          return;
        }

        CallMediaDirectConnectParams params;
        CallMediaDirectCallbacks cbs;
        {
          std::lock_guard lock(self->mu);
          params = self->active_params;
          cbs = self->callbacks;
        }

        std::vector<uint8_t> frame_bytes(self->payload_buf.begin(), self->payload_buf.end());
        if (!frame_bytes.empty() && frame_bytes[0] != '{') {
          auto opus =
              DecryptCallMediaAudioFrame(params.media_key, params.call_id, params.media_epoch, frame_bytes);
          if (!opus) {
            if ((self->decrypt_fail_log_.fetch_add(1) % 25) == 0) {
              logging::getLogger("CallMediaDirect").warning
                  << "Call-media decrypt failed call_id=" << params.call_id
                  << " epoch=" << params.media_epoch << " err=" << opus.error().message;
            }
          } else if (cbs.on_audio) {
            cbs.on_audio(*opus);
          }
        }

        self->PumpIo();
      });
    });
  }

  void HandleInbound(libp2p::StreamAndProtocol stream_in) {
    logging::getLogger("CallMediaDirect").warning << "Inbound call-media stream (protocol negotiated)";
    auto stream = std::move(stream_in.stream);
    std::thread([self = shared_from_this(), stream = std::move(stream)]() mutable {
      auto hello = ReadJson(stream);
      if (!hello || hello->value("type", "") != "hello") {
        logging::getLogger("CallMediaDirect").warning
            << "Inbound call-media hello read failed err="
            << (hello ? "bad type" : hello.error().message);
        stream->close([](auto&&) {});
        return;
      }
      CallMediaDirectConnectParams params;
      params.call_id = hello->value("call_id", "");
      params.media_epoch = hello->value("media_epoch", 1u);
      params.offerer = hello->value("role", "") == "offerer";

      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler;
      {
        std::lock_guard lock(self->mu);
        handler = self->inbound_handler;
      }
      CallMediaDirectCallbacks cbs;
      if (handler) {
        handler(params, cbs);
      }
      if (params.media_key.empty() || params.call_id.empty()) {
        logging::getLogger("CallMediaDirect").warning
            << "Inbound call-media hello rejected call_id=" << params.call_id
            << " key_empty=" << (params.media_key.empty() ? 1 : 0);
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
      self->StartIoPump();
      if (cbs.on_connected) {
        cbs.on_connected();
      }
    }).detach();
  }
};

CallMediaDirectService::CallMediaDirectService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_shared<Impl>()), host_(host), sessions_(sessions) {
  impl_->host = &host_;
}

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

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 15000) + 1000;

  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();

  sessions_.OpenStream(params.peer_key, {ProtocolName{kCallMediaDirectProtocolId}},
                       [this, params, callbacks = std::move(callbacks), settled, result_promise](
                           outcome::result<libp2p::StreamAndProtocol> stream_res) mutable {
                         std::thread([this, params, callbacks = std::move(callbacks), settled, result_promise,
                                      stream_res = std::move(stream_res)]() mutable {
                           auto finish = [&](Roe<void> value) {
                             if (!settled->exchange(true)) {
                               try {
                                 result_promise->set_value(std::move(value));
                               } catch (const std::future_error&) {
                               }
                             }
                           };
                           if (!stream_res) {
                             std::string detail = "call-media dial failed";
                             try {
                               detail += ": ";
                               detail += stream_res.error().message();
                             } catch (...) {
                             }
                             logging::getLogger("CallMediaDirect").warning
                                 << "Call-media OpenStream failed peer=" << params.peer_key
                                 << " role=" << (params.offerer ? "offerer" : "answerer")
                                 << " err=" << detail;
                             finish(Error(detail));
                             return;
                           }
                           logging::getLogger("CallMediaDirect").warning
                               << "Call-media OpenStream ok peer=" << params.peer_key
                               << " role=" << (params.offerer ? "offerer" : "answerer")
                               << " call_id=" << params.call_id;
                           auto stream = std::move(stream_res.value().stream);
                           const std::string role = params.offerer ? "offerer" : "answerer";
                           if (!(WriteJson(stream, {{"v", 1},
                                                    {"type", "hello"},
                                                    {"call_id", params.call_id},
                                                    {"media_epoch", params.media_epoch},
                                                    {"role", role}}))) {
                             logging::getLogger("CallMediaDirect").warning
                                 << "Call-media hello write failed peer=" << params.peer_key;
                             finish(Error("call-media hello write failed"));
                             stream->close([](auto&&) {});
                             return;
                           }
                           auto ack = ReadJson(stream);
                           if (!ack || !ack->value("ok", false)) {
                             const std::string why =
                                 ack ? ack->value("error", "hello rejected") : ack.error().message;
                             logging::getLogger("CallMediaDirect").warning
                                 << "Call-media hello rejected peer=" << params.peer_key
                                 << " err=" << why;
                             finish(Error(why));
                             stream->close([](auto&&) {});
                             return;
                           }
                           {
                             std::lock_guard lock(impl_->mu);
                             impl_->stream = stream;
                             impl_->active_params = params;
                             impl_->callbacks = std::move(callbacks);
                           }
                           impl_->StartIoPump();
                           if (impl_->callbacks.on_connected) {
                             impl_->callbacks.on_connected();
                           }
                           finish({});
                         }).detach();
                       });

  if (result_future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
    settled->exchange(true);
    return Error("call-media connect timed out");
  }
  return result_future.get();
}

Roe<void> CallMediaDirectService::SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark) {
  CallMediaDirectConnectParams params;
  {
    std::lock_guard lock(impl_->mu);
    if (!impl_->stream) {
      return Error("call-media not connected");
    }
    params = impl_->active_params;
  }
  auto body = EncryptCallMediaAudioFrame(params.media_key, params.call_id, params.media_epoch, seq, mark,
                                         opus_payload);
  if (!body) {
    return body.error();
  }
  if (!impl_->EnqueueOutbound(std::move(*body))) {
    return Error("call-media pump not running");
  }
  return {};
}

} // namespace pbr
