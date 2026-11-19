#pragma once

#include "foundation/platform/PlatformShare.h"

#include <string>

namespace pbr {

/** Whether this platform hands a file to a system share sheet (iOS / iPadOS, Android); the chat menu offers it by it. */
bool PlatformCanShareFile();

/**
 * Hands the local file at `path` (`mime` its type, may be empty) to the system share sheet anchored to `anchor` inside
 * `window`: the app the user picks gets its own copy (saving to Files or Photos is one of the choices). Nothing is
 * deleted afterwards. False when there is no sheet or it could not be shown. UI thread only.
 */
bool PlatformShareFile(SDL_Window* window, const std::string& path, const std::string& mime, ShareAnchor anchor = {});

} // namespace pbr
