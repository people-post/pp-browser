/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>
#include <string_view>

namespace libp2p::common {

  /**
   * Split @p str on @p delim into @p out (cleared first).
   * Matches boost::algorithm::split(..., is_any_of(delim)) for a single-char
   * delimiter, including empty tokens for consecutive delimiters.
   */
  template <typename StringContainer>
  void split(StringContainer &out, std::string_view str, char delim) {
    out.clear();
    std::size_t start = 0;
    while (start <= str.size()) {
      const auto pos = str.find(delim, start);
      if (pos == std::string_view::npos) {
        out.emplace_back(str.substr(start));
        break;
      }
      out.emplace_back(str.substr(start, pos - start));
      start = pos + 1;
    }
  }

}  // namespace libp2p::common
