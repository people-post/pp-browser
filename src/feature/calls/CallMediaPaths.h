#pragma once

#include "common/Error.h"
#include "feature/calls/CallMediaSeat.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * V036 Phase 3 — Direct path plugin façade.
 * Stack/CSM close over Bridge + seat ops; this type does not hold CallMediaBridge* or
 * CallMediaSeat* (V048 ha8–ha9).
 */
class CallDirectPath {
public:
  struct Ops {
    std::function<void(const std::string& call_id, const std::string& peer_identity, bool offerer)>
        schedule_start;
    std::function<void(const CallMediaSeat::Token& token)> release_transport;
    std::function<CallMediaSeat::Token(const std::string& call_id)> acquire;
    std::function<bool(const CallMediaSeat::Token& token)> allows_path_op;
    std::function<void(CallMediaSeat::PathKind kind)> note_path;
  };

  explicit CallDirectPath(Ops ops);

  /** Bind + schedule 1:1 StartSfu (Acquire then schedule). */
  void ScheduleStart(const std::string& call_id, const std::string& peer_identity, bool offerer);
  /** SoftMigrate: drop 1:1 transport under token; seat stays bound. */
  Roe<void> ReleaseTransport(const CallMediaSeat::Token& token);

private:
  Ops ops_;
};

/**
 * V036 Phase 3 — Hop path seat bind façade.
 * Topology SoftMigrate/Attach remain on the controller; this only Acquire/AllowsPathOp.
 * Does not hold CallMediaSeat* — callers project seat ops (V048 ha9).
 */
class CallHopPath {
public:
  struct Ops {
    std::function<CallMediaSeat::Token(const std::string& call_id)> acquire;
    std::function<bool(const CallMediaSeat::Token& token)> allows_path_op;
  };

  explicit CallHopPath(Ops ops);

  /** Acquire (or confirm) bind for hop attach; empty token on failure. */
  CallMediaSeat::Token BindForAttach(const std::string& call_id);
  bool Allows(const CallMediaSeat::Token& token) const;

private:
  Ops ops_;
};

} // namespace pbr
