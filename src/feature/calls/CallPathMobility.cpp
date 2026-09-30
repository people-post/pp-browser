#include "feature/calls/CallPathMobility.h"


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

bool CallPathMobility::OnAttachment(const MobilityAttachment& attachment, const bool changed,
                                    const Clock::time_point now) {
  local_.OnAttachment(attachment, changed, now);
  return Reevaluate(now);
}

bool CallPathMobility::OnObservedAddressChanged(const Clock::time_point now) {
  local_.OnObservedAddressChanged(now);
  return Reevaluate(now);
}

bool CallPathMobility::SetOverride(const std::optional<MobilityClass> pinned, const Clock::time_point now) {
  local_.SetOverride(pinned);
  if (pinned) {
    log().info << "mobility pinned to " << MobilityClassWire(*pinned);
  }
  return Reevaluate(now);
}

bool CallPathMobility::Reevaluate(const Clock::time_point now) {
  const MobilityClass before = published_.load(std::memory_order_acquire);
  const MobilityClass after = local_.Evaluate(now);
  published_.store(after, std::memory_order_release);
  if (after == before) {
    return false;
  }
  log().info << "mobility " << MobilityClassWire(before) << " -> " << MobilityClassWire(after);
  return true;
}

bool CallPathMobility::NoteRemote(const std::string& call_id, const MobilityClass mobility) {
  if (call_id.empty()) {
    return false;
  }
  if (remote_.size() > kRemoteKept && !remote_.contains(call_id)) {
    remote_.clear();
  }
  auto [it, inserted] = remote_.try_emplace(call_id, mobility);
  if (!inserted && it->second == mobility) {
    return false;
  }
  it->second = mobility;
  return true;
}

CallPathPolicy CallPathMobility::PolicyFor(const std::string& call_id) const {
  const auto it = remote_.find(call_id);
  return DecideCallPathPolicy(local_.Class(), it == remote_.end() ? MobilityClass::Unknown : it->second);
}

} // namespace pbr
