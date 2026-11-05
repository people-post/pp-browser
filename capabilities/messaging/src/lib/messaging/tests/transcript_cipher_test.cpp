#include "foundation/crypto/CryptoConstants.h"
#include "lib/messaging/TranscriptBodyCodec.h"
#include "lib/messaging/TranscriptCipher.h"
#include "lib/messaging/GroupMembershipCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize, 0x42);
  return dek;
}

TEST(TranscriptBodyCodecTest, RoundTripPreservesFields) {
  TranscriptBodyPlaintext body;
  body.chat_payload = {0x01, 0x00, 0x05, 'h', 'i'};
  body.text = "visible";
  body.payload_json = R"({"control_type":"system"})";
  body.content_rml = "<p>rml</p>";
  body.chat_actions.push_back(TranscriptChatAction{.label = "Go", .message = "do it"});
  body.working_set_json = R"({"candidates":[{"block_index":1,"kind":"long_list","affinity":"feed","auto_open":true,"title":"T","subtitle":"1 item","artifact_rml":"<div/>","teaser_rml":"<button/>"}]})";
  body.ai_image_json = R"({"content_hash":"ab","mime":"image/jpeg","width":4,"height":3})";

  auto encoded = TranscriptBodyCodec::Encode(body);
  ASSERT_TRUE(encoded);
  auto decoded = TranscriptBodyCodec::Decode(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->chat_payload, body.chat_payload);
  EXPECT_EQ(decoded->text, body.text);
  EXPECT_EQ(decoded->payload_json, body.payload_json);
  ASSERT_TRUE(decoded->content_rml);
  EXPECT_EQ(*decoded->content_rml, *body.content_rml);
  ASSERT_EQ(decoded->chat_actions.size(), 1u);
  EXPECT_EQ(decoded->chat_actions.front().label, "Go");
  ASSERT_TRUE(decoded->working_set_json);
  EXPECT_EQ(*decoded->working_set_json, *body.working_set_json);
  ASSERT_TRUE(decoded->ai_image_json);
  EXPECT_EQ(*decoded->ai_image_json, *body.ai_image_json);
}

TEST(TranscriptBodyCodecTest, MessageWithoutAiImageStaysWithout) {
  TranscriptBodyPlaintext body;
  body.chat_payload = {0x01, 0x00, 0x05, 'h', 'i'};
  auto encoded = TranscriptBodyCodec::Encode(body);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(std::string(encoded->begin(), encoded->end()).find("ai_image_json"), std::string::npos);
  auto decoded = TranscriptBodyCodec::Decode(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(decoded->ai_image_json);
}

TEST(TranscriptBodyCodecTest, AnAnsweredInvitationStaysAnsweredWhenReadBack) {
  // Kenneth 2026-11-04 (PLAN-ui-refresh-impl 62.2): after Accept / Decline the card came back, whole width, no buttons.
  GroupInvitePayload invite;
  invite.group_id = "group:hike";
  invite.group_title = "Hike";
  invite.inviter_identity = "acct-ann";
  invite.invitee_identity = "acct-me";
  invite.invite_nonce = "n1";
  const auto detail = GroupMembershipCodec::EncodeInvite(invite);
  ASSERT_TRUE(detail);
  auto message = GroupMembershipCodec::BuildSystemMessage("t", GroupMembershipControlType::GroupInvite,
                                                          "Group invitation: Hike", *detail, "acct-ann");
  ASSERT_TRUE(message);
  message->chat_actions = GroupMembershipCodec::BuildInviteChatActions(invite);
  GroupMembershipCodec::ApplyInviteResolution(*message, InviteStatus::Declined, "You declined Hike");

  auto body = TranscriptBodyCodec::FromMessage(*message);
  ASSERT_TRUE(body);
  auto encoded = TranscriptBodyCodec::Encode(*body);
  ASSERT_TRUE(encoded);
  auto decoded = TranscriptBodyCodec::Decode(*encoded);
  ASSERT_TRUE(decoded);
  ThreadMessage read;
  read.id = message->id;
  read.thread_id = "t";
  ASSERT_TRUE(TranscriptBodyCodec::ApplyToMessage(*decoded, read));
  EXPECT_EQ(read.content_type, ChatContentType::System);
  EXPECT_EQ(GroupMembershipCodec::InviteResolutionFromMessage(read), InviteStatus::Declined);
  ASSERT_TRUE(GroupMembershipCodec::DecodeInviteFromMessage(read)) << "the invitation's detail is still there";
  EXPECT_EQ(GroupMembershipCodec::DecodeInviteFromMessage(read)->group_title, "Hike");
  EXPECT_TRUE(read.chat_actions.empty());

  // A system message nobody answered reads back without one.
  auto pending = GroupMembershipCodec::BuildSystemMessage("t", GroupMembershipControlType::GroupInvite,
                                                          "Group invitation: Hike", *detail, "acct-ann");
  ASSERT_TRUE(pending);
  auto pending_body = TranscriptBodyCodec::FromMessage(*pending);
  ASSERT_TRUE(pending_body);
  ThreadMessage pending_read;
  ASSERT_TRUE(TranscriptBodyCodec::ApplyToMessage(*pending_body, pending_read));
  EXPECT_FALSE(GroupMembershipCodec::InviteResolutionFromMessage(pending_read));
}

TEST(TranscriptCipherTest, MessageBodyRoundTripAndAadBind) {
  const ByteVector dek = TestDek();
  const ByteVector plain = {0x01, 0x02, 0x03};
  auto cipher = TranscriptCipher::EncryptMessageBody(dek, "profile-a", "thread-1", "msg-1", plain);
  ASSERT_TRUE(cipher);
  auto round = TranscriptCipher::DecryptMessageBody(dek, "profile-a", "thread-1", "msg-1", *cipher);
  ASSERT_TRUE(round);
  EXPECT_EQ(*round, plain);

  auto wrong_thread =
      TranscriptCipher::DecryptMessageBody(dek, "profile-a", "thread-2", "msg-1", *cipher);
  EXPECT_FALSE(static_cast<bool>(wrong_thread));
}

TEST(TranscriptCipherTest, PreviewRoundTrip) {
  const ByteVector dek = TestDek();
  auto cipher = TranscriptCipher::EncryptPreview(dek, "profile-a", "thread-1", "hello preview");
  ASSERT_TRUE(cipher);
  auto plain = TranscriptCipher::DecryptPreview(dek, "profile-a", "thread-1", *cipher);
  ASSERT_TRUE(plain);
  EXPECT_EQ(*plain, "hello preview");
}

TEST(TranscriptCipherTest, MemoryRoundTrip) {
  const ByteVector dek = TestDek();
  auto cipher = TranscriptCipher::EncryptMemoryValue(dek, "profile-a", "thread-1", "summary", "facts");
  ASSERT_TRUE(cipher);
  auto plain = TranscriptCipher::DecryptMemoryValue(dek, "profile-a", "thread-1", "summary", *cipher);
  ASSERT_TRUE(plain);
  EXPECT_EQ(*plain, "facts");
}

} // namespace
} // namespace pbr
