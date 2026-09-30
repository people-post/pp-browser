#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "feature/broadcast/BroadcastViewerWorkflow.h"
#include "feature/broadcast/BroadcasterWorkflow.h"
#include "foundation/runtime/OwnerTasks.h"

#include "common/Error.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

class AmpBroadcastRpcClient;
class PeerReachCoordinator;

/** Neutral mesh pieces the product hub is built from (app wiring hands them over). */
struct BroadcastMeshDeps {
  IChatPeerLinks* links = nullptr;
  MeshIoContext io;
  /** Lent by the product hub's MeshMediaRelay (built on MeshConnectivity); the hub must be destroyed before they are rewired (L015). */
  MediaRelayAttachPorts relay;
  std::function<std::optional<ByteVector>(const std::string& peer_id)> publisher_key;
  std::function<std::string(const std::string& hop_peer_id)> hop_multiaddr;
  // Publishing (l5): the ticket server's program keys and the signed announce.
  std::function<void(const std::string& program_id, const std::string& join_handle, BroadcastProgramKey key)>
      put_program_key;
  std::function<void(const std::string& program_id, const std::string& join_handle)> clear_program_key;
  /** Runs on the calling owner's thread; the hub hands the result back to the broadcaster. */
  std::function<Roe<void>(const BroadcastTipDraft& draft)> announce;
  /** Where `announce` must run (the product: UI, which owns mesh messaging). Empty = inline. */
  std::function<void(std::function<void()>)> post_announce;
};

/** What the GUI reads, published by the hub on the media-sessions owner after every step. */
struct BroadcastUiState {
  BroadcastViewerWorkflow::Status viewer;
  BroadcasterWorkflow::Status live;
};

/**
 * Broadcast feature root (media-client-layers L001 / B008): the viewer (playback engine) and the
 * broadcaster (capture engine), each on its own engine session and device leases. Sibling of the
 * call stack — no call session, lifecycle or ringing. Watching and broadcasting share the single
 * media_relay client session (L013), so only one of them attaches at a time.
 *
 * Threading (THREADING.md § Owner runners): the hub is broadcast's runner — the only broadcast type
 * that posts, schedules or waits. It shares the media-sessions owner with the call stack (T001:
 * one media thread) but runs its own tree there: intents and the workflows' events (their outboxes)
 * are its tasks; the workflows are passive. Results go to UI; reads come from `BroadcastUiState`,
 * published after every step; the frame counter is read live. Built on any thread; destroyed from
 * the thread that drives it (the product hub, on UI) — teardown runs on the owner, which the
 * destructor waits for.
 */
class BroadcastHub {
public:
  /**
   * Product hub: broadcast RPC client + a link-reach coordinator of its own (publisher tickets)
   * + viewer + broadcaster on the media-sessions owner. Null when the mesh pieces are missing.
   */
  static std::unique_ptr<BroadcastHub> ForMesh(BroadcastMeshDeps deps, MediaDeviceArbiter& devices);

  /**
   * `viewer.engine` / `broadcaster.engine` are ignored: the hub owns a playback and a capture
   * engine on `devices` (must outlive the hub). Empty broadcaster ports → GoLive is refused.
   * `executor`: where the hub runs (null: the media-sessions owner; tests pass their own).
   */
  BroadcastHub(BroadcastViewerPorts viewer, MediaDeviceArbiter& devices, BroadcasterPorts broadcaster = {},
               OwnerExecutor* executor = nullptr);
  ~BroadcastHub();
  BroadcastHub(const BroadcastHub&) = delete;
  BroadcastHub& operator=(const BroadcastHub&) = delete;

  /** Watch the live program a signed tip announces (replaces any current watch). `on_done` on UI. */
  void WatchLive(const PeerAnnounceTip& tip, std::function<void(Roe<void>)> on_done = {});
  void StopWatching();
  /** Publish a program live (ends any current show). `on_done` on UI. */
  void GoLive(BroadcastLiveRequest request, std::function<void(Roe<void>)> on_done = {});
  void EndLive();

  // Any thread: the owner's last published step.
  std::shared_ptr<const BroadcastUiState> State() const;
  bool IsWatching() const;
  BroadcastViewerWorkflow::Status Viewer() const { return State()->viewer; }
  bool IsLive() const;
  /** With the live frame counter. */
  BroadcasterWorkflow::Status Live() const;

  /** Levels / health for the watch UI (engine health reads are thread-safe). */
  const CallMediaEngine& Media() const { return *engine_; }
  const CallMediaEngine& CaptureMedia() const { return *capture_engine_; }

  /** Runs on UI after every viewer or broadcaster status change. */
  void SetOnChanged(std::function<void()> callback);

private:
  /** Run `step` as the hub's task on its owner, then publish. */
  void OnOwner(std::function<void()> step);
  void Publish();
  void OnStatusChanged();

  OwnerExecutor& executor_;
  /** The hub's work on its owner: intents and the workflows' events (dropped with the hub). */
  OwnerTasks tasks_;
  // Destroyed after the workflows (declared first).
  std::unique_ptr<AmpBroadcastRpcClient> rpc_;
  std::unique_ptr<PeerReachCoordinator> reach_;
  std::unique_ptr<CallMediaEngine> engine_;
  std::unique_ptr<CallMediaEngine> capture_engine_;
  std::unique_ptr<BroadcastViewerWorkflow> viewer_;
  std::unique_ptr<BroadcasterWorkflow> broadcaster_;
  std::shared_ptr<const std::atomic<uint64_t>> frames_sent_;
  std::function<void()> on_changed_;  // owner

  mutable std::mutex state_mu_;
  std::shared_ptr<const BroadcastUiState> state_ = std::make_shared<BroadcastUiState>();
};

} // namespace pbr
