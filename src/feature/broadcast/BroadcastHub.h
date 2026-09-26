#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "feature/broadcast/BroadcastViewerWorkflow.h"
#include "domain/mesh/host/MeshPorts.h"

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
class AmpBroadcastRpcClient;
class PeerReachCoordinator;

/** Neutral mesh pieces the product hub is built from (app wiring hands them over). */
struct BroadcastMeshDeps {
  IChatPeerLinks* links = nullptr;
  MeshIoContext io;
  /** Lent by the call plane today; the hub must be destroyed before they are rewired (L013). */
  MediaRelayAttachPorts relay;
  std::function<std::optional<ByteVector>(const std::string& peer_id)> publisher_key;
  std::function<std::string(const std::string& hop_peer_id)> hop_multiaddr;
};

class BroadcastHub {
public:
  /**
   * Product hub: broadcast RPC client + a link-reach coordinator of its own (publisher tickets)
   * + viewer, posting to the UI thread through AppRuntime. Null when the mesh pieces are missing.
   */
  static std::unique_ptr<BroadcastHub> ForMesh(BroadcastMeshDeps deps, MediaDeviceArbiter& devices);

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
  // Destroyed after the viewer (declared first).
  std::unique_ptr<AmpBroadcastRpcClient> rpc_;
  std::unique_ptr<PeerReachCoordinator> reach_;
  std::unique_ptr<CallMediaEngine> engine_;
  std::unique_ptr<BroadcastViewerWorkflow> viewer_;
};

} // namespace pbr
