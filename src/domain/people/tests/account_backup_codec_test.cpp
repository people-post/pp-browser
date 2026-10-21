#include "domain/people/AccountBackupCodec.h"

#include "domain/people/ContactTypes.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

Contact SampleContact() {
  Contact contact;
  contact.id = "c1";
  contact.local.display_name = "Alice at work";
  contact.local.trust = TrustLevel::Friendly;
  contact.remote.nickname = "alice";
  contact.remote.ids = {{mesh::ContactIdKind::Account, "account:abc", true}};
  SyncContactMirrors(contact);
  return contact;
}

TEST(AccountBackupCodecTest, RoundTripsVersion2) {
  AccountBackupV2 backup;
  backup.updated_at_ms = 1700000000009;
  backup.nickname = "alice";
  backup.profile_id = "default";
  backup.vault = {'P', 'P', 'B', 'V', 1, 2, 3};
  backup.sealed = {9, 8, 7, 0, 255};

  auto bytes = AccountBackupCodec::Serialize(backup);
  ASSERT_TRUE(static_cast<bool>(bytes)) << bytes.error().message;
  auto parsed = AccountBackupCodec::Parse(*bytes);
  ASSERT_TRUE(static_cast<bool>(parsed)) << parsed.error().message;

  EXPECT_EQ(parsed->updated_at_ms, 1700000000009);
  EXPECT_EQ(parsed->nickname, "alice");
  EXPECT_EQ(parsed->profile_id, "default");
  EXPECT_EQ(parsed->vault, backup.vault);
  EXPECT_EQ(parsed->sealed, backup.sealed);
}

TEST(AccountBackupCodecTest, RoundTripsContents) {
  AccountBackupContents contents;
  contents.link_bundle_json = R"({"schema":"pp-browser-link-device-v1","relay_user_id":"relay:u1","nickname":"alice"})";
  contents.contacts = {SampleContact()};

  auto bytes = AccountBackupCodec::SerializeContents(contents);
  ASSERT_TRUE(static_cast<bool>(bytes)) << bytes.error().message;
  auto parsed = AccountBackupCodec::ParseContents(*bytes);
  ASSERT_TRUE(static_cast<bool>(parsed)) << parsed.error().message;

  EXPECT_NE(parsed->link_bundle_json.find("\"relay_user_id\":\"relay:u1\""), std::string::npos);
  ASSERT_EQ(parsed->contacts.size(), 1u);
  EXPECT_EQ(parsed->contacts[0].id, "c1");
  EXPECT_EQ(parsed->contacts[0].local.display_name, "Alice at work");
  EXPECT_EQ(parsed->contacts[0].local.trust, TrustLevel::Friendly);
  EXPECT_EQ(parsed->contacts[0].remote.nickname, "alice");
}

TEST(AccountBackupCodecTest, RejectsOtherSchemasAndVersions) {
  const std::string other = R"({"schema":"something-else","version":2,"profile_id":"p","vault_b64":"AQ==","sealed_b64":"AQ=="})";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Parse(std::vector<uint8_t>(other.begin(), other.end()))));
  // Version 1 kept the link bundle in the clear; it is not read.
  const std::string v1 = R"({"schema":"pp-browser-account-backup","version":1,"link_bundle":{},"contacts":[]})";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Parse(std::vector<uint8_t>(v1.begin(), v1.end()))));
  const std::string no_vault = R"({"schema":"pp-browser-account-backup","version":2,"profile_id":"p","sealed_b64":"AQ=="})";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Parse(std::vector<uint8_t>(no_vault.begin(), no_vault.end()))));
  const std::string garbage = "not json";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Parse(std::vector<uint8_t>(garbage.begin(), garbage.end()))));
}

TEST(AccountBackupCodecTest, SerializeNeedsAJsonBundleAndTheBlobs) {
  AccountBackupContents contents;
  contents.link_bundle_json = "nope";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::SerializeContents(contents)));
  AccountBackupV2 backup;
  backup.profile_id = "default";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Serialize(backup)));
}

} // namespace
} // namespace pbr
