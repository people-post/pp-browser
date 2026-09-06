#pragma once

#include "common/thread/ThreadTypes.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/net/OrgBackendClients.h"
#include "common/chat/IDirectMessageClient.h"

#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * `/pp-browser/rpc/chat/1.0.0` over AMP ChannelSession (PeerLinkManager::OpenChannel).
 * Product single-entry when MeshHost Amp is attached ([A020]); legacy path remains for tests/fallback.
 */
class AmpDirectChatTransport : public IDirectMessageClient {
public:
  using InboundHandler = IDirectMessageClient::InboundHandler;
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;

  AmpDirectChatTransport(IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker = {});
  ~AmpDirectChatTransport() override;

  AmpDirectChatTransport(const AmpDirectChatTransport&) = delete;
  AmpDirectChatTransport& operator=(const AmpDirectChatTransport&) = delete;

  void Start();
  void Stop();

  void SetInboundHandler(InboundHandler handler);

  bool IsPeerReachable(const std::string& peer_identity_value) const override;
  Roe<void> SendEnvelope(const std::string& peer_relay_user_id, const RelayEnvelope& envelope) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  IChatPeerLinks& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
