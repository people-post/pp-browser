#pragma once

#include <cstdint>
#include <vector>

namespace pbr {

/** One camera the OS lists, as the call engine sees it. */
struct CameraCandidate {
  uint32_t id = 0;
  bool front_facing = false;
  /** Another device's camera lent to this one (Apple Continuity Camera: a nearby iPhone). */
  bool borrowed = false;
};

/**
 * The order to try cameras in: this device's own cameras first (front-facing before the rest, OS
 * order otherwise), borrowed ones last. A Mac calling its owner's iPhone must not take that
 * iPhone's camera: the phone switched to "connected as camera", lost its own camera and audio
 * session, and the call never recovered (device test 2026-09-30).
 */
inline std::vector<uint32_t> OrderCameraCandidates(const std::vector<CameraCandidate>& cameras) {
  std::vector<uint32_t> out;
  out.reserve(cameras.size());
  for (const bool borrowed : {false, true}) {
    for (const bool front : {true, false}) {
      for (const CameraCandidate& c : cameras) {
        if (c.borrowed == borrowed && c.front_facing == front) {
          out.push_back(c.id);
        }
      }
    }
  }
  return out;
}

/** True when the OS camera with this display name is borrowed from another device. */
bool IsBorrowedCameraName(const char* name);

} // namespace pbr
