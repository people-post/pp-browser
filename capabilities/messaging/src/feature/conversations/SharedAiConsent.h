#pragma once

#include <functional>
#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Which chats the user already said "don't ask again" for when sharing them with the AI ("@ai+" / "@ai++"):
 * this session's ticks, plus whatever the stored per-chat setting says (the port).
 */
class SharedAiConsent {
public:
  using StoredPort = std::function<bool(const std::string& thread_id)>;

  void SetStored(StoredPort stored) { stored_ = std::move(stored); }
  void Remember(const std::string& thread_id) { session_.insert(thread_id); }
  void Forget(const std::string& thread_id) { session_.erase(thread_id); }
  bool Given(const std::string& thread_id) const {
    return session_.find(thread_id) != session_.end() || (stored_ && stored_(thread_id));
  }

private:
  StoredPort stored_;
  std::unordered_set<std::string> session_;
};

} // namespace pbr
