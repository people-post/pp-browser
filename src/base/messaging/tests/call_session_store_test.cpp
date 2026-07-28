#include "base/messaging/CallSessionStore.h"
#include "base/messaging/SqliteThreadStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>

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
  alice.identity = "relay:alice";
  alice.state = CallParticipantState::Joined;
  alice.joined_at = session.created_at;
  ASSERT_TRUE(calls_->UpsertParticipant(alice));

  CallParticipant bob;
  bob.call_id = session.call_id;
  bob.identity = "relay:bob";
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
  invite.inviter_identity = "relay:alice";
  invite.invitee_identity = "relay:bob";
  invite.media_mode = CallMediaMode::Voice;
  invite.created_at = session.created_at;
  invite.expires_at = session.created_at + 60'000;
  invite.status = "pending";
  ASSERT_TRUE(calls_->UpsertPendingInvite(invite));

  auto pending = calls_->ListPendingInvitesForInvitee("relay:bob");
  ASSERT_TRUE(pending);
  ASSERT_EQ(pending->size(), 1u);
  EXPECT_EQ(pending->front().call_id, session.call_id);

  ASSERT_TRUE(calls_->UpdateInviteStatus(session.call_id, "relay:bob", "accepted"));
  auto after = calls_->LoadPendingInvite(session.call_id, "relay:bob");
  ASSERT_TRUE(after && after->has_value());
  EXPECT_EQ((*after)->status, "accepted");
}

} // namespace
} // namespace pbr
