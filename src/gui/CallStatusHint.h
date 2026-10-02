#pragma once

#include <vector>

namespace pbr {

/**
 * Locale keys for the hint under "Couldn't connect" in a 1:1 call, in display order. Only causes we
 * have evidence for name a cause: our own seeds unreachable (a VPN or firewall dropping UDP) and a
 * blocked microphone. Without either, the failure is most likely on the other side, so the line stays
 * neutral instead of sending the user into network or permission settings (dogfood 2026-10-01).
 */
inline std::vector<const char*> P2pStatusHintKeys(const bool missing_mic, const bool seed_unreachable) {
  std::vector<const char*> keys;
  if (seed_unreachable) {
    keys.push_back("call.hint.seed_unreachable");
  }
  if (missing_mic) {
    keys.push_back("hints.mic_blocked");
  }
  if (keys.empty()) {
    keys.push_back("call.hint.peer_unreachable");
  }
  return keys;
}

} // namespace pbr
