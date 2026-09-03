#pragma once

#include "common/Error.h"
#include "common/ResultOrError.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * One-shot ResultOrError waiter for RPCs that hop off the io thread.
 * Copies share state so OpenChannel / worker callbacks can Finish without UAF
 * if Wait times out first. Finish is CAS-once (timeout vs late callback).
 *
 * Default `E = Error` preserves existing `SettledWait<T>` call sites.
 * L4 coded paths use `SettledWait<T, Module::Failure>`.
 */
template <typename T, typename E = Error>
class SettledWait {
public:
  using RoeT = pp::ResultOrError<T, E>;

  SettledWait() : state_(std::make_shared<State>()) {}

  bool Finish(RoeT value) const {
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

  RoeT Wait(std::chrono::milliseconds timeout, E timed_out) {
    if (state_->future.wait_for(timeout) != std::future_status::ready) {
      Finish(RoeT(std::move(timed_out)));
    }
    return state_->future.get();
  }

private:
  struct State {
    std::promise<RoeT> promise;
    std::atomic<bool> settled{false};
    std::future<RoeT> future;

    State() : future(promise.get_future()) {}
  };

  std::shared_ptr<State> state_;
};

} // namespace pbr
