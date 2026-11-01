#include "lib/people/ContactsStore.h"
#include "lib/people/PeerDisplayLabel.h"
#include "common/MeshAlias.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

TEST(PeerDisplayLabelTest, FormatsContactDirectoryAndShortId) {
  Contact contact;
  contact.display_name = "Bob Builder";
  contact.server_nickname = "bob";
  EXPECT_EQ(FormatContactTitle(contact), "Bob Builder");
  contact.display_name.clear();
  EXPECT_EQ(FormatContactTitle(contact), "bob");

  mesh::DirectoryHit hit;
  hit.nickname = "alice";
  hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:lHaEnkO4vSbVnl6A", true}};
  EXPECT_EQ(FormatDirectoryTitle(hit), "~alice @relay:lHaEnkO4…");

  mesh::DirectoryHit nick_only;
  nick_only.nickname = "bob";
  EXPECT_EQ(FormatDirectoryTitle(nick_only), "~bob");

  EXPECT_EQ(ShortRelayId("relay:lHaEnkO4vSbVnl6A"), "relay:lHaEnkO4…");
  EXPECT_EQ(ShortRelayId("relay:short"), "relay:short");
}

TEST(ContactsStoreFindByIdentityTest, FindsExactRelayId) {
  const auto data_dir = std::filesystem::temp_directory_path() / "pp_browser_find_by_identity";
  std::filesystem::remove_all(data_dir);
  ContactsStore store(data_dir.string());

  Contact contact;
  contact.id = "c1";
  contact.display_name = "Alice";
  contact.ids = {{mesh::ContactIdKind::RelayUser, "relay:alice123", true}};
  ASSERT_TRUE(store.Upsert(contact));

  auto found = store.FindByIdentity("relay:alice123", mesh::ContactIdKind::RelayUser);
  ASSERT_TRUE(found);
  ASSERT_TRUE(*found);
  EXPECT_EQ((*found)->id, "c1");

  auto missing = store.FindByIdentity("relay:other", mesh::ContactIdKind::RelayUser);
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing->has_value());

  std::filesystem::remove_all(data_dir);
}

} // namespace
