#pragma once

namespace pbr {

/**
 * Desktop custom title-bar helpers: hit-test drag/resize regions and
 * minimize / maximize / close. No-ops on mobile.
 */
class DesktopWindowChrome {
public:
  static bool Enabled();

  /** Install SDL hit-test on the current Backend window. */
  static void Install();
  static void Uninstall();

  /**
   * Update hit-test geometry (dp). Converted to window coordinates using the
   * current SDL display/pixel scale.
   */
  static void SetLayout(float titlebar_height_dp, float controls_width_dp, float edge_margin_dp = 5.f);

  static void Minimize();
  static void ToggleMaximize();
  static void Close();
  static bool IsMaximized();
};

} // namespace pbr
