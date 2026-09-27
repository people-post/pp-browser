#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "feature/broadcast/BroadcastViewerWorkflow.h"
#include "feature/broadcast/BroadcasterWorkflow.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

class AmpBroadcastRpcClient;
class PeerReachCoordinator;

/** Neutral mesh pieces the product hub is built from (app wiring hands them over). */
struct BroadcastMeshDeps {
  IChatPeerLinks* links = nullptr;
  MeshIoContext io;
  /** Lent by the call plane today; the hub must be destroyed before they are rewired (L014). */
  MediaRelayAttachPorts relay;
  std::function<std::optional<ByteVector>(const std::string& peer_id)> publisher_key;
  std::function<std::string(const std::string& hop_peer_id)> hop_multiaddr;
  // Publishing (l5): the ticket server's program keys and the signed announce.
  std::function<void(const std::string& program_id, const std::string& join_handle, BroadcastProgramKey key)>
      put_program_key;
  std::function<void(const std::string& program_id, const std::string& join_handle)> clear_program_key;
  std::function<Roe<void>(const BroadcastTipDraft& draft)> announce;
};

/**
 * Broadcast feature root (media-client-layers L001 / B008): the viewer (playback engine) and the
 * broadcaster (capture engine), each on its own engine session and device leases. Sibling of the
 * call stack — no call session, lifecycle or ringing. Watching and broadcasting share the single
 * media_relay client session (L013), so only one of them attaches at a time.
 *
 * UI thread.
 */
class BroadcastHub {
public:
  /**
   * Product hub: broadcast RPC client + a link-reach coordinator of its own (publisher tickets)
   * + viewer + broadcaster, posting to the UI thread through AppRuntime. Null when the mesh pieces
   * are missing.
   */
  static std::unique_ptr<BroadcastHub> ForMesh(BroadcastMeshDeps deps, MediaDeviceArbiter& devices);

  /**
   * `viewer.engine` / `broadcaster.engine` are ignored: the hub owns a playback and a capture
   * engine on `devices` (must outlive the hub). Empty broadcaster ports → GoLive is refused.
   */
  BroadcastHub(BroadcastViewerPorts viewer, MediaDeviceArbiter& devices, BroadcasterPorts broadcaster = {});
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

  /** Publish a program live (ends any current show). */
  Roe<void> GoLive(BroadcastLiveRequest request);
  void EndLive();
  bool IsLive() const;
  BroadcasterWorkflow::Status Live() const { return broadcaster_->CurrentStatus(); }
  const CallMediaEngine& CaptureMedia() const { return *capture_engine_; }

  /** UI thread, on every viewer or broadcaster status change. */
  void SetOnChanged(std::function<void()> callback);

private:
  // Destroyed after the workflows (declared first).
  std::unique_ptr<AmpBroadcastRpcClient> rpc_;
  std::unique_ptr<PeerReachCoordinator> reach_;
  std::unique_ptr<CallMediaEngine> engine_;
  std::unique_ptr<CallMediaEngine> capture_engine_;
  std::unique_ptr<BroadcastViewerWorkflow> viewer_;
  std::unique_ptr<BroadcasterWorkflow> broadcaster_;
};

} // namespace pbr
