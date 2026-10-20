#pragma once

#include "feature/broadcast/BroadcastHub.h"
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
 * Live broadcast riding the hub (sibling of calls, media-client-layers L013): owns the broadcast
 * hub, built while the mesh runs from its links, the media_relay client and messaging's announce
 * feed, and dropped — any watch or show stopped — before any of them is rewired or goes. UI thread.
 */
class BroadcastParticipant : public HubParticipant {
public:
  ~BroadcastParticipant() override;

  /** Null while the mesh is down or media_relay is not wired. */
  BroadcastHub* Hub() { return broadcast_.get(); }
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
  void OnMeshServicesStarted(ConversationsHub& hub) override { Rebuild(hub); }
  void OnMeshMediaRebound(ConversationsHub& hub) override { Rebuild(hub); }
  void OnMeshStopping(const std::function<void()>&) override { Reset(); }
  void OnMessagingDropped() override { Reset(); }
  void OnShutdown() override { Reset(); }

private:
  void Reset();
  void Rebuild(ConversationsHub& hub);

  ConversationsHub* hub_ = nullptr;
  std::unique_ptr<BroadcastHub> broadcast_;
  /** Guards announces posted to UI for the current broadcast hub (invalidated on reset). */
  pp::Lifetime deferred_;
  std::function<void()> on_changed_;
};

} // namespace pbr
