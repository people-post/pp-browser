// #210: the mark beside a message we sent — a clock while it is being sent (at most a minute), a red mark when it did
// not go out, nothing once it is out (DEV/PLAN-message-send-status.md).
#include "feature/presenters/SendMark.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kSentAt = 1'000'000;

ThreadMessage Ours(const MessageDelivery delivery, const ChatContentType type = ChatContentType::Text) {
  ThreadMessage message;
  message.id = "m1";
  message.thread_id = "t";
  message.sender_contact_id = kLocalSelfContactId;
  message.content_type = type;
  message.text = "hi";
  message.timestamp = kSentAt;
  message.delivery = delivery;
  message.relay_visible = true;
  return message;
}

struct MarkCase {
  const char* name;
  ThreadMessage message;
  std::optional<int64_t> started_ms;
  int64_t now_ms;
  SendMark expected;
};

TEST(SendMarkTest, EachStateGetsItsMark) {
  ThreadMessage theirs = Ours(MessageDelivery::Failed);
  theirs.sender_contact_id = "account:bob";
  ThreadMessage local_line = Ours(MessageDelivery::Pending);
  local_line.relay_visible = false;
  ThreadMessage ai_question = Ours(MessageDelivery::Local);
  ThreadMessage ai_answer = Ours(MessageDelivery::Pending);
  ai_answer.sender_contact_id = kAiAssistantContactId;
  ThreadMessage rendered = Ours(MessageDelivery::Pending);
  rendered.content_rml = "<p>hi</p>";
  ThreadMessage local_action = Ours(MessageDelivery::Pending);
  local_action.generation = kLocalActionGeneration;
  const int64_t started = kSentAt + 10;

  const std::vector<MarkCase> cases = {
      {"new row, its send not registered yet", Ours(MessageDelivery::Pending), std::nullopt, kSentAt + 5'000,
       SendMark::Queued},
      {"pending past the grace with nothing under way (a restart)", Ours(MessageDelivery::Pending), std::nullopt,
       kSentAt + 5'001, SendMark::Failed},
      {"pending, under way", Ours(MessageDelivery::Pending), started, started + 30'000, SendMark::Queued},
      {"failed, a retry under way, 59 999 ms", Ours(MessageDelivery::Failed), started, started + 59'999,
       SendMark::Queued},
      {"failed, a retry under way, 60 000 ms", Ours(MessageDelivery::Failed), started, started + 60'000,
       SendMark::Queued},
      {"failed, a retry under way, 60 001 ms", Ours(MessageDelivery::Failed), started, started + 60'001,
       SendMark::Failed},
      {"pending, under way past the minute", Ours(MessageDelivery::Pending), started, started + 60'001,
       SendMark::Failed},
      {"failed with nothing under way (failed before it was sent, retries used up)", Ours(MessageDelivery::Failed),
       std::nullopt, kSentAt + 1'000, SendMark::Failed},
      {"failed gets no grace", Ours(MessageDelivery::Failed), std::nullopt, kSentAt, SendMark::Failed},
      {"a group message no member got: still pending, nothing under way", Ours(MessageDelivery::Pending), std::nullopt,
       kSentAt + 60'000, SendMark::Failed},
      {"sent", Ours(MessageDelivery::Relayed), started, started + 1, SendMark::None},
      {"sent, later", Ours(MessageDelivery::Relayed), std::nullopt, kSentAt + 600'000, SendMark::None},
      {"local", Ours(MessageDelivery::Local), std::nullopt, kSentAt, SendMark::None},
      {"theirs", theirs, std::nullopt, kSentAt + 600'000, SendMark::None},
      {"never on the wire", local_line, std::nullopt, kSentAt + 600'000, SendMark::None},
      {"an AI question (local)", ai_question, std::nullopt, kSentAt + 600'000, SendMark::None},
      {"an AI answer", ai_answer, std::nullopt, kSentAt + 600'000, SendMark::None},
      {"a drawn AI row", rendered, std::nullopt, kSentAt + 600'000, SendMark::None},
      {"an on-device app action", local_action, std::nullopt, kSentAt + 600'000, SendMark::None},
      {"a system line we sent", Ours(MessageDelivery::Pending, ChatContentType::System), std::nullopt,
       kSentAt + 600'000, SendMark::None},
      {"a reaction", Ours(MessageDelivery::Failed, ChatContentType::Annotation), std::nullopt, kSentAt + 600'000,
       SendMark::None},
      {"a deleted message", Ours(MessageDelivery::Failed, ChatContentType::Deleted), std::nullopt, kSentAt + 600'000,
       SendMark::None},
      {"a picture or file", Ours(MessageDelivery::Failed, ChatContentType::Attachment), std::nullopt, kSentAt + 600'000,
       SendMark::Failed},
      {"a contact card", Ours(MessageDelivery::Failed, ChatContentType::ContactCard), std::nullopt, kSentAt + 600'000,
       SendMark::Failed},
  };
  for (const MarkCase& c : cases) {
    EXPECT_EQ(MarkOf(c.message, c.started_ms, c.now_ms), c.expected) << c.name;
  }
}

TEST(SendMarkTest, TheSoonestClockToTurnRedIsNoted) {
  std::optional<int64_t> earliest;
  // Nothing shown changes on its own: none.
  NoteMarkChange(earliest, Ours(MessageDelivery::Relayed), std::nullopt, kSentAt);
  NoteMarkChange(earliest, Ours(MessageDelivery::Failed), std::nullopt, kSentAt);
  EXPECT_FALSE(earliest);
  // A send under way since kSentAt + 20: red at +60 001.
  NoteMarkChange(earliest, Ours(MessageDelivery::Failed), kSentAt + 20, kSentAt + 30);
  EXPECT_EQ(earliest, kSentAt + 20 + kQueuedMarkLimitMs + 1);
  // A new row without its send yet: red at its time + 5 001, sooner.
  NoteMarkChange(earliest, Ours(MessageDelivery::Pending), std::nullopt, kSentAt + 30);
  EXPECT_EQ(earliest, kSentAt + kQueuedMarkGraceMs + 1);
  // A later one does not move it.
  NoteMarkChange(earliest, Ours(MessageDelivery::Pending), kSentAt + 40, kSentAt + 50);
  EXPECT_EQ(earliest, kSentAt + kQueuedMarkGraceMs + 1);
  // At the noted time the clock is red.
  EXPECT_EQ(MarkOf(Ours(MessageDelivery::Pending), std::nullopt, *earliest), SendMark::Failed);
  EXPECT_EQ(MarkOf(Ours(MessageDelivery::Failed), kSentAt + 20, kSentAt + 20 + kQueuedMarkLimitMs + 1),
            SendMark::Failed);
}

// A group message that had nobody to go to (we were alone: just created, invitees not joined yet) makes no outbox send
// and stays Pending: no clock, no red mark — the red mark means it was meant for someone and did not get there.
TEST(SendMarkTest, AGroupMessageWithNobodyToGoToHasNoMark) {
  const std::vector<int64_t> alone;
  const bool no_recipients = !GroupRowHadRecipients(alone, kSentAt);
  EXPECT_TRUE(no_recipients);
  const ThreadMessage pending = Ours(MessageDelivery::Pending);
  EXPECT_EQ(MarkOf(pending, std::nullopt, kSentAt + 1, no_recipients), SendMark::None) << "right after sending";
  EXPECT_EQ(MarkOf(pending, std::nullopt, kSentAt + 60'001, no_recipients), SendMark::None) << "a minute on";
  std::optional<int64_t> earliest;
  NoteMarkChange(earliest, pending, std::nullopt, kSentAt + 1, no_recipients);
  EXPECT_FALSE(earliest) << "nothing to redraw for";
  // Someone who joined after it was written does not make it a failure.
  EXPECT_FALSE(GroupRowHadRecipients({kSentAt + 1}, kSentAt));
}

TEST(SendMarkTest, AGroupMessageEveryOtherMemberMissedIsRed) {
  // Bob and Carol were members when it was sent (0: join time not known, counted as there); no copy got out: the row
  // stays Pending with nothing under way.
  const bool no_recipients = !GroupRowHadRecipients({kSentAt - 1000, 0}, kSentAt);
  EXPECT_FALSE(no_recipients);
  EXPECT_TRUE(GroupRowHadRecipients({kSentAt}, kSentAt));
  const ThreadMessage pending = Ours(MessageDelivery::Pending);
  EXPECT_EQ(MarkOf(pending, std::nullopt, kSentAt + 1, no_recipients), SendMark::Queued);
  EXPECT_EQ(MarkOf(pending, std::nullopt, kSentAt + 60'001, no_recipients), SendMark::Failed);
  EXPECT_EQ(MarkOf(pending, kSentAt + 10, kSentAt + 30'000, no_recipients), SendMark::Queued) << "copies under way";
}

} // namespace
} // namespace pbr
