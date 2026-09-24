#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace pbr {

/**
 * Owner-lifetime gate for async work that captures raw `this`.
 *
 * DeferredSelf makes *queued* work no-op after Invalidate; it cannot help a task that is already
 * running when the owner is destroyed (dogfood 2026-09-24: relay Send inside curl on a worker while
 * ConversationsHub::Shutdown freed the orchestrator → heap corruption). TaskGate covers both:
 * guarded callables no-op once closed, and CloseAndWait blocks until running ones return.
 *
 * Rules: wrap every async entry point (Post*, completion callbacks) that touches the owner;
 * synchronous nested calls inside a guarded task need no extra guard. Do not CloseAndWait from
 * inside a guarded task (it would wait for itself until the budget expires).
 */
class TaskGate {
public:
  TaskGate() : state_(std::make_shared<State>()) {}

  /** Wrap `fn`: runs only while the gate is open, counted as running until it returns. */
  template <typename F>
  auto Guard(F&& fn) const {
    auto state = state_;
    auto held = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
    return [state, held](auto&&... args) {
      if (!state->Enter()) {
        return;
      }
      struct Leave {
        State& state;
        ~Leave() { state.Leave(); }
      } leave{*state};
      std::invoke(*held, std::forward<decltype(args)>(args)...);
    };
  }

  /**
   * Close (no new guarded work starts) and wait up to `budget` for running work to finish.
   * Returns false if work is still running — the owner must then outlive it (leak at shutdown).
   */
  bool CloseAndWait(const std::chrono::milliseconds budget) {
    std::unique_lock lock(state_->mu);
    state_->closed = true;
    return state_->cv.wait_for(lock, budget, [this] { return state_->running == 0; });
  }

  bool IsClosed() const {
    std::lock_guard lock(state_->mu);
    return state_->closed;
  }

private:
  struct State {
    std::mutex mu;
    std::condition_variable cv;
    bool closed = false;
    int running = 0;

    bool Enter() {
      std::lock_guard lock(mu);
      if (closed) {
        return false;
      }
      ++running;
      return true;
    }

    void Leave() {
      std::lock_guard lock(mu);
      --running;
      cv.notify_all();
    }
  };

  std::shared_ptr<State> state_;
};

} // namespace pbr
