#pragma once

#include "common/Error.h"
#include "feature/calls/CallMediaSeat.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * V036 Phase 3 — Direct path plugin façade.
 * Stack/CSM close over Bridge ops; this type does not hold CallMediaBridge*.
 */
class CallDirectPath {
public:
  struct Ops {
    std::function<void(const std::string& call_id, const std::string& peer_identity, bool offerer)>
        schedule_start;
    std::function<void(const CallMediaSeat::Token& token)> release_transport;
  };

  CallDirectPath(Ops ops, CallMediaSeat* seat);

  /** Bind + schedule 1:1 StartSfu (Acquire then schedule). */
  void ScheduleStart(const std::string& call_id, const std::string& peer_identity, bool offerer);
  /** SoftMigrate: drop 1:1 transport under token; seat stays bound. */
  Roe<void> ReleaseTransport(const CallMediaSeat::Token& token);

private:
  Ops ops_;
  CallMediaSeat* seat_ = nullptr;
};

/**
 * V036 Phase 3 — Hop path seat bind façade.
 * Topology SoftMigrate/Attach remain on the controller; this only Acquire/AllowsPathOp.
 */
class CallHopPath {
public:
  explicit CallHopPath(CallMediaSeat* seat);

  /** Acquire (or confirm) bind for hop attach; empty token on failure. */
  CallMediaSeat::Token BindForAttach(const std::string& call_id);
  bool Allows(const CallMediaSeat::Token& token) const;

private:
  CallMediaSeat* seat_ = nullptr;
};

} // namespace pbr
