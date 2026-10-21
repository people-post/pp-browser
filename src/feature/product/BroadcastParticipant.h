#pragma once

#include "feature/broadcast/BroadcastHub.h"
#include "feature/broadcast/BroadcastServices.h"
#include "feature/conversations/HubParticipant.h"
#include "domain/broadcast/PeerAnnounceTypes.h"
#include "common/Lifetime.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Live broadcast riding the hub (sibling of calls, media-client-layers L013). Owns:
 * - the broadcast services (announce feed, publisher, notifications, ticket server) for as long as
 *   the mesh runs;
 * - the broadcast hub (viewer, broadcaster), built from the mesh links, the media_relay client and
 *   the services, and dropped — any watch or show stopped — before any of them is rewired or goes.
 * UI thread.
 */
class BroadcastParticipant : public HubParticipant {
public:
  explicit BroadcastParticipant(ConversationsHub& hub);
  ~BroadcastParticipant() override;

  /** Null while the mesh is down or media_relay is not wired. */
  BroadcastHub* Hub() { return broadcast_.get(); }
  /** Null while the mesh is down. */
  BroadcastServices* Services() { return services_.get(); }

  /** Spine B: reply to an announce publisher by DM (not in-topic speech). */
  Roe<ThreadMessage> ReplyToAnnouncePublisher(const std::string& tip_peer_id, const std::string& text);
  /** An on-screen (overlay) reply request: a DM with intent=overlay; the publisher's policy decides. */
  Roe<ThreadMessage> ReplyToAnnounceOverlay(const std::string& tip_peer_id, const std::string& join_handle,
                                            const std::string& text, const std::string& viewer_msg_id);
  /** Fires (UI thread) on every status change, across rebuilds. */
  void SetOnChanged(std::function<void()> callback);

  /** Listen to the live program a signed tip announces (replaces any current watch); `on_done` on UI. */
  void WatchLive(const PeerAnnounceTip& tip, std::function<void(Roe<void>)> on_done);
  /** Same, from the latest stored tip of that program. */
  void WatchStoredLive(const std::string& peer_id, const std::string& topic_id, const std::string& program_id,
                       std::function<void(Roe<void>)> on_done);
  void StopWatching();
  /** Current watch (phase, hop, error); nullopt while broadcast viewing is unavailable. */
  std::optional<BroadcastViewerWorkflow::Status> WatchStatus() const;
  /** Publish a program live through `hops` (media_relay nodes, preference order); `on_done` on UI. */
  void GoLive(const std::string& topic_id, const std::string& program_id, std::vector<std::string> hops,
              std::function<void(Roe<void>)> on_done);
  void EndLive();
  /** Current show (phase, join handle, hop, frames sent); nullopt while broadcasting is unavailable. */
  std::optional<BroadcasterWorkflow::Status> LiveStatus() const;

  void OnMeshMediaDetach() override { Reset(); }
  void OnMeshServicesStarted(ConversationsHub& hub) override;
  void OnMeshMediaRebound(ConversationsHub& hub) override { Rebuild(hub); }
  void OnMeshStopping(const std::function<void()>&) override;
  void OnShutdown() override;

private:
  void Reset();
  void Rebuild(ConversationsHub& hub);

  ConversationsHub& hub_;
  std::unique_ptr<BroadcastServices> services_;
  // Borrows the services: declared after them, goes first.
  std::unique_ptr<BroadcastHub> broadcast_;
  /** Guards announces posted to UI for the current broadcast hub (invalidated on reset). */
  pp::Lifetime deferred_;
  std::function<void()> on_changed_;
};

} // namespace pbr
