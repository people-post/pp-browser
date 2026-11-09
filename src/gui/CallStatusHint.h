#pragma once

#include <vector>

namespace pbr {

/**
 * Locale keys for the hint under "Couldn't connect" in a 1:1 call, in display order. Only causes we
 * have evidence for name a cause: our own seeds unreachable (a VPN or firewall dropping UDP) and a
 * blocked microphone. Without either there is no hint line: the failed page already says "Couldn't reach them" (#181), and
 * a neutral second line only repeated it (dogfood 2026-10-01 asked not to send the user into settings).
 */
inline std::vector<const char*> P2pStatusHintKeys(const bool missing_mic, const bool seed_unreachable) {
  std::vector<const char*> keys;
  if (seed_unreachable) {
    keys.push_back("call.hint.seed_unreachable");
  }
  if (missing_mic) {
    keys.push_back("hints.mic_blocked");
  }
  return keys;
}

} // namespace pbr
