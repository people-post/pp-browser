#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class CallSessionStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_session_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    calls_ = std::make_unique<CallSessionStore>(store_->ProfileDbPath());
  }

  void TearDown() override {
    calls_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<CallSessionStore> calls_;
};

TEST_F(CallSessionStoreTest, UpsertSessionAndParticipants) {
  CallSession session;
  session.call_id = "call:test-1";
  session.origin_thread_id = "thread-1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Ringing;
  session.created_at = util::NowUnixMs();
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(calls_->UpsertSession(session));

  CallParticipant alice;
  alice.call_id = session.call_id;
  alice.identity = "account:alice";
  alice.state = CallParticipantState::Joined;
  alice.joined_at = session.created_at;
  ASSERT_TRUE(calls_->UpsertParticipant(alice));

  CallParticipant bob;
  bob.call_id = session.call_id;
  bob.identity = "account:bob";
  bob.state = CallParticipantState::Ringing;
  ASSERT_TRUE(calls_->UpsertParticipant(bob));

  auto loaded = calls_->LoadSession(session.call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->origin_thread_id, "thread-1");
  EXPECT_EQ((*loaded)->state, CallSessionState::Ringing);

  auto joined = calls_->CountJoined(session.call_id);
  ASSERT_TRUE(joined);
  EXPECT_EQ(*joined, 1u);

  PendingCallInvite invite;
  invite.call_id = session.call_id;
  invite.inviter_identity = "account:alice";
  invite.invitee_identity = "account:bob";
  invite.media_mode = CallMediaMode::Voice;
  invite.created_at = session.created_at;
  invite.expires_at = session.created_at + 60'000;
  invite.status = "pending";
  ASSERT_TRUE(calls_->UpsertPendingInvite(invite));

  auto pending = calls_->ListPendingInvitesForInvitee("account:bob");
  ASSERT_TRUE(pending);
  ASSERT_EQ(pending->size(), 1u);
  EXPECT_EQ(pending->front().call_id, session.call_id);

  ASSERT_TRUE(calls_->UpdateInviteStatus(session.call_id, "account:bob", "accepted"));
  auto after = calls_->LoadPendingInvite(session.call_id, "account:bob");
  ASSERT_TRUE(after && after->has_value());
  EXPECT_EQ((*after)->status, "accepted");
}

TEST_F(CallSessionStoreTest, UpsertPreservesFirstJoinedAt) {
  CallSession session;
  session.call_id = "call:joined-at";
  session.origin_thread_id = "thread-1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = 1000;
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(calls_->UpsertSession(session));

  CallParticipant moto;
  moto.call_id = session.call_id;
  moto.identity = "account:moto";
  moto.state = CallParticipantState::Joined;
  moto.joined_at = 1000;
  ASSERT_TRUE(calls_->UpsertParticipant(moto));

  // CallRoster restamps with "now" on accept fan-out — must not steal SoftMigrate initiator.
  moto.joined_at = 9999;
  ASSERT_TRUE(calls_->UpsertParticipant(moto));

  auto parts = calls_->ListParticipants(session.call_id);
  ASSERT_TRUE(parts);
  ASSERT_EQ(parts->size(), 1u);
  ASSERT_TRUE(parts->front().joined_at);
  EXPECT_EQ(*parts->front().joined_at, 1000);
}

TEST_F(CallSessionStoreTest, UpsertTakesEarlierJoinedAtFromRoster) {
  CallSession session;
  session.call_id = "call:earlier-stamp";
  session.origin_thread_id = "thread-1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = 1000;
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(calls_->UpsertSession(session));

  CallParticipant owner;
  owner.call_id = session.call_id;
  owner.identity = "account:linux";
  owner.state = CallParticipantState::Joined;
  owner.joined_at = 5000; // wrong/late local stamp
  ASSERT_TRUE(calls_->UpsertParticipant(owner));

  owner.joined_at = 1000; // owner's CallRoster earliest stamp
  ASSERT_TRUE(calls_->UpsertParticipant(owner));

  auto parts = calls_->ListParticipants(session.call_id);
  ASSERT_TRUE(parts);
  ASSERT_TRUE(parts->front().joined_at);
  EXPECT_EQ(*parts->front().joined_at, 1000);
}

} // namespace
} // namespace pbr
