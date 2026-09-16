#pragma once

#include "common/Error.h"
#include "feature/calls/CallMediaSeat.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallMediaBridge;
class CallTopologyController;

/**
 * V036 Phase 3 — Direct path plugin façade over CallMediaBridge.
 * Path ops require a seat token (`AllowsPathOp`); CSM schedules via this, not raw Stop/Start.
 */
class CallDirectPath {
public:
  CallDirectPath(CallMediaBridge* bridge, CallMediaSeat* seat);

  /** Bind + schedule 1:1 StartSfu (Acquire then bridge Schedule*). */
  void ScheduleStart(const std::string& call_id, const std::string& peer_identity, bool offerer);
  /** SoftMigrate: drop 1:1 transport under token; seat stays bound. */
  Roe<void> ReleaseTransport(const CallMediaSeat::Token& token);

private:
  CallMediaBridge* bridge_ = nullptr;
  CallMediaSeat* seat_ = nullptr;
};

/**
 * V036 Phase 3 — Hop path plugin façade over CallTopologyController.
 * Topology SoftMigrate/Attach remain on the controller; this gates token checks.
 */
class CallHopPath {
public:
  CallHopPath(CallTopologyController* topology, CallMediaSeat* seat);

  /** Acquire (or confirm) bind for hop attach; empty token on failure. */
  CallMediaSeat::Token BindForAttach(const std::string& call_id);
  bool Allows(const CallMediaSeat::Token& token) const;

private:
  CallTopologyController* topology_ = nullptr;
  CallMediaSeat* seat_ = nullptr;
};

} // namespace pbr
