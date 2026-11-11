#include "foundation/data/Config.h"
#include "foundation/data/MeshRole.h"
#include "foundation/data/UserPreferences.h"
#include "lib/messaging/AttachmentDownloadPolicy.h"
#include "feature/product/HubSettingsProjection.h"
#include "foundation/data/ConfigJson.h"
#include "foundation/platform/ILocalNotifier.h"

#include <gtest/gtest.h>

#include <vector>

TEST(HubSettingsProjectionTest, ProjectsNetworkSliceFromAppConfig) {
  pbr::AppConfig config = pbr::Config::DefaultAppConfig();
  config.relay.base_url = "https://relay.example";
  config.directory.base_url = "https://dir.example";
  config.registration.base_url = "https://reg.example";
  config.mesh.node_enabled = false;
  config.mesh.capabilities.circuit_relay = true;
  config.mesh.capabilities.media_relay = false;
  config.mesh.capabilities.dht = true;
  config.mesh.prefer_contacts_for_routing = false;
  config.llm.model = "ignored-by-network-slice";

  const pbr::ConversationsHub::NetworkConfig slice = pbr::ProjectNetwork(config);
  EXPECT_EQ(slice.relay.base_url, "https://relay.example");
  EXPECT_EQ(slice.directory.base_url, "https://dir.example");
  EXPECT_EQ(slice.registration.base_url, "https://reg.example");
  EXPECT_FALSE(slice.node_enabled);
  EXPECT_TRUE(slice.circuit_relay);
  EXPECT_FALSE(slice.media_relay);
  EXPECT_TRUE(slice.dht);
  EXPECT_FALSE(slice.prefer_contacts_for_routing);

  pbr::AppConfig other = config;
  other.llm.model = "different-llm";
  EXPECT_EQ(pbr::ProjectNetwork(other), slice);

  other.mesh.node_enabled = true;
  EXPECT_NE(pbr::ProjectNetwork(other), slice);
}

/** N009: Client may store circuit_relay=true on disk; hosting still requires Node. Consume does not. */
TEST(HubSettingsProjectionTest, ClientCircuitRelayFlagDoesNotImplyHostingRole) {
  pbr::AppConfig config = pbr::Config::DefaultAppConfig();
  config.mesh.node_enabled = false;
  config.mesh.capabilities.circuit_relay = true;
  EXPECT_EQ(pbr::ResolveMeshRole(config.mesh), pbr::MeshRole::Client);
  EXPECT_TRUE(config.mesh.capabilities.circuit_relay);
  // Host gate in ConversationsHub: role == Node && capabilities.circuit_relay.
  EXPECT_FALSE(pbr::ResolveMeshRole(config.mesh) == pbr::MeshRole::Node && config.mesh.capabilities.circuit_relay);
}

TEST(HubSettingsProjectionTest, ProjectsPolicyAndNotificationPrefs) {
  pbr::ProfilePreferences prefs = pbr::UserPreferences::DefaultProfile();
  prefs.group_invite_policy = "everyone";
  prefs.attachment_download_policy = "on_demand";
  prefs.show_notifications = false;
  prefs.appearance = "dark";

  const pbr::ConversationsHub::PolicyPrefs policy = pbr::ProjectPolicy(prefs);
  EXPECT_EQ(policy.group_invite_policy, pbr::GroupInvitePolicy::Everyone);
  EXPECT_EQ(policy.attachment_download_policy, pbr::AttachmentDownloadPolicy::OnDemand);

  const pbr::ConversationsHub::NotificationPrefs notifications = pbr::ProjectNotifications(prefs);
  EXPECT_FALSE(notifications.show_notifications);

  EXPECT_TRUE(policy.attachment_auto_on_wifi);
  EXPECT_FALSE(policy.attachment_auto_on_cellular);

  pbr::ProfilePreferences other = prefs;
  other.appearance = "light";
  EXPECT_EQ(pbr::ProjectPolicy(other), policy);
  EXPECT_EQ(pbr::ProjectNotifications(other), notifications);

  other.group_invite_policy = "nobody";
  EXPECT_NE(pbr::ProjectPolicy(other), policy);
  other.group_invite_policy = "everyone";
  other.attachment_auto_on_cellular = true;
  EXPECT_NE(pbr::ProjectPolicy(other), policy);
  other.attachment_auto_on_cellular = false;
  other.attachment_auto_on_wifi = false;
  EXPECT_NE(pbr::ProjectPolicy(other), policy);
  other.show_notifications = true;
  EXPECT_NE(pbr::ProjectNotifications(other), notifications);
}

namespace {
class RecordingNotifier final : public pbr::ILocalNotifier {
public:
  void NotifyIncoming(const std::string&, const std::string&, const std::string&, int) override {}
  void ClearForThread(const std::string&) override {}
  void SetMessageBannersOn(bool on) override { calls.push_back(on); }
  std::vector<bool> calls;
};
} // namespace

// Startup hands the saved Show notifications to the platform notifier, as a later change does.
TEST(HubSettingsProjectionTest, SavedShowNotificationsReachTheNotifier) {
  RecordingNotifier notifier;
  pbr::ILocalNotifier::SetInstance(&notifier);
  for (const bool saved : {false, true}) {
    pbr::ProfilePreferences prefs;
    prefs.show_notifications = saved;
    pbr::ApplyMessageBanners(pbr::ILocalNotifier::Instance(), pbr::ProjectNotifications(prefs));
  }
  pbr::ILocalNotifier::SetInstance(nullptr);
  EXPECT_EQ(notifier.calls, (std::vector<bool>{false, true}));
}

// The hub's settings are values the app projects (capability-repos CR015): its config's slices and the profile's
// policies, nothing the hub reads itself.
TEST(HubSettingsProjectionTest, TheHubsSettingsCarryTheConfigsSlicesAndTheProfilesPolicies) {
  pbr::AppConfig config = pbr::Config::DefaultAppConfig();
  config.relay.base_url = "https://relay.example";
  config.initiation_floor = 7;
  pbr::ProfilePreferences prefs;
  prefs.group_invite_policy = "anyone";
  prefs.attachment_download_policy = "never";
  const pbr::HubSettings settings = pbr::HubSettingsFrom(config, prefs);
  EXPECT_EQ(settings.relay.base_url, "https://relay.example");
  EXPECT_EQ(settings.mesh.bootstrap_peers, config.mesh.bootstrap_peers) << "normalized by the app";
  EXPECT_EQ(settings.initiation_floor, 7);
  EXPECT_FALSE(settings.app_data_dir.empty());
  EXPECT_EQ(settings.policy, pbr::ProjectPolicy(prefs));
}

TEST(HubSettingsProjectionTest, UnrecognisedGroupInviteValueStaysContactsOnly) {
  // A schema 16 file with a value this build does not know is kept as written and projected strictly.
  auto object = pbr::TryParseObject(R"({ "schema_version": 16, "group_invite_policy": "someone-new" })");
  ASSERT_TRUE(object.has_value());
  pbr::ProfilePreferences prefs;
  pbr::ProfilePrefsFromObject(*object, prefs);
  EXPECT_EQ(pbr::ProjectPolicy(prefs).group_invite_policy, pbr::GroupInvitePolicy::ContactsOnly);

  // No stored value is the default: everyone.
  EXPECT_EQ(pbr::ProjectPolicy(pbr::UserPreferences::DefaultProfile()).group_invite_policy,
            pbr::GroupInvitePolicy::Everyone);
}
