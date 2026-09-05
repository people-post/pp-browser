#pragma once

#include "foundation/crypto/IPskSessionStore.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/net/ServiceClients.h"
#include "domain/people/IdentityStore.h"

#include "amp/L3/ChannelSession.h"

#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** D060 peer-direct history over AMP ChannelSession — product single-entry when Amp is attached ([A020]). */
class AmpChatHistoryService : public IChatHistoryPeerClient {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpChatHistoryService(IChatPeerLinks& links, IoPump io_pump, IThreadStore& store, IdentityStore& identity,
                        IPskSessionStore& psk_store, WorkerPost post_worker = {});
  ~AmpChatHistoryService() override;

  AmpChatHistoryService(const AmpChatHistoryService&) = delete;
  AmpChatHistoryService& operator=(const AmpChatHistoryService&) = delete;

  /** When register_handler is false, inbound is served only via ServeInbound (rpc demux). */
  void Start(bool register_handler = true);
  void Stop();

  /** Shared `/pp-browser/rpc/1.0.0` demux entry — already-bound session + first DATA body. */
  void ServeInbound(std::shared_ptr<pp::amp::ChannelSession> session, std::vector<uint8_t> body);

  void RegisterPeerEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  IChatPeerLinks& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
