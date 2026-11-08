#pragma once

#include <algorithm>
#include <cmath>

namespace pbr {

/**
 * #166 (Kenneth 2026-11-08): where the minimised call chip rests after a drag, as the phone AI dot does — the nearer
 * side, the height left where it was dropped. Pure geometry in dp, relative to the chip's layer (the call mount). The
 * height is kept as a share of the band the chip may rest in, so it stays in the band when the window or the chip
 * changes size. The band matches the corner classes the chip had before (components.rcss
 * .shell-call-minimized-chip--tr / --br: 12dp from the top, 72dp from the bottom, safe areas already outside the
 * layer): a light drag from the default top-right corner does not move it down. Session only (CallController).
 */
struct CallChipArea {
  float width_dp = 0.f;
  float height_dp = 0.f;
};

/** The chip's box: top-left corner and size. */
struct CallChipRect {
  float left_dp = 0.f;
  float top_dp = 0.f;
  float width_dp = 0.f;
  float height_dp = 0.f;
};

struct CallChipPlace {
  /** kSideDefault: never dragged (the --tr corner class), kSideLeft, kSideRight. */
  int side = 0;
  /** 0 (top of the band) .. 1000 (bottom); ignored for kSideDefault. */
  int y_permille = 0;
  bool operator==(const CallChipPlace& other) const = default;
};

struct CallChipPoint {
  float left_dp = 0.f;
  float top_dp = 0.f;
};

/**
 * Where a dropped chip was last put on screen (ShellCallChromeGesture: at the drop, at the end of its slide, at each
 * correction) and the heights that was worked out with — kept so the chip's markup puts a rebuilt chip exactly there
 * (ShellCallChromeRml). 0: not known yet.
 */
struct CallChipLayout {
  CallChipPlace place;
  float top_dp = 0.f;
  float area_height_dp = 0.f;
  float chip_height_dp = 0.f;
};

class CallChipDock {
public:
  static constexpr int kSideDefault = 0;
  static constexpr int kSideLeft = 1;
  static constexpr int kSideRight = 2;
  /** The gap to the side it rests on, and the band's ends (the old corner classes' 12dp / 72dp). */
  static constexpr float kEdgeDp = 12.f;
  static constexpr float kTopDp = 12.f;
  static constexpr float kBottomDp = 72.f;

  static float MinTop() { return kTopDp; }
  static float MaxTop(const CallChipArea& area, const float chip_height_dp) {
    return std::max(MinTop(), area.height_dp - kBottomDp - chip_height_dp);
  }

  /** A chip let go at `chip`: the side its centre is nearer, its top clamped into the band as a share of it. */
  static CallChipPlace Release(const CallChipRect& chip, const CallChipArea& area) {
    CallChipPlace place;
    place.side = chip.left_dp + chip.width_dp * 0.5f < area.width_dp * 0.5f ? kSideLeft : kSideRight;
    const float min_top = MinTop();
    const float span = MaxTop(area, chip.height_dp) - min_top;
    if (span > 0.f) {
      const float top = std::clamp(chip.top_dp, min_top, min_top + span);
      place.y_permille = std::clamp(static_cast<int>(std::lround((top - min_top) / span * 1000.f)), 0, 1000);
    }
    return place;
  }

  /** Where a chip of `chip_width_dp` × `chip_height_dp` rests at `place` in `area` now (top-left corner). */
  static CallChipPoint Place(const CallChipPlace& place, const CallChipArea& area, const float chip_width_dp,
                             const float chip_height_dp) {
    const float min_top = MinTop();
    const float span = MaxTop(area, chip_height_dp) - min_top;
    const float permille = static_cast<float>(std::clamp(place.y_permille, 0, 1000));
    CallChipPoint point;
    point.top_dp = min_top + span * permille / 1000.f;
    point.left_dp = place.side == kSideLeft ? kEdgeDp : std::max(kEdgeDp, area.width_dp - kEdgeDp - chip_width_dp);
    return point;
  }
};

} // namespace pbr
