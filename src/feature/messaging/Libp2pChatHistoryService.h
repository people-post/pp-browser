#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"
#include "common/PbrCompat.h"

#include <memory>
#include <string>

namespace pbr {

class Libp2pHost;
class PeerSessionManager;

/** Legacy TCP chat-history. Product path is AmpChatHistoryService (D10/A017). */
class Libp2pChatHistoryService : public IChatHistoryPeerClient {
public:
  Libp2pChatHistoryService(Libp2pHost& host, PeerSessionManager& sessions, IThreadStore& store,
                           IdentityStore& identity, IPskSessionStore& psk_store);
  ~Libp2pChatHistoryService() override;

  Libp2pChatHistoryService(const Libp2pChatHistoryService&) = delete;
  Libp2pChatHistoryService& operator=(const Libp2pChatHistoryService&) = delete;

  void Start();
  void Stop();
  void RegisterPeerEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);
  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
