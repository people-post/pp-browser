#include "feature/messaging/Libp2pDirectChatService.h"

#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/StreamJsonFrame.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

void CloseQuiet(const std::shared_ptr<Stream>& stream) {
  if (stream) {
    stream->close([](auto&&) {});
  }
}

} // namespace

struct Libp2pDirectChatService::Impl {
  std::mutex handler_mutex;
  Libp2pHost* host = nullptr;
  Libp2pExecutorConfig executor_config;
  InboundHandler inbound;

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    if (!host) {
      return;
    }
    auto stream = std::move(stream_and_protocol.stream);
    PostLibp2pWorker(*host, WorkerLane::Normal, [this, stream = std::move(stream)]() mutable {
      auto json_utf8 = BlockingReadStreamJson(stream, kMaxRelayEnvelopeJsonBytes);
      if (!json_utf8) {
        CloseQuiet(stream);
        return;
      }
      nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
      if (root.is_discarded()) {
        CloseQuiet(stream);
        return;
      }
      auto envelope = ParseRelayEnvelope(root);
      if (!envelope) {
        CloseQuiet(stream);
        return;
      }

      static const std::string kAck = R"({"ok":true})";
      (void)BlockingWriteStreamJson(stream, kAck, kMaxRelayEnvelopeJsonBytes);
      CloseQuiet(stream);

      InboundHandler handler;
      {
        std::lock_guard lock(handler_mutex);
        handler = inbound;
      }
      if (handler) {
        handler(std::move(*envelope));
      }
    });
  }
};

Libp2pDirectChatService::Libp2pDirectChatService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_unique<Impl>()), host_(host), sessions_(sessions) {
  impl_->host = &host_;
}

Libp2pDirectChatService::~Libp2pDirectChatService() {
  Stop();
}

void Libp2pDirectChatService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  host_.GetHost().setProtocolHandler({ProtocolName{kDirectChatProtocolId}},
                                     [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
                                       impl->HandleStream(std::move(stream));
                                     });
}

void Libp2pDirectChatService::Stop() {
  started_ = false;
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = nullptr;
}

void Libp2pDirectChatService::SetExecutorConfig(Libp2pExecutorConfig config) {
  std::lock_guard lock(impl_->handler_mutex);
  impl_->executor_config = config;
}

void Libp2pDirectChatService::SetInboundHandler(InboundHandler handler) {
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = std::move(handler);
}

bool Libp2pDirectChatService::IsPeerReachable(const std::string& peer_identity_value) const {
  return sessions_.IsDialable(peer_identity_value);
}

Roe<void> Libp2pDirectChatService::SendEnvelope(const std::string& peer_relay_user_id,
                                                const RelayEnvelope& envelope) {
  if (!started_ || !host_.IsRunning()) {
    return Error("libp2p direct chat service not started");
  }
  if (!sessions_.IsDialable(peer_relay_user_id)) {
    return Error("Peer-direct endpoint not registered")
        .WithUser("No usable peer address — add a dialable multiaddr on the contact.");
  }

  const std::string envelope_json = RelayEnvelopeToJson(envelope).dump();

  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();

  sessions_.OpenStream(peer_relay_user_id, {ProtocolName{kDirectChatProtocolId}},
                       [host = &host_, envelope_json, result_promise](
                           libp2p::StreamAndProtocolOrError stream_res) mutable {
                         PostLibp2pWorker(*host, WorkerLane::Normal,
                                          [envelope_json, result_promise,
                                           stream_res = std::move(stream_res)]() mutable {
                           auto finish = [&](Roe<void> value) {
                             try {
                               result_promise->set_value(std::move(value));
                             } catch (const std::future_error&) {
                             }
                           };
                           if (!stream_res) {
                             finish(Error("libp2p chat stream open failed")
                                        .WithUser("Reached the peer but chat handshake failed."));
                             return;
                           }
                           auto stream = std::move(stream_res.value().stream);
                           if (!BlockingWriteStreamJson(stream, envelope_json, kMaxRelayEnvelopeJsonBytes)) {
                             CloseQuiet(stream);
                             finish(Error("Failed to send direct chat envelope")
                                        .WithUser("Direct send didn't confirm — will use relay if available."));
                             return;
                           }
                           auto ack = BlockingReadStreamJson(stream, kMaxRelayEnvelopeJsonBytes);
                           CloseQuiet(stream);
                           if (!ack) {
                             finish(Error("Failed to read direct chat ack")
                                        .WithUser("Direct send didn't confirm — will use relay if available."));
                             return;
                           }
                           finish({});
                         });
                       });

  constexpr int kDirectChatSendTimeoutMs = 4000;
  if (result_future.wait_for(std::chrono::milliseconds(kDirectChatSendTimeoutMs)) !=
      std::future_status::ready) {
    return Error("libp2p chat send timed out")
        .WithUser("Direct send didn't confirm — will use relay if available.");
  }
  return result_future.get();
}

} // namespace pbr
