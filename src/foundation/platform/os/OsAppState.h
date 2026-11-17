#pragma once

namespace pbr::os {

/**
 * Whether the app is on screen (or about to be) right now. iOS: false while it runs in the background — launched
 * there by a push or left there — and true while active or inactive (a foreground launch before its scene is
 * active). True where there is no such state. UI thread only.
 */
bool AppShownNow();

} // namespace pbr::os
