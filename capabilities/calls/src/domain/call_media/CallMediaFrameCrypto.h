#pragma once

#include <string>

namespace pbr {

/**
 * The seal contexts of a call's media frames (pp-cpp-mesh MediaFrame, A029 H2): through a hop's fan-out, and on the
 * 1:1 path. A frame sealed for one never opens under the other, or under another call (or a broadcast, whose context
 * is BroadcastMediaFrameContext).
 */
inline std::string CallMediaSfuContext(const std::string& call_id) {
  return "call-media-sfu|" + call_id;
}
inline std::string CallMediaDirectContext(const std::string& call_id) {
  return "call-media|" + call_id;
}

} // namespace pbr
