#pragma once

#include "common/Error.h"

#include <memory>
#include <string>
#include <vector>

namespace libp2p {
struct Host;
namespace protocol {
class Identify;
class IdentifyPush;
class IdentifyMessageProcessor;
} // namespace protocol
} // namespace libp2p

namespace pbr {

class Libp2pHost;
class PeerSessionManager;

/**
 * Wires libp2p Identify + Identify-Push on the shared host (media-hop L2).
 * Self listen addrs are published separately via PublishSelfAdvertisedAddrs.
 */
class IdentifyIntegrationService {
public:
  IdentifyIntegrationService() = default;
  ~IdentifyIntegrationService();

  IdentifyIntegrationService(const IdentifyIntegrationService&) = delete;
  IdentifyIntegrationService& operator=(const IdentifyIntegrationService&) = delete;

  /** Start Identify handlers on the host io thread. */
  Roe<void> Start(Libp2pHost& host, PeerSessionManager* sessions = nullptr);

  void Stop();

  bool IsStarted() const { return started_; }

  /**
   * Upsert self multiaddrs into the host peerstore and push Identify updates.
   * Must run on the host io thread (use Libp2pHost::Post).
   */
  Roe<void> PublishSelfAdvertisedAddrs(const std::vector<std::string>& multiaddrs);

private:
  void StopOnIo();
  std::shared_ptr<libp2p::protocol::IdentifyMessageProcessor> msg_processor_;
  std::shared_ptr<libp2p::protocol::Identify> identify_;
  std::shared_ptr<libp2p::protocol::IdentifyPush> identify_push_;
  libp2p::Host* host_ = nullptr;
  PeerSessionManager* sessions_ = nullptr;
  bool started_ = false;
};

} // namespace pbr
