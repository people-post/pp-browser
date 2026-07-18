#include "base/messaging/GroupMembershipCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(GroupRenamedCodecTest, RoundTripDetail) {
  auto encoded = GroupMembershipCodec::EncodeGroupRenamed("group:1", "New Title", 3);
  ASSERT_TRUE(encoded);
  auto decoded = GroupMembershipCodec::DecodeGroupRenamed(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->group_id, "group:1");
  EXPECT_EQ(decoded->title, "New Title");
  EXPECT_EQ(decoded->roster_epoch, 3u);
}

TEST(GroupRenamedCodecTest, DecodeFromSystemMessage) {
  auto detail = GroupMembershipCodec::EncodeGroupRenamed("group:2", "Crew", 1);
  ASSERT_TRUE(detail);
  auto message = GroupMembershipCodec::BuildSystemMessage("thread-1", GroupMembershipControlType::GroupRenamed,
                                                          "Group renamed to Crew", *detail, "relay:owner");
  ASSERT_TRUE(message);
  auto decoded = GroupMembershipCodec::DecodeGroupRenamedFromMessage(*message);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->group_id, "group:2");
  EXPECT_EQ(decoded->title, "Crew");
}

} // namespace
} // namespace pbr
