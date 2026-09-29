#pragma once

#include "domain/media/CallMediaEngine.h"
#include "feature/calls/CallMediaSeat.h"

#include "common/Error.h"

#include <string>

namespace pbr {

/**
 * One call's media: this call's use of the one media engine and the media seat. Owned by the
 * call's LiveCall; the path drivers (1:1 CallMediaBridge, group hop workflow) start and stop the
 * engine through it instead of touching the engine and seat themselves. Calls owner only.
 *
 * Grows toward the call's media-mode owner (Direct ↔ Hop selection and switching); today it owns
 * the engine start sequence and the stops.
 */
class CallMediaCoordinator {
public:
  /** `seat` may be null (no seat bound, e.g. unit harnesses): the engine alone is driven. */
  CallMediaCoordinator(std::string call_id, CallMediaEngine& engine, CallMediaSeat* seat);

  const std::string& CallId() const { return call_id_; }

  /**
   * Run the engine for this call on `path`, sending with `send`: take the seat (releasing any other
   * call's media), start — or, when it already runs this call, re-point — the engine, and mark the
   * seat started on `path`. Fails when the seat will not hold this call.
   */
  Roe<void> StartEngine(CallMediaSeat::PathKind path, CallMediaEngine::SfuSendFn send);

  /** Stop the engine (capture threads, send). The seat stays bound — the call may restart. */
  void StopEngine(const char* why);

  /** The path the engine was last started on for this call. */
  CallMediaSeat::PathKind Path() const { return path_; }

private:
  std::string call_id_;
  CallMediaEngine& engine_;
  CallMediaSeat* seat_ = nullptr;
  CallMediaSeat::PathKind path_ = CallMediaSeat::PathKind::None;
};

} // namespace pbr
