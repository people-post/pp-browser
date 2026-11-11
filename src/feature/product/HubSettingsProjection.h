#pragma once

#include "feature/conversations/ConversationsHub.h"
#include "foundation/data/Config.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/platform/ILocalNotifier.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

// The app's settings as the conversations hub takes them (capability-repos CR015): the app reads its config file
// and the profile's preferences, the hub gets values.

/** The hub's settings from the app's config and the profile's preferences. */
HubSettings HubSettingsFrom(const AppConfig& config, const ProfilePreferences& prefs);
/** The same, the profile's preferences read from `profile_data_dir` (defaults when unreadable). */
HubSettings HubSettingsFor(const AppConfig& config, const std::string& profile_data_dir);

/** The hot-reloadable network / mesh slice of the app's config. */
ConversationsHub::NetworkConfig ProjectNetwork(const AppConfig& config);
/** Inbound group invites, attachment downloads and call invites from the profile's preferences. */
HubPolicyPrefs ProjectPolicy(const ProfilePreferences& prefs);
/** Banners and staying connected (P011) from the profile's preferences. */
ConversationsHub::NotificationPrefs ProjectNotifications(const ProfilePreferences& prefs);
/** Hands Show notifications to the platform notifier (Android keeps it for pushes that wake a stopped app). */
void ApplyMessageBanners(ILocalNotifier& notifier, const ConversationsHub::NotificationPrefs& prefs);

} // namespace pbr
