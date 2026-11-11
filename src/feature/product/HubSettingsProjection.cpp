#include "feature/product/HubSettingsProjection.h"

#include "foundation/data/AppPaths.h"
#include "lib/messaging/AttachmentDownloadPolicy.h"
#include "lib/messaging/GroupTypes.h"

namespace pbr {

HubSettings HubSettingsFrom(const AppConfig& config, const ProfilePreferences& prefs) {
  HubSettings settings;
  settings.mesh = config.mesh;
  settings.relay = config.relay;
  settings.directory = config.directory;
  settings.registration = config.registration;
  settings.initiation_floor = config.initiation_floor;
  settings.app_data_dir = AppPaths::DataDir(config.data_dir);
  settings.policy = ProjectPolicy(prefs);
  return settings;
}

HubSettings HubSettingsFor(const AppConfig& config, const std::string& profile_data_dir) {
  auto prefs = UserPreferences::LoadProfile(profile_data_dir);
  return HubSettingsFrom(config, prefs ? *prefs : ProfilePreferences{});
}

ConversationsHub::NetworkConfig ProjectNetwork(const AppConfig& config) {
  ConversationsHub::NetworkConfig out;
  out.mesh = config.mesh;
  out.relay = config.relay;
  out.directory = config.directory;
  out.registration = config.registration;
  out.node_enabled = config.mesh.node_enabled;
  out.circuit_relay = config.mesh.capabilities.circuit_relay;
  out.media_relay = config.mesh.capabilities.media_relay;
  out.dht = config.mesh.capabilities.dht;
  out.prefer_contacts_for_routing = config.mesh.prefer_contacts_for_routing;
  out.direct_connections = config.mesh.direct_connections;
  out.trusted_relays_only = config.mesh.trusted_relays_only;
  out.reachable_without_relay = config.mesh.reachable_without_relay;
  return out;
}

HubPolicyPrefs ProjectPolicy(const ProfilePreferences& prefs) {
  return {
      .group_invite_policy = GroupInvitePolicyFromString(prefs.group_invite_policy),
      .attachment_download_policy = AttachmentDownloadPolicyFromString(prefs.attachment_download_policy),
      .attachment_auto_on_wifi = prefs.attachment_auto_on_wifi,
      .attachment_auto_on_cellular = prefs.attachment_auto_on_cellular,
      .call_invite_policy = mesh::InboundAudienceFromName(prefs.call_invite_policy, mesh::InboundAudience::Everyone)};
}

ConversationsHub::NotificationPrefs ProjectNotifications(const ProfilePreferences& prefs) {
  return {.show_notifications = prefs.show_notifications, .stay_connected = prefs.stay_connected};
}

void ApplyMessageBanners(ILocalNotifier& notifier, const ConversationsHub::NotificationPrefs& prefs) {
  notifier.SetMessageBannersOn(prefs.show_notifications);
}

} // namespace pbr
