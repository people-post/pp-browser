#pragma once

#include <algorithm>
#include <cstdint>

namespace pbr {

/**
 * Simple token-bucket for media ↑/↓ budgets (N019 / V032).
 * rate_bps ≤ 0 means unbounded (always allow).
 */
class ByteRateLimiter {
public:
  ByteRateLimiter() = default;

  void Configure(int64_t rate_bps, int64_t burst_bytes = 0) {
    rate_bps_ = rate_bps;
    if (burst_bytes > 0) {
      burst_bytes_ = burst_bytes;
    } else if (rate_bps > 0) {
      // ~100 ms burst, minimum one Opus frame-ish (400 bytes).
      burst_bytes_ = std::max<int64_t>(400, rate_bps / 10);
    } else {
      burst_bytes_ = 0;
    }
    tokens_ = static_cast<double>(burst_bytes_);
    last_ms_ = 0;
    primed_ = false;
  }

  int64_t rate_bps() const { return rate_bps_; }

  /** Consume `bytes` at `now_ms`. Returns false when over budget (caller should drop). */
  bool TryConsume(int64_t bytes, int64_t now_ms) {
    if (rate_bps_ <= 0 || bytes <= 0) {
      return true;
    }
    if (!primed_) {
      primed_ = true;
      last_ms_ = now_ms;
      tokens_ = static_cast<double>(burst_bytes_);
    } else if (now_ms > last_ms_) {
      const double elapsed_s = static_cast<double>(now_ms - last_ms_) / 1000.0;
      tokens_ = std::min(static_cast<double>(burst_bytes_),
                         tokens_ + elapsed_s * static_cast<double>(rate_bps_) / 8.0);
      last_ms_ = now_ms;
    }
    if (tokens_ < static_cast<double>(bytes)) {
      return false;
    }
    tokens_ -= static_cast<double>(bytes);
    return true;
  }

private:
  int64_t rate_bps_ = 0;
  int64_t burst_bytes_ = 0;
  double tokens_ = 0;
  int64_t last_ms_ = 0;
  bool primed_ = false;
};

} // namespace pbr
