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
  /**
   * Everything up to `seq` is already in (the persisted state says so): the window moves there. A
   * window lives in memory; after a restart it starts at 0 while the thread's state does not (PB-064).
   */
  void CatchUpTo(uint64_t seq);

private:
  size_t window_size_;
  uint64_t last_contiguous_ = 0;
  std::unordered_set<uint64_t> pending_;
};

} // namespace pbr
