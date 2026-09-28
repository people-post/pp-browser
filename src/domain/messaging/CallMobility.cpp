#include "domain/messaging/CallMobility.h"

#include <mutex>

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

std::optional<MobilityClass> ParseMobilityOverride(const std::string_view value) {
  if (value == "stationary" || value == "mobile" || value == "unknown") {
    return ParseMobilityClass(value);
  }
  return std::nullopt;
}

namespace {

std::mutex& CliMutex() {
  static std::mutex mu;
  return mu;
}

std::string& CliValue() {
  static std::string value;
  return value;
}

} // namespace

void SetMobilityCliOverride(std::string value) {
  std::lock_guard lock(CliMutex());
  CliValue() = std::move(value);
}

std::string MobilityCliOverride() {
  std::lock_guard lock(CliMutex());
  return CliValue();
}

std::optional<MobilityClass> ResolveMobilityOverride(const std::string& config_value) {
  const std::string cli = MobilityCliOverride();
  return ParseMobilityOverride(cli.empty() ? config_value : cli);
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
  if (!churn_.empty() && now - churn_.back() < kMobilityChurnDedupe) {
    return;  // one move: the network change and the new observed address it brings
  }
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
