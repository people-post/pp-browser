#pragma once

#include "common/Error.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace pbr {

/**
 * One-shot Roe waiter for stream RPCs that hop off the libp2p io thread.
 * Copies share state so OpenStream / worker callbacks can Finish without UAF
 * if Wait times out first. Finish is CAS-once (timeout vs late callback).
 */
template <typename T>
class SettledWait {
public:
  SettledWait() : state_(std::make_shared<State>()) {}

  bool Finish(Roe<T> value) const {
    if (!state_->settled.exchange(true, std::memory_order_acq_rel)) {
      try {
        state_->promise.set_value(std::move(value));
      } catch (const std::future_error&) {
      }
      return true;
    }
    return false;
  }

  bool IsSettled() const { return state_->settled.load(std::memory_order_acquire); }

  /** True when both handles share the same waiter (circuit-relay inflight table). */
  bool SameAs(const SettledWait& other) const { return state_ == other.state_; }

  Roe<T> Wait(std::chrono::milliseconds timeout, Error timed_out) {
    if (state_->future.wait_for(timeout) != std::future_status::ready) {
      Finish(std::move(timed_out));
    }
    return state_->future.get();
  }

private:
  struct State {
    std::promise<Roe<T>> promise;
    std::atomic<bool> settled{false};
    std::future<Roe<T>> future;

    State() : future(promise.get_future()) {}
  };

  std::shared_ptr<State> state_;
};

} // namespace pbr
