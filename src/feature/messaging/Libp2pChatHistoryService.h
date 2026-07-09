#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <memory>
#include <string>

namespace pbr {

inline constexpr const char* kChatHistoryProtocolId = "/pp-browser/chat-history/1.0.0";

/** D060 libp2p peer-direct history — responder + requester over `/pp-browser/chat-history/1.0.0`. */
class Libp2pChatHistoryService : public IChatHistoryPeerClient {
public:
  Libp2pChatHistoryService(Libp2pHost& host, PeerSessionManager& sessions, IThreadStore& store,
                           IdentityStore& identity, IPskSessionStore& psk_store);
  ~Libp2pChatHistoryService() override;

  Libp2pChatHistoryService(const Libp2pChatHistoryService&) = delete;
  Libp2pChatHistoryService& operator=(const Libp2pChatHistoryService&) = delete;

  /** Register protocol handler on the shared host. */
  void Start();
  void Stop();

  /** Map relay communicating identity → dialable multiaddr (must include `/p2p/`). */
  void RegisterPeerEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  IThreadStore& store_;
  IdentityStore& identity_;
  IPskSessionStore& psk_store_;
  bool started_ = false;
};

} // namespace pbr
