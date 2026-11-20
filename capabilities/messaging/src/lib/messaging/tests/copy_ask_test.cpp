#include "lib/messaging/CopyAsk.h"
#include "lib/messaging/CardAsk.h"

#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/OutboundMessage.h"

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
  const CopyAsk ask{
      .message_id = "m-1", .group_id = "group:hike", .to_device = "QmBobPhone", .from_device = "QmLaptop"};
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

TEST(CardAskTest, AnAskNamesTheDeviceThatSaidItsListIsNewer) {
  auto notice = card_ask::BuildNotice("QmBobPhone");
  ASSERT_TRUE(notice);
  auto parsed = card_ask::Parse(OverTheWire(*notice));
  ASSERT_TRUE(parsed && parsed->has_value());
  EXPECT_EQ(**parsed, "QmBobPhone");
  EXPECT_FALSE(card_ask::BuildNotice(""));
  std::vector<std::pair<std::string, std::string>> answered;
  SystemControlFamily family =
      card_ask::MakeFamily([&](std::string device, std::string asker) { answered.emplace_back(device, asker); });
  ThreadMessage ask = OverTheWire(*notice);
  ASSERT_TRUE(family.on_lane(ask));
  ASSERT_TRUE(family.apply(ask, SystemControlContext{.sender_identity = "account:carol"}));
  ASSERT_EQ(answered.size(), 1u);
  EXPECT_EQ(answered[0], std::make_pair(std::string("QmBobPhone"), std::string("account:carol")));
}

// The answer to an ask re-sends an old message to the device that missed it: sealed as it was first sent, but it wakes
// nobody — a push would alert every device of the asker's account to a message they have already seen.
TEST(CopyAskTest, AnAnsweredCopyWakesNobody) {
  ThreadMessage text;
  text.id = "m-3";
  text.text = "hello";
  ASSERT_TRUE(RaisesWakePush(text, {})) << "it woke them when it was new";
  const SendRelayOptions resend = copy_ask::ResendOptions(text);
  EXPECT_FALSE(resend.content_type) << "a plain text seals as text";
  EXPECT_FALSE(RaisesWakePush(text, resend));
  const EnvelopeSender sender{"account:bob", "QmBobPhone"};
  EXPECT_EQ(BuildDirectEnvelope(text, resend, sender, ThreadChannel::E2ePublic, "account:carol", {}, 1).relay_wake_hint,
            "");
  EXPECT_EQ(BuildGroupMemberEnvelope(text, resend, sender, "group:hike", "account:carol", {}, 1).relay_wake_hint, "");

  ThreadMessage attachment = text;
  attachment.content_type = ChatContentType::Attachment;
  attachment.payload_json = R"({"attachment":{}})";
  const SendRelayOptions rich = copy_ask::ResendOptions(attachment);
  EXPECT_EQ(rich.content_type, std::optional<ChatContentType>(ChatContentType::Attachment));
  EXPECT_EQ(rich.payload_json, std::optional<std::string>(attachment.payload_json)) << "sealed with what it carried";
  EXPECT_FALSE(RaisesWakePush(attachment, rich));
}

} // namespace
} // namespace pbr
