#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A component's next steps waiting for their event (THREADING.md § Calls owner): owner-only
 * continuations keyed by id. The component stores a step, reports `Continue{id}` (or a result event
 * carrying the id) through its outbox, and runs the step when that event comes back. A step never
 * crosses a thread; an id with no step (already run, or the component rebuilt) is a no-op. Owner only.
 */
class CallsSteps {
public:
  uint64_t Store(std::function<void()> step) {
    const uint64_t id = ++next_;
    steps_.emplace(id, std::move(step));
    return id;
  }
  /** Take the step for `id` out (empty when none). */
  std::function<void()> Take(const uint64_t id) {
    auto it = steps_.find(id);
    if (it == steps_.end()) {
      return {};
    }
    auto step = std::move(it->second);
    steps_.erase(it);
    return step;
  }
  void Run(const uint64_t id) {
    if (auto step = Take(id)) {
      step();
    }
  }
  size_t Pending() const { return steps_.size(); }

private:
  uint64_t next_ = 0;
  std::unordered_map<uint64_t, std::function<void()>> steps_;
};

} // namespace pbr
