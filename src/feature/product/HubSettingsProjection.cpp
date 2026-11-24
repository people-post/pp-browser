#include "feature/product/HubSettingsProjection.h"

#include "foundation/data/AppPaths.h"
#include "foundation/data/BackendProfile.h"
#include "foundation/platform/os/OsDeviceName.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/product/DevTools.h"
#include "lib/messaging/AttachmentDownloadPolicy.h"
#include "lib/messaging/GroupTypes.h"

namespace pbr {

const std::string& StartupSystemDeviceName() {
  static const std::string name = os::SystemDeviceName();
  return name;
}

HubSettings HubSettingsFrom(const AppConfig& config, const ProfilePreferences& prefs) {
  HubSettings settings;
  settings.mesh = config.mesh;
  settings.relay = config.relay;

  settings.directory = config.directory;
  settings.registration = config.registration;
  const BackendTable table = ActiveBackendTable();
  // The other relays a new profile's list starts with are the backend's seeds (profile-model PM006, PM011): a custom
  // backend's `relay.extra_base_urls`, none for a built-in one. Under PP_BROWSER_ORIGIN (a developer's own backend) the
  // file's still seed it.
  settings.relay_extra_base_urls = table.seed_relays;
  if (settings.relay_extra_base_urls.empty() && OriginEnvOverrideActive()) {
    settings.relay_extra_base_urls = config.relay_extra_base_urls;
  }
  settings.account_locker_url = table.account_locker_url;
  settings.client_compat_url = table.client_compat_url;
  settings.llm_token_url = table.llm_token_url;
  settings.initiation_floor = config.initiation_floor;
  settings.app_data_dir = AppPaths::DataDir(config.data_dir);
  settings.backend_id = kUiDemoBuild ? std::string("prod") : BackendProfileKey(config); // as Bootstrap selects it
  settings.policy = ProjectPolicy(prefs);
  settings.system_device_name = StartupSystemDeviceName();
  settings.developer_relays = kDeveloperRelaysAllowed;
  // MR008 item 4: first use of the org's relay only in developer builds, or on the compiled backend of a build that
  // ships no PeerId for it (dogfood); never a release build's custom backend or PP_BROWSER_ORIGIN.
  settings.org_first_use = OrgRelayFirstUseAllowed(kDeveloperRelaysAllowed,
                                                   !OriginEnvOverrideActive() && ActiveBackend() != BackendEnv::Custom);
  // SIGNED_FORMATS § 3.5: the build's relay id for its backend (a placeholder fails closed) and its certificate, the
  // floor the pins start from — only for the relay the build names (not a custom backend's).
  settings.org_relay_pending = IsPendingRelayId(config.relay.relay_id);
  if (const OrgRelayIdentity built = BriefOrgRelayIdentity();
      !built.relay_id.empty() && built.relay_id == config.relay.relay_id) {
    settings.org_relay_certificate_b64 = built.certificate_b64;
  }
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
