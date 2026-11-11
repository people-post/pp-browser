// #210 step 2: a tap on the red mark of our text sends it again as a new message and takes the unsent row away — only
// once its automatic sending has stopped, only in public chats; anything else just says why
// (DEV/PLAN-message-resend.md). The ports run on the in-memory store, as the facade's run on the app's.
#include "feature/presenters/DeliveryNoticePlan.h"
#include "feature/presenters/MessageResend.h"
#include "lib/delivery/DeliveryNotice.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kSentAt = 1'000'000;
constexpr int64_t kNow = kSentAt + 10 * 60'000; // long after: the clock is over

Thread PublicChat(const ThreadKind kind = ThreadKind::Direct) {
  Thread thread;
  thread.id = "t1";
  thread.kind = kind;
  thread.channel = ThreadChannel::E2ePublic;
  thread.title = "Bob";
  thread.updated_at = 1;
  return thread;
}

ThreadMessage Unsent(const std::string& id, const std::string& text = "see you at 8") {
  ThreadMessage message;
  message.id = id;
  message.thread_id = "t1";
  message.sender_contact_id = kLocalSelfContactId;
  message.content_type = ChatContentType::Text;
  message.text = text;
  message.timestamp = kSentAt;
  message.delivery = MessageDelivery::Failed;
  message.relay_visible = true;
  return message;
}

class MessageResendTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(store_.UpsertThread(thread_)); }

  void Add(const ThreadMessage& message) { ASSERT_TRUE(store_.AppendMessage(message)); }

  std::optional<ThreadMessage> Row(const std::string& id) {
    auto row = store_.GetMessage("t1", id);
    return row && *row ? std::optional<ThreadMessage>(**row) : std::nullopt;
  }

  size_t Count() {
    auto page = store_.GetMessagesPage("t1", std::nullopt, 1000);
    return page ? page->size() : 0;
  }

  SendMarkTapFacts Facts(const std::string& id) {
    SendMarkTapFacts facts;
    facts.row = Row(id);
    facts.thread = thread_;
    facts.started_ms = started_ms_;
    facts.now_ms = kNow;
    return facts;
  }

  /** As the facade's: the send stores a new pending row and returns it; the removal is the store's. */
  ResendPorts Ports() {
    ResendPorts ports;
    ports.spans_on_roster = [this](std::vector<TextSpan> spans) {
      std::erase_if(spans, [this](const TextSpan& span) { return span.account_id == left_member_; });
      return spans;
    };
    auto send = [this](const std::string& text, const std::vector<TextSpan>& spans) -> Roe<ThreadMessage> {
      ++sends_;
      if (send_error_) {
        return Error(*send_error_);
      }
      if (deliver_old_on_send_) {
        ThreadMessage old = *Row(*deliver_old_on_send_);
        old.delivery = MessageDelivery::Relayed;
        (void)store_.UpdateMessage(old);
      }
      ThreadMessage fresh = Unsent("new-" + std::to_string(sends_), text);
      fresh.timestamp = kNow;
      fresh.delivery = MessageDelivery::Pending;
      sent_spans_ = spans;
      if (!spans.empty()) {
        fresh.payload_json = text_spans::ToPayloadJson(text, spans);
      }
      return store_.AppendMessage(fresh);
    };
    ports.send_direct = [this, send](const std::string& text) {
      ++direct_sends_;
      return send(text, {});
    };
    ports.send_group = [send](const std::string& text, std::vector<TextSpan> spans) { return send(text, spans); };
    ports.remove_unsent = [this](const std::string& id) { return store_.RemoveUnsentMessage("t1", id); };
    return ports;
  }

  SendMarkTap Tap(const std::string& id) { return ResendAfterTap(Facts(id), Ports()); }

  InMemoryDeliveryStore store_;
  Thread thread_ = PublicChat();
  std::optional<int64_t> started_ms_;
  std::optional<std::string> send_error_;
  std::optional<std::string> deliver_old_on_send_;
  std::string left_member_;
  std::vector<TextSpan> sent_spans_;
  int sends_ = 0;
  int direct_sends_ = 0;
};

TEST_F(MessageResendTest, TapSendsANewMessageThenRemovesTheOldRowAndItsNotice) {
  Add(Unsent("m1"));
  Add(MakeSendFailureRow("t1", "m1", SendFailureWord(SendFailure::RelayUnreachable).c_str(), kSentAt + 1));
  const SendMarkTap tap = Tap("m1");
  ASSERT_EQ(tap.kind, SendMarkTap::Kind::Resent);
  EXPECT_TRUE(tap.old_removed);
  EXPECT_EQ(direct_sends_, 1);
  ASSERT_NE(tap.new_id, "m1");
  const auto fresh = Row(tap.new_id);
  ASSERT_TRUE(fresh) << "the id is the row the send made, not a guess";
  EXPECT_EQ(fresh->text, "see you at 8");
  EXPECT_GT(fresh->timestamp, kSentAt);
  EXPECT_FALSE(Row("m1"));
  EXPECT_FALSE(Row(SendFailureRowId("m1", "")));
  EXPECT_EQ(Count(), 1u);
}

TEST_F(MessageResendTest, ASendErrorLeavesTheOldBubbleAndATapAfterItSendsAgain) {
  Add(Unsent("m1"));
  send_error_ = "Payment required";
  SendMarkTap tap = Tap("m1");
  ASSERT_EQ(tap.kind, SendMarkTap::Kind::SendFailed);
  ASSERT_TRUE(tap.error);
  EXPECT_EQ(tap.error->message, "Payment required");
  ASSERT_TRUE(Row("m1"));
  EXPECT_EQ(Row("m1")->delivery, MessageDelivery::Failed);
  EXPECT_EQ(Count(), 1u);

  send_error_.reset();
  tap = Tap("m1");
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::Resent) << "nothing was left holding it";
  EXPECT_FALSE(Row("m1"));
  EXPECT_EQ(sends_, 2);
}

TEST_F(MessageResendTest, ARowDeliveredInBetweenIsKept) {
  Add(Unsent("m1"));
  deliver_old_on_send_ = "m1";
  const SendMarkTap tap = Tap("m1");
  ASSERT_EQ(tap.kind, SendMarkTap::Kind::Resent);
  EXPECT_FALSE(tap.old_removed);
  ASSERT_TRUE(Row("m1"));
  EXPECT_EQ(Row("m1")->delivery, MessageDelivery::Relayed);
  EXPECT_EQ(Count(), 2u) << "the accepted duplicate";
}

TEST_F(MessageResendTest, ADoubleTapSendsOnce) {
  Add(Unsent("m1"));
  EXPECT_EQ(Tap("m1").kind, SendMarkTap::Kind::Resent);
  EXPECT_EQ(Tap("m1").kind, SendMarkTap::Kind::Nothing) << "the row is gone";
  EXPECT_EQ(sends_, 1);
}

TEST_F(MessageResendTest, WhileARequestIsStillAliveItOnlySaysSo) {
  Add(Unsent("m1"));
  started_ms_ = kNow - 90'000; // red after a minute, a retry still running (a black-holed network)
  const SendMarkTap tap = Tap("m1");
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::StillTrying);
  EXPECT_EQ(sends_, 0);
  EXPECT_TRUE(Row("m1"));
}

TEST_F(MessageResendTest, AQuoteReplyIsSentAsItWas) {
  const std::string quoted = "> where?\nsee you at 8";
  Add(Unsent("m1", quoted));
  const SendMarkTap tap = Tap("m1");
  ASSERT_EQ(tap.kind, SendMarkTap::Kind::Resent);
  EXPECT_EQ(Row(tap.new_id)->text, quoted);
}

TEST_F(MessageResendTest, AGroupTextKeepsItsMentionsStillOnTheRoster) {
  thread_ = PublicChat(ThreadKind::Group);
  ThreadMessage message = Unsent("m1", "@Bob @Carol lunch?");
  TextSpan bob{.kind = static_cast<uint8_t>(TextSpanKind::Mention), .start = 0, .end = 4, .account_id = "bob"};
  TextSpan carol{.kind = static_cast<uint8_t>(TextSpanKind::Mention), .start = 5, .end = 11, .account_id = "carol"};
  message.payload_json = text_spans::ToPayloadJson(message.text, {bob, carol});
  Add(message);
  left_member_ = "carol";
  const SendMarkTap tap = Tap("m1");
  ASSERT_EQ(tap.kind, SendMarkTap::Kind::Resent);
  EXPECT_EQ(direct_sends_, 0) << "a group's send";
  EXPECT_EQ(sent_spans_, (std::vector<TextSpan>{bob})) << "who left meanwhile is not lit";
  EXPECT_EQ(text_spans::FromPayloadJson(Row(tap.new_id)->payload_json), (std::vector<TextSpan>{bob}));
  EXPECT_FALSE(Row("m1"));
}

TEST_F(MessageResendTest, WhatIsNotResentWithATap) {
  const auto decide = [](const ThreadMessage& row, const Thread& thread, const std::optional<int64_t> started = {},
                         const bool no_recipients = false) {
    SendMarkTapFacts facts;
    facts.row = row;
    facts.thread = thread;
    facts.started_ms = started;
    facts.now_ms = kNow;
    facts.no_recipients = no_recipients;
    return DecideSendMarkTap(facts);
  };
  using Kind = SendMarkTap::Kind;
  using Refusal = SendMarkTap::Refusal;
  EXPECT_EQ(decide(Unsent("m1"), PublicChat()).kind, Kind::Resent);
  EXPECT_EQ(decide(Unsent("m1"), PublicChat(ThreadKind::Group)).kind, Kind::Resent);

  Thread private_chat = PublicChat();
  private_chat.channel = ThreadChannel::E2e;
  EXPECT_EQ(decide(Unsent("m1"), private_chat).refusal, Refusal::ThisKind);
  ThreadMessage shared_ai = Unsent("m1", "what time is it in Paris?");
  shared_ai.ai_invoke_mode = "shared_full";
  EXPECT_EQ(decide(shared_ai, PublicChat()).refusal, Refusal::ThisKind);
  for (const ChatContentType type : {ChatContentType::Attachment, ChatContentType::ContactCard}) {
    ThreadMessage other = Unsent("m1");
    other.content_type = type;
    const SendMarkTap tap = decide(other, PublicChat());
    EXPECT_EQ(tap.kind, Kind::ReasonOnly);
    EXPECT_EQ(tap.refusal, Refusal::NotText);
  }

  ThreadMessage sent = Unsent("m1");
  sent.delivery = MessageDelivery::Relayed;
  EXPECT_EQ(decide(sent, PublicChat()).kind, Kind::Nothing);
  ThreadMessage fresh = Unsent("m1");
  fresh.delivery = MessageDelivery::Pending;
  fresh.timestamp = kNow - 1'000;
  EXPECT_EQ(decide(fresh, PublicChat()).kind, Kind::Nothing) << "still the clock";
  ThreadMessage theirs = Unsent("m1");
  theirs.sender_contact_id = "account:bob";
  EXPECT_EQ(decide(theirs, PublicChat()).kind, Kind::Nothing);
  EXPECT_EQ(decide(Unsent("m1"), PublicChat(ThreadKind::Group), std::nullopt, true).kind, Kind::Nothing)
      << "a group message that had nobody to go to has no mark";
  EXPECT_EQ(decide(Unsent("m1"), PublicChat(), kNow - 1'000).kind, Kind::StillTrying);
  SendMarkTapFacts gone;
  gone.thread = PublicChat();
  gone.now_ms = kNow;
  EXPECT_EQ(DecideSendMarkTap(gone).kind, Kind::Nothing);
}

TEST_F(MessageResendTest, ANonTextRedMarkSendsNothing) {
  ThreadMessage picture = Unsent("m1");
  picture.content_type = ChatContentType::Attachment;
  Add(picture);
  const SendMarkTap tap = Tap("m1");
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::ReasonOnly);
  EXPECT_EQ(sends_, 0);
  EXPECT_TRUE(Row("m1"));
}

// --- the outcomes that come after a re-send (ApplyDeliveryNoticeTo) -----------------------------------------------

DeliveryNotice Failed(const std::string& message_id,
                      const DeliveryNotice::Kind kind = DeliveryNotice::Kind::SendFailed) {
  DeliveryNotice notice;
  notice.kind = kind;
  notice.send = SendFailure::RelayUnreachable;
  notice.thread_id = "t1";
  notice.message_id = message_id;
  return notice;
}

TEST_F(MessageResendTest, ALateFailureOfARemovedRowWritesNothing) {
  Add(Unsent("m1"));
  const SendMarkTap tap = Tap("m1");
  ASSERT_TRUE(tap.old_removed);
  ResentMessages resent;
  const size_t before = Count();
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1"), kNow, resent), NoticeTell::Nothing);
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1", DeliveryNotice::Kind::GroupMembersMissed), kNow, resent),
            NoticeTell::Nothing);
  EXPECT_EQ(Count(), before) << "no orphan line";
  EXPECT_FALSE(Row(SendFailureRowId("m1", "")));
  EXPECT_FALSE(Row(SendFailureRowId("m1", kGroupMembersMissedWire)));
  EXPECT_FALSE(Row("m1")) << "and the row is not brought back";
}

TEST_F(MessageResendTest, TheFirstFailureOfAResentMessageIsToldOnce) {
  Add(Unsent("m1"));
  const SendMarkTap tap = Tap("m1");
  ResentMessages resent{{tap.new_id, "t1"}};
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed(tap.new_id), kNow, resent), NoticeTell::ResentFailed)
      << "the pill says why, once";
  EXPECT_TRUE(Row(SendFailureRowId(tap.new_id, ""))) << "its line is stored, for a later tap";
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed(tap.new_id), kNow, resent), NoticeTell::Nothing);
  EXPECT_TRUE(resent.empty());
}

TEST_F(MessageResendTest, AResentMessageAlreadyDeliveredIsNotTold) {
  Add(Unsent("m1"));
  const SendMarkTap tap = Tap("m1");
  ThreadMessage fresh = *Row(tap.new_id);
  fresh.delivery = MessageDelivery::Relayed;
  ASSERT_TRUE(store_.UpdateMessage(fresh));
  ResentMessages resent{{tap.new_id, "t1"}};
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed(tap.new_id, DeliveryNotice::Kind::GroupMembersMissed), kNow, resent),
            NoticeTell::Nothing);
  EXPECT_TRUE(resent.empty());
}

// The set of re-sent messages does not grow for the session: one delivered without any failure is forgotten too.
TEST_F(MessageResendTest, AResentMessageIsForgottenOnceDelivered) {
  Add(Unsent("m1"));
  Add(Unsent("m2"));
  const SendMarkTap first = Tap("m1");
  const SendMarkTap second = Tap("m2");
  ResentMessages resent{{first.new_id, "t1"}, {second.new_id, "t1"}};
  ForgetDeliveredResends(&store_, resent);
  EXPECT_EQ(resent.size(), 2u) << "both still pending";
  ThreadMessage delivered = *Row(first.new_id);
  delivered.delivery = MessageDelivery::Relayed;
  ASSERT_TRUE(store_.UpdateMessage(delivered));
  ForgetDeliveredResends(&store_, resent);
  EXPECT_EQ(resent.size(), 1u);
  EXPECT_EQ(resent.count(second.new_id), 1u);
  ForgetDeliveredResends(nullptr, resent);
  EXPECT_EQ(resent.size(), 1u);
}

TEST_F(MessageResendTest, AnOrdinaryFailureIsAppliedAsBefore) {
  Add(Unsent("m1"));
  ResentMessages resent;
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1"), kNow, resent), NoticeTell::Nothing)
      << "a line in the chat, not a toast";
  EXPECT_TRUE(Row(SendFailureRowId("m1", "")));
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1"), kNow, resent), NoticeTell::Nothing) << "once";
  DeliveryNotice unknown = Failed("m1");
  unknown.send = SendFailure::Unknown;
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, unknown, kNow, resent), NoticeTell::Toast) << "an unknown cause is a toast";
  EXPECT_EQ(ApplyDeliveryNoticeTo(nullptr, Failed("m1"), kNow, resent), NoticeTell::Toast) << "no store: a toast";
}

// The pill for a failed re-sent message belongs to the chat on screen.
TEST(ResentFailurePillTest, OnlyInItsOwnOpenChatWhileLookedAt) {
  EXPECT_TRUE(ShowsResentFailurePill("t1", "t1", true));
  EXPECT_FALSE(ShowsResentFailurePill("t1", "t2", true)) << "another chat is open";
  EXPECT_FALSE(ShowsResentFailurePill("t1", "", true)) << "no chat open";
  EXPECT_FALSE(ShowsResentFailurePill("t1", "t1", false)) << "the user is not looking";
  EXPECT_FALSE(ShowsResentFailurePill("", "", true));
}

// The new copy went out but the old row could not be taken away: a later tap never sends a third copy, it only tries
// the removal again.
TEST_F(MessageResendTest, AFailedRemovalIsRetriedAndNothingIsSentAgain) {
  Add(Unsent("m1"));
  ResendPorts ports = Ports();
  int removal_tries = 0;
  ports.remove_unsent = [this, &removal_tries](const std::string& id) -> Roe<bool> {
    if (++removal_tries == 1) {
      return Error("database is locked");
    }
    return store_.RemoveUnsentMessage("t1", id);
  };
  SendMarkTap tap = ResendAfterTap(Facts("m1"), ports);
  ASSERT_EQ(tap.kind, SendMarkTap::Kind::Resent);
  EXPECT_TRUE(tap.removal_error);
  EXPECT_FALSE(tap.old_removed);
  ASSERT_TRUE(Row("m1")) << "still there, still red";

  SendMarkTapFacts again = Facts("m1");
  again.already_resent = true; // what ChatPresenter remembers after a removal error
  tap = ResendAfterTap(again, ports);
  EXPECT_EQ(sends_, 1) << "no third copy";
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::Resent);
  EXPECT_TRUE(tap.old_removed);
  EXPECT_TRUE(tap.new_id.empty());
  EXPECT_FALSE(Row("m1"));

  // And when the removal fails again: still nothing sent, the tap says so.
  Add(Unsent("m3"));
  ports.remove_unsent = [](const std::string&) -> Roe<bool> { return Error("database is locked"); };
  SendMarkTapFacts stuck = Facts("m3");
  stuck.already_resent = true;
  tap = ResendAfterTap(stuck, ports);
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::RemovalFailed);
  EXPECT_TRUE(tap.removal_error);
  EXPECT_EQ(sends_, 1);
  EXPECT_TRUE(Row("m3"));
}

} // namespace
} // namespace pbr
