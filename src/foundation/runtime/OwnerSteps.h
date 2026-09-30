#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A component's next steps waiting for their event (THREADING.md § Owner runners): owner-only
 * continuations keyed by id. The component stores a step, reports `Continue{id}` — or, for a result
 * arriving from another thread, `Continue{id, value}` — through its outbox, and runs the step when
 * that event comes back. A step never crosses a thread; only the value does. An id with no step
 * (already run, or the component rebuilt) is a no-op. Owner only (Store / Run).
 */
class OwnerSteps {
public:
  /** A step with no result. */
  uint64_t Store(std::function<void()> step) {
    return Keep([step = std::move(step)](std::any*) { step(); });
  }
  /** A step taking the `T` its event carries. */
  template <typename T>
  uint64_t StoreFor(std::function<void(T)> step) {
    return Keep([step = std::move(step)](std::any* value) {
      if (value && value->has_value()) {
        step(std::any_cast<T>(std::move(*value)));
      }
    });
  }
  /** Run the step for `id` (no-op when none), with the event's value if it carries one. */
  void Run(const uint64_t id, std::any* value = nullptr) {
    auto it = steps_.find(id);
    if (it == steps_.end()) {
      return;
    }
    auto step = std::move(it->second);
    steps_.erase(it);
    step(value);
  }
  /** Drop a step whose result will never matter (the flow was cancelled). */
  void Drop(const uint64_t id) { steps_.erase(id); }
  /** Drop every waiting step (the component is being reset). */
  void DropAll() { steps_.clear(); }
  size_t Pending() const { return steps_.size(); }

private:
  uint64_t Keep(std::function<void(std::any*)> step) {
    const uint64_t id = ++next_;
    steps_.emplace(id, std::move(step));
    return id;
  }

  uint64_t next_ = 0;
  std::unordered_map<uint64_t, std::function<void(std::any*)>> steps_;
};

/** The event half of a step: its id, and the value a result brings (copied across threads as data). */
struct OwnerStepReady {
  uint64_t id = 0;
  std::shared_ptr<std::any> value;
};

} // namespace pbr
