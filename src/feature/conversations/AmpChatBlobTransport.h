#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/net/OrgBackendClients.h"
#include "domain/people/IdentityStore.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * `/pp-browser/blob/1.0.0` over AMP ChannelSession ([A020] single entry when Amp is attached).
 */
class AmpChatBlobTransport : public IChatBlobPeerService {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpChatBlobTransport(IChatPeerLinks& links, IoPump io_pump, IThreadStore& store, IdentityStore& identity,
                     WorkerPost post_worker = {});
  ~AmpChatBlobTransport() override;

  AmpChatBlobTransport(const AmpChatBlobTransport&) = delete;
  AmpChatBlobTransport& operator=(const AmpChatBlobTransport&) = delete;

  void SetProfileDataDir(std::string profile_data_dir) override;
  void SetProfileId(std::string profile_id) override;
  void Start();
  void Stop();

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<std::vector<uint8_t>> FetchChatBlob(const ChatBlobRequest& request) override;
  Roe<void> PushChatBlob(const ChatBlobRequest& request, const std::vector<uint8_t>& ciphertext) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  IChatPeerLinks& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
