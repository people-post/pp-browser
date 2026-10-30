#include "lib/messaging/SentCopy.h"

#include "lib/messaging/ChatPayloadCodec.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

ThreadMessage SentText(const std::string& text) {
  ThreadMessage sent;
  sent.id = "m-1";
  sent.content_type = ChatContentType::Text;
  sent.text = text;
  sent.timestamp = 1'761'235'200'000;
  return sent;
}

/** The notice as a sibling reads it: through the wire's ChatPayload (a system payload keeps only its type and detail). */
ThreadMessage OverTheWire(const ThreadMessage& notice) {
  ThreadMessage read;
  EXPECT_TRUE(ChatPayloadCodec::ApplyRowToMessage(ChatPayloadCodec::EncodeToRow(notice).value(), read));
  return read;
}

TEST(SentCopyTest, ADirectMessageCarriesItsContentAndPeer) {
  auto built = sent_copy::BuildNotice(SentText("on my phone"), SentCopyTarget{.peer_account = "account:bob"});
  ASSERT_TRUE(built) << built.error().message;
  EXPECT_NE(built->id, "m-1") << "the notice is a message of its own";
  const ThreadMessage notice_read = OverTheWire(*built);
  const ThreadMessage* notice = &notice_read;
  EXPECT_EQ(notice->content_type, ChatContentType::System);
  auto copy = sent_copy::Parse(*notice);
  ASSERT_TRUE(copy) << copy.error().message;
  ASSERT_TRUE(copy->has_value());
  EXPECT_EQ((*copy)->target.peer_account, "account:bob");
  EXPECT_TRUE((*copy)->target.group_id.empty());
  EXPECT_EQ((*copy)->message.id, "m-1");
  EXPECT_EQ((*copy)->message.text, "on my phone");
  EXPECT_EQ((*copy)->message.timestamp, 1'761'235'200'000);
  EXPECT_EQ((*copy)->message.content_type, ChatContentType::Text);
}

TEST(SentCopyTest, AGroupMessageCarriesItsGroup) {
  auto notice = sent_copy::BuildNotice(SentText("hi all"), SentCopyTarget{.group_id = "group:hike"});
  ASSERT_TRUE(notice);
  auto copy = sent_copy::Parse(OverTheWire(*notice));
  ASSERT_TRUE(copy && copy->has_value());
  EXPECT_EQ((*copy)->target.group_id, "group:hike");
}

TEST(SentCopyTest, ControlsAndReactionsAreNotCopied) {
  ThreadMessage control = SentText("x");
  control.content_type = ChatContentType::System;
  EXPECT_FALSE(sent_copy::Mirrored(control));
  ThreadMessage reaction = SentText("");
  reaction.content_type = ChatContentType::Annotation;
  EXPECT_FALSE(sent_copy::Mirrored(reaction));
  EXPECT_TRUE(sent_copy::Mirrored(SentText("hello")));
  EXPECT_FALSE(sent_copy::BuildNotice(SentText("x"), SentCopyTarget{}));
  EXPECT_FALSE(sent_copy::BuildNotice(SentText("x"), SentCopyTarget{.peer_account = "a", .group_id = "g"}));
}

TEST(SentCopyTest, OtherSystemMessagesAreNotCopiesAndBrokenOnesAreRefused) {
  ThreadMessage device_list;
  device_list.content_type = ChatContentType::System;
  device_list.payload_json = R"({"control_type":"device_list"})";
  auto other = sent_copy::Parse(device_list);
  ASSERT_TRUE(other);
  EXPECT_FALSE(other->has_value());

  ThreadMessage broken = device_list;
  broken.payload_json =
      R"({"control_type":"sent_copy","detail":"{\"to_account\":\"account:bob\",\"message_id\":\"m\",\"row_b64\":\"\"}"})";
  EXPECT_FALSE(sent_copy::Parse(broken)) << "no content";
}

} // namespace
} // namespace pbr
