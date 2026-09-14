#include "foundation/data/Config.h"
#include "foundation/data/MeshRole.h"
#include "foundation/data/UserPreferences.h"
#include "domain/messaging/AttachmentDownloadPolicy.h"
#include "feature/conversations/ConversationsHub.h"

#include <gtest/gtest.h>

TEST(ConversationsHubConfigTest, ProjectsNetworkSliceFromAppConfig) {
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

  const pbr::ConversationsHub::NetworkConfig slice = pbr::ConversationsHub::ProjectNetwork(config);
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
  EXPECT_EQ(pbr::ConversationsHub::ProjectNetwork(other), slice);

  other.mesh.node_enabled = true;
  EXPECT_NE(pbr::ConversationsHub::ProjectNetwork(other), slice);
}

/** N009: Client may store circuit_relay=true on disk; hosting still requires Node. Consume does not. */
TEST(ConversationsHubConfigTest, ClientCircuitRelayFlagDoesNotImplyHostingRole) {
  pbr::AppConfig config = pbr::Config::DefaultAppConfig();
  config.mesh.node_enabled = false;
  config.mesh.capabilities.circuit_relay = true;
  EXPECT_EQ(pbr::ResolveMeshRole(config.mesh), pbr::MeshRole::Client);
  EXPECT_TRUE(config.mesh.capabilities.circuit_relay);
  // Host gate in ConversationsHub: role == Node && capabilities.circuit_relay.
  EXPECT_FALSE(pbr::ResolveMeshRole(config.mesh) == pbr::MeshRole::Node &&
               config.mesh.capabilities.circuit_relay);
}

TEST(ConversationsHubConfigTest, ProjectsPolicyAndNotificationPrefs) {
  pbr::ProfilePreferences prefs = pbr::UserPreferences::DefaultProfile();
  prefs.group_invite_policy = "everyone";
  prefs.attachment_download_policy = "on_demand";
  prefs.show_notifications = false;
  prefs.appearance = "dark";

  const pbr::ConversationsHub::PolicyPrefs policy = pbr::ConversationsHub::ProjectPolicy(prefs);
  EXPECT_EQ(policy.group_invite_policy, pbr::GroupInvitePolicy::Everyone);
  EXPECT_EQ(policy.attachment_download_policy, pbr::AttachmentDownloadPolicy::OnDemand);

  const pbr::ConversationsHub::NotificationPrefs notifications =
      pbr::ConversationsHub::ProjectNotifications(prefs);
  EXPECT_FALSE(notifications.show_notifications);

  pbr::ProfilePreferences other = prefs;
  other.appearance = "light";
  EXPECT_EQ(pbr::ConversationsHub::ProjectPolicy(other), policy);
  EXPECT_EQ(pbr::ConversationsHub::ProjectNotifications(other), notifications);

  other.group_invite_policy = "nobody";
  EXPECT_NE(pbr::ConversationsHub::ProjectPolicy(other), policy);
  other.show_notifications = true;
  EXPECT_NE(pbr::ConversationsHub::ProjectNotifications(other), notifications);
}
