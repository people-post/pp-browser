#include "lib/calls/RecentCallsOver.h"

#include <algorithm>

namespace pbr {

void RecentCallsOver::Note(const std::string& call_id) {
  if (call_id.empty()) {
    return;
  }
  ids.push_back(call_id);
  if (ids.size() > kKept) {
    ids.erase(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(ids.size() - kKept));
  }
  ++seq;
}

std::vector<std::string> RecentCallsOver::NewSince(const uint64_t seen) const {
  if (seen >= seq) {
    return {};
  }
  const std::size_t fresh = static_cast<std::size_t>(std::min<uint64_t>(seq - seen, ids.size()));
  return {ids.end() - static_cast<std::ptrdiff_t>(fresh), ids.end()};
}

} // namespace pbr
