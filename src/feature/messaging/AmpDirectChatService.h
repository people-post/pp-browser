#pragma once

#include "base/messaging/ThreadTypes.h"
#include "lib/amp/link/PeerLinkManager.h"
#include "base/net/ServiceClients.h"
#include "feature/messaging/IDirectMessageClient.h"

#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * `/pp-browser/chat/1.0.0` over AMP ChannelSession (PeerLinkManager::OpenChannel).
 * Product single-entry when MeshHost Amp is attached ([A020]/ libp2p path remains for tests/fallback.
 */
class AmpDirectChatService : public IDirectMessageClient {
public:
  using InboundHandler = IDirectMessageClient::InboundHandler;
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpDirectChatService(amp::PeerLinkManager& links, IoPump io_pump, WorkerPost post_worker = {});
  ~AmpDirectChatService() override;

  AmpDirectChatService(const AmpDirectChatService&) = delete;
  AmpDirectChatService& operator=(const AmpDirectChatService&) = delete;

  void Start();
  void Stop();

  void SetInboundHandler(InboundHandler handler);

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<void> SendEnvelope(const std::string& peer_relay_user_id, const RelayEnvelope& envelope) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
