#pragma once

#include "lib/messaging/PeerSigningKeyStore.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "mesh/host/MeshPorts.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/people/IdentityStore.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** D060 peer-direct history on `/pp/rpc/history/1.0.0` — product path when Amp is attached ([A020]). */
class AmpChatHistoryTransport : public IChatHistoryPeerClient {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  using IoPost = std::function<void(std::function<void()>)>;
  using IoAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

  AmpChatHistoryTransport(mesh::IChatPeerLinks& links, IoPump io_pump, IThreadStore& store, IdentityStore& identity,
                        IPskSessionStore& psk_store, IPeerSigningKeyResolver& signing_keys,
                        WorkerPost post_worker = {}, IoPost post_io = {}, IoAfter post_after = {});
  /** Tests whose mesh does not run on the identity's device key: the PeerId requests are signed for. */
  void SetLocalPeerIdForTest(std::string peer_id) { local_peer_id_override_ = std::move(peer_id); }
  ~AmpChatHistoryTransport() override;

  AmpChatHistoryTransport(const AmpChatHistoryTransport&) = delete;
  AmpChatHistoryTransport& operator=(const AmpChatHistoryTransport&) = delete;

  void Start();
  void Stop();

  void RegisterPeerEndpoint(const std::string& peer_relay_user_id, const std::string& multiaddr);

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override;
  void FetchChatHistoryAsync(const ChatHistoryRequest& request,
                             std::function<void(Roe<ChatHistoryResponse>)> on_done) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  mesh::IChatPeerLinks& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  IoPost post_io_;
  IoAfter post_after_;
  bool started_ = false;
  std::string local_peer_id_override_;
};

} // namespace pbr
