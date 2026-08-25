#include "feature/messaging/Libp2pChatBlobService.h"

#include "base/crypto/CryptoConstants.h"
#include "base/messaging/ChatBlobResponder.h"
#include "base/messaging/MessagingJson.h"
#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/StreamFrameIo.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <sodium.h>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;
using Clock = std::chrono::steady_clock;

constexpr auto kChatBlobOperationTimeout = std::chrono::milliseconds(15000);

void CloseQuiet(const std::shared_ptr<Stream>& stream) {
  if (stream) {
    stream->close([](auto&&) {});
  }
}

std::vector<uint8_t> JsonUtf8ToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::chrono::milliseconds RemainingTimeout(const Clock::time_point& deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

Roe<std::vector<uint8_t>> ParseFetchResponseBody(const std::vector<uint8_t>& body) {
  if (body.empty()) {
    return Error("Empty chat-blob fetch response");
  }
  if (body.front() == '{') {
    const std::string json_utf8(body.begin(), body.end());
    nlohmann::json root = nlohmann::json::parse(json_utf8, nullptr, false);
    if (!root.is_discarded() && root.contains("ok")) {
      if (root["ok"].get<bool>()) {
        return Error("Unexpected chat-blob ack on fetch");
      }
      if (root.contains("error") && root["error"].is_string()) {
        return Error(root["error"].get<std::string>());
      }
      return Error("Chat-blob fetch failed");
    }
  }
  return body;
}

using ChatBlobFetchResult = Roe<std::vector<uint8_t>>;

Roe<void> ParsePushAckBody(const std::vector<uint8_t>& body) {
  if (body.empty()) {
    return Error("Empty chat-blob push ack");
  }
  const std::string json_utf8(body.begin(), body.end());
  nlohmann::json root = nlohmann::json::parse(json_utf8, nullptr, false);
  if (root.is_discarded() || !root.contains("ok")) {
    return Error("Invalid chat-blob push ack");
  }
  if (!root["ok"].get<bool>()) {
    if (root.contains("error") && root["error"].is_string()) {
      return Error(root["error"].get<std::string>());
    }
    return Error("Chat-blob push rejected");
  }
  return {};
}


} // namespace

struct Libp2pChatBlobService::Impl {
  explicit Impl(IThreadStore& store_ref, IdentityStore& identity_ref)
      : store(store_ref), identity(identity_ref) {}

  IThreadStore& store;
  IdentityStore& identity;
  std::string profile_data_dir;
  std::string profile_id;
  ByteVector dek;
  mutable std::mutex dek_mutex;
  Libp2pHost* host = nullptr;
  std::atomic<bool> stopped{false};

  ByteVector CopyDek() const {
    std::lock_guard lock(dek_mutex);
    if (dek.size() != kDataEncryptionKeySize) {
      return {};
    }
    return dek;
  }

  void FailStream(const std::shared_ptr<DuplexFrameSession>& duplex, const std::shared_ptr<Stream>& stream) {
    host->Post([duplex, stream]() {
      duplex->Stop();
      CloseQuiet(stream);
    });
  }

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    if (!host || stopped.load(std::memory_order_acquire)) {
      return;
    }
    auto stream = std::move(stream_and_protocol.stream);
    if (!stream) {
      return;
    }
    auto duplex = std::make_shared<DuplexFrameSession>();
    auto pending_push = std::make_shared<std::optional<ChatBlobRequest>>();
    auto policy = ChatBlobIoPolicy(host->IoExecutor(), kDefaultControlFrameReadTimeout, /*read_once=*/false);

    duplex->Start(
        stream,
        [this, duplex, stream, pending_push](Roe<std::vector<uint8_t>> frame) {
          if (!frame || stopped.load(std::memory_order_acquire)) {
            return false;
          }

          if (!pending_push->has_value()) {
            const std::string json_utf8(frame->begin(), frame->end());
            nlohmann::json root = nlohmann::json::parse(json_utf8, nullptr, false);
            if (root.is_discarded()) {
              FailStream(duplex, stream);
              return false;
            }
            auto request = ChatBlobRequestFromJson(root);
            if (!request) {
              FailStream(duplex, stream);
              return false;
            }

            if (request->op == ChatBlobOp::Push) {
              *pending_push = *request;
              return true;
            }

            PostLibp2pWorker(*host, WorkerLane::Normal, [this, duplex, stream, request = *request]() mutable {
              auto fail = [this, duplex, stream]() { FailStream(duplex, stream); };
              if (stopped.load(std::memory_order_acquire)) {
                fail();
                return;
              }
              auto local_identity = identity.Get();
              if (!local_identity) {
                fail();
                return;
              }
              const ByteVector dek_copy = CopyDek();
              const ByteVector* dek_ptr = dek_copy.empty() ? nullptr : &dek_copy;
              auto ciphertext = ChatBlobResponder::ServeFetch(store, request, local_identity->relay_user_id,
                                                              profile_data_dir, dek_ptr, profile_id);
              host->Post([duplex, stream, ciphertext = std::move(ciphertext)]() mutable {
                auto close = [duplex, stream]() {
                  duplex->Stop();
                  CloseQuiet(stream);
                };
                if (ciphertext) {
                  if (!duplex->EnqueueOutbound(std::move(*ciphertext), [close](Roe<void>) { close(); })) {
                    close();
                  }
                  return;
                }
                const std::string ack_json = ChatBlobAckToJson(false, ciphertext.error().message).dump();
                if (!duplex->EnqueueOutbound(JsonUtf8ToBody(ack_json), [close](Roe<void>) { close(); })) {
                  close();
                }
              });
            });
            return false;
          }

          const ChatBlobRequest request = **pending_push;
          pending_push->reset();
          std::vector<uint8_t> ciphertext = std::move(*frame);

          PostLibp2pWorker(*host, WorkerLane::Normal,
                           [this, duplex, stream, request = std::move(request),
                            ciphertext = std::move(ciphertext)]() mutable {
                             auto fail = [this, duplex, stream](const std::string& message) {
                               const std::string ack_json = ChatBlobAckToJson(false, message).dump();
                               host->Post([duplex, stream, ack_json]() {
                                 auto close = [duplex, stream]() {
                                   duplex->Stop();
                                   CloseQuiet(stream);
                                 };
                                 if (!duplex->EnqueueOutbound(JsonUtf8ToBody(ack_json), [close](Roe<void>) {
                                       close();
                                     })) {
                                   close();
                                 }
                               });
                             };
                             if (stopped.load(std::memory_order_acquire)) {
                               fail("Service stopped");
                               return;
                             }
                             auto local_identity = identity.Get();
                             if (!local_identity) {
                               fail("Local relay identity missing");
                               return;
                             }
                             auto pushed = ChatBlobResponder::ServePush(store, request, local_identity->relay_user_id,
                                                                        profile_data_dir, ciphertext);
                             host->Post([duplex, stream, pushed]() {
                               auto close = [duplex, stream]() {
                                 duplex->Stop();
                                 CloseQuiet(stream);
                               };
                               const std::string ack_json =
                                   pushed ? ChatBlobAckToJson(true).dump()
                                          : ChatBlobAckToJson(false, pushed.error().message).dump();
                               if (!duplex->EnqueueOutbound(JsonUtf8ToBody(ack_json), [close](Roe<void>) {
                                     close();
                                   })) {
                                 close();
                               }
                             });
                           });
          return false;
        },
        [this]() { return stopped.load(std::memory_order_acquire); }, std::move(policy));
  }
};

Libp2pChatBlobService::Libp2pChatBlobService(Libp2pHost& host, PeerSessionManager& sessions, IThreadStore& store,
                                               IdentityStore& identity)
    : impl_(std::make_unique<Impl>(store, identity)), host_(host), sessions_(sessions) {
  impl_->host = &host_;
}

Libp2pChatBlobService::~Libp2pChatBlobService() {
  Stop();
}

void Libp2pChatBlobService::SetProfileDataDir(std::string profile_data_dir) {
  impl_->profile_data_dir = std::move(profile_data_dir);
}

void Libp2pChatBlobService::SetProfileId(std::string profile_id) {
  impl_->profile_id = std::move(profile_id);
}

Roe<void> Libp2pChatBlobService::SetDek(ByteVector dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  std::lock_guard lock(impl_->dek_mutex);
  if (!impl_->dek.empty()) {
    sodium_memzero(impl_->dek.data(), impl_->dek.size());
  }
  impl_->dek = std::move(dek);
  return {};
}

void Libp2pChatBlobService::ClearDek() {
  std::lock_guard lock(impl_->dek_mutex);
  if (!impl_->dek.empty()) {
    sodium_memzero(impl_->dek.data(), impl_->dek.size());
    impl_->dek.clear();
  }
}

void Libp2pChatBlobService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  host_.GetHost().setProtocolHandler({ProtocolName{kChatBlobProtocolId}},
                                     [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
                                       impl->HandleStream(std::move(stream));
                                     });
}

void Libp2pChatBlobService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
}

bool Libp2pChatBlobService::IsPeerReachable(const std::string& peer_identity_value) const {
  return sessions_.IsDialable(peer_identity_value);
}

Roe<std::vector<uint8_t>> Libp2pChatBlobService::FetchChatBlob(const ChatBlobRequest& request) {
  if (!started_ || !host_.IsRunning()) {
    return Error("libp2p chat-blob service not started");
  }
  if (!sessions_.IsDialable(request.peer_identity_value)) {
    return Error("Peer-direct endpoint not registered");
  }

  const auto deadline = Clock::now() + kChatBlobOperationTimeout;

  auto result_promise = std::make_shared<std::promise<ChatBlobFetchResult>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto active_mu = std::make_shared<std::mutex>();
  auto active_stream = std::make_shared<std::shared_ptr<Stream>>();

  auto finish = [settled, result_promise, active_mu, active_stream](ChatBlobFetchResult value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    {
      std::lock_guard lock(*active_mu);
      active_stream->reset();
    }
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  const std::string request_json = ChatBlobRequestToJson(request).dump();
  sessions_.OpenStream(request.peer_identity_value, {ProtocolName{kChatBlobProtocolId}},
                       [host = &host_, request_json, deadline, finish, settled, active_mu,
                        active_stream](libp2p::StreamAndProtocolOrError stream_res) mutable {
                         if (!stream_res) {
                           finish(Error("libp2p stream open failed"));
                           return;
                         }
                         auto stream = std::move(stream_res.value().stream);
                         {
                           std::lock_guard lock(*active_mu);
                           *active_stream = stream;
                         }
                         host->Post([host, stream = std::move(stream), request_json, deadline, finish, settled,
                                     active_mu, active_stream]() mutable {
                           if (settled->load(std::memory_order_acquire)) {
                             CloseQuiet(stream);
                             return;
                           }
                           auto duplex = std::make_shared<DuplexFrameSession>();
                           auto policy = ChatBlobIoPolicy(host->IoExecutor(), RemainingTimeout(deadline),
                                                          /*read_once=*/true);
                           if (!duplex->EnqueueOutbound(JsonUtf8ToBody(request_json))) {
                             finish(Error("Failed to send chat-blob request"));
                             return;
                           }
                           duplex->Start(
                               stream,
                               [finish, duplex, stream](Roe<std::vector<uint8_t>> frame) {
                                 if (!frame) {
                                   finish(Error("Failed to read chat-blob response"));
                                   return false;
                                 }
                                 finish(ParseFetchResponseBody(*frame));
                                 return false;
                               },
                               [] { return false; }, std::move(policy),
                               [finish, duplex, stream](const char* reason) {
                                 const char* tag = (reason && reason[0]) ? reason : "unknown";
                                 if (std::string(tag) == "handler_close") {
                                   return;
                                 }
                                 finish(Error("Failed to read chat-blob response"));
                               });
                         });
                       });

  if (result_future.wait_until(deadline) != std::future_status::ready) {
    std::shared_ptr<Stream> to_reset;
    {
      std::lock_guard lock(*active_mu);
      to_reset = *active_stream;
      active_stream->reset();
    }
    ResetStreamQuiet(to_reset);
    finish(Error("libp2p chat-blob fetch timed out"));
    return Error("libp2p chat-blob fetch timed out");
  }
  return result_future.get();
}

Roe<void> Libp2pChatBlobService::PushChatBlob(const ChatBlobRequest& request,
                                              const std::vector<uint8_t>& ciphertext) {
  if (!started_ || !host_.IsRunning()) {
    return Error("libp2p chat-blob service not started");
  }
  if (!sessions_.IsDialable(request.peer_identity_value)) {
    return Error("Peer-direct endpoint not registered");
  }
  if (ciphertext.empty()) {
    return Error("Empty chat-blob push body");
  }

  const auto deadline = Clock::now() + kChatBlobOperationTimeout;

  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto active_mu = std::make_shared<std::mutex>();
  auto active_stream = std::make_shared<std::shared_ptr<Stream>>();

  auto finish = [settled, result_promise, active_mu, active_stream](Roe<void> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    {
      std::lock_guard lock(*active_mu);
      active_stream->reset();
    }
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  const std::string request_json = ChatBlobRequestToJson(request).dump();
  sessions_.OpenStream(request.peer_identity_value, {ProtocolName{kChatBlobProtocolId}},
                       [host = &host_, request_json, ciphertext, deadline, finish, settled, active_mu,
                        active_stream](libp2p::StreamAndProtocolOrError stream_res) mutable {
                         if (!stream_res) {
                           finish(Error("libp2p stream open failed"));
                           return;
                         }
                         auto stream = std::move(stream_res.value().stream);
                         {
                           std::lock_guard lock(*active_mu);
                           *active_stream = stream;
                         }
                         host->Post([host, stream = std::move(stream), request_json, ciphertext, deadline, finish,
                                     settled, active_mu, active_stream]() mutable {
                           if (settled->load(std::memory_order_acquire)) {
                             CloseQuiet(stream);
                             return;
                           }
                           auto duplex = std::make_shared<DuplexFrameSession>();
                           auto policy = ChatBlobIoPolicy(host->IoExecutor(), RemainingTimeout(deadline),
                                                          /*read_once=*/true);
                           if (!duplex->EnqueueOutbound(JsonUtf8ToBody(request_json))) {
                             finish(Error("Failed to send chat-blob request"));
                             return;
                           }
                           if (!duplex->EnqueueOutbound(ciphertext)) {
                             finish(Error("Failed to send chat-blob body"));
                             return;
                           }
                           duplex->Start(
                               stream,
                               [finish, duplex, stream](Roe<std::vector<uint8_t>> frame) {
                                 if (!frame) {
                                   finish(Error("Failed to read chat-blob ack"));
                                   return false;
                                 }
                                 finish(ParsePushAckBody(*frame));
                                 return false;
                               },
                               [] { return false; }, std::move(policy),
                               [finish, duplex, stream](const char* reason) {
                                 const char* tag = (reason && reason[0]) ? reason : "unknown";
                                 if (std::string(tag) == "handler_close") {
                                   return;
                                 }
                                 finish(Error("Failed to read chat-blob ack"));
                               });
                         });
                       });

  if (result_future.wait_until(deadline) != std::future_status::ready) {
    std::shared_ptr<Stream> to_reset;
    {
      std::lock_guard lock(*active_mu);
      to_reset = *active_stream;
      active_stream->reset();
    }
    ResetStreamQuiet(to_reset);
    finish(Error("libp2p chat-blob push timed out"));
    return Error("libp2p chat-blob push timed out");
  }
  return result_future.get();
}

} // namespace pbr
