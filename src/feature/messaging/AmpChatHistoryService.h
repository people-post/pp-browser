#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/mesh/link/PeerLinkManager.h"
#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/Libp2pChatHistoryService.h"

#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** D060 peer-direct history over AMP ChannelSession ([A020] parallel stack). */
class AmpChatHistoryService : public IChatHistoryPeerClient {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpChatHistoryService(amp::PeerLinkManager& links, IoPump io_pump, IThreadStore& store, IdentityStore& identity,
                        IPskSessionStore& psk_store, WorkerPost post_worker = {});
  ~AmpChatHistoryService() override;

  AmpChatHistoryService(const AmpChatHistoryService&) = delete;
  AmpChatHistoryService& operator=(const AmpChatHistoryService&) = delete;

  void Start();
  void Stop();

  void RegisterPeerEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
