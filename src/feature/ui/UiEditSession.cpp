#include "feature/ui/UiEditSession.h"

namespace pbr {

UiEditSession& UiEditSession::Instance() {
  static UiEditSession session;
  return session;
}

void UiEditSession::BeginRemount() {
  ++remount_depth_;
  ++remount_epoch_;
}

void UiEditSession::EndRemount() {
  if (remount_depth_ > 0) {
    --remount_depth_;
  }
}

void UiEditSession::OnLoaded(const std::string& field_id, const std::string& value) {
  baselines_[field_id] = value;
}

bool UiEditSession::HasBaseline(const std::string& field_id) const {
  return baselines_.find(field_id) != baselines_.end();
}

const std::string* UiEditSession::Baseline(const std::string& field_id) const {
  const auto it = baselines_.find(field_id);
  return it == baselines_.end() ? nullptr : &it->second;
}

bool UiEditSession::IsMidEdit(const std::string& field_id, const std::string& live) const {
  // Remount/SetValue clears the binding to "" before we push loaded values. That must not
  // look like an intentional edit vs a prior baseline (or nickname stays blank and blur
  // commits an empty wipe).
  if (RemountBlocking()) {
    return false;
  }
  const std::string* baseline = Baseline(field_id);
  return baseline && live != *baseline;
}

bool UiEditSession::ShouldCommit(const std::string& field_id, const std::string& live) const {
  if (RemountBlocking()) {
    return false;
  }
  const std::string* baseline = Baseline(field_id);
  if (!baseline) {
    return false;
  }
  return live != *baseline;
}

bool UiEditSession::ShouldPushToView(const std::string& field_id, const std::string& live) const {
  return !IsMidEdit(field_id, live);
}

void UiEditSession::OnCommitted(const std::string& field_id, const std::string& value) {
  baselines_[field_id] = value;
}

std::string UiEditSession::ResolveAfterLoad(const std::string& field_id, const std::string& loaded,
                                            const std::string& live_binding) const {
  if (RemountBlocking()) {
    return loaded;
  }
  if (IsMidEdit(field_id, live_binding)) {
    return live_binding;
  }
  return loaded;
}

} // namespace pbr
