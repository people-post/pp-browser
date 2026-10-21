#pragma once

#include "domain/broadcast/AnnounceNotificationInbox.h"
#include "domain/broadcast/AnnounceOverlayReply.h"
#include "domain/broadcast/PeerAnnounceFeed.h"
#include "domain/broadcast/PeerAnnouncePublisher.h"
#include "domain/broadcast/PeerAnnounceRpcCodec.h"
#include "feature/broadcast/AmpBroadcastTransport.h"
#include "feature/broadcast/AmpPeerAnnounceTransport.h"
#include "mesh/host/MeshPorts.h"
#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** What the broadcast services take from the node: its links and lanes, and its keys. */
struct BroadcastServicesDeps {
  mesh::IChatPeerLinks* links = nullptr;
  /** The mesh's lanes; `post_worker` is where blocking work goes (the product: the worker pool). */
  mesh::MeshIoContext io;
  /** A publisher's device ML-DSA-65 public key by peer id (tips and tickets are signed with it). */
  std::function<std::optional<std::vector<uint8_t>>(const std::string& peer_id)> publisher_key;
  /** This node's peer id and device ML-DSA-65 secret: none, and it cannot publish. */
  std::string local_peer_id;
  std::function<std::optional<std::vector<uint8_t>>()> device_secret;
};

/**
 * The live-broadcast services a node runs on the mesh, for as long as the mesh runs: the signed
 * peer-announce feed (tips pushed and pulled, our publisher, announce notifications) and the
 * broadcast ticket server (viewer tickets for our live programs). They register protocol handlers
 * on the links, so they go before the mesh stops.
 */
class BroadcastServices {
public:
  explicit BroadcastServices(BroadcastServicesDeps deps);
  ~BroadcastServices();
  BroadcastServices(const BroadcastServices&) = delete;
  BroadcastServices& operator=(const BroadcastServices&) = delete;

  /** Sign and record a program tip in our feed (followers pull it, or get it pushed). */
  Roe<PeerAnnounceTip> PublishTip(const PeerAnnouncePublisher::Draft& draft);
  /** Publish a tip and push it to one follower. */
  void PublishAndPushAsync(const std::string& peer_key, const PeerAnnouncePublisher::Draft& draft, int64_t now_ms,
                           std::function<void(Roe<PeerAnnounceTipAck>)> on_done);
  Roe<PeerAnnounceTipAck> PublishAndPush(const std::string& peer_key, const PeerAnnouncePublisher::Draft& draft,
                                         int64_t now_ms);
  /** A viewer's overlay reply, rebroadcast as a live_chat tip (publisher policy decides before). */
  void PublishLiveChatFromOverlayAsync(const std::string& peer_key, const std::string& topic_id,
                                       const std::string& program_id, const std::string& join_handle,
                                       const std::string& viewer_peer_id, const AnnounceOverlayReplyBody& body,
                                       int64_t now_ms, std::function<void(Roe<PeerAnnounceTipAck>)> on_done);
  Roe<PeerAnnounceTipAck> PublishLiveChatFromOverlay(const std::string& peer_key, const std::string& topic_id,
                                                     const std::string& program_id, const std::string& join_handle,
                                                     const std::string& viewer_peer_id,
                                                     const AnnounceOverlayReplyBody& body, int64_t now_ms);
  /** Latest stored signed tip for a program. */
  std::optional<PeerAnnounceTip> LatestTip(const std::string& peer_id, const std::string& topic_id,
                                           const std::string& program_id) const;
  AnnounceNotificationInbox& Notifications() { return notifications_; }

  /** Broadcaster: the ticket server mints viewer tickets for this live program. */
  void PutLiveProgramKey(const std::string& program_id, const std::string& join_handle,
                         AmpBroadcastTransport::LiveProgramKey key);
  void ClearLiveProgramKey(const std::string& program_id, const std::string& join_handle);

  AmpPeerAnnounceTransport& AnnounceTransport() { return *announce_; }
  AmpBroadcastTransport& TicketTransport() { return *tickets_; }

private:
  Roe<void> RequirePublisher() const;

  PeerAnnounceFeed feed_;
  AnnounceNotificationInbox notifications_;
  std::unique_ptr<PeerAnnouncePublisher> publisher_;
  // Declared after the feed and inbox they use: stopped (handlers removed) first.
  std::unique_ptr<AmpPeerAnnounceTransport> announce_;
  std::unique_ptr<AmpBroadcastTransport> tickets_;
};

} // namespace pbr
