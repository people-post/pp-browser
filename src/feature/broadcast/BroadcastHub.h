#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "feature/broadcast/BroadcastViewerWorkflow.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Broadcast feature root (media-client-layers L001 / B008): owns the viewer's playback engine and
 * workflow. Sibling of the call stack — no call session, lifecycle or ringing. The broadcaster
 * (l5) joins here.
 *
 * UI thread.
 */
class BroadcastHub {
public:
  /** `ports.engine` is ignored: the hub owns a playback engine on `devices` (must outlive the hub). */
  BroadcastHub(BroadcastViewerPorts ports, MediaDeviceArbiter& devices);
  ~BroadcastHub();
  BroadcastHub(const BroadcastHub&) = delete;
  BroadcastHub& operator=(const BroadcastHub&) = delete;

  /** Watch the live program a signed tip announces (replaces any current watch). */
  Roe<void> WatchLive(const PeerAnnounceTip& tip);
  void StopWatching();
  bool IsWatching() const;
  const BroadcastViewerWorkflow::Status& Viewer() const { return viewer_->CurrentStatus(); }
  /** Levels / health for the watch UI. */
  const CallMediaEngine& Media() const { return *engine_; }
  void SetOnChanged(std::function<void()> callback);

private:
  std::unique_ptr<CallMediaEngine> engine_;
  std::unique_ptr<BroadcastViewerWorkflow> viewer_;
};

} // namespace pbr
