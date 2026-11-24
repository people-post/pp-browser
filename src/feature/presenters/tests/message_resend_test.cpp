// #210 step 2 / B236: a tap on the red mark of our message decides that it goes again as itself (the same message id,
// the same row: the caller starts it, ConversationsFacade::ResendMessageAsync) — only once its automatic sending has
// stopped, only in public chats; anything else just says why (DEV/PLAN-message-resend.md).
#include "feature/presenters/DeliveryNoticePlan.h"
#include "feature/presenters/MessageEdit.h"
#include "feature/presenters/MessageResend.h"
#include "feature/presenters/SendMark.h"
#include "common/runtime/AppRuntime.h"
#include "common/tests/TeardownLikeTheApp.h"
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

  SendMarkTap Tap(const std::string& id) { return DecideSendMarkTap(Facts(id)); }

  InMemoryDeliveryStore store_;
  Thread thread_ = PublicChat();
  std::optional<int64_t> started_ms_;
};

// B236 R1: a text goes again as itself too — a new message beside it would reach a peer that has the first copy (its
// answer lost, or a start's resume already on its way) twice. Nothing is sent here, nothing taken away.
TEST_F(MessageResendTest, ATapSendsTheSameTextAgainNotANewMessage) {
  Add(Unsent("m1"));
  Add(MakeSendFailureRow("t1", "m1", SendFailureWord(SendFailure::RelayUnreachable).c_str(), kSentAt + 1));
  const SendMarkTap tap = Tap("m1");
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::ResendSameMessage);
  EXPECT_TRUE(tap.new_id.empty()) << "no new message";
  EXPECT_FALSE(tap.old_removed);
  EXPECT_TRUE(Row("m1")) << "the same row goes again";
  EXPECT_TRUE(Row(SendFailureRowId("m1", ""))) << "its reason stays until it is through";
  EXPECT_EQ(Count(), 2u);

  thread_ = PublicChat(ThreadKind::Group);
  Add(Unsent("g1", "@Bob lunch?"));
  EXPECT_EQ(Tap("g1").kind, SendMarkTap::Kind::ResendSameMessage) << "a group's text the same";
}

TEST_F(MessageResendTest, WhileARequestIsStillAliveItOnlySaysSo) {
  Add(Unsent("m1"));
  started_ms_ = kNow - 90'000; // a retry still running, or the start about to send it again (B236 R7)
  const SendMarkTap tap = Tap("m1");
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::StillTrying);
  EXPECT_TRUE(Row("m1"));
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
  EXPECT_EQ(decide(Unsent("m1"), PublicChat()).kind, Kind::ResendSameMessage);
  EXPECT_EQ(decide(Unsent("m1"), PublicChat(ThreadKind::Group)).kind, Kind::ResendSameMessage);

  Thread private_chat = PublicChat();
  private_chat.channel = ThreadChannel::E2e;
  EXPECT_EQ(decide(Unsent("m1"), private_chat).refusal, Refusal::ThisKind);
  ThreadMessage shared_ai = Unsent("m1", "what time is it in Paris?");
  shared_ai.ai_invoke_mode = "shared_full";
  EXPECT_EQ(decide(shared_ai, PublicChat()).refusal, Refusal::ThisKind);
  // B236: a picture, file, voice message or card goes again as itself, in a public chat or a group; not in a private
  // chat; and never while a send of it is still tried.
  for (const ChatContentType type : {ChatContentType::Attachment, ChatContentType::ContactCard}) {
    ThreadMessage other = Unsent("m1");
    other.content_type = type;
    EXPECT_EQ(decide(other, PublicChat()).kind, Kind::ResendSameMessage);
    EXPECT_EQ(decide(other, PublicChat(ThreadKind::Group)).kind, Kind::ResendSameMessage);
    EXPECT_EQ(decide(other, private_chat).refusal, Refusal::ThisKind);
    EXPECT_EQ(decide(other, PublicChat(), kNow - 1'000).kind, Kind::StillTrying);
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

// B236: the tap on a picture's red mark only decides; the controller sends it again as itself (the same id), so
// nothing goes as a new message here and its row stays where it is.
TEST_F(MessageResendTest, ANonTextRedMarkIsSentAgainAsItselfNotAsANewMessage) {
  ThreadMessage picture = Unsent("m1");
  picture.content_type = ChatContentType::Attachment;
  Add(picture);
  const SendMarkTap tap = Tap("m1");
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::ResendSameMessage);
  EXPECT_TRUE(tap.new_id.empty()) << "no new message";
  EXPECT_TRUE(Row("m1")) << "the same row goes again";
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
  auto removed = store_.RemoveUnsentMessage("t1", "m1"); // as an edit sent again takes its unsent one away
  ASSERT_TRUE(removed && *removed);
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
  ASSERT_EQ(Tap("m1").kind, SendMarkTap::Kind::ResendSameMessage);
  ResentMessages resent{{"m1", "t1"}}; // ChatPresenter::NoteResent once it is on its way again
  EXPECT_FALSE(FirstResentTurnedRed(resent, "t1", {})) << "still a clock, or delivered: nothing red";
  EXPECT_FALSE(FirstResentTurnedRed(resent, "t1", {"other"})) << "another red row is not a re-sent one";
  EXPECT_FALSE(FirstResentTurnedRed(resent, "t2", {"m1"})) << "drawn in another chat";
  EXPECT_EQ(FirstResentTurnedRed(resent, "t1", {"other", "m1"}), "m1");
  resent.erase("m1"); // what ChatPresenter::ForgetResent does once it is told
  EXPECT_FALSE(FirstResentTurnedRed(resent, "t1", {"m1"})) << "once";
}

// B236: a re-sent message shows the clock for as long as its send is tried, and turns red, with no failure reported,
// once nothing of it is being tried (a restart that could not send it again).
TEST_F(MessageResendTest, AResentMessageIsQueuedWhileTriedAndRedOnceNothingIsTried) {
  ThreadMessage queued = Unsent("m1");
  queued.delivery = MessageDelivery::Pending; // queued again by the resend (ResendStoredMessage)
  queued.timestamp = kNow;
  Add(queued);
  ThreadMessage fresh = *Row("m1");
  EXPECT_EQ(MarkOf(fresh, kNow, kNow + 60'001), SendMark::Queued);
  EXPECT_EQ(MarkOf(fresh, std::nullopt, kNow + 60'001), SendMark::Failed);
}

TEST_F(MessageResendTest, AFailureNoticeOfAResentMessageIsAppliedAsAnyOther) {
  Add(Unsent("m1"));
  EXPECT_EQ(ApplyDeliveryNoticeTo(&store_, Failed("m1"), kNow), NoticeTell::Nothing) << "its line, no toast";
  EXPECT_TRUE(Row(SendFailureRowId("m1", ""))) << "stored: the reason the pill tells when it turns red";
}

// The set of re-sent messages does not grow for the session: one delivered without any failure is forgotten too.
TEST_F(MessageResendTest, AResentMessageIsForgottenOnceDelivered) {
  Add(Unsent("m1"));
  Add(Unsent("m2"));
  ResentMessages resent{{"m1", "t1"}, {"m2", "t1"}};
  ForgetDeliveredResends(&store_, resent);
  EXPECT_EQ(resent.size(), 2u) << "both still not through";
  ThreadMessage delivered = *Row("m1");
  delivered.delivery = MessageDelivery::Relayed;
  ASSERT_TRUE(store_.UpdateMessage(delivered));
  ForgetDeliveredResends(&store_, resent);
  EXPECT_EQ(resent.size(), 1u);
  EXPECT_EQ(resent.count("m2"), 1u);
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
  const SendMarkTap tap = DecideSendMarkTap(GuidedFacts(Facts("m1"), SendFailure::RecipientNotOnRelay));
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_EQ(tap.contact_id, "contact:bob");
  EXPECT_TRUE(tap.resendable);
  EXPECT_EQ(AdviceOf(tap), SendAdvice::UpdateTheirCardAndResend);
  EXPECT_TRUE(Row("m1")) << "the tap itself takes nothing away";
}

TEST_F(MessageResendTest, AMessageThatCannotBeResentSaysSoInTheFlag) {
  ThreadMessage voice = Unsent("m1");
  voice.content_type = ChatContentType::Attachment;
  Add(voice);
  SendMarkTap tap = DecideSendMarkTap(GuidedFacts(Facts("m1"), SendFailure::RecipientNotOnRelay));
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_TRUE(tap.resendable) << "B236: a voice message is sent again as itself after the card update";
  EXPECT_EQ(AdviceOf(tap), SendAdvice::UpdateTheirCardAndResend);

  thread_.channel = ThreadChannel::E2e; // a private chat's message is not re-sent with a tap either
  Add(Unsent("m2"));
  tap = DecideSendMarkTap(GuidedFacts(Facts("m2"), SendFailure::RecipientNotOnRelay));
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_FALSE(tap.resendable);
  tap = DecideSendMarkTap(GuidedFacts(Facts("m2"), SendFailure::SenderNotRegistered));
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateMyCard);
  EXPECT_FALSE(tap.resendable);
}

TEST_F(MessageResendTest, SenderNotRegisteredAsksToUpdateOurOwnCard) {
  Add(Unsent("m1"));
  const SendMarkTap tap = DecideSendMarkTap(GuidedFacts(Facts("m1"), SendFailure::SenderNotRegistered));
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateMyCard);
  EXPECT_TRUE(tap.resendable);
  EXPECT_EQ(AdviceOf(tap), SendAdvice::UpdateMyCard);
  EXPECT_TRUE(Row("m1"));
}

TEST_F(MessageResendTest, GroupMessagesAndPeersWhoAreNotContactsNeverTakeTheNewBranches) {
  Add(Unsent("m1"));
  // A non-contact peer: no card update offered, the message goes again as itself.
  EXPECT_EQ(DecideSendMarkTap(GuidedFacts(Facts("m1"), SendFailure::RecipientNotOnRelay, /*contact=*/false)).kind,
            SendMarkTap::Kind::ResendSameMessage);
  EXPECT_EQ(DecideSendMarkTap(GuidedFacts(Facts("m1"), SendFailure::SenderNotRegistered, /*contact=*/false)).kind,
            SendMarkTap::Kind::ResendSameMessage);
  thread_ = PublicChat(ThreadKind::Group);
  Add(Unsent("m3"));
  SendMarkTapFacts group = GuidedFacts(Facts("m3"), SendFailure::RecipientNotOnRelay);
  EXPECT_EQ(DecideSendMarkTap(group).kind, SendMarkTap::Kind::ResendSameMessage) << "a group message as before";
  group.failure = SendFailure::SenderNotRegistered;
  EXPECT_EQ(DecideSendMarkTap(group).kind, SendMarkTap::Kind::ResendSameMessage);
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
    EXPECT_EQ(DecideSendMarkTap(guided).kind, SendMarkTap::Kind::ResendSameMessage);
    EXPECT_EQ(AdviceOf(DecideSendMarkTap(guided)), SendAdvice::None);
  }
  // Not-text with another reason: sent again as itself, no guidance.
  ThreadMessage picture = Unsent("pic");
  picture.content_type = ChatContentType::Attachment;
  Add(picture);
  const SendMarkTap tap = DecideSendMarkTap(GuidedFacts(Facts("pic"), SendFailure::RelayUnreachable));
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::ResendSameMessage);
  EXPECT_EQ(AdviceOf(tap), SendAdvice::None);
  // The server cannot find them: their card is updated first, and the picture is sent again after it.
  const SendMarkTap fix = DecideSendMarkTap(GuidedFacts(Facts("pic"), SendFailure::RecipientNotOnRelay));
  EXPECT_EQ(fix.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_TRUE(fix.resendable);
  EXPECT_EQ(AdviceOf(fix), SendAdvice::UpdateTheirCardAndResend);
}

TEST_F(MessageResendTest, AMessageStillTryingIsNotGuided) {
  Add(Unsent("m1"));
  started_ms_ = kNow - 1000;
  EXPECT_EQ(DecideSendMarkTap(GuidedFacts(Facts("m1"), SendFailure::RecipientNotOnRelay)).kind,
            SendMarkTap::Kind::StillTrying);
}

TEST_F(MessageResendTest, AfterTheCardIsUpdatedTheMessageIsResentWithoutAskingAgain) {
  Add(Unsent("m1"));
  SendMarkTapFacts facts = GuidedFacts(Facts("m1"), SendFailure::RecipientNotOnRelay);
  facts.offer_fix = false;
  const SendMarkTap tap = DecideSendMarkTap(facts);
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::ResendSameMessage) << "the same message, as itself";
  EXPECT_TRUE(Row("m1"));
}

/** A relay that answers every send as HttpRelayClient does a 404 `recipient_not_registered` (no registration there). */
class RecipientNotFoundRelay final : public IRelayClient {
public:
  Roe<void> Send(const RelayEnvelope&) override {
    ++sends;
    return AppError::Network(Err::Network::RecipientNotRegistered, "Relay send: the recipient is not registered here");
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return Error("unused"); }
  int sends = 0;
};

// The whole way a failed message takes, but for the orchestrator that joins the two halves (it has no test fixture):
// a relay answering 404 recipient_not_registered -> the outbox's outcome -> the notice the orchestrator builds from it ->
// the stored send_failure row -> the tap's decision.
TEST_F(MessageResendTest, ARelayAnswering404RecipientNotRegisteredLeadsToUpdateTheirCardFirst) {
  AppRuntime::Initialize(AppRuntimeConfig{});
  RecipientNotFoundRelay relay;
  std::vector<DeliveryOutcome> outcomes;
  {
    test::TestOwner<DeliveryOutbox> outbox([]() -> IDirectMessageClient* { return nullptr; });
    outbox->SetRelayClient(&relay);
    outbox->SetOnOutcome([&outcomes](const DeliveryOutcome& outcome) { outcomes.push_back(outcome); });
    DeliveryRequest request;
    request.envelope.message_id = "m1";
    request.envelope.timestamp = 1;
    request.thread_id = "t1";
    request.message_id = "m1";
    request.relay_recipient = "account:bob";
    outbox->Send(std::move(request));
    ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
  }
  AppRuntime::Shutdown();
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_FALSE(outcomes[0].ok);
  ASSERT_EQ(outcomes[0].failure, SendFailure::RecipientNotOnRelay);

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
  EXPECT_EQ(SendFailureRowReason(**line), "recipient_not_on_relay") << "the stored word";

  const std::optional<SendFailure> reason = SendFailureReason(*line, /*group_thread=*/false);
  ASSERT_TRUE(reason);
  EXPECT_EQ(*reason, SendFailure::RecipientNotOnRelay);
  const SendMarkTap tap = DecideSendMarkTap(GuidedFacts(Facts("m1"), reason));
  EXPECT_EQ(tap.kind, SendMarkTap::Kind::UpdateTheirCard);
  EXPECT_EQ(tap.contact_id, "contact:bob");
}

// The words that tell what a tap does and the tap itself share this decision (ChatPresenter::SendAdviceOf /
// TapSendMarkWith): an edited message whose own mark is none resends its edit, never offers a card update.
TEST_F(MessageResendTest, AnEditedMessageWithNoMarkOfItsOwnTakesTheEditBranchBeforeAnyCardUpdate) {
  ThreadMessage edited = Unsent("m1");
  edited.delivery = MessageDelivery::Relayed;
  edited.payload_json = R"({"edited_at":1000500,"edited_by":"e1"})";
  EXPECT_EQ(EditResentByTap(edited, std::nullopt, kNow), std::optional<std::string>("e1"));

  ThreadMessage failed_itself = Unsent("m2"); // red by its own send: an ordinary tap, which may offer the fix
  failed_itself.payload_json = edited.payload_json;
  EXPECT_FALSE(EditResentByTap(failed_itself, std::nullopt, kNow));

  EXPECT_FALSE(EditResentByTap(Unsent("m3"), std::nullopt, kNow)) << "never edited";
}

} // namespace
} // namespace pbr
