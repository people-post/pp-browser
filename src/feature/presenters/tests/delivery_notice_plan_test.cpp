#include "feature/presenters/DeliveryNoticePlan.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// #182: a send that did not go out is a line in its chat, once, and not a toast as well.

DeliveryNotice Failed(const SendFailure failure) {
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::SendFailed;
  notice.send = failure;
  notice.thread_id = "t1";
  notice.message_id = "m1";
  return notice;
}

TEST(DeliveryNoticePlanTest, ASendFailureIsALineInItsChatAndNotAToast) {
  const DeliveryNoticePlan plan = PlanDeliveryNotice(Failed(SendFailure::PeerNoAnswer), 1000);
  ASSERT_EQ(plan.action, DeliveryNoticePlan::Action::TranscriptRow);
  ASSERT_TRUE(plan.row);
  EXPECT_EQ(plan.row->thread_id, "t1");
  EXPECT_EQ(plan.row->timestamp, 1000);
  const auto reason = SendFailureRowReason(*plan.row);
  ASSERT_TRUE(reason);
  EXPECT_EQ(SendFailureFromWire(*reason), SendFailure::PeerNoAnswer);
}

TEST(DeliveryNoticePlanTest, EveryKnownCauseGetsALine) {
  for (const SendFailure failure :
       {SendFailure::MeshOff, SendFailure::NoAddress, SendFailure::BackingOff, SendFailure::DialBusy,
        SendFailure::PeerNoAnswer, SendFailure::HandshakeFailed, SendFailure::DirectUnconfirmed,
        SendFailure::RelayNotConfigured, SendFailure::RelayBusy, SendFailure::RelayRateLimited,
        SendFailure::RelayUnreachable, SendFailure::RelayRejected}) {
    EXPECT_EQ(PlanDeliveryNotice(Failed(failure), 1).action, DeliveryNoticePlan::Action::TranscriptRow)
        << SendFailureWord(failure).c_str();
  }
}

TEST(DeliveryNoticePlanTest, AnUnknownCauseStaysAToast) {
  const DeliveryNoticePlan plan = PlanDeliveryNotice(Failed(SendFailure::Unknown), 1);
  EXPECT_EQ(plan.action, DeliveryNoticePlan::Action::Toast);
  EXPECT_FALSE(plan.row);
}

TEST(DeliveryNoticePlanTest, ANoticeWithNoChatToPutItInIsAToast) {
  DeliveryNotice notice = Failed(SendFailure::RelayBusy);
  notice.thread_id.clear();
  EXPECT_EQ(PlanDeliveryNotice(notice, 1).action, DeliveryNoticePlan::Action::Toast);
  notice = Failed(SendFailure::RelayBusy);
  notice.message_id.clear();
  EXPECT_EQ(PlanDeliveryNotice(notice, 1).action, DeliveryNoticePlan::Action::Toast);
}

TEST(DeliveryNoticePlanTest, AGroupSendThatMissedMembersIsOneLineForTheMessage) {
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::GroupMembersMissed;
  notice.thread_id = "g1";
  notice.message_id = "m9";
  const DeliveryNoticePlan first = PlanDeliveryNotice(notice, 1);
  const DeliveryNoticePlan again = PlanDeliveryNotice(notice, 2);
  ASSERT_EQ(first.action, DeliveryNoticePlan::Action::TranscriptRow);
  ASSERT_TRUE(first.row && again.row);
  EXPECT_EQ(first.row->id, again.row->id) << "each member that missed reports; the store keeps one line";
  EXPECT_EQ(SendFailureRowReason(*first.row), std::string(kGroupMembersMissedWire));

  notice.thread_id.clear();
  EXPECT_EQ(PlanDeliveryNotice(notice, 1).action, DeliveryNoticePlan::Action::Toast);
}

TEST(DeliveryNoticePlanTest, AnUnreadableInboundMessageIsNeitherAToastNorASecondLine) {
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::ReceiveFailed;
  notice.thread_id = "t1";
  notice.message_id = "m1";
  const DeliveryNoticePlan plan = PlanDeliveryNotice(notice, 1);
  EXPECT_EQ(plan.action, DeliveryNoticePlan::Action::Nothing) << "its line was stored with the notice";
  EXPECT_FALSE(plan.row);

  notice.thread_id.clear();
  EXPECT_EQ(PlanDeliveryNotice(notice, 1).action, DeliveryNoticePlan::Action::Toast)
      << "no stored line, so the toast is the only word";
}

} // namespace
} // namespace pbr
