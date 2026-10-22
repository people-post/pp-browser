// What a call's stored session says (call_queries), a peer's roster applied to it, and the roster
// helpers the session workflow uses.
#include "domain/calls/CallSessionLogic.h"
#include "domain/calls/CallSessionQueries.h"
#include "domain/calls/CallSessionStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char* kCall = "call:q-1";

class CallSessionQueriesTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_queries_" + util::GenerateUuid());
    std::filesystem::create_directories(data_dir_);
    calls_ = std::make_unique<CallSessionStore>((data_dir_ / "profile.db").string());
    CallSession session;
    session.call_id = kCall;
    session.origin_thread_id = "thread-1";
    session.initiator_identity = "account:alice";
    session.state = CallSessionState::Active;
    session.created_at = 100;
    session.media_epoch = 1;
    ASSERT_TRUE(calls_->UpsertSession(session));
  }

  void TearDown() override {
    calls_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  void Participant(const std::string& identity, CallParticipantState state, std::optional<int64_t> joined_at = {},
                   bool video = false) {
    CallParticipant p;
    p.call_id = kCall;
    p.identity = identity;
    p.state = state;
    p.joined_at = joined_at;
    p.media.video_enabled = video;
    ASSERT_TRUE(calls_->UpsertParticipant(p));
  }

  CallParticipantState StateOf(const std::string& identity) {
    auto p = calls_->FindParticipant(kCall, identity);
    EXPECT_TRUE(p && p->has_value());
    return (*p)->state;
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<CallSessionStore> calls_;
};

TEST_F(CallSessionQueriesTest, WhoIsInTheCall) {
  Participant("account:alice", CallParticipantState::Joined, 10);
  Participant("account:bob", CallParticipantState::Joined, 20, /*video=*/true);
  Participant("account:carol", CallParticipantState::Ringing);
  Participant("account:dave", CallParticipantState::Left);

  EXPECT_TRUE(call_queries::SenderIsInCall(*calls_, kCall, "account:alice")) << "the initiator";
  EXPECT_TRUE(call_queries::SenderIsInCall(*calls_, kCall, "account:carol")) << "ringing";
  EXPECT_FALSE(call_queries::SenderIsInCall(*calls_, kCall, "account:dave")) << "left";
  EXPECT_FALSE(call_queries::SenderIsInCall(*calls_, kCall, "account:eve")) << "never in it";
  EXPECT_FALSE(call_queries::SenderIsInCall(*calls_, "call:other", "account:alice")) << "another call";

  EXPECT_EQ(call_queries::JoinedRemoteIdentities(*calls_, kCall, "account:alice"),
            std::vector<std::string>{"account:bob"});
  auto peer = call_queries::FirstRemoteParticipant(*calls_, kCall, "account:alice");
  ASSERT_TRUE(peer && peer->has_value());
  EXPECT_EQ((*peer)->identity, "account:bob");
  EXPECT_TRUE((*peer)->media.video_enabled);
  EXPECT_TRUE(call_queries::IsStickyInitiator(*calls_, kCall, "account:alice")) << "joined first";
  EXPECT_FALSE(call_queries::IsStickyInitiator(*calls_, kCall, "account:bob"));

  auto detail = call_queries::RosterDetail(*calls_, kCall);
  ASSERT_TRUE(detail);
  EXPECT_EQ(detail->participants.size(), 4u);
  EXPECT_FALSE(call_queries::RosterDetail(*calls_, "call:none")) << "no session";
}

TEST_F(CallSessionQueriesTest, AnOwnInviteeNotYetAcceptedIsAwaited) {
  Participant("account:alice", CallParticipantState::Joined, 10);
  Participant("account:bob", CallParticipantState::Joined, 20);
  PendingCallInvite invite;
  invite.call_id = kCall;
  invite.inviter_identity = "account:alice";
  invite.invitee_identity = "account:bob";
  invite.status = "pending";
  ASSERT_TRUE(calls_->UpsertPendingInvite(invite));
  EXPECT_TRUE(call_queries::AwaitsOwnInviteeAccept(*calls_, kCall, "account:alice"));
  EXPECT_FALSE(call_queries::AwaitingExplicitAnswer(*calls_, kCall).value());

  invite.status = "accepted_implicit";
  ASSERT_TRUE(calls_->UpsertPendingInvite(invite));
  EXPECT_FALSE(call_queries::AwaitsOwnInviteeAccept(*calls_, kCall, "account:alice"));
  EXPECT_TRUE(call_queries::AwaitingExplicitAnswer(*calls_, kCall).value());
}

TEST_F(CallSessionQueriesTest, APeerRosterNeverOverridesOurOwnStateNorResurrectsNorDemotes) {
  Participant("account:me", CallParticipantState::Joined, 50, /*video=*/true);
  Participant("account:bob", CallParticipantState::Joined, 20);
  Participant("account:dave", CallParticipantState::Left);

  CallRosterDetail roster;
  roster.call_id = kCall;
  roster.media_epoch = 3;
  auto entry = [](const std::string& identity, CallParticipantState state, std::optional<int64_t> joined,
                  bool video) {
    CallRosterEntry e;
    e.identity = identity;
    e.state = state;
    e.joined_at = joined;
    e.video_enabled = video;
    return e;
  };
  roster.participants = {
      entry("account:me", CallParticipantState::Ringing, 40, false),     // stale view of us
      entry("account:bob", CallParticipantState::Ringing, std::nullopt, true), // a joiner's stale view
      entry("account:dave", CallParticipantState::Joined, 30, false),     // left: stays left
      entry("account:carol", CallParticipantState::Invited, std::nullopt, false),
  };
  ASSERT_TRUE(ApplyCallRosterToStore(*calls_, roster, "account:me"));

  auto me = calls_->FindParticipant(kCall, "account:me");
  EXPECT_EQ((*me)->state, CallParticipantState::Joined) << "only we know our state";
  EXPECT_TRUE((*me)->media.video_enabled) << "and our camera";
  EXPECT_EQ((*me)->joined_at, std::optional<int64_t>(40)) << "the join stamp is the inviter's";
  auto bob = calls_->FindParticipant(kCall, "account:bob");
  EXPECT_EQ((*bob)->state, CallParticipantState::Joined) << "not demoted";
  EXPECT_TRUE((*bob)->media.video_enabled) << "its media follows";
  EXPECT_EQ(StateOf("account:dave"), CallParticipantState::Left) << "not resurrected";
  EXPECT_EQ(StateOf("account:carol"), CallParticipantState::Invited) << "new co-invitee listed";
  EXPECT_EQ(calls_->LoadSession(kCall).value()->media_epoch, 3u);

  // V053: back in — a join stamped after the one we know; and the epoch never moves back.
  Participant("account:erin", CallParticipantState::Left, 25);
  Participant("account:fay", CallParticipantState::Declined);
  roster.media_epoch = 2;
  roster.participants = {entry("account:erin", CallParticipantState::Joined, 25, false),  // stale
                         entry("account:dave", CallParticipantState::Joined, 30, false)};
  ASSERT_TRUE(ApplyCallRosterToStore(*calls_, roster, "account:me"));
  EXPECT_EQ(StateOf("account:erin"), CallParticipantState::Left) << "the old stamp: not back";
  EXPECT_EQ(calls_->LoadSession(kCall).value()->media_epoch, 3u) << "an older epoch is not the key now";
  roster.participants = {entry("account:erin", CallParticipantState::Joined, 60, false),
                         entry("account:fay", CallParticipantState::Joined, 61, false)};
  ASSERT_TRUE(ApplyCallRosterToStore(*calls_, roster, "account:me"));
  EXPECT_EQ(StateOf("account:erin"), CallParticipantState::Joined) << "joined again, later";
  EXPECT_EQ(StateOf("account:fay"), CallParticipantState::Joined) << "declined, then joined";
}

TEST_F(CallSessionQueriesTest, CallingEachOtherAndAnEarlyHello) {
  // kCall: alice placed it; bob is still ringing.
  Participant("account:alice", CallParticipantState::Joined, 10);
  Participant("account:bob", CallParticipantState::Ringing);
  auto crossed = call_queries::OurUnansweredCallTo(*calls_, "account:bob", "account:alice", "call:bobs");
  ASSERT_TRUE(crossed);
  EXPECT_EQ(crossed->call_id, kCall);
  EXPECT_FALSE(call_queries::OurUnansweredCallTo(*calls_, "account:bob", "account:alice", kCall)) << "not itself";
  EXPECT_FALSE(call_queries::OurUnansweredCallTo(*calls_, "account:bob", "account:bob", "call:x")) << "not ours";
  EXPECT_TRUE(call_queries::PeerStillToAnswerOurCall(*calls_, kCall, "account:bob", "account:alice"));

  Participant("account:carol", CallParticipantState::Invited);
  EXPECT_FALSE(call_queries::OurUnansweredCallTo(*calls_, "account:bob", "account:alice", "call:bobs"))
      << "someone else is in it";
  EXPECT_FALSE(call_queries::PeerStillToAnswerOurCall(*calls_, kCall, "account:bob", "account:alice")) << "1:1 only";
}

TEST(CallSessionLogicRosterTest, StampsAndCoInvitees) {
  std::vector<CallRosterEntry> roster(1);
  roster[0].identity = "account:alice";
  roster[0].joined_at = 7;
  EXPECT_EQ(CallSessionLogic::RosterJoinStamp(roster, "account:alice"), std::optional<int64_t>(7));
  EXPECT_EQ(CallSessionLogic::RosterJoinStamp(roster, "account:bob"), std::nullopt);
  CallSession session;
  EXPECT_EQ(CallSessionLogic::LocalJoinStampFallback(session), 1) << "never 0";
  session.created_at = 99;
  EXPECT_EQ(CallSessionLogic::LocalJoinStampFallback(session), 99);

  EXPECT_EQ(CallSessionLogic::CountDistinctInvitees({"a", "b", "a", "", "me"}, "me"), 2u);
  CallSessionLogic::AppendInvitedCoInvitees(roster, {"account:alice", "account:bob", "me", ""}, "me");
  ASSERT_EQ(roster.size(), 2u);
  EXPECT_EQ(roster[1].identity, "account:bob");
  EXPECT_EQ(roster[1].state, CallParticipantState::Invited);
}

// V053: who dropped out of a group call may ask back in while someone is in it; a stranger, a 1:1
// call, an ended call (everyone left) or one too old are not joinable.
TEST_F(CallSessionQueriesTest, WhoMayJoinAGroupCallAndWhichCallIsJoinable) {
  const int64_t now = 1'000'000;
  auto session = calls_->LoadSession(kCall);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_FALSE(call_queries::SenderMayJoin(*calls_, kCall, "account:bob")) << "not offered";
  Participant("account:bob", CallParticipantState::Declined);
  EXPECT_FALSE(call_queries::SenderMayJoin(*calls_, kCall, "account:bob")) << "a 1:1 call";
  CallSession group = **session;
  group.origin_group_id = "group-1";
  group.created_at = now - 1000;
  ASSERT_TRUE(calls_->UpsertSession(group));
  EXPECT_TRUE(call_queries::SenderMayJoin(*calls_, kCall, "account:bob")) << "offered, declined";
  EXPECT_TRUE(call_queries::SenderMayJoin(*calls_, kCall, "account:alice")) << "started it";
  EXPECT_FALSE(call_queries::SenderMayJoin(*calls_, kCall, "account:mallory"));

  // From bob's side: he declined, alice is in it.
  Participant("account:alice", CallParticipantState::Joined, 1);
  auto joinable = call_queries::JoinableGroupCall(*calls_, "group-1", "account:bob", now);
  ASSERT_TRUE(joinable);
  EXPECT_EQ(joinable->call_id, kCall);
  EXPECT_FALSE(call_queries::JoinableGroupCall(*calls_, "group-1", "account:alice", now)) << "alice is in it";
  EXPECT_FALSE(call_queries::JoinableGroupCall(*calls_, "group-2", "account:bob", now));
  EXPECT_FALSE(call_queries::JoinableGroupCall(*calls_, "group-1", "account:bob",
                                               now + call_queries::kJoinableGroupCallMs))
      << "too old without word that it ended";
  Participant("account:alice", CallParticipantState::Left);
  EXPECT_FALSE(call_queries::JoinableGroupCall(*calls_, "group-1", "account:bob", now)) << "nobody left in it";
}

} // namespace
} // namespace pbr
