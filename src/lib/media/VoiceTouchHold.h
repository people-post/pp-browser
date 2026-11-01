#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pbr {

/**
 * Who is holding the microphone button on a touch screen. A finger that is lifted outside the window, or
 * while the layout moves under it, may never produce a mouse-up, so on phones the real touch state is read
 * on every tick: the finger that pressed must still be down. Another finger touching and lifting changes
 * nothing. Where the press did not come from a touch (a mouse on a tablet, an emulator) there is nothing to
 * read and the mouse-up is what ends the hold.
 */
class VoiceTouchHold {
public:
  enum class Verdict { Holding, Lost };

  /** The button was pressed; `fingers` are the touch fingers down now (their ids), possibly none. */
  void Press(const std::vector<uint64_t>& fingers) {
    holding_ = false;
    ticks_ = 0;
    if (!fingers.empty()) {
      holding_ = true;
      finger_ = fingers.back(); // the newest one
    }
  }

  /**
   * One look at the touch state. The press can be seen a moment before SDL has listed its finger, so for the
   * first few ticks a finger that appears is taken as the one holding; later ones are not (another finger).
   */
  Verdict Tick(const std::vector<uint64_t>& fingers) {
    ++ticks_;
    if (!holding_) {
      if (ticks_ <= kAdoptTicks && !fingers.empty()) {
        holding_ = true;
        finger_ = fingers.front();
      }
      return Verdict::Holding;
    }
    return std::find(fingers.begin(), fingers.end(), finger_) == fingers.end() ? Verdict::Lost : Verdict::Holding;
  }

  /** The hold is a touch hold (a finger is being followed). */
  bool FollowsFinger() const { return holding_; }
  /** Whether the finger that pressed is still down (true when no finger is followed: the mouse says). */
  bool StillDown(const std::vector<uint64_t>& fingers) const {
    return !holding_ || std::find(fingers.begin(), fingers.end(), finger_) != fingers.end();
  }

private:
  static constexpr int kAdoptTicks = 5;
  bool holding_ = false;
  uint64_t finger_ = 0;
  int ticks_ = 0;
};

} // namespace pbr
