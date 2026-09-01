#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "lib/amp/link/PeerLinkManager.h"
#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * `/pp-browser/chat-blob/1.0.0` over AMP ChannelSession ([A020] single entry when Amp is attached).
 */
class AmpChatBlobService : public IChatBlobPeerService {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpChatBlobService(pp::amp::PeerLinkManager& links, IoPump io_pump, IThreadStore& store, IdentityStore& identity,
                     WorkerPost post_worker = {});
  ~AmpChatBlobService() override;

  AmpChatBlobService(const AmpChatBlobService&) = delete;
  AmpChatBlobService& operator=(const AmpChatBlobService&) = delete;

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
  pp::amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
