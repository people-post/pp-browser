#pragma once

struct SDL_Window;

namespace pbr {
namespace desktop {

/**
 * Asks the system to round the borderless window's corners where it can: Windows 11's DWM draws its small (about 8px)
 * rounded corner and squares it by itself while the window is maximized. Returns whether the system took the request
 * (false on Windows 10, on the other desktops and with no window); a refusal is silent and the window stays square.
 * `.shell-body--framed.shell-body--round-small` in components.rcss draws our 1dp outline on the same radius.
 * WindowCorners_Win32.cpp; WindowCorners_Default.cpp elsewhere (macOS shapes its window in WindowChrome_Darwin.mm).
 */
bool RequestSystemRoundedCorners(SDL_Window* window);

} // namespace desktop
} // namespace pbr
