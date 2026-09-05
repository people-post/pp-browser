#pragma once

#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/PeerAnnounceFeed.h"
#include "domain/messaging/PeerAnnounceRpcCodec.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * `/pp-browser/rpc/peer-announce/1.0.0` tip push/ack over AMP ChannelSession (Spine B 1:1).
 * Domain codecs/feed stay Amp-free; this feature service owns OpenChannel fan-out only.
 */
class AmpPeerAnnounceService {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  /** Resolve publisher ML-DSA-65 public key for tip.peer_id (device key). */
  using ResolvePublisherKey = std::function<std::optional<std::vector<uint8_t>>(const std::string& peer_id)>;

  AmpPeerAnnounceService(IChatPeerLinks& links, PeerAnnounceFeed& feed, IoPump io_pump,
                         WorkerPost post_worker = {}, ResolvePublisherKey resolve_key = {});
  ~AmpPeerAnnounceService();

  AmpPeerAnnounceService(const AmpPeerAnnounceService&) = delete;
  AmpPeerAnnounceService& operator=(const AmpPeerAnnounceService&) = delete;

  void Start();
  void Stop();

  void SetPublisherKeyResolver(ResolvePublisherKey resolve_key);

  bool IsPeerReachable(const std::string& peer_identity_value) const;

  /** Push an already-signed tip; waits for tip_ack. */
  Roe<PeerAnnounceTipAck> PushTip(const std::string& peer_key, const PeerAnnounceTip& tip);

  PeerAnnounceFeed& Feed() { return feed_; }
  const PeerAnnounceFeed& Feed() const { return feed_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  IChatPeerLinks& links_;
  PeerAnnounceFeed& feed_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
