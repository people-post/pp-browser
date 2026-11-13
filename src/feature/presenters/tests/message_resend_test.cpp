// #210 step 2: a tap on the red mark of our text sends it again as a new message and takes the unsent row away — only
// once its automatic sending has stopped, only in public chats; anything else just says why
// (DEV/PLAN-message-resend.md). The ports run on the in-memory store, as the facade's run on the app's.
#include "feature/presenters/DeliveryNoticePlan.h"
#include "feature/presenters/MessageResend.h"
#include "feature/presenters/SendMark.h"
#include "common/runtime/AppRuntime.h"
#include "domain/delivery/DeliveryOutbox.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/DeliveryNotice.h"
#include "lib/delivery/HttpRelayClient.h"
#include "lib/delivery/RelayClients.h"
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
  const size_t before = Count();
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1"), kNow), NoticeTell::Nothing);
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1", DeliveryNotice::Kind::GroupMembersMissed), kNow),
            NoticeTell::Nothing);
  EXPECT_EQ(Count(), before) << "no orphan line";
  EXPECT_FALSE(Row(SendFailureRowId("m1", "")));
  EXPECT_FALSE(Row(SendFailureRowId("m1", kGroupMembersMissedWire)));
  EXPECT_FALSE(Row("m1")) << "and the row is not brought back";
}

// A re-sent message is told once when it is drawn red again — whatever turned it: a failure notice may never come
// (a copy to a peer's device reports none), the clock's limit turns it red all the same.
TEST_F(MessageResendTest, AResentMessageIsToldWhenItTurnsRedAgain) {
  Add(Unsent("m1"));
  const SendMarkTap tap = Tap("m1");
  ResentMessages resent{{tap.new_id, "t1"}};
  EXPECT_FALSE(FirstResentTurnedRed(resent, "t1", {})) << "still a clock, or delivered: nothing red";
  EXPECT_FALSE(FirstResentTurnedRed(resent, "t1", {"other"})) << "another red row is not a re-sent one";
  EXPECT_FALSE(FirstResentTurnedRed(resent, "t2", {tap.new_id})) << "drawn in another chat";
  EXPECT_EQ(FirstResentTurnedRed(resent, "t1", {"other", tap.new_id}), tap.new_id);
  resent.erase(tap.new_id); // what ChatPresenter::ForgetResent does once it is told
  EXPECT_FALSE(FirstResentTurnedRed(resent, "t1", {tap.new_id})) << "once";
}

// The red mark at the clock's limit, with no failure reported, is red for the drawing that tells it.
TEST_F(MessageResendTest, TheClocksLimitTurnsAResentMessageRedWithoutAFailure) {
  Add(Unsent("m1"));
  const SendMarkTap tap = Tap("m1");
  ThreadMessage fresh = *Row(tap.new_id);
  ASSERT_EQ(fresh.delivery, MessageDelivery::Pending) << "no outcome ever came";
  EXPECT_EQ(MarkOf(fresh, kNow, kNow + 60'000), SendMark::Queued);
  EXPECT_EQ(MarkOf(fresh, kNow, kNow + 60'001), SendMark::Failed);
}

TEST_F(MessageResendTest, AFailureNoticeOfAResentMessageIsAppliedAsAnyOther) {
  Add(Unsent("m1"));
  const SendMarkTap tap = Tap("m1");
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed(tap.new_id), kNow), NoticeTell::Nothing) << "its line, no toast";
  EXPECT_TRUE(Row(SendFailureRowId(tap.new_id, ""))) << "stored: the reason the pill tells when it turns red";
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
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1"), kNow), NoticeTell::Nothing)
      << "a line in the chat, not a toast";
  EXPECT_TRUE(Row(SendFailureRowId("m1", "")));
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1"), kNow), NoticeTell::Nothing) << "once";
  DeliveryNotice unknown = Failed("m1");
  unknown.send = SendFailure::Unknown;
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, unknown, kNow), NoticeTell::Toast) << "an unknown cause is a toast";
  EXPECT_EQ(ApplyDeliveryNoticeTo(nullptr, Failed("m1"), kNow), NoticeTell::Toast) << "no store: a toast";
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

// "Update their card first" / "update my card first" (DEV/PLAN-send-failure-guidance.md): which result a tap on a red
// mark gets, decided from the stored reason, the chat and whether the peer is a contact.
SendMarkTapFacts GuidedFacts(const SendMarkTapFacts& base, const std::optional<SendFailure> failure,
                             const bool contact = true) {
  SendMarkTapFacts facts = base;
  facts.failure = failure;
  if (contact) {
    facts.peer_contact_id = "contact:bob";
  }
  return facts;
}

TEST_F(MessageResendTest, RecipientNotFoundInADirectChatWithAContactAsksToUpdateTheirCard) {
  Add(Unsent("m1"));
  const SendMarkTap tap = ResendAfterTap(GuidedFacts(Facts("m1"), SendFailure::RecipientNotFound), Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_EQ(tap.contact_id, "contact:bob");
  EXPECT_TRUE(tap.resendable);
  EXPECT_EQ(AdviceOf(tap), SendAdvice::UpdateTheirCardAndResend);
  EXPECT_EQ(sends_, 0) << "the tap itself sends nothing";
  EXPECT_TRUE(Row("m1")) << "and takes nothing away";
}

TEST_F(MessageResendTest, AMessageThatCannotBeResentSaysSoInTheFlag) {
  ThreadMessage voice = Unsent("m1");
  voice.content_type = ChatContentType::Attachment;
  Add(voice);
  SendMarkTap tap = ResendAfterTap(GuidedFacts(Facts("m1"), SendFailure::RecipientNotFound), Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_FALSE(tap.resendable);
  EXPECT_EQ(AdviceOf(tap), SendAdvice::UpdateTheirCardOnly);

  thread_.channel = ThreadChannel::E2e; // a private chat's message is not re-sent with a tap either
  Add(Unsent("m2"));
  tap = ResendAfterTap(GuidedFacts(Facts("m2"), SendFailure::RecipientNotFound), Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_FALSE(tap.resendable);
  tap = ResendAfterTap(GuidedFacts(Facts("m2"), SendFailure::SenderNotRegistered), Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateMyCard);
  EXPECT_FALSE(tap.resendable);
}

TEST_F(MessageResendTest, SenderNotRegisteredAsksToUpdateOurOwnCard) {
  Add(Unsent("m1"));
  const SendMarkTap tap = ResendAfterTap(GuidedFacts(Facts("m1"), SendFailure::SenderNotRegistered), Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateMyCard);
  EXPECT_TRUE(tap.resendable);
  EXPECT_EQ(AdviceOf(tap), SendAdvice::UpdateMyCard);
  EXPECT_EQ(sends_, 0);
  EXPECT_TRUE(Row("m1"));
}

TEST_F(MessageResendTest, GroupMessagesAndPeersWhoAreNotContactsNeverTakeTheNewBranches) {
  Add(Unsent("m1"));
  // A non-contact peer: the old path (a direct re-send).
  SendMarkTap tap =
      ResendAfterTap(GuidedFacts(Facts("m1"), SendFailure::RecipientNotFound, /*contact=*/false), Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::Resent);
  EXPECT_EQ(sends_, 1);

  Add(Unsent("m2"));
  tap = ResendAfterTap(GuidedFacts(Facts("m2"), SendFailure::SenderNotRegistered, /*contact=*/false), Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::Resent);

  thread_ = PublicChat(ThreadKind::Group);
  Add(Unsent("m3"));
  SendMarkTapFacts group = GuidedFacts(Facts("m3"), SendFailure::RecipientNotFound);
  tap = ResendAfterTap(group, Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::Resent) << "a group message is re-sent as before";
  group.failure = SendFailure::SenderNotRegistered;
  Add(Unsent("m4"));
  group.row = Row("m4");
  EXPECT_EQ(ResendAfterTap(group, Ports()).kind, SendMarkTap::Kind::Resent);
}

TEST_F(MessageResendTest, EveryOtherReasonGoesTheOldWay) {
  int n = 0;
  for (const std::optional<SendFailure> failure :
       {std::optional<SendFailure>(), std::optional<SendFailure>(SendFailure::RelayRejected),
        std::optional<SendFailure>(SendFailure::RelayUnreachable),
        std::optional<SendFailure>(SendFailure::PeerNoAnswer), std::optional<SendFailure>(SendFailure::RelayBusy)}) {
    SendMarkTapFacts guided = GuidedFacts(Facts("none"), failure);
    ThreadMessage row = Unsent("g" + std::to_string(++n));
    Add(row);
    guided.row = row;
    SendMarkTapFacts plain = guided;
    plain.failure.reset();
    plain.peer_contact_id.reset();
    EXPECT_EQ(DecideSendMarkTap(guided).kind, DecideSendMarkTap(plain).kind);
    EXPECT_EQ(DecideSendMarkTap(guided).kind, SendMarkTap::Kind::Resent);
    EXPECT_EQ(AdviceOf(DecideSendMarkTap(guided)), SendAdvice::None);
  }
  // Not-text with another reason: the old reason-only result.
  ThreadMessage picture = Unsent("pic");
  picture.content_type = ChatContentType::Attachment;
  Add(picture);
  const SendMarkTap tap = DecideSendMarkTap(GuidedFacts(Facts("pic"), SendFailure::RelayUnreachable));
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::ReasonOnly);
  EXPECT_EQ(tap.refusal, SendMarkTap::Refusal::NotText);
}

TEST_F(MessageResendTest, AMessageStillTryingOrAlreadyResentIsNotGuided) {
  Add(Unsent("m1"));
  started_ms_ = kNow - 1000;
  EXPECT_EQ(DecideSendMarkTap(GuidedFacts(Facts("m1"), SendFailure::RecipientNotFound)).kind,
            SendMarkTap::Kind::StillTrying);
  started_ms_.reset();
  SendMarkTapFacts again = GuidedFacts(Facts("m1"), SendFailure::RecipientNotFound);
  again.already_resent = true;
  EXPECT_EQ(DecideSendMarkTap(again).kind, SendMarkTap::Kind::Resent) << "only its removal is retried";
}

TEST_F(MessageResendTest, AfterTheCardIsUpdatedTheMessageIsResentWithoutAskingAgain) {
  Add(Unsent("m1"));
  SendMarkTapFacts facts = GuidedFacts(Facts("m1"), SendFailure::RecipientNotFound);
  facts.offer_fix = false;
  const SendMarkTap tap = ResendAfterTap(facts, Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::Resent);
  EXPECT_EQ(sends_, 1);
  EXPECT_FALSE(Row("m1"));
}

/** A relay that answers every send with the real 404 body a server gives for a recipient it has no registration for. */
class RecipientNotFoundRelay final : public IRelayClient {
public:
  Roe<void> Send(const RelayEnvelope&) override {
    ++sends;
    return RelaySendHttpError(404, R"({"error":"Recipient not registered","code":"recipient_not_found"})");
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return Error("unused"); }
  int sends = 0;
};

// The whole way a failed message takes, but for the orchestrator that joins the two halves (it has no test fixture):
// a relay answering 404 recipient_not_found -> the outbox's outcome -> the notice the orchestrator builds from it ->
// the stored send_failure row -> the tap's decision.
TEST_F(MessageResendTest, ARelayAnswering404RecipientNotFoundLeadsToUpdateTheirCardFirst) {
  AppRuntime::Initialize(AppRuntimeConfig{});
  RecipientNotFoundRelay relay;
  std::vector<DeliveryOutcome> outcomes;
  {
    DeliveryOutbox outbox([]() -> IDirectMessageClient* { return nullptr; });
    outbox.SetRelayClient(&relay);
    outbox.SetOnOutcome([&outcomes](const DeliveryOutcome& outcome) { outcomes.push_back(outcome); });
    DeliveryRequest request;
    request.envelope.message_id = "m1";
    request.envelope.timestamp = 1;
    request.thread_id = "t1";
    request.message_id = "m1";
    request.relay_route = "relay:bob";
    outbox.Send(std::move(request));
    ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
  }
  AppRuntime::Shutdown();
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_FALSE(outcomes[0].ok);
  ASSERT_EQ(outcomes[0].failure, SendFailure::RecipientNotFound);

  // What MeshDeliveryOrchestrator::NotifyDeliveryIssue builds, and ChatPresenter applies to the store.
  Add(Unsent("m1"));
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::SendFailed;
  notice.thread_id = "t1";
  notice.message_id = "m1";
  notice.send = SendFailureForThread(outcomes[0].failure, /*trusted_peer=*/false);
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, notice, kNow), NoticeTell::Nothing);
  auto line = store_.GetMessage("t1", SendFailureRowId("m1", ""));
  ASSERT_TRUE(line && *line);
  EXPECT_EQ(SendFailureRowReason(**line), "recipient_not_found") << "the stored word";

  const std::optional<SendFailure> reason = SendFailureReason(*line, /*group_thread=*/false);
  ASSERT_TRUE(reason);
  EXPECT_EQ(*reason, SendFailure::RecipientNotFound);
  const SendMarkTap tap = ResendAfterTap(GuidedFacts(Facts("m1"), reason), Ports());
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_EQ(tap.contact_id, "contact:bob");
}

} // namespace
} // namespace pbr
