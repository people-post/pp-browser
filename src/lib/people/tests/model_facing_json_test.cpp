#include "lib/people/ModelFacingJson.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

// PB-057: the model sees names and ids, never keys, addresses, endpoints, icons or message text.
TEST(ModelFacingJsonTest, ContactKeepsNamesAndIdOnly) {
  pbr::Contact contact;
  contact.id = "contact:1";
  contact.local.display_name = "Owes-me Ada";  // remark
  contact.local.trust = pbr::TrustLevel::Friendly;
  contact.remote.nickname = "ada";
  contact.remote.multiaddrs = {"/ip4/203.0.113.9/tcp/4001/p2p/QmPeer"};
  contact.remote.fetched_at = 42;
  const std::string json = pbr::DumpJson(pbr::ContactForModel(contact));
  EXPECT_NE(json.find("\"id\":\"contact:1\""), std::string::npos) << json;
  // The remark is the user's private note: it never reaches a model, only the person's own nickname does.
  EXPECT_EQ(json.find("display_name"), std::string::npos) << json;
  EXPECT_EQ(json.find("Owes-me"), std::string::npos) << json;
  EXPECT_NE(json.find("\"nickname\":\"ada\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"trust\""), std::string::npos) << json;
  for (const char* forbidden : {"multiaddrs", "endpoints", "\"ids\"", "icon", "fetched_at", "203.0.113.9", "QmPeer"}) {
    EXPECT_EQ(json.find(forbidden), std::string::npos) << forbidden << " in " << json;
  }
}

TEST(ModelFacingJsonTest, ThreadDropsPreviewAndPeerIdentity) {
  pbr::Thread thread;
  thread.id = "thread:1";
  thread.title = "Ada";
  thread.preview = "the secret plan";
  thread.peer_identity_kind = "account";
  thread.peer_identity_value = "account:QmSecret";
  thread.unread_count = 3;
  const std::string json = pbr::DumpJson(pbr::ThreadForModel(thread));
  EXPECT_NE(json.find("\"id\":\"thread:1\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"title\":\"Ada\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"unread_count\":3"), std::string::npos) << json;
  EXPECT_EQ(json.find("secret plan"), std::string::npos) << json;
  EXPECT_EQ(json.find("QmSecret"), std::string::npos) << json;
  EXPECT_EQ(json.find("peer_identity_value"), std::string::npos) << json;
}
