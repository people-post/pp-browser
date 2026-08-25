#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/IDekConsumer.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"

#include "common/Error.h"

#include <memory>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kChatBlobProtocolId = "/pp-browser/chat-blob/1.0.0";

/** R019 libp2p peer-direct attachment blobs — responder + requester over `/pp-browser/chat-blob/1.0.0`. */
class Libp2pChatBlobService : public IChatBlobPeerClient, public IDekConsumer {
public:
  Libp2pChatBlobService(Libp2pHost& host, PeerSessionManager& sessions, IThreadStore& store, IdentityStore& identity);
  ~Libp2pChatBlobService() override;

  Libp2pChatBlobService(const Libp2pChatBlobService&) = delete;
  Libp2pChatBlobService& operator=(const Libp2pChatBlobService&) = delete;

  void SetProfileDataDir(std::string profile_data_dir);
  void SetProfileId(std::string profile_id);
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
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
