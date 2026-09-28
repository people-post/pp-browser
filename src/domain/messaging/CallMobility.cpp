#include "domain/messaging/CallMobility.h"

namespace pbr {

const char* MobilityClassWire(const MobilityClass mobility) {
  switch (mobility) {
  case MobilityClass::Stationary:
    return "stationary";
  case MobilityClass::Mobile:
    return "mobile";
  case MobilityClass::Unknown:
    break;
  }
  return "unknown";
}

MobilityClass ParseMobilityClass(const std::string_view wire) {
  if (wire == "stationary") {
    return MobilityClass::Stationary;
  }
  if (wire == "mobile") {
    return MobilityClass::Mobile;
  }
  return MobilityClass::Unknown;
}

void MobilityClassifier::OnAttachment(const MobilityAttachment& attachment, const bool changed,
                                      const Clock::time_point now) {
  attachment_ = attachment;
  if (changed) {
    NoteChurn(now);
  }
}

void MobilityClassifier::OnObservedAddressChanged(const Clock::time_point now) { NoteChurn(now); }

void MobilityClassifier::SetOverride(std::optional<MobilityClass> pinned) { override_ = pinned; }

void MobilityClassifier::NoteChurn(const Clock::time_point now) {
  churn_.push_back(now);
  DropOldChurn(now);
}

void MobilityClassifier::DropOldChurn(const Clock::time_point now) {
  while (!churn_.empty() && now - churn_.front() > kMobilityChurnWindow) {
    churn_.pop_front();
  }
}

MobilityClass MobilityClassifier::Evaluate(const Clock::time_point now) {
  DropOldChurn(now);
  if (override_) {
    class_ = *override_;
    return class_;
  }
  if (!attachment_) {
    class_ = MobilityClass::Unknown;
    return class_;
  }
  if (!attachment_->online) {
    return class_;  // offline says nothing about the next attachment: keep what we had
  }
  const bool primary_mobile = attachment_->cellular || attachment_->expensive;
  if (primary_mobile || churn_.size() >= kMobilityChurnToMobile) {
    class_ = MobilityClass::Mobile;
    return class_;
  }
  if (class_ == MobilityClass::Mobile) {
    // Hysteresis: a churning endpoint stays Mobile until it has calmed down.
    const bool calm = churn_.size() <= 1 && (churn_.empty() || now - churn_.back() >= kMobilityCalmBeforeStationary);
    if (!calm) {
      return class_;
    }
  }
  class_ = MobilityClass::Stationary;
  return class_;
}

} // namespace pbr
