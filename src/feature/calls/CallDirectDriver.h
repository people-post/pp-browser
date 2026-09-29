#pragma once

#include "feature/calls/CallMediaSeat.h"

#include <string>

namespace pbr {

/**
 * The 1:1 direct media path as a call's media coordinator drives it (CallMediaBridge implements it).
 * The coordinator decides; the driver runs the path.
 */
class CallDirectDriver {
public:
  virtual ~CallDirectDriver() = default;
  /** Arm and start the 1:1 connect for `call_id` (offerer reaches, answerer awaits). */
  virtual void ScheduleDirectStart(const std::string& call_id, const std::string& peer_identity, bool offerer) = 0;
  /** The call moved to the hop: drop the 1:1 transport under `token`, keep the engine running. */
  virtual void ReleaseDirectTransport(const CallMediaSeat::Token& token) = 0;
  /** Same, with no seat bound (harnesses). */
  virtual void ReleaseDirectTransport() = 0;
};

} // namespace pbr
