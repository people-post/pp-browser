// Editing a sent message (DEV/PLAN-message-edit.md): who may edit which message, until when; how an edit is judged
// where it arrives; what an applied edit leaves in the row.
#include "lib/messaging/EditMessage.h"

#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/delivery/TextSpans.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr int64_t kSentAt = 1'000'000;
constexpr int64_t kMinute = 60'000;

ThreadMessage Sent() {
  ThreadMessage message;
  message.id = "m1";
  message.thread_id = "t1";
  message.sender_contact_id = kLocalSelfContactId;
  message.content_type = ChatContentType::Text;
  message.text = "see you at 8";
  message.relay_visible = true;
  message.delivery = MessageDelivery::Relayed;
  message.sender_seq = 7;
  message.timestamp = kSentAt;
  return message;
}

TEST(CanEditMessageTest, OurOwnSentTextWithinFifteenMinutes) {
  EXPECT_TRUE(CanEditMessage(Sent(), ThreadKind::Direct, kSentAt));
  EXPECT_TRUE(CanEditMessage(Sent(), ThreadKind::Group, kSentAt + 5 * kMinute));
  EXPECT_TRUE(CanEditMessage(Sent(), ThreadKind::Direct, kSentAt + 14 * kMinute + 59'000));
  EXPECT_FALSE(CanEditMessage(Sent(), ThreadKind::Direct, kSentAt + 15 * kMinute + 1'000));
}

TEST(CanEditMessageTest, SomeoneElsesMessageIsNot) {
  ThreadMessage theirs = Sent();
  theirs.sender_contact_id = "account:peer";
  EXPECT_FALSE(CanEditMessage(theirs, ThreadKind::Direct, kSentAt));
  ThreadMessage assistant = Sent();
  assistant.sender_contact_id = kAiAssistantContactId;
  EXPECT_FALSE(CanEditMessage(assistant, ThreadKind::Direct, kSentAt));
}

TEST(CanEditMessageTest, OnlyText) {
  for (const ChatContentType type : {ChatContentType::Attachment, ChatContentType::ContactCard,
                                     ChatContentType::CryptoTx, ChatContentType::System, ChatContentType::Annotation,
                                     ChatContentType::Deleted, ChatContentType::Unsupported}) {
    ThreadMessage other = Sent();
    other.content_type = type;
    EXPECT_FALSE(CanEditMessage(other, ThreadKind::Direct, kSentAt)) << static_cast<int>(type);
  }
  ThreadMessage rich = Sent();
  rich.content_rml = "<p>card</p>"; // an app-drawn row, not something the user typed
  EXPECT_FALSE(CanEditMessage(rich, ThreadKind::Direct, kSentAt));
}

TEST(CanEditMessageTest, OnlyOnceItWentOut) {
  ThreadMessage queued = Sent();
  queued.delivery = MessageDelivery::Pending;
  EXPECT_FALSE(CanEditMessage(queued, ThreadKind::Direct, kSentAt));
  ThreadMessage failed = Sent();
  failed.delivery = MessageDelivery::Failed;
  EXPECT_FALSE(CanEditMessage(failed, ThreadKind::Direct, kSentAt));
  ThreadMessage local_only = Sent();
  local_only.relay_visible = false; // an in-chat question kept on this device
  local_only.delivery = MessageDelivery::Local;
  EXPECT_FALSE(CanEditMessage(local_only, ThreadKind::Direct, kSentAt));
  ThreadMessage from_sibling = Sent();
  from_sibling.sender_seq.reset();
  from_sibling.sender_device = "QmSibling"; // sent by another of our devices
  EXPECT_TRUE(CanEditMessage(from_sibling, ThreadKind::Direct, kSentAt));
}

TEST(CanEditMessageTest, NeverAnAiQuestionNorAnythingInAnAiConversation) {
  ThreadMessage asked = Sent();
  asked.ai_invoke_mode = "shared_full";
  EXPECT_FALSE(CanEditMessage(asked, ThreadKind::Direct, kSentAt));
  EXPECT_FALSE(CanEditMessage(Sent(), ThreadKind::Ai, kSentAt));
  ThreadMessage local_action = Sent();
  local_action.generation = kLocalActionGeneration;
  EXPECT_FALSE(CanEditMessage(local_action, ThreadKind::Direct, kSentAt));
}

// ---- Judging an edit where it arrives (the five conditions) ----

ThreadMessage Received(const std::string& sender = "account:peer") {
  ThreadMessage message = Sent();
  message.sender_contact_id = sender;
  return message;
}

ThreadMessage Edit(const std::string& id, const std::string& text, int64_t at,
                   const std::string& sender = "account:peer", const std::string& target = "m1") {
  ThreadMessage edit;
  edit.id = id;
  edit.thread_id = "t1";
  edit.sender_contact_id = sender;
  edit.content_type = ChatContentType::Annotation;
  edit.target_message_id = target;
  edit.text = text;
  edit.payload_json = BuildEditPayloadJson(target, text);
  edit.timestamp = at;
  return edit;
}

TEST(CheckEditTest, TheAuthorsEditWithinTheWindowPasses) {
  EXPECT_EQ(CheckEdit(Edit("e1", "see you at 9", kSentAt + kMinute), Received()), EditRefusal::None);
  // 15 minutes and the one minute of slack.
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt + 16 * kMinute), Received()), EditRefusal::None);
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt + 16 * kMinute + 1), Received()), EditRefusal::TooLate);
}

TEST(CheckEditTest, OnlyFromTheMessagesOwnSenderInAChatOrAGroup) {
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt, "account:mallory"), Received()), EditRefusal::NotFromAuthor);
  // A group: another member, or an admin / the owner, cannot edit a member's message.
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt, "account:carol"), Received("account:bob")), EditRefusal::NotFromAuthor);
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt, "account:owner"), Received("account:bob")), EditRefusal::NotFromAuthor);
  // A peer cannot edit ours.
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt), Sent()), EditRefusal::NotFromAuthor);
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt, kLocalSelfContactId), Sent()), EditRefusal::None);
}

TEST(CheckEditTest, OnlyALiveTextMessage) {
  ThreadMessage picture = Received();
  picture.content_type = ChatContentType::Attachment;
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt), picture), EditRefusal::NotText);
  ThreadMessage deleted = Received();
  deleted.content_type = ChatContentType::Deleted;
  deleted.text.clear();
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt), deleted), EditRefusal::TargetDeleted);
  EXPECT_EQ(CheckEdit(Edit("e1", "x", kSentAt, "account:peer", "other"), Received()), EditRefusal::NotEdit);
}

TEST(CheckEditTest, TheTextMustBeThereAndNotTooLong) {
  EXPECT_EQ(CheckEdit(Edit("e1", "", kSentAt), Received()), EditRefusal::EmptyText);
  EXPECT_EQ(CheckEdit(Edit("e1", std::string(kMaxEditTextBytes + 1, 'a'), kSentAt), Received()),
            EditRefusal::TooLong);
  EXPECT_EQ(CheckEdit(Edit("e1", std::string(kMaxEditTextBytes, 'a'), kSentAt), Received()), EditRefusal::None);
}

TEST(CheckEditTest, ANewerEditByItsOwnTimeThenByIdWins) {
  const ThreadMessage first = WithEdit(Received(), Edit("e5", "nine", kSentAt + 2 * kMinute));
  EXPECT_EQ(CheckEdit(Edit("e1", "ten", kSentAt + 3 * kMinute), first), EditRefusal::None);
  EXPECT_EQ(CheckEdit(Edit("e9", "ten", kSentAt + kMinute), first), EditRefusal::NotNewer);
  // Same time: the larger id is the newer one.
  EXPECT_EQ(CheckEdit(Edit("e6", "ten", kSentAt + 2 * kMinute), first), EditRefusal::None);
  EXPECT_EQ(CheckEdit(Edit("e4", "ten", kSentAt + 2 * kMinute), first), EditRefusal::NotNewer);
  EXPECT_EQ(CheckEdit(Edit("e5", "nine", kSentAt + 2 * kMinute), first), EditRefusal::NotNewer);
}

// A device that was reinstalled numbers from 1 again: the seq plays no part, only the send time.
TEST(CheckEditTest, TheSeqPlaysNoPart) {
  ThreadMessage applied = Edit("e5", "nine", kSentAt + 2 * kMinute);
  applied.sender_seq = 900;
  const ThreadMessage first = WithEdit(Received(), applied);
  ThreadMessage after_reinstall = Edit("e1", "ten", kSentAt + 3 * kMinute);
  after_reinstall.sender_seq = 1;
  EXPECT_EQ(CheckEdit(after_reinstall, first), EditRefusal::None);
}

TEST(EditSendTimeTest, NeverBeforeTheEditThisDeviceAlreadyHas) {
  EXPECT_EQ(EditSendTime(kSentAt + kMinute, Sent()), kSentAt + kMinute);
  // A slow clock here: the edit still goes after the one we received from our other device.
  const ThreadMessage edited = WithEdit(Sent(), Edit("e1", "nine", kSentAt + 5 * kMinute, kLocalSelfContactId));
  EXPECT_EQ(EditSendTime(kSentAt + 2 * kMinute, edited), kSentAt + 5 * kMinute + 1);
  EXPECT_EQ(EditSendTime(kSentAt + 9 * kMinute, edited), kSentAt + 9 * kMinute);
}

TEST(WithEditTest, TheRowKeepsItsPlaceAndGetsTheNewTextAndTheMark) {
  ThreadMessage target = Received();
  target.display_order = 42;
  const ThreadMessage edited = WithEdit(target, Edit("e1", "see you at 9", kSentAt + kMinute));
  EXPECT_EQ(edited.id, "m1");
  EXPECT_EQ(edited.display_order, 42);
  EXPECT_EQ(edited.timestamp, kSentAt);
  EXPECT_EQ(edited.sender_seq, target.sender_seq);
  EXPECT_EQ(edited.text, "see you at 9");
  const auto mark = EditMarkOf(edited);
  ASSERT_TRUE(mark.has_value());
  EXPECT_EQ(mark->edited_at, kSentAt + kMinute);
  EXPECT_EQ(mark->edited_by, "e1");
  EXPECT_FALSE(EditMarkOf(target).has_value());
}

TEST(WithEditTest, AMessageWithMentionsBecomesPlainText) {
  ThreadMessage target = Received();
  target.text = "@Bob see you";
  TextSpan mention;
  mention.kind = static_cast<uint8_t>(TextSpanKind::Mention);
  mention.start = 0;
  mention.end = 4;
  mention.account_id = "account:bob";
  target.payload_json = text_spans::ToPayloadJson(target.text, {mention});
  ASSERT_FALSE(text_spans::FromPayloadJson(target.payload_json).empty());
  const ThreadMessage edited = WithEdit(target, Edit("e1", "@Bob see you at 9", kSentAt + kMinute));
  EXPECT_EQ(edited.content_type, ChatContentType::Text);
  EXPECT_TRUE(text_spans::FromPayloadJson(edited.payload_json).empty());
  EXPECT_TRUE(EditMarkOf(edited).has_value());
}

TEST(EditPayloadTest, AnyTextSurvivesTheAnnotationJson) {
  const std::string text = "line one\nline \"two\"\t\\ end";
  auto fields = ChatPayloadCodec::DecodeAnnotationJson(BuildEditPayloadJson("m1", text));
  ASSERT_TRUE(fields);
  EXPECT_EQ(fields->annotation_type, kAnnotationTypeEdit);
  EXPECT_EQ(fields->target_message_id, "m1");
  EXPECT_EQ(fields->value, text);
}

TEST(BlankedEditTest, KeepsTheRowButNoText) {
  const ThreadMessage blank = BlankedEdit(Edit("e1", "secret words", kSentAt));
  EXPECT_EQ(blank.id, "e1");
  EXPECT_EQ(blank.content_type, ChatContentType::Annotation);
  EXPECT_EQ(blank.target_message_id, std::optional<std::string>("m1"));
  EXPECT_EQ(blank.timestamp, kSentAt);
  EXPECT_TRUE(blank.text.empty());
  EXPECT_EQ(blank.payload_json.find("secret"), std::string::npos);
  EXPECT_TRUE(IsEditAnnotation(blank));
  EXPECT_EQ(EditTextOf(blank), std::optional<std::string>(""));
  EXPECT_EQ(CheckEdit(blank, Received()), EditRefusal::EmptyText);
}

} // namespace
} // namespace pbr
