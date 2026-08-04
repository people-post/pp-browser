#pragma once

#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "libp2p/integration/host/Libp2pExecutorConfig.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace pbr {

inline constexpr const char* kDirectChatProtocolId = "/pp-browser/chat/1.0.0";

/** Direct push of RelayEnvelope over libp2p. */
class IDirectMessageClient {
public:
  virtual ~IDirectMessageClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  virtual Roe<void> SendEnvelope(const std::string& peer_relay_user_id, const RelayEnvelope& envelope) = 0;
};

/**
 * `/pp-browser/chat/1.0.0` — one short stream per message (length-prefixed JSON RelayEnvelope).
 * Inbound envelopes are delivered via OnInboundEnvelope.
 */
class Libp2pDirectChatService : public IDirectMessageClient {
public:
  using InboundHandler = std::function<void(RelayEnvelope envelope)>;

  Libp2pDirectChatService(Libp2pHost& host, PeerSessionManager& sessions);
  ~Libp2pDirectChatService() override;

  Libp2pDirectChatService(const Libp2pDirectChatService&) = delete;
  Libp2pDirectChatService& operator=(const Libp2pDirectChatService&) = delete;

  void Start();
  void Stop();

  void SetExecutorConfig(Libp2pExecutorConfig config);
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
