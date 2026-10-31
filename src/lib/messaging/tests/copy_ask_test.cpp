#include "lib/messaging/CopyAsk.h"

#include "lib/messaging/ChatPayloadCodec.h"

#include <gtest/gtest.h>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** The ask as the sender's device reads it: through the wire's ChatPayload (type and one detail string). */
ThreadMessage OverTheWire(const ThreadMessage& notice) {
  ThreadMessage read;
  EXPECT_TRUE(ChatPayloadCodec::ApplyRowToMessage(ChatPayloadCodec::EncodeToRow(notice).value(), read));
  return read;
}

TEST(CopyAskTest, AnAskNamesTheMessageTheSendingDeviceAndTheAskingOne) {
  const CopyAsk ask{.message_id = "m-1", .group_id = "group:hike", .to_device = "QmBobPhone", .from_device = "QmLaptop"};
  auto notice = copy_ask::BuildNotice(ask);
  ASSERT_TRUE(notice) << notice.error().message;
  auto parsed = copy_ask::Parse(OverTheWire(*notice));
  ASSERT_TRUE(parsed && parsed->has_value());
  EXPECT_EQ((*parsed)->message_id, "m-1");
  EXPECT_EQ((*parsed)->group_id, "group:hike");
  EXPECT_EQ((*parsed)->to_device, "QmBobPhone");
  EXPECT_EQ((*parsed)->from_device, "QmLaptop");
  EXPECT_FALSE(copy_ask::BuildNotice(CopyAsk{.message_id = "m-1"})) << "no devices: nothing to answer";
}

TEST(CopyAskTest, TheFamilyAnswersOnlyAsksWithTheirAsker) {
  std::vector<std::pair<CopyAsk, std::string>> answered;
  SystemControlFamily family =
      copy_ask::MakeFamily([&](CopyAsk ask, std::string asker) { answered.emplace_back(std::move(ask), asker); });
  ThreadMessage ask = OverTheWire(
      copy_ask::BuildNotice(CopyAsk{.message_id = "m-2", .to_device = "QmBob", .from_device = "QmLaptop"}).value());
  ASSERT_TRUE(family.owns(ask));
  ASSERT_TRUE(family.on_lane(ask)) << "control lane only: never a transcript row";
  ASSERT_TRUE(family.apply(ask, SystemControlContext{.sender_identity = "account:alice"}));
  ASSERT_EQ(answered.size(), 1u);
  EXPECT_EQ(answered[0].second, "account:alice");
  EXPECT_TRUE(answered[0].first.group_id.empty()) << "a direct chat";

  ThreadMessage other;
  other.content_type = ChatContentType::System;
  other.payload_json = R"({"control_type":"device_list"})";
  EXPECT_FALSE(family.owns(other));
}

} // namespace
} // namespace pbr
