#pragma once

namespace pbr {

/** SDL_HINT_ORIENTATIONS value for a phone: portrait only (also what Android uses). */
inline const char* PhoneOrientationsHint() {
  return "Portrait";
}

/** SDL_HINT_ORIENTATIONS value for an iPad: every orientation; the layout follows the window width. */
inline const char* TabletOrientationsHint() {
  return "Portrait PortraitUpsideDown LandscapeLeft LandscapeRight";
}

/**
 * The orientations the window may take, for SDL_HINT_ORIENTATIONS (set before SDL_Init). iOS: all four on an iPad,
 * portrait on an iPhone (Info.plist lists the same sets). Android: portrait. Desktops: nullptr (no hint; the window
 * resizes freely).
 */
const char* MobileOrientationsHint();

} // namespace pbr
