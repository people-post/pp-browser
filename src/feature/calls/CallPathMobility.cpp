#include "feature/calls/CallPathMobility.h"

#include "feature/calls/CallsThread.h"
#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include <chrono>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** One live call at a time: older entries are history. */
constexpr size_t kRemoteKept = 32;

} // namespace

CallPathMobility::CallPathMobility() {
  redirectLogger("CallPathMobility");
}

CallPathMobility::~CallPathMobility() {
  Detach();
  CancelReevaluation();
}

void CallPathMobility::Detach() {
  alive_->store(false, std::memory_order_release);
}

void CallPathMobility::OnAttachment(const MobilityAttachment& attachment, const bool changed) {
  local_.OnAttachment(attachment, changed, MobilityClassifier::Clock::now());
  Reevaluate();
}

void CallPathMobility::OnObservedAddressChanged() {
  local_.OnObservedAddressChanged(MobilityClassifier::Clock::now());
  Reevaluate();
}

void CallPathMobility::SetOverride(const std::optional<MobilityClass> pinned) {
  local_.SetOverride(pinned);
  if (pinned) {
    log().info << "mobility pinned to " << MobilityClassWire(*pinned);
  }
  Reevaluate();
}

void CallPathMobility::Reevaluate() {
  const MobilityClass before = published_.load(std::memory_order_acquire);
  const MobilityClass now = local_.Evaluate(MobilityClassifier::Clock::now());
  published_.store(now, std::memory_order_release);
  ScheduleReevaluation();
  if (now == before) {
    return;
  }
  log().info << "mobility " << MobilityClassWire(before) << " -> " << MobilityClassWire(now);
  if (ports_.on_local_class_changed) {
    ports_.on_local_class_changed();
  }
}

void CallPathMobility::ScheduleReevaluation() {
  // A churn-driven Mobile relaxes with time alone: re-evaluate when the classifier says it could.
  CancelReevaluation();
  const auto at = local_.NextReevaluationAt(MobilityClassifier::Clock::now());
  if (!at) {
    return;
  }
  const auto delay = std::max(
      std::chrono::duration_cast<std::chrono::milliseconds>(*at - MobilityClassifier::Clock::now()),
      std::chrono::milliseconds(1));
  timer_id_ = AppRuntime::ScheduleCoordinatorOneShot(delay, [this, alive = alive_]() {
    CallsThread::Post([this, alive]() {
      if (alive->load(std::memory_order_acquire)) {
        timer_id_ = 0;
        Reevaluate();
      }
    });
  });
}

void CallPathMobility::CancelReevaluation() {
  if (timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(timer_id_);
    timer_id_ = 0;
  }
}

void CallPathMobility::NoteRemote(const std::string& call_id, const MobilityClass mobility) {
  if (call_id.empty()) {
    return;
  }
  if (remote_.size() > kRemoteKept && !remote_.contains(call_id)) {
    remote_.clear();
  }
  auto [it, inserted] = remote_.try_emplace(call_id, mobility);
  if (!inserted && it->second == mobility) {
    return;
  }
  it->second = mobility;
  if (ports_.on_policy_changed) {
    ports_.on_policy_changed(call_id);
  }
}

CallPathPolicy CallPathMobility::PolicyFor(const std::string& call_id) const {
  const auto it = remote_.find(call_id);
  return DecideCallPathPolicy(local_.Class(), it == remote_.end() ? MobilityClass::Unknown : it->second);
}

} // namespace pbr
