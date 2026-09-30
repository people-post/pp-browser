#pragma once

#include "feature/calls/CallsExecutor.h"

#include <chrono>
#include <functional>
#include <memory>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A child's way to report to its parent: events of the child's own type, handed up and wrapped by
 * each parent into its own, until they reach the calls owner's queue (THREADING.md § Calls owner).
 * `Emit` may be called from any thread (a result arriving from I/O or a worker); `After` is how a
 * passive component gets a timer. An unbound outbox drops everything (a component with no parent).
 */
template <typename Event>
class CallsOutbox {
public:
  struct Sink {
    std::function<void(Event)> emit;
    std::function<CallsExecutor::TimerId(std::chrono::milliseconds, Event)> after;
    std::function<void(CallsExecutor::TimerId)> cancel;
  };

  CallsOutbox() = default;
  explicit CallsOutbox(Sink sink) : sink_(std::make_shared<const Sink>(std::move(sink))) {}

  bool IsBound() const { return sink_ && sink_->emit; }
  void Emit(Event event) const {
    if (IsBound()) {
      sink_->emit(std::move(event));
    }
  }
  /** `event` back to this component after `delay`; 0 when not scheduled. */
  CallsExecutor::TimerId After(const std::chrono::milliseconds delay, Event event) const {
    return sink_ && sink_->after ? sink_->after(delay, std::move(event)) : 0;
  }
  /** Cancel `id` (0: no-op) and reset it. A stale delayed event is also dropped by its receiver. */
  void Cancel(CallsExecutor::TimerId& id) const {
    if (id != 0 && sink_ && sink_->cancel) {
      sink_->cancel(id);
    }
    id = 0;
  }

  /** A child's outbox: its events wrapped into this one's by `wrap`. */
  template <typename Child>
  CallsOutbox<Child> For(std::function<Event(Child)> wrap) const {
    if (!sink_) {
      return {};
    }
    auto parent = sink_;
    typename CallsOutbox<Child>::Sink child;
    child.emit = [parent, wrap](Child event) { parent->emit(wrap(std::move(event))); };
    child.after = [parent, wrap](std::chrono::milliseconds delay, Child event) {
      return parent->after ? parent->after(delay, wrap(std::move(event))) : CallsExecutor::TimerId{0};
    };
    child.cancel = [parent](CallsExecutor::TimerId id) {
      if (parent->cancel) {
        parent->cancel(id);
      }
    };
    return CallsOutbox<Child>(std::move(child));
  }

private:
  std::shared_ptr<const Sink> sink_;
};

} // namespace pbr
