#pragma once

#include "domain/media/CallMediaEngine.h"
#include "feature/calls/CallDirectDriver.h"
#include "feature/calls/CallMediaSeat.h"

#include "common/Error.h"

#include <string>

namespace pbr {

/** What every call's coordinator drives: the one engine, the seat and the 1:1 driver (bindable). */
struct CallMediaResources {
  CallMediaEngine* engine = nullptr;
  /** Null: no seat bound (mesh stopped / harness). */
  CallMediaSeat* seat = nullptr;
  /** Null: no 1:1 path (mesh media not wired). */
  CallDirectDriver* direct = nullptr;
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

private:
  CallMediaEngine& Engine() const { return *resources_.engine; }

  std::string call_id_;
  const CallMediaResources& resources_;
  CallMediaSeat::PathKind path_ = CallMediaSeat::PathKind::None;
};

} // namespace pbr
