#include "common/chat/SystemControl.h"

#include <algorithm>
#include <utility>

namespace pbr {

void SystemControlRegistry::Bind(SystemControlFamily family) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto next = std::make_shared<Families>(*families_);
  auto same = std::find_if(next->begin(), next->end(),
                           [&](const SystemControlFamily& f) { return f.name == family.name; });
  if (same != next->end()) {
    *same = std::move(family);
  } else {
    next->push_back(std::move(family));
  }
  families_ = std::move(next);
}

void SystemControlRegistry::Unbind(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto next = std::make_shared<Families>(*families_);
  next->erase(std::remove_if(next->begin(), next->end(), [&](const SystemControlFamily& f) { return f.name == name; }),
              next->end());
  families_ = std::move(next);
}

std::shared_ptr<const SystemControlRegistry::Families> SystemControlRegistry::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return families_;
}

std::optional<SystemControlFamily> SystemControlRegistry::Find(const ThreadMessage& message) const {
  if (message.content_type != ChatContentType::System) {
    return std::nullopt;
  }
  const auto families = Snapshot();
  for (const auto& family : *families) {
    if (family.owns && family.owns(message)) {
      return family;
    }
  }
  return std::nullopt;
}

bool SystemControlRegistry::AppliesOutOfOrder(const ThreadMessage& message) const {
  const auto family = Find(message);
  return family && family->apply_out_of_order;
}

bool SystemControlRegistry::Hidden(const ThreadMessage& message) const {
  const auto family = Find(message);
  return family && family->hidden && family->hidden(message);
}

bool SystemControlRegistry::Quiet(const ThreadMessage& message) const {
  const auto family = Find(message);
  return family && family->quiet && family->quiet(message);
}

std::optional<std::string> SystemControlRegistry::LiveFamily() const {
  const auto families = Snapshot();
  for (const auto& family : *families) {
    if (family.live && family.live()) {
      return family.name;
    }
  }
  return std::nullopt;
}

std::optional<std::string> SystemControlRegistry::PreferredPublicThread() const {
  const auto families = Snapshot();
  for (const auto& family : *families) {
    if (family.preferred_public_thread) {
      if (auto thread = family.preferred_public_thread()) {
        return thread;
      }
    }
  }
  return std::nullopt;
}

} // namespace pbr
