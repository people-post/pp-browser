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
 */
void ApplyMacWindowRoundedCorners(SDL_Window* window, bool square_corners);

} // namespace desktop
} // namespace pbr
