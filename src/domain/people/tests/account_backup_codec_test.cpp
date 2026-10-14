#include "domain/people/AccountBackupCodec.h"

#include "domain/people/ContactTypes.h"

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
  contact.remote.ids = {{ContactIdKind::Account, "account:abc", true}};
  SyncContactMirrors(contact);
  return contact;
}

TEST(AccountBackupCodecTest, RoundTripsBundleAndContacts) {
  AccountBackupV1 backup;
  backup.updated_at_ms = 1700000000009;
  backup.link_bundle_json = R"({"schema":"pp-browser-link-device-v1","relay_user_id":"relay:u1","nickname":"alice"})";
  backup.contacts = {SampleContact()};

  auto bytes = AccountBackupCodec::Serialize(backup);
  ASSERT_TRUE(static_cast<bool>(bytes)) << bytes.error().message;
  auto parsed = AccountBackupCodec::Parse(*bytes);
  ASSERT_TRUE(static_cast<bool>(parsed)) << parsed.error().message;

  EXPECT_EQ(parsed->updated_at_ms, 1700000000009);
  EXPECT_NE(parsed->link_bundle_json.find("\"relay_user_id\":\"relay:u1\""), std::string::npos);
  ASSERT_EQ(parsed->contacts.size(), 1u);
  EXPECT_EQ(parsed->contacts[0].id, "c1");
  EXPECT_EQ(parsed->contacts[0].local.display_name, "Alice at work");
  EXPECT_EQ(parsed->contacts[0].local.trust, TrustLevel::Friendly);
  EXPECT_EQ(parsed->contacts[0].remote.nickname, "alice");
}

TEST(AccountBackupCodecTest, RejectsOtherSchemasAndVersions) {
  const std::string other = R"({"schema":"something-else","version":1,"link_bundle":{}})";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Parse(std::vector<uint8_t>(other.begin(), other.end()))));
  const std::string v2 = R"({"schema":"pp-browser-account-backup","version":2,"link_bundle":{}})";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Parse(std::vector<uint8_t>(v2.begin(), v2.end()))));
  const std::string garbage = "not json";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Parse(std::vector<uint8_t>(garbage.begin(), garbage.end()))));
}

TEST(AccountBackupCodecTest, SerializeNeedsAJsonBundle) {
  AccountBackupV1 backup;
  backup.link_bundle_json = "nope";
  EXPECT_FALSE(static_cast<bool>(AccountBackupCodec::Serialize(backup)));
}

} // namespace
} // namespace pbr
