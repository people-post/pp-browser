#include "base/data/Config.h"
#include "base/data/UserPreferences.h"
#include "base/messaging/AttachmentDownloadPolicy.h"
#include "feature/messaging/MessagingHub.h"

#include <gtest/gtest.h>

TEST(MessagingHubConfigTest, ProjectsNetworkSliceFromAppConfig) {
  pbr::AppConfig config = pbr::Config::DefaultAppConfig();
  config.relay.base_url = "https://relay.example";
  config.directory.base_url = "https://dir.example";
  config.registration.base_url = "https://reg.example";
  config.mesh.node_enabled = false;
  config.mesh.capabilities.circuit_relay = true;
  config.mesh.capabilities.media_relay = false;
  config.mesh.prefer_contacts_for_routing = false;
  config.llm.model = "ignored-by-network-slice";

  const pbr::MessagingHub::NetworkConfig slice = pbr::MessagingHub::ProjectNetwork(config);
  EXPECT_EQ(slice.relay.base_url, "https://relay.example");
  EXPECT_EQ(slice.directory.base_url, "https://dir.example");
  EXPECT_EQ(slice.registration.base_url, "https://reg.example");
  EXPECT_FALSE(slice.node_enabled);
  EXPECT_TRUE(slice.circuit_relay);
  EXPECT_FALSE(slice.media_relay);
  EXPECT_FALSE(slice.prefer_contacts_for_routing);

  pbr::AppConfig other = config;
  other.llm.model = "different-llm";
  EXPECT_EQ(pbr::MessagingHub::ProjectNetwork(other), slice);

  other.mesh.node_enabled = true;
  EXPECT_NE(pbr::MessagingHub::ProjectNetwork(other), slice);
}

TEST(MessagingHubConfigTest, ProjectsPolicyAndNotificationPrefs) {
  pbr::ProfilePreferences prefs = pbr::UserPreferences::DefaultProfile();
  prefs.group_invite_policy = "everyone";
  prefs.attachment_download_policy = "on_demand";
  prefs.show_notifications = false;
  prefs.appearance = "dark";

  const pbr::MessagingHub::PolicyPrefs policy = pbr::MessagingHub::ProjectPolicy(prefs);
  EXPECT_EQ(policy.group_invite_policy, pbr::GroupInvitePolicy::Everyone);
  EXPECT_EQ(policy.attachment_download_policy, pbr::AttachmentDownloadPolicy::OnDemand);

  const pbr::MessagingHub::NotificationPrefs notifications =
      pbr::MessagingHub::ProjectNotifications(prefs);
  EXPECT_FALSE(notifications.show_notifications);

  pbr::ProfilePreferences other = prefs;
  other.appearance = "light";
  EXPECT_EQ(pbr::MessagingHub::ProjectPolicy(other), policy);
  EXPECT_EQ(pbr::MessagingHub::ProjectNotifications(other), notifications);

  other.group_invite_policy = "nobody";
  EXPECT_NE(pbr::MessagingHub::ProjectPolicy(other), policy);
  other.show_notifications = true;
  EXPECT_NE(pbr::MessagingHub::ProjectNotifications(other), notifications);
}
