#pragma once

#include "foundation/platform/ImageAffine.h"

#include <array>

namespace pbr {

/**
 * The avatar crop page's arithmetic (PLAN-avatar-edit item 7): a fixed round viewport over a picture the user moves,
 * pinches and turns. No GUI or SDL type; the page draws `Quad()` and the upload samples `OutputAffine(N)`.
 *
 * Conventions (one set, used everywhere in this class and its tests):
 * - Screen space: dp, origin at the centre of the viewport circle, x to the right, y DOWN.
 * - Image space: the picture's own pixels (already upright), origin at the picture's centre, x to the right, y down
 *   (the direction of the pixel rows). Image pixel coordinate X (continuous, pixel i covers [i, i + 1)) is
 *   image-space x + w / 2; the same for y.
 * - Rotation `theta`, radians, normalised to (-pi, pi]: a positive angle turns the picture CLOCKWISE as the user sees
 *   it. On a y-down screen this is the usual matrix R(t) = [cos t, -sin t; sin t, cos t]: R(90 deg) takes "right" to
 *   "down".
 * - Scale `s` in [1, 5]: 1 = the picture's short side exactly spans the circle. Screen dp per image pixel:
 *   k = s * D / min(w, h).
 * - Position: c = the image-space point under the circle's centre. Image to screen: q = k * R(theta) * (p - c);
 *   screen to image: p = c + R(-theta) * q / k.
 * - The circle stays inside the picture (no empty corner can show): r_img = min(w, h) / (2 s) is the circle's radius
 *   in image pixels, and |c.x| <= w / 2 - r_img, |c.y| <= h / 2 - r_img. A circle is the same at any angle, so the
 *   limit does not depend on theta. Every operation clamps s, then c.
 * - Degenerate sizes: a picture side below 1 px is taken as 1 px and a diameter below 1 dp (the page not laid out
 *   yet) as 1 dp; a non-finite or non-positive argument is ignored. No operation produces NaN.
 */
class AvatarCropFrame {
public:
  struct Point {
    double x = 0.0;
    double y = 0.0;
  };

  static constexpr double kMinScale = 1.0;
  static constexpr double kMaxScale = 5.0;

  /** A picture `image_w` x `image_h` px, centred, scale 1, no rotation, in a circle `viewport_diameter_dp` across. */
  AvatarCropFrame(int image_w, int image_h, double viewport_diameter_dp);

  /** The page's layout changed. The picture keeps its place relative to the circle (s and c do not depend on D). */
  void SetViewportDiameter(double viewport_diameter_dp);

  /** A drag of (dx, dy) dp on screen: the picture moves with the finger. */
  void Pan(double dx_dp, double dy_dp);
  /**
   * Scale by `scale_factor` and turn by `dtheta` (clockwise positive) at once, about `focus` (screen dp): the picture
   * point under the focus stays under it (with the clamped scale), then the position is clamped.
   */
  void Pinch(Point focus_dp, double scale_factor, double dtheta);
  /** Mouse wheel: `Pinch` without a turn. */
  void ZoomAt(Point focus_dp, double factor);
  /** The rotate button: +90 degrees clockwise; the point under the circle's centre stays. */
  void RotateQuarter();

  /**
   * The picture's four corners on screen (dp, relative to the circle's centre), in the order
   * top-left, top-right, bottom-right, bottom-left of the picture, i.e. texture coordinates (0,0), (1,0), (1,1), (0,1).
   */
  std::array<Point, 4> Quad() const;
  /**
   * For an `output_side` x `output_side` picture whose inscribed circle shows exactly what the circle on screen shows:
   * output pixel coordinates -> source pixel coordinates (see ImageAffine). `output_side` below 1 is taken as 1.
   */
  ImageAffine OutputAffine(int output_side) const;

  /** The circle's diameter in source pixels: min(w, h) / s (the upload's N = min(1024, this)). */
  double CircleDiameterImagePx() const;

  int image_w() const { return w_; }
  int image_h() const { return h_; }
  double viewport_diameter() const { return d_; }
  double scale() const { return s_; }
  double theta() const { return theta_; }
  Point centre() const { return c_; }

private:
  /** Screen dp per image pixel. */
  double K() const;
  void Clamp();

  int w_ = 1;
  int h_ = 1;
  double d_ = 1.0;
  double s_ = 1.0;
  double theta_ = 0.0;
  Point c_;
};

} // namespace pbr
