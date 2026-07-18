#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/people/ContactsStore.h"
#include "feature/messaging/DirectoryShadowCache.h"
#include "feature/messaging/PeerDisplayResolver.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

namespace pbr {
namespace {

TEST(PeerDisplayResolverTest, PrefersContactThenShadowThenShortId) {
  const auto data_dir = std::filesystem::temp_directory_path() / "pp_browser_peer_resolver";
  std::filesystem::remove_all(data_dir);

  ContactsStore contacts(data_dir.string());
  MockDirectoryClient directory;
  DirectoryShadowCache shadows(directory);
  PeerDisplayResolver resolver(contacts, shadows);

  Thread thread;
  thread.kind = ThreadKind::Direct;
  thread.peer_identity_kind = "relay_user";
  thread.peer_identity_value = "relay:lHaEnkO4vSbVnl6A";
  thread.title = "relay:lHaEnkO4vSbVnl6A";

  PeerDisplayLabel raw = resolver.ResolveThread(thread);
  EXPECT_EQ(raw.trust, PeerLabelTrust::RawId);
  EXPECT_EQ(raw.title, "relay:lHaEnkO4…");

  DirectoryHit hit;
  hit.nickname = "alice";
  hit.ids = {{ContactIdKind::RelayUser, thread.peer_identity_value, true}};
  shadows.Put(hit);
  PeerDisplayLabel unverified = resolver.ResolveThread(thread);
  EXPECT_EQ(unverified.trust, PeerLabelTrust::DirectoryUnverified);
  EXPECT_EQ(unverified.title, "~alice @relay:lHaEnkO4…");

  Contact contact;
  contact.id = "c1";
  contact.display_name = "Alice Example";
  contact.server_nickname = "alice";
  contact.ids = {{ContactIdKind::RelayUser, thread.peer_identity_value, true}};
  ASSERT_TRUE(contacts.Upsert(contact));
  PeerDisplayLabel trusted = resolver.ResolveThread(thread);
  EXPECT_EQ(trusted.trust, PeerLabelTrust::Contact);
  EXPECT_EQ(trusted.title, "Alice Example");
  ASSERT_TRUE(trusted.contact_id);
  EXPECT_EQ(*trusted.contact_id, "c1");

  std::filesystem::remove_all(data_dir);
}

TEST(PeerDisplayResolverTest, GroupLocalTitleWinsOverShared) {
  const auto data_dir = std::filesystem::temp_directory_path() / "pp_browser_peer_resolver_group";
  std::filesystem::remove_all(data_dir);

  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.ListThreads());
  ContactsStore contacts(data_dir.string());
  MockDirectoryClient directory;
  DirectoryShadowCache shadows(directory);
  GroupRosterStore roster(store.ProfileDbPath());
  PeerDisplayResolver resolver(contacts, shadows, &roster);

  GroupMetadata meta;
  meta.group_id = "group:1";
  meta.owner_identity = "relay:owner";
  meta.title = "Hiking Crew";
  meta.roster_epoch = 1;
  ASSERT_TRUE(roster.UpsertMetadata(meta));

  Thread thread;
  thread.id = "t-group";
  thread.kind = ThreadKind::Group;
  thread.group_id = "group:1";
  thread.title = "Hiking Crew";
  thread.local_title = "Weekend hike";
  ASSERT_TRUE(store.UpsertThread(thread));

  PeerDisplayLabel label = resolver.ResolveThread(thread);
  EXPECT_EQ(label.title, "Weekend hike");
  ASSERT_TRUE(label.shared_title);
  EXPECT_EQ(*label.shared_title, "Hiking Crew");

  thread.local_title.clear();
  label = resolver.ResolveThread(thread);
  EXPECT_EQ(label.title, "Hiking Crew");
  EXPECT_FALSE(label.shared_title.has_value());

  std::filesystem::remove_all(data_dir);
}

TEST(SqliteThreadStoreLocalTitleTest, MigratesAndPersistsLocalTitle) {
  const auto data_dir = std::filesystem::temp_directory_path() / "pp_browser_local_title_migrate";
  std::filesystem::remove_all(data_dir);

  {
    SqliteThreadStore store(data_dir.string());
    ASSERT_TRUE(store.ListThreads());
    Thread thread;
    thread.id = "t1";
    thread.kind = ThreadKind::Group;
    thread.group_id = "group:x";
    thread.title = "Shared";
    thread.local_title = "Mine";
    ASSERT_TRUE(store.UpsertThread(thread));
  }

  SqliteThreadStore reloaded(data_dir.string());
  auto loaded = reloaded.GetThread("t1");
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->title, "Shared");
  EXPECT_EQ((*loaded)->local_title, "Mine");

  std::filesystem::remove_all(data_dir);
}

} // namespace
} // namespace pbr
