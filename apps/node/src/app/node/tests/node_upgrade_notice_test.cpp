#include "app/node/NodeUpgradeNotice.h"

#include <gtest/gtest.h>

#include <string>

namespace {

// A relay answering 426 `client_upgrade_required` is told to the operator once per relay per run, in plain words.
TEST(NodeUpgradeNoticeTest, OnceForEachRelay) {
  pbr::NodeUpgradeNotice notice;
  EXPECT_TRUE(notice.Note("https://relay-a.example/api/relay"));
  EXPECT_FALSE(notice.Note("https://relay-a.example/api/relay")) << "each heartbeat after the first says nothing";
  EXPECT_TRUE(notice.Note("https://relay-b.example/api/relay")) << "another relay is another cause";
}

TEST(NodeUpgradeNoticeTest, SaysThisNodeIsTooOldForThatRelay) {
  const std::string message = pbr::NodeUpgradeNotice::Message("https://relay-a.example/api/relay");
  EXPECT_NE(message.find("this pp-node is too old for the relay at https://relay-a.example/api/relay: upgrade it"),
            std::string::npos)
      << message;
}

} // namespace
