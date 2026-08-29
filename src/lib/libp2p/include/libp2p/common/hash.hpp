/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <functional>

namespace libp2p::common {

  /// Boost.hash_combine-compatible mix (replaces boost::hash_combine).
  inline void hashCombine(std::size_t &seed, std::size_t value) noexcept {
    seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
  }

  template <typename T>
  void hashCombine(std::size_t &seed, const T &value) {
    hashCombine(seed, std::hash<T>{}(value));
  }

  template <typename Iterator>
  std::size_t hashRange(Iterator first, Iterator last) {
    std::size_t seed = 0;
    for (; first != last; ++first) {
      hashCombine(seed, *first);
    }
    return seed;
  }

}  // namespace libp2p::common
