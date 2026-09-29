#pragma once

#include "domain/media/CallMediaEngine.h"
#include "feature/calls/CallDirectDriver.h"
#include "feature/calls/CallHopDriver.h"
#include "feature/calls/CallMediaSeat.h"

#include "common/Error.h"

#include <optional>
#include <string>

namespace pbr {

/** What every call's coordinator drives: the one engine, the seat and the 1:1 driver (bindable). */
struct CallMediaResources {
  CallMediaEngine* engine = nullptr;
  /** Null: no seat bound (mesh stopped / harness). */
  CallMediaSeat* seat = nullptr;
  /** Null: no 1:1 path (mesh media not wired). */
  CallDirectDriver* direct = nullptr;
  /** Null: no group path (harness). */
  CallHopDriver* hop = nullptr;
};

/** Which path carries a call's media, as its coordinator decided. */
enum class CallMediaPath {
  /** Not decided yet (before a join). */
  Undecided,
  /** 1:1 direct / punched / relayed carrier (Bridge). */
  Direct,
  /** Group, on a hop / media_relay (Topology). A call never goes back to Direct. */
  Hop,
};

/**
 * One call's media: this call's use of the one media engine and the media seat. Owned by the
 * call's LiveCall; the path drivers (1:1 CallMediaBridge, group hop workflow) start and stop the
 * engine through it instead of touching the engine and seat themselves. Calls owner only.
 *
 * Grows toward the call's media-mode owner (Direct ↔ Hop selection and switching); today it owns
 * the engine start / stops, the 1:1 start (BeginDirect) and the Direct → Hop hand-off
 * (HoldSeatForHop, ReleaseDirect).
 */
class CallMediaCoordinator {
public:
  /** `resources` outlives the coordinator (LiveCalls owns both); its engine must be set. */
  CallMediaCoordinator(std::string call_id, const CallMediaResources& resources);

  const std::string& CallId() const { return call_id_; }

  /**
   * Run the engine for this call on `path`, sending with `send`: take the seat (releasing any other
   * call's media), start — or, when it already runs this call, re-point — the engine, and mark the
   * seat started on `path`. Fails when the seat will not hold this call.
   */
  Roe<void> StartEngine(CallMediaSeat::PathKind path, CallMediaEngine::SfuSendFn send);

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

  /** The path the engine was last started on for this call. */
  CallMediaSeat::PathKind Path() const { return path_; }

  // --- Media mode: which path carries the call ----------------------------------------------------

  /**
   * Our accept landed with `n_joined` in the call: the hop driver takes it (group, via hint or
   * SoftMigrate) or it stays 1:1. Records and returns the mode; the caller starts Direct (after its
   * own chrome / store steps) when this returns Direct.
   */
  CallMediaPath DecideOnLocalAccept(size_t n_joined, const std::optional<std::string>& sfu_hint);
  /** A peer accepted our call: as above (Hop also when the call is no longer the active one). */
  CallMediaPath DecideOnRemoteAccept(size_t n_joined, const std::string& joiner_identity);
  CallMediaPath MediaPath() const { return media_path_; }

  // --- What the 1:1 path needs to know about the group path ----------------------------------------

  /** Media runs on a hop now. */
  bool HopAttached() const;
  /** A hop attach / migration / recovery is in flight. */
  bool HopInFlight() const;
  /** The call is (or is about to be) a group call: its 1:1 stream closing is expected. */
  bool ExpectsHop() const;
  /** The 1:1 stream dropped ahead of the hop attach: have the hop wait for it. */
  void ExpectHopAttach();

private:
  CallMediaEngine& Engine() const { return *resources_.engine; }

  std::string call_id_;
  const CallMediaResources& resources_;
  CallMediaSeat::PathKind path_ = CallMediaSeat::PathKind::None;
  CallMediaPath media_path_ = CallMediaPath::Undecided;
};

} // namespace pbr
