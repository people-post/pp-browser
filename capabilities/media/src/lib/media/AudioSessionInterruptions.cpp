#include "lib/media/AudioSession.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace pbr {
namespace AudioSession {
namespace {

struct InterruptionHandlers {
  std::mutex mu;
  InterruptionHandlerId next_id = 1;
  std::vector<std::pair<InterruptionHandlerId, std::function<void()>>> handlers;
};

InterruptionHandlers& Registry() {
  static InterruptionHandlers registry;
  return registry;
}

std::atomic<bool> g_interrupted{false};

} // namespace

InterruptionHandlerId AddInterruptionEndedHandler(std::function<void()> fn) {
  if (!fn) {
    return 0;
  }
  InterruptionHandlers& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mu);
  const InterruptionHandlerId id = registry.next_id++;
  registry.handlers.emplace_back(id, std::move(fn));
  return id;
}

void RemoveInterruptionEndedHandler(const InterruptionHandlerId id) {
  if (id == 0) {
    return;
  }
  InterruptionHandlers& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mu); // waits for a running NotifyInterruptionEnded
  std::erase_if(registry.handlers, [id](const auto& entry) { return entry.first == id; });
}

void NotifyInterruptionEnded() {
  g_interrupted.store(false);
  InterruptionHandlers& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mu);
  for (const auto& [id, fn] : registry.handlers) {
    fn();
  }
}

void NotifyInterruptionBegan() {
  g_interrupted.store(true);
}

bool IsInterrupted() {
  return g_interrupted.load();
}

void ClearInterruption() {
  g_interrupted.store(false);
}

} // namespace AudioSession
} // namespace pbr
