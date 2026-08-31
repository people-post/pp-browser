#pragma once

#include "feature/messaging/IDirectMessageClient.h"
#include "common/PbrCompat.h"

#include <memory>

namespace pbr {

class Libp2pHost;
class PeerSessionManager;

/**
 * Legacy TCP direct-chat. Product path is AmpDirectChatService (D10/A017).
 */
class Libp2pDirectChatService : public IDirectMessageClient {
public:
  using InboundHandler = IDirectMessageClient::InboundHandler;

  Libp2pDirectChatService(Libp2pHost& host, PeerSessionManager& sessions);
  ~Libp2pDirectChatService() override;

  Libp2pDirectChatService(const Libp2pDirectChatService&) = delete;
  Libp2pDirectChatService& operator=(const Libp2pDirectChatService&) = delete;

  void Start();
  void Stop();
  void SetInboundHandler(InboundHandler handler);
  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<void> SendEnvelope(const std::string& peer_relay_user_id, const RelayEnvelope& envelope) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
