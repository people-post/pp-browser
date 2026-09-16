#pragma once

#include <cstdlib>

namespace pbr {

enum class ShellGestureAxis { None, Horizontal, Vertical };

/** Shared lock so horizontal swipe-back and vertical sheet dismiss can arm together. */
class ShellGestureAxisLock {
public:
  ShellGestureAxis Get() const { return locked_; }

  /** Call when an edge swipe-back arms, before either gesture observes movement. */
  void SetPreferHorizontal(bool prefer) { prefer_horizontal_ = prefer; }
  bool PreferHorizontal() const { return prefer_horizontal_; }

  /**
   * Decide axis once movement exceeds deadzone. Idempotent after lock.
   * When prefer_horizontal_ is set (edge swipe-back armed), require a clearly
   * vertical drag before giving the sheet the lock.
   */
  ShellGestureAxis Observe(int dx_px, int dy_px, int deadzone_px) {
    if (locked_ != ShellGestureAxis::None) {
      return locked_;
    }
    const int adx = std::abs(dx_px);
    const int ady = std::abs(dy_px);
    if (adx < deadzone_px && ady < deadzone_px) {
      return ShellGestureAxis::None;
    }
    if (prefer_horizontal_) {
      // Edge back: horizontal wins unless vertical is clearly dominant (~1.5x).
      if (ady > deadzone_px && ady * 2 >= adx * 3) {
        locked_ = ShellGestureAxis::Vertical;
      } else if (adx >= deadzone_px) {
        locked_ = ShellGestureAxis::Horizontal;
      } else {
        return ShellGestureAxis::None;
      }
      return locked_;
    }
    // Default: slight horizontal bias (adx >= ady * 1.2).
    if (adx * 5 >= ady * 6) {
      locked_ = ShellGestureAxis::Horizontal;
    } else {
      locked_ = ShellGestureAxis::Vertical;
    }
    return locked_;
  }

  void Unlock() {
    locked_ = ShellGestureAxis::None;
    prefer_horizontal_ = false;
  }

private:
  ShellGestureAxis locked_ = ShellGestureAxis::None;
  bool prefer_horizontal_ = false;
};

} // namespace pbr
