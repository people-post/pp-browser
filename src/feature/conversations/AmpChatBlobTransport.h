#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "mesh/host/MeshPorts.h"
#include "domain/net/OrgBackendClients.h"
#include "domain/messaging/PeerSigningKeyStore.h"
#include "domain/people/IdentityStore.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * `/pp-browser/blob/1.0.0` over AMP ChannelSession ([A020] single entry when Amp is attached).
 */
class AmpChatBlobTransport : public IChatBlobPeerService {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  using IoPost = std::function<void(std::function<void()>)>;
  using IoAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

  AmpChatBlobTransport(mesh::IChatPeerLinks& links, IoPump io_pump, IThreadStore& store, IdentityStore& identity,
                     IPeerSigningKeyResolver& signing_keys, WorkerPost post_worker = {}, IoPost post_io = {},
                     IoAfter post_after = {});
  ~AmpChatBlobTransport() override;

  AmpChatBlobTransport(const AmpChatBlobTransport&) = delete;
  AmpChatBlobTransport& operator=(const AmpChatBlobTransport&) = delete;

  /** Tests whose mesh does not run on the identity's device key: the PeerId requests are signed for. */
  void SetLocalPeerIdForTest(std::string peer_id) { local_peer_id_override_ = std::move(peer_id); }
  void SetProfileDataDir(std::string profile_data_dir) override;
  void SetProfileId(std::string profile_id) override;
  void Start();
  void Stop();

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<std::vector<uint8_t>> FetchChatBlob(const ChatBlobRequest& request) override;
  Roe<void> PushChatBlob(const ChatBlobRequest& request, const std::vector<uint8_t>& ciphertext) override;

  void FetchChatBlobAsync(const ChatBlobRequest& request,
                          std::function<void(Roe<std::vector<uint8_t>>)> on_done) override;
  void PushChatBlobAsync(const ChatBlobRequest& request, const std::vector<uint8_t>& ciphertext,
                         std::function<void(Roe<void>)> on_done) override;

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
