#pragma once

struct SDL_Window;

namespace pbr {
namespace desktop {

/**
 * Apply macOS-native rounded window corners on a borderless SDL window.
 * macOS-only (this TU is not built on Win/Linux). Requires the window to have
 * been created with SDL_WINDOW_TRANSPARENT so clipped corners stay clear rather
 * than opaque black. Pass square_corners=true when maximized / fullscreen-tiled
 * so edges meet the display cleanly.
 *
 * The same layer that clips the corners draws the 1-device-pixel outline (colour by `dark`, equal to the shell's
 * `.shell-body--framed` border; none when square), so the window's edge is one system-drawn curve. Safe to call again
 * (theme, scale or window-state change); each call also invalidates the window shadow.
 */
void ApplyMacWindowRoundedCorners(SDL_Window* window, bool square_corners, bool dark);

} // namespace desktop
} // namespace pbr
