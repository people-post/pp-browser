#pragma once

#include "common/Module.h"
#include "domain/messaging/CallMobility.h"
#include "domain/messaging/CallPathPolicy.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * k6: this device's mobility class (attachment changes, NAT rebinds, config override; re-evaluated on
 * its own schedule as churn ages out), each call peer's class from its caps, and the path policy the
 * two give a call. Calls owner, except LocalClass (published for caps on any thread).
 */
class CallPathMobility : public Module {
public:
  struct Ports {
    /** This device's class changed: the active call re-plans and tells its peer. */
    std::function<void()> on_local_class_changed;
    /** A call peer's class changed: that call's path policy may have. */
    std::function<void(const std::string& call_id)> on_policy_changed;
  };

  CallPathMobility();
  ~CallPathMobility() override;

  void SetPorts(Ports ports) { ports_ = std::move(ports); }
  /** Stop re-evaluating for good: pending timer callbacks drop (any thread). */
  void Detach();
  /** Cancel the pending re-evaluation (teardown; the next change schedules again). Calls owner. */
  void CancelReevaluation();

  void OnAttachment(const MobilityAttachment& attachment, bool changed);
  void OnObservedAddressChanged();
  /** A pinned class (config `mesh.mobility` / `--mobility=`), or nullopt for auto. */
  void SetOverride(std::optional<MobilityClass> pinned);
  /** The peer's class for `call_id` (invite / accept / caps update). */
  void NoteRemote(const std::string& call_id, MobilityClass mobility);

  CallPathPolicy PolicyFor(const std::string& call_id) const;
  /** This device's class as advertised in caps (any thread). */
  MobilityClass LocalClass() const { return published_.load(std::memory_order_acquire); }

private:
  void Reevaluate();
  void ScheduleReevaluation();

  Ports ports_;
  MobilityClassifier local_;
  std::atomic<MobilityClass> published_{MobilityClass::Unknown};
  /** The remote's class per call. */
  std::unordered_map<std::string, MobilityClass> remote_;
  uint64_t timer_id_ = 0;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace pbr
