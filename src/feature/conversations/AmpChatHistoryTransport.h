#pragma once

#include "foundation/crypto/IPskSessionStore.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/net/OrgBackendClients.h"
#include "domain/people/IdentityStore.h"

#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** D060 peer-direct history on `/pp-browser/rpc/history/1.0.0` — product path when Amp is attached ([A020]). */
class AmpChatHistoryTransport : public IChatHistoryPeerClient {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpChatHistoryTransport(IChatPeerLinks& links, IoPump io_pump, IThreadStore& store, IdentityStore& identity,
                        IPskSessionStore& psk_store, WorkerPost post_worker = {});
  ~AmpChatHistoryTransport() override;

  AmpChatHistoryTransport(const AmpChatHistoryTransport&) = delete;
  AmpChatHistoryTransport& operator=(const AmpChatHistoryTransport&) = delete;

  void Start();
  void Stop();

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
