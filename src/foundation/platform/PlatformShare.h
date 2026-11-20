#pragma once

#include <string>

struct SDL_Window;

namespace pbr {

/** Where the sheet / popover points: window pixels, origin top-left (RmlUi coordinates). All zero = window centre. */
struct ShareAnchor {
  float x = 0.f;
  float y = 0.f;
  float w = 0.f;
  float h = 0.f;
};

/**
 * Share `text` through the system sheet anchored to `anchor` inside `window`. False when the platform
 * has no sheet or it could not be shown; the caller then copies to the clipboard instead. UI thread only.
 */
bool PlatformShareText(SDL_Window* window, const std::string& text, ShareAnchor anchor = {});

/**
 * Whether a system modal (a share sheet, the Files / photo picker) is presented over `window` now; never closed around.
 * iOS only; false elsewhere. UI thread only.
 */
bool PlatformSystemModalUp(SDL_Window* window);

} // namespace pbr
