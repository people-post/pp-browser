#pragma once

namespace pbr {

/**
 * Desktop custom title-bar helpers: hit-test drag/resize regions and
 * minimize / maximize / close. No-ops on mobile.
 */
class DesktopWindowChrome {
public:
  static bool Enabled();

  /** True on macOS desktop: window controls sit on the leading (left) edge. */
  static bool ControlsLeading();

  /** Install SDL hit-test on the current Backend window. */
  static void Install();
  static void Uninstall();

  /**
   * Update hit-test geometry (dp). Converted to window coordinates using the
   * current SDL display/pixel scale.
   * @param controls_leading When true, exclude the left controls band from drag
   *        (macOS traffic lights); otherwise exclude the right band (Win/Linux).
   */
  static void SetLayout(float titlebar_height_dp, float controls_width_dp, float edge_margin_dp = 5.f,
                        bool controls_leading = false);

  /** Re-apply platform window appearance (e.g. macOS corner radius, Windows 11's rounded corners). */
  static void RefreshAppearance();

  /**
   * True once the system took the request to round the window's corners (Windows 11's small radius, about 8px; set by
   * Install / RefreshAppearance). The shell's 1dp outline follows it (`.shell-body--round-small`).
   */
  static bool SystemRoundsCorners();

  static void Minimize();
  static void ToggleMaximize();
  static void Close();
  static bool IsMaximized();

  /** Restore if minimized and raise/activate the main window (UI thread). */
  static void RaiseAndFocus();
};

} // namespace pbr
