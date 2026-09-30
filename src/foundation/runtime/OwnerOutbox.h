#pragma once

#include "foundation/runtime/OwnerTasks.h"

#include <chrono>
#include <functional>
#include <memory>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A child's way to report to its parent: events of the child's own type, handed up and wrapped by
 * each parent into its own, until they reach the runner's queue on its owner thread (THREADING.md § Owner runners).
 * `Emit` may be called from any thread (a result arriving from I/O or a worker); `After` is how a
 * passive component gets a timer. An unbound outbox drops everything (a component with no parent).
 */
template <typename Event>
class OwnerOutbox {
public:
  struct Sink {
    std::function<void(Event)> emit;
    std::function<OwnerExecutor::TimerId(std::chrono::milliseconds, Event)> after;
    std::function<void(OwnerExecutor::TimerId)> cancel;
  };

  OwnerOutbox() = default;
  explicit OwnerOutbox(Sink sink) : sink_(std::make_shared<const Sink>(std::move(sink))) {}

  bool IsBound() const { return sink_ && sink_->emit; }
  void Emit(Event event) const {
    if (IsBound()) {
      sink_->emit(std::move(event));
    }
  }
  /** `event` back to this component after `delay`; 0 when not scheduled. */
  OwnerExecutor::TimerId After(const std::chrono::milliseconds delay, Event event) const {
    return sink_ && sink_->after ? sink_->after(delay, std::move(event)) : 0;
  }
  /** Cancel `id` (0: no-op) and reset it. A stale delayed event is also dropped by its receiver. */
  void Cancel(OwnerExecutor::TimerId& id) const {
    if (id != 0 && sink_ && sink_->cancel) {
      sink_->cancel(id);
    }
    id = 0;
  }

  /** A child's outbox: its events wrapped into this one's by `wrap`. */
  template <typename Child>
  OwnerOutbox<Child> For(std::function<Event(Child)> wrap) const {
    if (!sink_) {
      return {};
    }
    auto parent = sink_;
    typename OwnerOutbox<Child>::Sink child;
    child.emit = [parent, wrap](Child event) { parent->emit(wrap(std::move(event))); };
    child.after = [parent, wrap](std::chrono::milliseconds delay, Child event) {
      return parent->after ? parent->after(delay, wrap(std::move(event))) : OwnerExecutor::TimerId{0};
    };
    child.cancel = [parent](OwnerExecutor::TimerId id) {
      if (parent->cancel) {
        parent->cancel(id);
      }
    };
    return OwnerOutbox<Child>(std::move(child));
  }

private:
  std::shared_ptr<const Sink> sink_;
};

/**
 * A runner's outbox for one child: events come back to `handle` on the owner, posted / delayed
 * through `tasks` (dropped with it). `tasks` must outlive the child's use of the outbox.
 */
template <typename Event>
OwnerOutbox<Event> MakeOwnerOutbox(OwnerTasks& tasks, std::function<void(Event&)> handle) {
  auto run = std::make_shared<const std::function<void(Event&)>>(std::move(handle));
  typename OwnerOutbox<Event>::Sink sink;
  sink.emit = [&tasks, run](Event event) {
    tasks.Post([run, event = std::make_shared<Event>(std::move(event))]() { (*run)(*event); });
  };
  sink.after = [&tasks, run](std::chrono::milliseconds delay, Event event) {
    return tasks.After(delay, [run, event = std::make_shared<Event>(std::move(event))]() { (*run)(*event); });
  };
  sink.cancel = [&tasks](OwnerExecutor::TimerId id) { tasks.Cancel(id); };
  return OwnerOutbox<Event>(std::move(sink));
}

} // namespace pbr
