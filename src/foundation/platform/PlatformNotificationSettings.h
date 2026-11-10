#pragma once

#include <functional>

namespace pbr {

/** How the system shows PP's notification banner (iOS "Banner Style"). */
enum class BannerStyle {
  Unknown,    // not asked, not iOS, or the system did not say
  None,       // notifications are off for PP, or the style is "None"
  Temporary,  // the banner goes away after a few seconds (a ringtone tied to it stops with it)
  Persistent, // the banner stays until the person acts on it
};

/**
 * Reads the banner style of PP's notifications. Asynchronous: `done` runs once, on the UI thread, whatever the
 * platform (elsewhere it answers Unknown).
 */
void QueryBannerStyle(std::function<void(BannerStyle)> done);

/**
 * Opens the system settings page for PP's notifications (iOS; the app's settings page where the system has no
 * notification page). False where there is no such page or nothing opened.
 */
bool OpenNotificationSettings();

} // namespace pbr
