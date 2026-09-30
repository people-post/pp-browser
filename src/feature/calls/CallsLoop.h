#pragma once

#include "feature/calls/CallStackEvents.h"
#include "feature/calls/CallsExecutor.h"

#include <chrono>
#include <functional>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The calls owner's event queue. Edge adapters enqueue from any thread; the handler (CallStack's
 * dispatch) runs each event on the owner, in order. Events enqueued while one is handled run after
 * it (no re-entrancy). Delayed events are the only timers. Everything still queued or scheduled is
 * dropped with the loop.
 */
class CallsLoop {
public:
  using Handler = std::function<void(CallStackEvent&)>;
  using Clock = std::chrono::steady_clock;

  CallsLoop(CallsExecutor& executor, Handler handler) : tasks_(executor), handler_(std::move(handler)) {}

  /** From any thread. */
  void Enqueue(CallStackEvent event);
  /** Ahead of queued events — only for an ordering the owner documents at the call site. */
  void EnqueueFront(CallStackEvent event);
  /** `event` after `delay` (owner); 0 when not scheduled. */
  CallsExecutor::TimerId After(std::chrono::milliseconds delay, CallStackEvent event);
  void Cancel(CallsExecutor::TimerId& id) { tasks_.Cancel(id); }
  /** Drop what is queued or scheduled (teardown); later events run normally. */
  void DropPending() { tasks_.DropPending(); }

private:
  void Handle(CallStackEvent& event);

  CallsTasks tasks_;
  Handler handler_;
};

/**
 * The owner's timer for one passive component: a delayed event at the deadline the component reports
 * after each event (its `NextWakeAt`). Re-arming at the same deadline keeps it; nullopt disarms.
 */
class CallsWakeSlot {
public:
  using Clock = CallsLoop::Clock;

  CallsWakeSlot(CallsLoop& loop, CallStackEvent wake) : loop_(loop), wake_(std::move(wake)) {}
  ~CallsWakeSlot() { Disarm(); }
  CallsWakeSlot(const CallsWakeSlot&) = delete;
  CallsWakeSlot& operator=(const CallsWakeSlot&) = delete;

  void ArmAt(std::optional<Clock::time_point> at);
  void Disarm();
  /** The owner handles the wake event: the slot is free again. */
  void Fired();

private:
  CallsLoop& loop_;
  CallStackEvent wake_;
  CallsExecutor::TimerId timer_ = 0;
  std::optional<Clock::time_point> at_;
};

} // namespace pbr
