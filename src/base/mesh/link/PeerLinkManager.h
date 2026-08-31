#pragma once

#include "base/adp/Endpoint.h"
#include "base/mesh/channel/ChannelPolicy.h"
#include "base/mesh/link/PeerLink.h"
#include "base/mesh/link/Types.h"
#include "base/mesh/session/Types.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr::amp {

/** Dial + warm policy over ADP/AMP (replaces libp2p PeerSessionManager on the AMP path). */
class PeerLinkManager {
public:
  using LinkCb = std::function<void(Roe<void>)>;
  using ChannelCb = std::function<void(Roe<uint32_t>)>;
  using ProtocolHandler = std::function<void(PeerLink& link, uint32_t channel_id)>;

  PeerLinkManager(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                  PeerLinkConfig config = {});

  adp::Endpoint& GetEndpoint() { return endpoint_; }
  const std::string& LocalPeerId() const { return local_peer_id_; }

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr);

  void EnsureAssociation(const std::string& peer_key, LinkCb on_complete);
  void OpenChannel(const std::string& peer_key, const std::string& protocol_id, ChannelPolicy policy,
                   ChannelCb on_complete);

  /** L4 entry — applied to every link mux (existing + future). */
  void SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler);
  void RemoveProtocolHandler(const std::string& protocol_id);
  void ClearProtocolHandlers();

  PeerLinkSnapshot GetLinkSnapshot(const std::string& peer_key) const;
  bool IsConnected(const std::string& peer_key) const;

  void MarkWarm(const std::string& peer_key);
  void ClearWarm(const std::string& peer_key);

  PeerLink* FindLink(const std::string& peer_key);
  const PeerLink* FindLink(const std::string& peer_key) const;
  PeerLink* FindConnectedInboundLink();

  void Tick();

private:
  friend class PeerLink;

  struct EndpointRecord {
    std::string multiaddr;
    adp::IpEndpoint endpoint;
    std::string peer_id;
  };

  void InstallAcceptHandler();
  void OnInboundConnection(std::shared_ptr<adp::Connection> connection);
  void OnLinkEstablished(PeerLink& link);
  void ApplyProtocolHandlers(PeerLink& link);
  void FinishDial(const std::string& peer_key, Roe<void> result);

  adp::Endpoint& endpoint_;
  MshIdentity local_identity_;
  std::string local_peer_id_;
  PeerLinkConfig config_;

  std::unordered_map<std::string, EndpointRecord> endpoints_;
  std::unordered_map<std::string, std::unique_ptr<PeerLink>> links_;
  std::unordered_map<std::string, ProtocolHandler> protocol_handlers_;
  std::unordered_map<std::string, std::vector<LinkCb>> inflight_associations_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> dial_failed_until_;
  std::unordered_map<std::string, std::string> last_error_;
  size_t concurrent_dials_ = 0;
};

} // namespace pbr::amp
