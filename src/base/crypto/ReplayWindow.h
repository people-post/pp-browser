#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <unordered_set>

namespace pbr {

/** Out-of-order seq acceptance helper (D020) — classifier is authoritative. */
class ReplayWindow {
public:
  explicit ReplayWindow(size_t window_size = 32);

  bool Accept(uint64_t seq);
  uint64_t LastContiguous() const { return last_contiguous_; }

private:
  size_t window_size_;
  uint64_t last_contiguous_ = 0;
  std::unordered_set<uint64_t> pending_;
};

} // namespace pbr
