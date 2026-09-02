#include "feature/messaging/AmpChatBlobService.h"

#include "amp/link/PeerLink.h"

#include "foundation/crypto/CryptoConstants.h"
#include "base/messaging/ChatBlobResponder.h"
#include "common/chat/MessagingJson.h"
#include "base/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <sodium.h>
#include <string>
#include <vector>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kChatBlobOperationTimeout = std::chrono::milliseconds(15000);

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpChatBlobService::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

std::chrono::milliseconds RemainingTimeout(const Clock::time_point deadline) {
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
    auto root = TryParseObject(json_utf8);
    if (root && root->contains("ok")) {
      if (root->getIf<bool>("ok").value_or(false)) {
        return Error("Unexpected chat-blob ack on fetch");
      }
      if (auto err = root->getString("error")) {
        return Error(*err);
      }
      return Error("Chat-blob fetch failed");
    }
  }
  return body;
}

Roe<void> ParsePushAckBody(const std::vector<uint8_t>& body) {
  if (body.empty()) {
    return Error("Empty chat-blob push ack");
  }
  const std::string json_utf8(body.begin(), body.end());
  auto root = TryParseObject(json_utf8);
  if (!root || !root->contains("ok")) {
    return Error("Invalid chat-blob push ack");
  }
  if (!root->getIf<bool>("ok").value_or(false)) {
    if (auto err = root->getString("error")) {
      return Error(*err);
    }
    return Error("Chat-blob push rejected");
  }
  return {};
}

} // namespace

struct AmpChatBlobService::Impl {
  Impl(IThreadStore& store_in, IdentityStore& identity_in) : store(store_in), identity(identity_in) {}

  IThreadStore& store;
  IdentityStore& identity;
  std::string profile_data_dir;
  std::string profile_id;
  ByteVector dek;
  mutable std::mutex dek_mutex;
  IChatPeerLinks* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};

  ByteVector CopyDek() const {
    std::lock_guard lock(dek_mutex);
    if (dek.size() != kDataEncryptionKeySize) {
      return {};
    }
    return dek;
  }

  void IoPumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      }
    }
  }

  void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !link.Mux()) {
      return;
    }
    auto session = std::make_shared<pp::amp::ChannelSession>();
    auto pending_push = std::make_shared<std::optional<ChatBlobRequest>>();
    session->Bind(*link.Mux(), channel_id, pp::amp::ChatBlobChannelPolicy(/*read_once=*/false),
                  [this, session, pending_push](Roe<std::vector<uint8_t>> frame) {
                    if (!frame || stopped.load(std::memory_order_acquire)) {
                      return false;
                    }
                    auto body = std::move(*frame);

                    if (!pending_push->has_value()) {
                      const std::string json_utf8(body.begin(), body.end());
                      auto root = TryParseObject(json_utf8);
                      if (!root) {
                        session->Close();
                        return false;
                      }
                      auto request = ChatBlobRequestFromJson(*root);
                      if (!request) {
                        session->Close();
                        return false;
                      }
                      if (request->op == ChatBlobOp::Push) {
                        *pending_push = *request;
                        return true;
                      }

                      RunWorker(post_worker, [this, session, request = *request]() mutable {
                        if (stopped.load(std::memory_order_acquire)) {
                          session->Close();
                          return;
                        }
                        auto local_identity = identity.Get();
                        if (!local_identity) {
                          session->Close();
                          return;
                        }
                        const ByteVector dek_copy = CopyDek();
                        const ByteVector* dek_ptr = dek_copy.empty() ? nullptr : &dek_copy;
                        auto ciphertext = ChatBlobResponder::ServeFetch(
                            store, request, local_identity->relay_user_id, profile_data_dir, dek_ptr, profile_id);
                        if (ciphertext) {
                          if (!session->EnqueueOutbound(std::move(*ciphertext))) {
                            session->Close();
                            return;
                          }
                        } else {
                          const std::string ack_json =
                              DumpJson(ChatBlobAckToJson(false, ciphertext.error().message));
                          if (!session->EnqueueOutbound(JsonToBody(ack_json))) {
                            session->Close();
                            return;
                          }
                        }
                        if (io_pump) {
                          io_pump();
                        }
                        session->Close();
                      });
                      return false;
                    }

                    const ChatBlobRequest request = **pending_push;
                    pending_push->reset();
                    RunWorker(post_worker, [this, session, request, ciphertext = std::move(body)]() mutable {
                      if (stopped.load(std::memory_order_acquire)) {
                        session->Close();
                        return;
                      }
                      auto local_identity = identity.Get();
                      if (!local_identity) {
                        const std::string ack_json = DumpJson(ChatBlobAckToJson(false, "Local relay identity missing"));
                        (void)session->EnqueueOutbound(JsonToBody(ack_json));
                        session->Close();
                        return;
                      }
                      auto pushed = ChatBlobResponder::ServePush(store, request, local_identity->relay_user_id,
                                                                 profile_data_dir, ciphertext);
                      const std::string ack_json = pushed ? DumpJson(ChatBlobAckToJson(true))
                                                          : DumpJson(ChatBlobAckToJson(false, pushed.error().message));
                      (void)session->EnqueueOutbound(JsonToBody(ack_json));
                      if (io_pump) {
                        io_pump();
                      }
                      session->Close();
                    });
                    return false;
                  });
  }
};

AmpChatBlobService::AmpChatBlobService(IChatPeerLinks& links, IoPump io_pump, IThreadStore& store,
                                       IdentityStore& identity, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>(store, identity)), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
}

AmpChatBlobService::~AmpChatBlobService() {
  Stop();
}

void AmpChatBlobService::SetProfileDataDir(std::string profile_data_dir) {
  impl_->profile_data_dir = std::move(profile_data_dir);
}

void AmpChatBlobService::SetProfileId(std::string profile_id) {
  impl_->profile_id = std::move(profile_id);
}

Roe<void> AmpChatBlobService::SetDek(ByteVector dek) {
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

void AmpChatBlobService::ClearDek() {
  std::lock_guard lock(impl_->dek_mutex);
  if (!impl_->dek.empty()) {
    sodium_memzero(impl_->dek.data(), impl_->dek.size());
    impl_->dek.clear();
  }
}

void AmpChatBlobService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kChatBlobProtocolId,
                            [impl = impl_.get()](pp::amp::PeerLink& link, const uint32_t channel_id) {
                              impl->HandleInboundChannel(link, channel_id);
                            });
}

void AmpChatBlobService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kChatBlobProtocolId);
}

bool AmpChatBlobService::IsPeerReachable(const std::string& peer_identity_value) const {
  return links_.GetLinkSnapshot(peer_identity_value).has_endpoint || links_.IsConnected(peer_identity_value);
}

Roe<std::vector<uint8_t>> AmpChatBlobService::FetchChatBlob(const ChatBlobRequest& request) {
  if (!started_) {
    return Error("amp chat-blob service not started");
  }
  if (!IsPeerReachable(request.peer_identity_value)) {
    return Error("Peer-direct endpoint not registered");
  }

  const auto deadline = Clock::now() + kChatBlobOperationTimeout;
  auto result_promise = std::make_shared<std::promise<Roe<std::vector<uint8_t>>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto session = std::make_shared<pp::amp::ChannelSession>();

  auto finish = [settled, result_promise, session](Roe<std::vector<uint8_t>> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    session->Close();
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  const std::string peer_key = request.peer_identity_value;
  const std::string request_json = DumpJson(ChatBlobRequestToJson(request));
  const auto read_timeout = RemainingTimeout(deadline);

  links_.EnsureAssociation(peer_key, [this, peer_key, request_json, finish, settled, session, deadline,
                                      read_timeout](IChatPeerLinks::LinkRoe assoc) mutable {
    if (!assoc) {
      finish(Error(assoc.error().message));
      return;
    }
    links_.OpenChannel(peer_key, kChatBlobProtocolId, pp::amp::ChatBlobChannelPolicy(/*read_once=*/true),
                       [this, peer_key, request_json, finish, settled, session, deadline,
                        read_timeout](IChatPeerLinks::ChannelRoe channel) mutable {
                         if (!channel) {
                           finish(Error(channel.error().message));
                           return;
                         }
                         impl_->IoPumpUntil(
                             [&] {
                               auto* link = links_.FindLink(peer_key);
                               return link && link->Mux() &&
                                      link->Mux()->State(*channel) == pp::amp::ChannelState::Open;
                             },
                             deadline);
                         auto* link = links_.FindLink(peer_key);
                         if (!link || !link->Mux() || link->Mux()->State(*channel) != pp::amp::ChannelState::Open) {
                           finish(Error("amp chat-blob: channel open failed"));
                           return;
                         }

                         auto policy = pp::amp::ChatBlobChannelPolicy(/*read_once=*/true);
                         policy.read_timeout = read_timeout;
                         session->Bind(*link->Mux(), *channel, std::move(policy),
                                       [finish](Roe<std::vector<uint8_t>> frame) {
                                         if (!frame) {
                                           finish(Error("Failed to read chat-blob response"));
                                           return false;
                                         }
                                         finish(ParseFetchResponseBody(*frame));
                                         return false;
                                       });

                         if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                           finish(Error("Failed to send chat-blob request"));
                           return;
                         }

                         impl_->IoPumpUntil([settled] { return settled->load(std::memory_order_acquire); }, deadline);
                         if (!settled->load(std::memory_order_acquire)) {
                           finish(Error("amp chat-blob fetch timed out"));
                         }
                       });
  });

  impl_->IoPumpUntil([&] { return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
                     deadline);
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    finish(Error("amp chat-blob fetch timed out"));
    return Error("amp chat-blob fetch timed out");
  }
  return result_future.get();
}

Roe<void> AmpChatBlobService::PushChatBlob(const ChatBlobRequest& request,
                                           const std::vector<uint8_t>& ciphertext) {
  if (!started_) {
    return Error("amp chat-blob service not started");
  }
  if (!IsPeerReachable(request.peer_identity_value)) {
    return Error("Peer-direct endpoint not registered");
  }
  if (ciphertext.empty()) {
    return Error("Empty chat-blob push body");
  }

  const auto deadline = Clock::now() + kChatBlobOperationTimeout;
  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto session = std::make_shared<pp::amp::ChannelSession>();

  auto finish = [settled, result_promise, session](Roe<void> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    session->Close();
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  const std::string peer_key = request.peer_identity_value;
  const std::string request_json = DumpJson(ChatBlobRequestToJson(request));
  const auto read_timeout = RemainingTimeout(deadline);

  links_.EnsureAssociation(
      peer_key, [this, peer_key, request_json, ciphertext, finish, settled, session, deadline,
                 read_timeout](IChatPeerLinks::LinkRoe assoc) mutable {
        if (!assoc) {
          finish(Error(assoc.error().message));
          return;
        }
        links_.OpenChannel(
            peer_key, kChatBlobProtocolId, pp::amp::ChatBlobChannelPolicy(/*read_once=*/true),
            [this, peer_key, request_json, ciphertext, finish, settled, session, deadline,
             read_timeout](IChatPeerLinks::ChannelRoe channel) mutable {
              if (!channel) {
                finish(Error(channel.error().message));
                return;
              }
              impl_->IoPumpUntil(
                  [&] {
                    auto* link = links_.FindLink(peer_key);
                    return link && link->Mux() && link->Mux()->State(*channel) == pp::amp::ChannelState::Open;
                  },
                  deadline);
              auto* link = links_.FindLink(peer_key);
              if (!link || !link->Mux() || link->Mux()->State(*channel) != pp::amp::ChannelState::Open) {
                finish(Error("amp chat-blob: channel open failed"));
                return;
              }

              auto policy = pp::amp::ChatBlobChannelPolicy(/*read_once=*/true);
              policy.read_timeout = read_timeout;
              session->Bind(*link->Mux(), *channel, std::move(policy),
                            [finish](Roe<std::vector<uint8_t>> frame) {
                              if (!frame) {
                                finish(Error("Failed to read chat-blob ack"));
                                return false;
                              }
                              finish(ParsePushAckBody(*frame));
                              return false;
                            });

              if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                finish(Error("Failed to send chat-blob request"));
                return;
              }
              if (!session->EnqueueOutbound(ciphertext)) {
                finish(Error("Failed to send chat-blob body"));
                return;
              }

              impl_->IoPumpUntil([settled] { return settled->load(std::memory_order_acquire); }, deadline);
              if (!settled->load(std::memory_order_acquire)) {
                finish(Error("amp chat-blob push timed out"));
              }
            });
      });

  impl_->IoPumpUntil([&] { return result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
                     deadline);
  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    finish(Error("amp chat-blob push timed out"));
    return Error("amp chat-blob push timed out");
  }
  return result_future.get();
}

} // namespace pbr
