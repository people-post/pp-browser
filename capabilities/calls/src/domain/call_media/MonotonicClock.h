#pragma once

#include "amp/L1/Clock.h"

#include <chrono>
#include <cstdint>

namespace pbr {

/**
 * A `pp::adp::Clock` on std::chrono::steady_clock: ms that only move forward, whatever the wall clock does
 * (an NTP step, a manual time change). The product's timer clock for the call-media legs — amp's own
 * `WallClock` reads system_clock. Its epoch is arbitrary: compare its times only with each other.
 */
class MonotonicClock final : public pp::adp::Clock {
public:
  int64_t NowMs() const override {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  }

  /** The process's one (stateless; lives as long as the program). */
  static const MonotonicClock& Process() {
    static const MonotonicClock clock;
    return clock;
  }
};

} // namespace pbr
