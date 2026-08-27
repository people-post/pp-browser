#include "feature/messaging/Libp2pDirectChatService.h"

#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/StreamFrameIo.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>
#include "common/ValueJson.h"

namespace pbr {

namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

void CloseQuiet(const std::shared_ptr<Stream>& stream) {
  if (stream) {
    stream->close([](auto&&) {});
  }
}

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

} // namespace

struct Libp2pDirectChatService::Impl {
  std::mutex handler_mutex;
  Libp2pHost* host = nullptr;
  InboundHandler inbound;
  std::atomic<bool> stopped{false};

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    if (!host || stopped.load(std::memory_order_acquire)) {
      return;
    }
    auto stream = std::move(stream_and_protocol.stream);
    if (!stream) {
      return;
    }
    auto duplex = std::make_shared<DuplexFrameSession>();
    auto policy = ControlJsonIoPolicy(host->IoExecutor(), kDefaultControlFrameReadTimeout,
                                      kMaxRelayEnvelopeJsonBytes);
    duplex->Start(
        stream,
        [this, duplex, stream](Roe<std::vector<uint8_t>> frame) {
          if (!frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }
          auto body = *frame;
          PostLibp2pWorker(*host, WorkerLane::Normal, [this, duplex, stream, body = std::move(body)]() mutable {
            if (stopped.load(std::memory_order_acquire)) {
              host->Post([duplex, stream]() {
                duplex->Stop();
                CloseQuiet(stream);
              });
              return;
            }
            const std::string json_utf8(body.begin(), body.end());
            auto root = TryParseObject(json_utf8);
            if (!root) {
              host->Post([duplex, stream]() {
                duplex->Stop();
                CloseQuiet(stream);
              });
              return;
            }
            auto envelope = ParseRelayEnvelope(*root);
            if (!envelope) {
              host->Post([duplex, stream]() {
                duplex->Stop();
                CloseQuiet(stream);
              });
              return;
            }

            InboundHandler handler;
            {
              std::lock_guard lock(handler_mutex);
              handler = inbound;
            }
            static const std::string kAck = R"({"ok":true})";
            auto ack = JsonToBody(kAck);
            host->Post([this, duplex, stream, envelope = std::move(*envelope), handler = std::move(handler),
                        ack = std::move(ack)]() mutable {
              if (!duplex->EnqueueOutbound(std::move(ack), [this, duplex, stream, envelope = std::move(envelope),
                                                            handler = std::move(handler)](Roe<void> wrote) mutable {
                    duplex->Stop();
                    CloseQuiet(stream);
                    if (!wrote || !handler) {
                      return;
                    }
                    PostLibp2pWorker(*host, WorkerLane::Normal,
                                     [handler = std::move(handler), envelope = std::move(envelope)]() mutable {
                                       handler(std::move(envelope));
                                     });
                  })) {
                duplex->Stop();
                CloseQuiet(stream);
              }
            });
          });
          return true;
        },
        [this]() { return stopped.load(std::memory_order_acquire); }, std::move(policy));
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
  impl_->stopped.store(false, std::memory_order_release);
  host_.GetHost().setProtocolHandler({ProtocolName{kDirectChatProtocolId}},
                                     [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
                                       impl->HandleStream(std::move(stream));
                                     });
}

void Libp2pDirectChatService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = nullptr;
}

void Libp2pDirectChatService::SetInboundHandler(InboundHandler handler) {
  std::lock_guard lock(impl_->handler_mutex);
  impl_->inbound = std::move(handler);
}

bool Libp2pDirectChatService::IsPeerReachable(const std::string& peer_identity_value) const {
  return sessions_.IsReachableForProtocol(peer_identity_value, kDirectChatProtocolId);
}

Roe<void> Libp2pDirectChatService::SendEnvelope(const std::string& peer_relay_user_id,
                                                const RelayEnvelope& envelope) {
  if (!started_ || !host_.IsRunning()) {
    return Error("libp2p direct chat service not started");
  }
  if (!sessions_.IsReachableForProtocol(peer_relay_user_id, kDirectChatProtocolId)) {
    return Error("Peer-direct endpoint not registered")
        .WithUser("No usable peer address — add a dialable multiaddr on the contact.");
  }

  const std::string envelope_json = DumpJson(RelayEnvelopeToJson(envelope));

  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto active_mu = std::make_shared<std::mutex>();
  auto active_stream = std::make_shared<std::shared_ptr<Stream>>();
  auto active_duplex = std::make_shared<std::shared_ptr<DuplexFrameSession>>();

  auto finish = [settled, result_promise, active_mu, active_stream, active_duplex](Roe<void> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    {
      std::lock_guard lock(*active_mu);
      active_stream->reset();
      active_duplex->reset();
    }
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  sessions_.OpenStream(peer_relay_user_id, {ProtocolName{kDirectChatProtocolId}},
                       [host = &host_, envelope_json, finish, settled, active_mu, active_stream, active_duplex](
                           libp2p::StreamAndProtocolOrError stream_res) mutable {
                         if (!stream_res) {
                           finish(Error("libp2p chat stream open failed")
                                      .WithUser("Reached the peer but chat handshake failed."));
                           return;
                         }
                         auto stream = std::move(stream_res.value().stream);
                         {
                           std::lock_guard lock(*active_mu);
                           *active_stream = stream;
                         }
                         host->Post([host, stream = std::move(stream), envelope_json, finish, settled, active_mu,
                                     active_stream, active_duplex]() mutable {
                           if (settled->load(std::memory_order_acquire)) {
                             CloseQuiet(stream);
                             return;
                           }
                           auto duplex = std::make_shared<DuplexFrameSession>();
                           {
                             std::lock_guard lock(*active_mu);
                             *active_duplex = duplex;
                           }
                           auto policy = ControlJsonIoPolicy(host->IoExecutor(), kDefaultControlFrameReadTimeout,
                                                             kMaxRelayEnvelopeJsonBytes);
                           if (!duplex->EnqueueOutbound(JsonToBody(envelope_json))) {
                             finish(Error("Failed to send direct chat envelope")
                                        .WithUser("Direct send didn't confirm — will use relay if available."));
                             return;
                           }
                           duplex->Start(
                               stream,
                               [finish, duplex](Roe<std::vector<uint8_t>> frame) {
                                 if (!frame) {
                                   finish(Error("Failed to read direct chat ack")
                                              .WithUser("Direct send didn't confirm — will use relay if available."));
                                   return false;
                                 }
                                 finish({});
                                 return false;
                               },
                               [] { return false; }, std::move(policy),
                               [finish, duplex](const char* reason) {
                                 const char* tag = (reason && reason[0]) ? reason : "unknown";
                                 if (std::string(tag) == "handler_close") {
                                   return;
                                 }
                                 finish(Error("Failed to read direct chat ack")
                                            .WithUser("Direct send didn't confirm — will use relay if available."));
                               });
                         });
                       });

  constexpr int kDirectChatSendTimeoutMs = 4000;
  if (result_future.wait_for(std::chrono::milliseconds(kDirectChatSendTimeoutMs)) !=
      std::future_status::ready) {
    std::shared_ptr<Stream> to_reset;
    {
      std::lock_guard lock(*active_mu);
      to_reset = *active_stream;
      active_stream->reset();
    }
    ResetStreamQuiet(to_reset);
    finish(Error("libp2p chat send timed out")
               .WithUser("Direct send didn't confirm — will use relay if available."));
    return Error("libp2p chat send timed out")
        .WithUser("Direct send didn't confirm — will use relay if available.");
  }
  return result_future.get();
}

} // namespace pbr
