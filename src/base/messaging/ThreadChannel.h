#pragma once

namespace pbr {

/** Direct P2P tier (D089). None for AI / non-E2E threads. */
enum class ThreadChannel { None, E2e, E2ePublic };

inline bool ThreadChannelIsE2e(const ThreadChannel channel) {
  return channel == ThreadChannel::E2e || channel == ThreadChannel::E2ePublic;
}

} // namespace pbr
