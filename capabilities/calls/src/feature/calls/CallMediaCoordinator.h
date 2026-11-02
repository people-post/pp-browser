#pragma once

#include "lib/media/MediaEngine.h"
#include "feature/calls/CallDirectDriver.h"
#include "feature/calls/CallHopDriver.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallMediaSeat.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pbr {

/** Builds one call's 1:1 path (V056); `serial` names that path in its events. */
using CallDirectPathFactory =
    std::function<std::unique_ptr<CallDirectDriver>(const std::string& call_id, uint64_t serial)>;

/** What every call's coordinator drives: the one engine, the seat, and how its paths are made. */
struct CallMediaResources {
  MediaEngine* engine = nullptr;
  /** Null: no seat bound (mesh stopped / harness). */
  CallMediaSeat* seat = nullptr;
  /** Empty: no 1:1 path (mesh media not wired). */
  CallDirectPathFactory make_direct;
  /** Names each 1:1 path built (shared by the coordinators). */
  uint64_t next_direct_serial = 0;
  /** Null: no group path (harness). */
  CallHopDriver* hop = nullptr;
};

/**
 * One call's media: this call's use of the one media engine and the media seat. Owned by
 * LiveCallMedia for one admission of the call; the path drivers (1:1 CallMediaBridge, group hop
 * workflow) start and stop the engine through it instead of touching the engine and seat
 * themselves. Calls owner only.
 *
 * Grows toward the call's media-mode owner (Direct ↔ Hop selection and switching); today it owns
 * the engine start / stops, the 1:1 start (BeginDirect) and the Direct → Hop hand-off
 * (HoldSeatForHop, ReleaseDirect).
 */
class CallMediaCoordinator {
public:
  /** `resources` outlives the coordinator (LiveCallMedia owns both); its engine must be set. */
  CallMediaCoordinator(std::string call_id, CallMediaResources& resources);
  ~CallMediaCoordinator();
  CallMediaCoordinator(const CallMediaCoordinator&) = delete;
  CallMediaCoordinator& operator=(const CallMediaCoordinator&) = delete;

  const std::string& CallId() const { return call_id_; }

  /**
   * Run the engine for this call on `path` (Direct / Hop), sending with `send`: take the seat
   * (releasing any other call's media), start — or, when it already runs this call, re-point — the
   * engine, and mark the seat started. Fails when the seat will not hold this call.
   */
  Roe<void> StartEngine(CallMediaPath path, MediaEngine::SfuSendFn send);

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

  // --- What the 1:1 path needs to know about the group path ----------------------------------------

  /** Media runs on a hop now. */
  bool HopAttached() const;
  /** A hop attach / migration / recovery is in flight. */
  bool HopInFlight() const;
  /** The call is (or is about to be) a group call: its 1:1 stream closing is expected. */
  bool ExpectsHop() const;
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
  MediaEngine& Engine() const { return *resources_.engine; }

  std::string call_id_;
  CallMediaResources& resources_;
  std::unique_ptr<CallDirectDriver> direct_;
  uint64_t direct_serial_ = 0;
};

} // namespace pbr
