// Editing a sent message in the chat screen (DEV/PLAN-message-edit.md): the composer's edit state per chat (nothing the
// user typed is lost unless they cancel), and the mark an edited message shows while its edit has not gone out.
#include "feature/presenters/MessageEdit.h"

#include "lib/messaging/EditMessage.h"
#include "lib/messaging/QuoteReply.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace pbr {
namespace {

TEST(ChatEditsTest, EditingPutsTheWordsInTheComposerAndKeepsTheDraft) {
  ChatEdits edits;
  EXPECT_EQ(edits.Begin("chat-a", "m1", "see you at 8", "half a thought"), "see you at 8");
  const EditingMessage* editing = edits.In("chat-a");
  ASSERT_NE(editing, nullptr);
  EXPECT_EQ(editing->message_id, "m1");
  EXPECT_EQ(editing->draft_before, "half a thought");
  EXPECT_EQ(edits.In("chat-b"), nullptr);
}

TEST(ChatEditsTest, SentOrUnchangedTheDraftComesBack) {
  ChatEdits edits;
  edits.Begin("chat-a", "m1", "see you at 8", "half a thought");
  const ChatEdits::Submit changed = edits.Prepare("chat-a", "see you at 9 ");
  EXPECT_FALSE(changed.unchanged);
  EXPECT_EQ(changed.text, "see you at 9");
  EXPECT_EQ(edits.End("chat-a"), "half a thought");
  EXPECT_EQ(edits.In("chat-a"), nullptr);

  edits.Begin("chat-a", "m1", "see you at 8", "");
  EXPECT_TRUE(edits.Prepare("chat-a", " see you at 8").unchanged) << "nothing changed: nothing is sent";
}

TEST(ChatEditsTest, TheUsersCancelBringsTheDraftBack) {
  ChatEdits edits;
  edits.Begin("chat-a", "m1", "see you at 8", "half a thought");
  EXPECT_EQ(edits.End("chat-a"), "half a thought");
  EXPECT_EQ(edits.In("chat-a"), nullptr);
}

// Chat A edited halfway, then chat B: B's own state is untouched; back in A it is still editing, with A's draft kept.
// (The half-edited words themselves are A's composer text, kept per chat by ThreadDrafts.)
TEST(ChatEditsTest, AnotherChatDoesNotEndTheEditOrTouchItsState) {
  ChatEdits edits;
  edits.Begin("chat-a", "m1", "see you at 8", "a's draft");
  EXPECT_EQ(edits.In("chat-b"), nullptr) << "B is not editing";
  EXPECT_TRUE(edits.Prepare("chat-b", "anything").text.empty()) << "and B's send is an ordinary one";
  const EditingMessage* back = edits.In("chat-a");
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->message_id, "m1");
  EXPECT_EQ(back->draft_before, "a's draft");
}

// Editing A, then "edit B" (however it was reached): refused; A's half-edited words and the draft before stay.
TEST(ChatEditsTest, WhileEditingAMessageAnotherOneIsNotStarted) {
  ChatEdits edits;
  edits.Begin("chat-a", "m1", "first", "the real draft");
  EXPECT_EQ(edits.Begin("chat-a", "m2", "second", "first, half edited"), std::nullopt)
      << "no text for the composer: it keeps the words being edited";
  EXPECT_EQ(edits.In("chat-a")->message_id, "m1");
  EXPECT_EQ(edits.In("chat-a")->original, "first");
  EXPECT_EQ(edits.End("chat-a"), "the real draft");
  EXPECT_EQ(edits.Begin("chat-a", "m2", "second", "the real draft"), "second") << "once it ended, another may start";
}

// The message went away while edited: with no draft before, the typed words stay; with one, it comes back.
TEST(ChatEditsTest, WhenTheMessageIsDeletedTheTypedWordsOrTheDraftStay) {
  ChatEdits edits;
  edits.Begin("chat-a", "m1", "see you at 8", "");
  EXPECT_EQ(edits.EndTargetGone("chat-a", "see you at 9"), "see you at 9");
  EXPECT_EQ(edits.In("chat-a"), nullptr);
  edits.Begin("chat-a", "m1", "see you at 8", "half a thought");
  EXPECT_EQ(edits.EndTargetGone("chat-a", "see you at 9"), "half a thought");
}

// A quote reply: only the reply is edited; the quote goes back under it exactly as it was sent.
TEST(ChatEditsTest, AQuoteReplyEditsOnlyItsReply) {
  ChatEdits edits;
  const std::string sent = ComposeQuoteReply("yes", "are you coming?");
  EXPECT_EQ(edits.Begin("chat-a", "m1", sent, ""), "yes");
  const ChatEdits::Submit submit = edits.Prepare("chat-a", "yes, at 9");
  const auto parts = SplitQuoteReply(submit.text);
  ASSERT_TRUE(parts);
  EXPECT_EQ(parts->reply, "yes, at 9");
  EXPECT_EQ(parts->quote, "are you coming?");
}

// An edit carries its text twice, so text near a message's limit does not fit as an edit; a quote reply's quote is part
// of the text sent. Prepare says so (Send then changes nothing and stays in the edit).
TEST(ChatEditsTest, AnEditTooLongForOnePayloadIsRefusedQuoteIncluded) {
  size_t fits = kMaxEditTextBytes;
  while (fits > 0 && !EditFitsOnTheWire("m1", std::string(fits, 'a'))) {
    --fits;
  }
  ASSERT_LT(fits, kMaxComposeTextBytes);
  ChatEdits edits;
  edits.Begin("chat-a", "m1", "see you at 8", "");
  EXPECT_FALSE(edits.Prepare("chat-a", std::string(fits, 'a')).too_long) << "just fits: sent";
  const ChatEdits::Submit over = edits.Prepare("chat-a", std::string(fits + 1, 'a'));
  EXPECT_TRUE(over.too_long);
  EXPECT_FALSE(over.unchanged);
  ASSERT_NE(edits.In("chat-a"), nullptr) << "still editing";
  edits.End("chat-a");

  // A quote reply: what is typed fits on its own, but not with the quote that goes under it.
  const std::string typed(fits - 100, 'a');
  edits.Begin("chat-a", "m1", ComposeQuoteReply("yes", std::string(300, 'q')), "");
  const ChatEdits::Submit with_quote = edits.Prepare("chat-a", typed);
  EXPECT_TRUE(with_quote.too_long);
  EXPECT_GT(with_quote.text.size(), typed.size());
  EXPECT_TRUE(EditFitsOnTheWire("m1", typed));
}

TEST(ChatEditsTest, AClosedChatForgetsItsEdit) {
  ChatEdits edits;
  edits.Begin("chat-a", "m1", "see you at 8", "");
  edits.Forget("chat-a");
  EXPECT_EQ(edits.In("chat-a"), nullptr);
}

// ---- 9a: an edit that has not gone out shows on its message ----

constexpr int64_t kAt = 1'000'000;

ThreadMessage EditedOurs(const std::string& edit_id) {
  ThreadMessage message;
  message.id = "m1";
  message.thread_id = "t1";
  message.sender_contact_id = kLocalSelfContactId;
  message.content_type = ChatContentType::Text;
  message.text = "see you at 9";
  message.delivery = MessageDelivery::Relayed;
  message.payload_json = R"({"edited_at":1000000,"edited_by":")" + edit_id + R"("})";
  return message;
}

ThreadMessage EditRow(MessageDelivery delivery) {
  ThreadMessage edit;
  edit.id = "e1";
  edit.thread_id = "t1";
  edit.sender_contact_id = kLocalSelfContactId;
  edit.content_type = ChatContentType::Annotation;
  edit.target_message_id = "m1";
  edit.text = "see you at 9";
  edit.payload_json = BuildEditPayloadJson("m1", "see you at 9");
  edit.timestamp = kAt;
  edit.delivery = delivery;
  edit.relay_visible = true;
  return edit;
}

Thread Chat(ThreadKind kind = ThreadKind::Direct, ThreadChannel channel = ThreadChannel::E2ePublic) {
  Thread thread;
  thread.id = "t1";
  thread.kind = kind;
  thread.channel = channel;
  return thread;
}

TEST(EditSendMarkTest, TheEditedMessageReadsItsEditsSend) {
  EXPECT_EQ(EditRowIdOf(EditedOurs("e1")), std::optional<std::string>("e1"));
  ThreadMessage theirs = EditedOurs("e1");
  theirs.sender_contact_id = "account:bob";
  EXPECT_FALSE(EditRowIdOf(theirs)) << "only ours carries a send mark";
  ThreadMessage plain = EditedOurs("e1");
  plain.payload_json.clear();
  EXPECT_FALSE(EditRowIdOf(plain));
}

TEST(EditSendMarkTest, AClockWhileQueuedRedWhenItFailedNothingOnceOut) {
  EXPECT_EQ(EditSendMark(EditRow(MessageDelivery::Pending), kAt, kAt + 10'000), SendMark::Queued);
  EXPECT_EQ(EditSendMark(EditRow(MessageDelivery::Pending), kAt, kAt + kQueuedMarkLimitMs + 1), SendMark::Failed);
  EXPECT_EQ(EditSendMark(EditRow(MessageDelivery::Failed), std::nullopt, kAt + 10'000), SendMark::Failed);
  EXPECT_EQ(EditSendMark(EditRow(MessageDelivery::Relayed), std::nullopt, kAt + 10'000), SendMark::None);
}

TEST(EditSendMarkTest, ATapOnTheRedMarkSendsTheEditAgainInPublicChats) {
  const auto failed = std::optional<ThreadMessage>(EditRow(MessageDelivery::Failed));
  EXPECT_EQ(DecideEditResendTap(failed, Chat(), std::nullopt, kAt + 10'000).kind, SendMarkTap::Kind::Resent);
  EXPECT_EQ(DecideEditResendTap(failed, Chat(ThreadKind::Group, ThreadChannel::None), std::nullopt, kAt).kind,
            SendMarkTap::Kind::Resent);
  EXPECT_EQ(DecideEditResendTap(failed, Chat(), kAt, kAt + 10'000).kind, SendMarkTap::Kind::StillTrying);
  const SendMarkTap secure = DecideEditResendTap(failed, Chat(ThreadKind::Direct, ThreadChannel::E2e), std::nullopt, kAt);
  EXPECT_EQ(secure.kind, SendMarkTap::Kind::ReasonOnly);
  EXPECT_EQ(secure.refusal, SendMarkTap::Refusal::ThisKind);
  const auto out = std::optional<ThreadMessage>(EditRow(MessageDelivery::Relayed));
  EXPECT_EQ(DecideEditResendTap(out, Chat(), std::nullopt, kAt).kind, SendMarkTap::Kind::Nothing);
  EXPECT_EQ(DecideEditResendTap(std::nullopt, Chat(), std::nullopt, kAt).kind, SendMarkTap::Kind::Nothing);
}

} // namespace
} // namespace pbr
