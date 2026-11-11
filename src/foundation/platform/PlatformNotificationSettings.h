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

/** Why PP cannot be woken by the system once it is in the background (Android). */
enum class PushGap {
  None,             // not known, not Android, or the system can wake PP
  BuildHasNone,     // this build of PP has no system wake-up
  NoGoogleServices, // the build has one, the phone has no Google services
};

/**
 * What the phone's settings do to PP's alerts (Android; elsewhere the default: nothing wrong). A flag is set only when
 * the system said so: what could not be read stays false, so a row never claims a problem that was not seen.
 */
struct NotificationHealth {
  bool notifications_blocked = false; // PP's notifications (or its messages channel) are turned off
  bool silenced = false;              // Do Not Disturb is "total silence"
  bool battery_restricted = false;    // the system may stop PP in the background (battery optimisation)
  bool autostart_vendor = false; // a Xiaomi-family or vivo-family phone: it has an auto-start switch we cannot read
  PushGap push_gap = PushGap::None;
};

/** Reads NotificationHealth. Asynchronous like QueryBannerStyle: `done` runs once, on the UI thread. */
void QueryNotificationHealth(std::function<void(NotificationHealth)> done);

/** The system screen a notification tip leads to. */
enum class NotificationFix { AppNotifications, DoNotDisturb, Battery, Autostart };

/**
 * Opens the screen for `fix` (Android; a vendor screen that does not exist falls back to PP's app-details page). False
 * where there is no such screen or nothing opened.
 */
bool OpenNotificationFix(NotificationFix fix);

/**
 * Opens the system settings page for PP's notifications (iOS; the app's settings page where the system has no
 * notification page). False where there is no such page or nothing opened.
 */
bool OpenNotificationSettings();

} // namespace pbr
