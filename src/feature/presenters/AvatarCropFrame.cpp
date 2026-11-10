#include "feature/presenters/AvatarCropFrame.h"

#include <algorithm>
#include <cmath>

namespace pbr {
namespace {

constexpr double kPi = 3.14159265358979323846;

/** (-pi, pi]. */
double WrapAngle(double a) {
  a = std::remainder(a, 2.0 * kPi); // [-pi, pi]
  return a <= -kPi ? a + 2.0 * kPi : a;
}

/** R(t) * v, the clockwise-positive rotation on a y-down screen. */
AvatarCropFrame::Point Rotate(const AvatarCropFrame::Point v, const double t) {
  const double c = std::cos(t);
  const double s = std::sin(t);
  return {c * v.x - s * v.y, s * v.x + c * v.y};
}

bool Finite(const double v) {
  return std::isfinite(v);
}

} // namespace

AvatarCropFrame::AvatarCropFrame(const int image_w, const int image_h, const double viewport_diameter_dp)
    : w_(std::max(1, image_w)), h_(std::max(1, image_h)) {
  SetViewportDiameter(viewport_diameter_dp);
}

void AvatarCropFrame::SetViewportDiameter(const double viewport_diameter_dp) {
  d_ = Finite(viewport_diameter_dp) ? std::max(1.0, viewport_diameter_dp) : 1.0;
}

double AvatarCropFrame::K() const {
  return s_ * d_ / static_cast<double>(std::min(w_, h_));
}

double AvatarCropFrame::CircleDiameterImagePx() const {
  return static_cast<double>(std::min(w_, h_)) / s_;
}

void AvatarCropFrame::Clamp() {
  s_ = std::clamp(s_, kMinScale, kMaxScale);
  const double r_img = CircleDiameterImagePx() / 2.0;
  const double lim_x = std::max(0.0, w_ / 2.0 - r_img);
  const double lim_y = std::max(0.0, h_ / 2.0 - r_img);
  c_.x = std::clamp(c_.x, -lim_x, lim_x);
  c_.y = std::clamp(c_.y, -lim_y, lim_y);
}

void AvatarCropFrame::Pan(const double dx_dp, const double dy_dp) {
  if (!Finite(dx_dp) || !Finite(dy_dp)) {
    return;
  }
  // q + d = k R (p - c') for the point p that was at q  =>  c' = c - R(-theta) d / k.
  const Point d = Rotate({dx_dp, dy_dp}, -theta_);
  const double k = K();
  c_.x -= d.x / k;
  c_.y -= d.y / k;
  Clamp();
}

void AvatarCropFrame::Pinch(const Point focus_dp, const double scale_factor, const double dtheta) {
  if (!Finite(focus_dp.x) || !Finite(focus_dp.y) || !Finite(scale_factor) || scale_factor <= 0.0 || !Finite(dtheta)) {
    return;
  }
  // The picture point under the focus now ...
  const double k0 = K();
  const Point under = Rotate(focus_dp, -theta_);
  const Point p{c_.x + under.x / k0, c_.y + under.y / k0};
  // ... stays under it after the (clamped) scale and the turn: focus = k' R(theta') (p - c').
  s_ = std::clamp(s_ * scale_factor, kMinScale, kMaxScale);
  theta_ = WrapAngle(theta_ + dtheta);
  const double k1 = K();
  const Point back = Rotate(focus_dp, -theta_);
  c_ = {p.x - back.x / k1, p.y - back.y / k1};
  Clamp();
}

void AvatarCropFrame::ZoomAt(const Point focus_dp, const double factor) {
  Pinch(focus_dp, factor, 0.0);
}

void AvatarCropFrame::RotateQuarter() {
  theta_ = WrapAngle(theta_ + kPi / 2.0);
  Clamp();
}

std::array<AvatarCropFrame::Point, 4> AvatarCropFrame::Quad() const {
  const double hw = w_ / 2.0;
  const double hh = h_ / 2.0;
  const std::array<Point, 4> corners{Point{-hw, -hh}, Point{hw, -hh}, Point{hw, hh}, Point{-hw, hh}};
  const double k = K();
  std::array<Point, 4> out;
  for (size_t i = 0; i < corners.size(); ++i) {
    const Point r = Rotate({corners[i].x - c_.x, corners[i].y - c_.y}, theta_);
    out[i] = {k * r.x, k * r.y};
  }
  return out;
}

ImageAffine AvatarCropFrame::OutputAffine(const int output_side) const {
  const double n = static_cast<double>(std::max(1, output_side));
  // Output (u, v) -> screen q = (u - N/2, v - N/2) * D / N (the output's inscribed circle is the screen's circle) ->
  // image p = c + R(-theta) q / k -> source pixel X = p + (w/2, h/2). Linear part: R(-theta) * (D / (N k)).
  const double m = d_ / (n * K());
  const double cs = std::cos(theta_);
  const double sn = std::sin(theta_);
  // R(-theta) = [cos, sin; -sin, cos].
  ImageAffine a;
  a.xu = m * cs;
  a.xv = m * sn;
  a.yu = -m * sn;
  a.yv = m * cs;
  const double half = n / 2.0;
  a.x0 = c_.x + w_ / 2.0 - (a.xu * half + a.xv * half);
  a.y0 = c_.y + h_ / 2.0 - (a.yu * half + a.yv * half);
  return a;
}

} // namespace pbr
