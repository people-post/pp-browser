#pragma once

#include <string>

namespace pbr {

/** Direct P2P tier (D089). None for AI / non-E2E threads. */
enum class ThreadChannel { None, E2e, E2ePublic };

inline bool ThreadChannelIsE2e(const ThreadChannel channel) {
  return channel == ThreadChannel::E2e || channel == ThreadChannel::E2ePublic;
}

inline std::string ThreadChannelToString(const ThreadChannel channel) {
  switch (channel) {
  case ThreadChannel::E2e:
    return "e2e";
  case ThreadChannel::E2ePublic:
    return "e2e_public";
  case ThreadChannel::None:
    return "";
  }
  return "";
}

inline ThreadChannel ThreadChannelFromString(const std::string& value) {
  if (value == "e2e_public") {
    return ThreadChannel::E2ePublic;
  }
  if (value == "e2e") {
    return ThreadChannel::E2e;
  }
  return ThreadChannel::None;
}

} // namespace pbr
