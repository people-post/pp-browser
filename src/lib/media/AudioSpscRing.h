#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pbr {

/**
 * Lock-free single-producer / single-consumer ring of int16 samples. Used between Apple
 * VoiceProcessingIO real-time callbacks and the call engine's capture / playout threads:
 * no locks, no allocation after construction. Write() drops what does not fit.
 */
class AudioSpscRing {
public:
  explicit AudioSpscRing(size_t capacity_samples) : buf_(capacity_samples) {}

  size_t Write(const int16_t* src, size_t n) {
    const size_t w = write_pos_.load(std::memory_order_relaxed);
    const size_t r = read_pos_.load(std::memory_order_acquire);
    n = std::min(n, buf_.size() - (w - r));
    CopyIn(w, src, n);
    write_pos_.store(w + n, std::memory_order_release);
    return n;
  }

  size_t Read(int16_t* dst, size_t n) {
    const size_t r = read_pos_.load(std::memory_order_relaxed);
    const size_t w = write_pos_.load(std::memory_order_acquire);
    n = std::min(n, w - r);
    CopyOut(r, dst, n);
    read_pos_.store(r + n, std::memory_order_release);
    return n;
  }

  size_t Size() const {
    const size_t r = read_pos_.load(std::memory_order_acquire);
    const size_t w = write_pos_.load(std::memory_order_acquire);
    return w - r;
  }

  size_t Capacity() const { return buf_.size(); }

  /** Only when neither side is running (e.g. before the AudioUnit starts). */
  void Reset() {
    read_pos_.store(0, std::memory_order_relaxed);
    write_pos_.store(0, std::memory_order_relaxed);
  }

private:
  void CopyIn(size_t pos, const int16_t* src, size_t n) {
    const size_t at = pos % buf_.size();
    const size_t first = std::min(n, buf_.size() - at);
    std::memcpy(buf_.data() + at, src, first * sizeof(int16_t));
    std::memcpy(buf_.data(), src + first, (n - first) * sizeof(int16_t));
  }

  void CopyOut(size_t pos, int16_t* dst, size_t n) const {
    const size_t at = pos % buf_.size();
    const size_t first = std::min(n, buf_.size() - at);
    std::memcpy(dst, buf_.data() + at, first * sizeof(int16_t));
    std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(int16_t));
  }

  std::vector<int16_t> buf_;
  std::atomic<size_t> read_pos_{0};   // total samples ever read (monotonic)
  std::atomic<size_t> write_pos_{0};  // total samples ever written (monotonic)
};

}  // namespace pbr
