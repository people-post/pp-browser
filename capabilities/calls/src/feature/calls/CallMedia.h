#pragma once

#include "lib/media/MediaEngine.h"
#include "feature/calls/CallDirectDriver.h"
#include "feature/calls/CallHopDriver.h"
#include "feature/calls/CallEngine.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallMediaSeat.h"

#include "common/Error.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pbr {

/**
 * Builds one call's 1:1 / group path (V056); `serial` names that path in its events, `engine` is the
 * call's handle on the media engine (V057: the path's only way to it), `hop_carries_media` is the
 * call's flag the hop path writes and the 1:1 path reads on I/O.
 */
using CallDirectPathFactory = std::function<std::unique_ptr<CallDirectDriver>(
    const std::string& call_id, uint64_t serial, std::shared_ptr<CallEngine> engine,
    std::shared_ptr<std::atomic<bool>> hop_carries_media)>;
using CallHopPathFactory = std::function<std::unique_ptr<CallHopDriver>(
    const std::string& call_id, uint64_t serial, std::shared_ptr<CallEngine> engine,
    std::shared_ptr<std::atomic<bool>> hop_carries_media)>;

/** What every call's media drives: the one engine, the seat, and how its paths are made. */
struct CallMediaResources {
  MediaEngine* engine = nullptr;
  /** Null: no seat bound (mesh stopped / harness). */
  CallMediaSeat* seat = nullptr;
  /** Empty: no 1:1 path (mesh media not wired). */
  CallDirectPathFactory make_direct;
  /** Empty: no group path (harness). */
  CallHopPathFactory make_hop;
  /** Names each path built (shared by the calls' media). */
  uint64_t next_path_serial = 0;
};

/**
 * One call's media (V056, call level): this call's use of the one media engine and the seat, and
 * the call's paths — it builds and owns its 1:1 path (CallMediaBridge) and its group path (hop
 * topology + migrate), both naming this call. Owned by LiveCallMedia for one admission of the call.
 * The session manager and the device reach a path only through this object; the paths start and stop the engine
 * through it. Direct → Hop is a hand-off: both paths live until the hop is heard. Calls owner only.
 */
class CallMedia {
public:
  /** `resources` outlives this object (LiveCallMedia owns both); its engine must be set. */
  CallMedia(std::string call_id, CallMediaResources& resources);
  ~CallMedia();
  CallMedia(const CallMedia&) = delete;
  CallMedia& operator=(const CallMedia&) = delete;

  const std::string& CallId() const { return call_id_; }

  /**
   * Run the engine for this call on `path` (Direct / Hop), sending with `send`: take the seat
   * (releasing any other call's media), start — or, when it already runs this call, re-point — the
   * engine, and mark the seat started. Fails when the seat will not hold this call.
   */
  Roe<void> StartEngine(CallMediaPath path, MediaEngine::PacketSendFn send);

  /** Stop the engine (capture threads, send). The seat stays bound — the call may restart. */
  void StopEngine(const char* why);

  /** Start the call on the 1:1 direct path: take the seat, then have the direct driver connect. */
  Roe<void> BeginDirect(const std::string& peer_identity, bool offerer);
  /** The hop path is attaching this call: hold the seat for it. */
  void HoldSeatForHop();
  /**
   * The call now runs on the hop (engine send-swapped): drop the 1:1 transport while this call still
   * holds the seat. The engine keeps running.
   */
  void ReleaseDirect();

  // --- Media mode: which path carries the call ----------------------------------------------------

  /**
   * Our accept landed with `n_joined` in the call: the hop driver takes it (group, via hint or
   * SoftMigrate) or it stays 1:1. Returns the path for the call to record (LiveCall owns it); the
   * caller starts Direct (after its own chrome / store steps) when this returns Direct.
   */
  CallMediaPath DecideOnLocalAccept(size_t n_joined, const std::optional<std::string>& sfu_hint);
  /** A peer accepted our call: as above. A call already on `current` Hop stays there. */
  CallMediaPath DecideOnRemoteAccept(size_t n_joined, const std::string& joiner_identity, CallMediaPath current);

  // --- The call's 1:1 path (V056: built for this call, lives with it) --------------------------------

  /** The call's 1:1 path, built on first use; null when none can be made (not wired). */
  CallDirectDriver* Direct();
  /** The path if built (never builds one). */
  CallDirectDriver* DirectIfBuilt() const { return direct_.get(); }
  /** The built path's serial (0: none) — its events carry it; a path built since drops the old ones. */
  uint64_t DirectSerial() const { return direct_serial_; }
  /** Drop the built path (the mesh's transport goes; a later use builds a new one). */
  void DropDirect();

  /** The call's group path, built on first use; null when none can be made (harness). */
  CallHopDriver* Hop();
  CallHopDriver* HopIfBuilt() const { return hop_.get(); }
  uint64_t HopSerial() const { return hop_serial_; }

  // --- What the call's paths are told (the session routes; the device never reaches a path) ---------

  /** The call's media key landed: the deferred 1:1 answerer goes on. */
  void OnMediaKeyReady();
  /** The call's media key epoch landed: the hop re-keys its streams. */
  void OnMediaKeyInstalled();
  /** The doorway accepted an inbound 1:1 bundle for the call (the 1:1 path is built if needed). */
  CallMediaDirectCallbacks AcceptInbound(const CallMediaInboundHello& hello);
  /** H013: the call's peer gave up reaching us. */
  void OnPeerGaveUpReaching();
  void OnPathPolicyChanged();
  void OnLocalNetworkMoved();
  /** Group: the joined count moved (the group path is built if needed). */
  void OnJoinedCountObserved(size_t n_joined);
  /** Inbound hop control for the call (an attach builds the group path if needed). */
  Roe<void> OnInboundSfuAttach(const CallSfuAttachDetail& attach, const std::string& sender);
  void OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail);
  void OnInboundHopRefuse(const CallHopRefuseDetail& detail);
  void ClearSfuAttachWait();
  void OnPeerMediaRelayCapLearned(const std::string& peer_id);
  /** The engine runs the call on the hop. */
  bool IsOnHop() const;
  /** The seat released the call's media: the hop lets go of the call (Detach, attach state). */
  void OnMediaStopped();
  /** The seat stops the call's media: the 1:1 path tears down (engine included), else the engine stops. */
  void StopMedia();
  /** A path's own event: run if `serial` is still that path. */
  void HandleDirectEvent(uint64_t serial, DirectPathEvent& event);
  void HandleHopEvent(uint64_t serial, TopologyEvent& event);
  /** A report from 1:1 path `serial` is still this call's current path's. */
  bool IsDirectPath(uint64_t serial) const { return serial != 0 && serial == direct_serial_; }

  /** What the screen reads of the call's paths. */
  struct View {
    CallDirectPathStatus direct;
    bool hop_attached = false;
    /** A hop attach / migration / recovery is in flight. */
    bool hop_in_flight = false;
    CallRecovery recovery = CallRecovery::None;
    CallHopHealth hop_health;
  };
  View PathsView() const;

  // --- What the 1:1 path needs to know about the group path ----------------------------------------

  /** Media runs on a hop now. */
  bool HopAttached() const;
  /** A hop attach / migration / recovery is in flight. */
  bool HopInFlight() const;
  /** The call is (or is about to be) a group call: its 1:1 stream closing is expected. */
  bool ExpectsHop();
  /** The 1:1 stream dropped ahead of the hop attach: have the hop wait for it. */
  void ExpectHopAttach();

  // --- The call's devices and restarts -------------------------------------------------------------

  void SetMuted(bool muted);
  /** Camera on / off; the hop re-picks what it sends. False when the camera would not open. */
  Roe<void> SetCamera(bool enabled, int display_rotation_degrees);
  bool CameraOn() const;
  /** Our own video: send a keyframe (a viewer asked). */
  void RequestKeyframe();
  /** A failed, open call: Retry / resume over the peer's connection (the 1:1 driver runs them). */
  Roe<void> Retry();
  Roe<void> ResumeFromInbound();

private:
  MediaEngine& Engine() const { return engine_->Engine(); }

  std::string call_id_;
  CallMediaResources& resources_;
  /** This call's handle on the engine, lent to its paths (V057). */
  std::shared_ptr<CallEngine> engine_;
  /** The hop carries this call's media: the hop path writes it, the 1:1 path reads it on I/O. */
  std::shared_ptr<std::atomic<bool>> hop_carries_media_ = std::make_shared<std::atomic<bool>>(false);
  /** Destroyed hop first (it releases the call's media hop), then the 1:1 path. */
  std::unique_ptr<CallDirectDriver> direct_;
  uint64_t direct_serial_ = 0;
  std::unique_ptr<CallHopDriver> hop_;
  uint64_t hop_serial_ = 0;
};

} // namespace pbr
