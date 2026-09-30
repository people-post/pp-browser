#pragma once

#include "common/Module.h"
#include "domain/messaging/CallMobility.h"
#include "domain/messaging/CallPathPolicy.h"

#include <atomic>
#include <optional>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * k6: this device's mobility class (attachment changes, NAT rebinds, config override; churn ages out
 * with time), each call peer's class from its caps, and the path policy the two give a call.
 *
 * Passive: the owner feeds it events with the time, reacts to what they return, and wakes it at
 * `NextWakeAt` (a churn-driven Mobile relaxes with time alone). Calls owner, except LocalClass
 * (published for caps on any thread).
 */
class CallPathMobility : public Module {
public:
  using Clock = MobilityClassifier::Clock;

  CallPathMobility();

  /** The events below return true when this device's class changed (tell the peer, re-plan). */
  bool OnAttachment(const MobilityAttachment& attachment, bool changed, Clock::time_point now);
  bool OnObservedAddressChanged(Clock::time_point now);
  /** A pinned class (config `mesh.mobility` / `--mobility=`), or nullopt for auto. */
  bool SetOverride(std::optional<MobilityClass> pinned, Clock::time_point now);
  /** The deadline `NextWakeAt` reported came. */
  bool OnWake(Clock::time_point now) { return Reevaluate(now); }
  /** When this device's class could change with time alone; nullopt: not until the next event. */
  std::optional<Clock::time_point> NextWakeAt(Clock::time_point now) const { return local_.NextReevaluationAt(now); }

  /** The peer's class for `call_id` (invite / accept / caps update); true when it changed. */
  bool NoteRemote(const std::string& call_id, MobilityClass mobility);

  CallPathPolicy PolicyFor(const std::string& call_id) const;
  /** This device's class as advertised in caps (any thread). */
  MobilityClass LocalClass() const { return published_.load(std::memory_order_acquire); }

private:
  bool Reevaluate(Clock::time_point now);

  MobilityClassifier local_;
  std::atomic<MobilityClass> published_{MobilityClass::Unknown};
  /** The remote's class per call. */
  std::unordered_map<std::string, MobilityClass> remote_;
};

} // namespace pbr
