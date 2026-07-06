#include "feature/messaging/Libp2pChatHistoryService.h"

#include "base/messaging/ChatHistoryResponder.h"
#include "base/messaging/ChatHistoryStreamCodec.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/MessagingJson.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/injector/host_injector.hpp>
#include <libp2p/log/configurator.hpp>
#include <libp2p/log/logger.hpp>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/peer/peer_info.hpp>
#include <libp2p/security/noise.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <condition_variable>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>

#include <soralog/impl/configurator_from_yaml.hpp>

namespace pbr {

namespace {

void EnsureLibp2pLoggingInitialized() {
  static std::once_flag once;
  std::call_once(once, [] {
    static const std::string kLibp2pLogConfig = R"(
sinks:
  - name: console
    type: console
    color: false
groups:
  - name: libp2p
    sink: console
    level: error
)";
    auto logging_system = std::make_shared<soralog::LoggingSystem>(
        std::make_shared<soralog::ConfiguratorFromYAML>(std::make_shared<libp2p::log::Configurator>(),
                                                        kLibp2pLogConfig));
    const auto result = logging_system->configure();
    if (result.has_error) {
      return;
    }
    libp2p::log::setLoggingSystem(logging_system);
  });
}

using libp2p::Bytes;
using libp2p::connection::Stream;
using libp2p::peer::PeerInfo;
using libp2p::peer::ProtocolName;

Roe<std::vector<uint8_t>> ReadExactFrame(const std::shared_ptr<Stream>& stream) {
  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  if (!header_future.get()) {
    return Error("Failed to read chat-history frame header");
  }

  std::vector<uint8_t> frame(header.begin(), header.end());
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame[i];
  }
  if (payload_len > kMaxRelayEnvelopeJsonBytes) {
    return Error("Chat-history frame too large");
  }

  Bytes payload(payload_len);
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read chat-history frame body");
  }

  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

Roe<void> WriteExactFrame(const std::shared_ptr<Stream>& stream, const std::vector<uint8_t>& frame) {
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(stream, libp2p::Bytes(frame), [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write chat-history frame");
  }
  return {};
}

} // namespace

struct Libp2pChatHistoryService::Impl {
  explicit Impl(IThreadStore& store_ref, IdentityStore& identity_ref)
      : store(store_ref), identity(identity_ref) {}

  IThreadStore& store;
  IdentityStore& identity;
  std::shared_ptr<boost::asio::io_context> io_context;
  std::shared_ptr<libp2p::Host> host;
  std::thread io_thread;
  std::atomic<bool> running{false};

  mutable std::mutex peer_mutex;
  std::unordered_map<std::string, PeerInfo> peer_endpoints;

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    auto stream = std::move(stream_and_protocol.stream);
    auto frame = ReadExactFrame(stream);
    if (!frame) {
      stream->close([](auto&&) {});
      return;
    }
    auto json_utf8 = ChatHistoryStreamCodec::DecodeFrame(*frame);
    if (!json_utf8) {
      stream->close([](auto&&) {});
      return;
    }

    nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
    if (root.is_discarded()) {
      stream->close([](auto&&) {});
      return;
    }
    auto request = ChatHistoryRequestFromJson(root);
    if (!request) {
      stream->close([](auto&&) {});
      return;
    }

    auto local_identity = identity.Get();
    if (!local_identity) {
      stream->close([](auto&&) {});
      return;
    }

    auto response = ChatHistoryResponder::Serve(store, identity, *request, local_identity->relay_user_id);
    if (!response) {
      stream->close([](auto&&) {});
      return;
    }

    const std::string response_json = ChatHistoryResponseToJson(*response).dump();
    auto encoded = ChatHistoryStreamCodec::EncodeFrame(response_json);
    if (!encoded) {
      stream->close([](auto&&) {});
      return;
    }
    (void)WriteExactFrame(stream, *encoded);
    stream->close([](auto&&) {});
  }
};

Libp2pChatHistoryService::Libp2pChatHistoryService(IThreadStore& store, IdentityStore& identity)
    : impl_(std::make_unique<Impl>(store, identity)), store_(store), identity_(identity) {}

Libp2pChatHistoryService::~Libp2pChatHistoryService() {
  Stop();
}

void Libp2pChatHistoryService::Start(const std::string& listen_multiaddr) {
  if (impl_->running.exchange(true)) {
    return;
  }

  EnsureLibp2pLoggingInitialized();

  impl_->io_context = std::make_shared<boost::asio::io_context>(1);
  auto injector = libp2p::injector::makeHostInjector();
  impl_->host = injector.create<std::shared_ptr<libp2p::Host>>();

  impl_->host->setProtocolHandler({ProtocolName{kChatHistoryProtocolId}},
                                  [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
                                    impl->HandleStream(std::move(stream));
                                  });

  auto ma_res = libp2p::multi::Multiaddress::create(listen_multiaddr);
  if (!ma_res) {
    impl_->running = false;
    return;
  }
  const libp2p::multi::Multiaddress ma = ma_res.value();

  impl_->io_thread = std::thread([impl = impl_.get(), ma]() {
    boost::asio::post(*impl->io_context, [impl, ma]() {
      auto listen_res = impl->host->listen(ma);
      if (listen_res) {
        impl->host->start();
      }
    });
    impl->io_context->run();
  });
}

void Libp2pChatHistoryService::Stop() {
  if (!impl_->running.exchange(false)) {
    return;
  }
  if (impl_->io_context) {
    impl_->io_context->stop();
  }
  if (impl_->io_thread.joinable()) {
    impl_->io_thread.join();
  }
  impl_->host.reset();
  impl_->io_context.reset();
}

void Libp2pChatHistoryService::RegisterPeerEndpoint(const std::string& peer_relay_user_id,
                                                    const std::string& multiaddr) {
  auto parsed = libp2p::multi::Multiaddress::create(multiaddr);
  if (!parsed) {
    return;
  }
  const libp2p::multi::Multiaddress& address = parsed.value();
  const auto peer_id_str = address.getPeerId();
  if (!peer_id_str) {
    return;
  }
  auto peer_id = libp2p::peer::PeerId::fromBase58(*peer_id_str);
  if (!peer_id) {
    return;
  }

  PeerInfo info{peer_id.value(), {address}};
  if (impl_->host) {
    (void)impl_->host->getPeerRepository().getAddressRepository().upsertAddresses(
        peer_id.value(), std::span<const libp2p::multi::Multiaddress>(info.addresses),
        std::chrono::hours(24));
  }
  std::lock_guard lock(impl_->peer_mutex);
  impl_->peer_endpoints.insert_or_assign(peer_relay_user_id, std::move(info));
}

bool Libp2pChatHistoryService::IsPeerReachable(const std::string& peer_identity_value) const {
  std::lock_guard lock(impl_->peer_mutex);
  return impl_->peer_endpoints.contains(peer_identity_value);
}

Roe<ChatHistoryResponse> Libp2pChatHistoryService::FetchChatHistory(const ChatHistoryRequest& request) {
  if (!impl_->running || !impl_->host || !impl_->io_context) {
    return Error("libp2p chat-history service not started");
  }

  std::optional<PeerInfo> peer_info;
  {
    std::lock_guard lock(impl_->peer_mutex);
    const auto it = impl_->peer_endpoints.find(request.peer_identity_value);
    if (it != impl_->peer_endpoints.end()) {
      peer_info = it->second;
    }
  }
  if (!peer_info) {
    return Error("Peer-direct endpoint not registered");
  }

  std::shared_ptr<std::promise<Roe<ChatHistoryResponse>>> result_promise =
      std::make_shared<std::promise<Roe<ChatHistoryResponse>>>();
  auto result_future = result_promise->get_future();

  boost::asio::post(*impl_->io_context, [this, request, peer_info = *peer_info, result_promise]() {
    impl_->host->newStream(peer_info, {ProtocolName{kChatHistoryProtocolId}},
                           [this, request, result_promise](libp2p::StreamAndProtocolOrError stream_res) {
                             if (!stream_res) {
                               result_promise->set_value(Error("libp2p stream open failed"));
                               return;
                             }
                             auto stream = std::move(stream_res.value().stream);
                             const std::string request_json = ChatHistoryRequestToJson(request).dump();
                             auto frame = ChatHistoryStreamCodec::EncodeFrame(request_json);
                             if (!frame) {
                               result_promise->set_value(frame.error());
                               return;
                             }
                             if (!WriteExactFrame(stream, *frame)) {
                               result_promise->set_value(Error("Failed to send chat-history request"));
                               return;
                             }
                             auto response_frame = ReadExactFrame(stream);
                             stream->close([](auto&&) {});
                             if (!response_frame) {
                               result_promise->set_value(response_frame.error());
                               return;
                             }
                             auto response_json = ChatHistoryStreamCodec::DecodeFrame(*response_frame);
                             if (!response_json) {
                               result_promise->set_value(response_json.error());
                               return;
                             }
                             nlohmann::json root = nlohmann::json::parse(*response_json, nullptr, false);
                             if (root.is_discarded()) {
                               result_promise->set_value(Error("Invalid chat-history response JSON"));
                               return;
                             }
                             result_promise->set_value(ChatHistoryResponseFromJson(root));
                           });
  });

  return result_future.get();
}

} // namespace pbr
