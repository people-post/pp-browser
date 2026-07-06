#pragma once

#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace pbr {

inline constexpr const char* kChatHistoryProtocolId = "/pp-browser/chat-history/1.0.0";

/** D060 libp2p peer-direct history — responder + requester over `/pp-browser/chat-history/1.0.0`. */
class Libp2pChatHistoryService : public IChatHistoryPeerClient {
public:
  Libp2pChatHistoryService(IThreadStore& store, IdentityStore& identity);
  ~Libp2pChatHistoryService() override;

  Libp2pChatHistoryService(const Libp2pChatHistoryService&) = delete;
  Libp2pChatHistoryService& operator=(const Libp2pChatHistoryService&) = delete;

  void Start(const std::string& listen_multiaddr = "/ip4/127.0.0.1/tcp/40123");
  void Stop();

  /** Map relay communicating identity → dialable multiaddr (must include `/p2p/`). */
  void RegisterPeerEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  IThreadStore& store_;
  IdentityStore& identity_;
};

} // namespace pbr
