// Calls' store ports (CR024): one contract, run against the SQLite stores and the in-memory ones calls' tests use.
#include "lib/calls/testing/InMemoryCallStores.h"
#include "lib/storage/SqliteCallMediaKeyStore.h"
#include "lib/storage/SqliteCallSessionStore.h"

#include "common/Utilities.h"
#include "foundation/crypto/CryptoConstants.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace pbr {
namespace {

enum class Impl { Sqlite, Memory };

class CallStoreContractTest : public ::testing::TestWithParam<Impl> {
protected:
  void SetUp() override {
    if (GetParam() == Impl::Sqlite) {
      dir_ = std::filesystem::temp_directory_path() / ("pp_call_store_contract_" + util::GenerateUuid());
      std::filesystem::create_directories(dir_);
      const std::string db = (dir_ / "profile.db").string();
      auto keys = std::make_unique<SqliteCallMediaKeyStore>(db, "contract");
      ASSERT_TRUE(keys->SetDek(ByteVector(kDataEncryptionKeySize, 0x42)));
      keys_ = std::move(keys);
      sessions_ = std::make_unique<SqliteCallSessionStore>(db);
    } else {
      keys_ = std::make_unique<InMemoryCallMediaKeyStore>();
      sessions_ = std::make_unique<InMemoryCallSessionStore>();
    }
  }
  void TearDown() override {
    keys_.reset();
    sessions_.reset();
    if (!dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
    }
  }

  static CallSession Session(const std::string& id, int64_t created_at, CallSessionState state,
                             std::optional<std::string> group = std::nullopt) {
    CallSession session;
    session.call_id = id;
    session.created_at = created_at;
    session.state = state;
    session.origin_group_id = std::move(group);
    return session;
  }
  static CallParticipant Participant(CallParticipantState state, std::optional<int64_t> joined_at) {
    CallParticipant participant;
    participant.call_id = "call:a";
    participant.identity = "account:bob";
    participant.state = state;
    participant.joined_at = joined_at;
    return participant;
  }

  std::filesystem::path dir_;
  std::unique_ptr<ICallSessionStore> sessions_;
  std::unique_ptr<ICallMediaKeyStore> keys_;
};

TEST_P(CallStoreContractTest, ActiveSessionsAreTheUnendedNewestFirstAndAGroupsAreItsOwn) {
  ASSERT_TRUE(sessions_->UpsertSession(Session("call:old", 10, CallSessionState::Active, "group:g")));
  ASSERT_TRUE(sessions_->UpsertSession(Session("call:new", 20, CallSessionState::Ringing, "group:g")));
  ASSERT_TRUE(sessions_->UpsertSession(Session("call:ended", 30, CallSessionState::Ended, "group:g")));
  ASSERT_TRUE(sessions_->UpsertSession(Session("call:other", 40, CallSessionState::Active)));

  auto active = sessions_->ListActiveSessions().value();
  ASSERT_EQ(active.size(), 3u);
  EXPECT_EQ(active[0].call_id, "call:other");
  EXPECT_EQ(active[2].call_id, "call:old");
  auto group = sessions_->ListSessionsForGroup("group:g", 2).value();
  ASSERT_EQ(group.size(), 2u) << "at most `limit`, any state";
  EXPECT_EQ(group[0].call_id, "call:ended");
  EXPECT_EQ(sessions_->LoadSession("call:new").value()->state, CallSessionState::Ringing);
  EXPECT_FALSE(sessions_->LoadSession("call:none").value().has_value());
}

TEST_P(CallStoreContractTest, AStayKeepsItsEarliestJoinAndARejoinStartsANewOne) {
  ASSERT_TRUE(sessions_->UpsertParticipant(Participant(CallParticipantState::Joined, 100)));
  ASSERT_TRUE(sessions_->UpsertParticipant(Participant(CallParticipantState::Joined, 200)));
  EXPECT_EQ(sessions_->FindParticipant("call:a", "account:bob").value()->joined_at, std::optional<int64_t>(100));
  ASSERT_TRUE(sessions_->UpsertParticipant(Participant(CallParticipantState::Joined, std::nullopt)));
  EXPECT_EQ(sessions_->FindParticipant("call:a", "account:bob").value()->joined_at, std::optional<int64_t>(100));
  EXPECT_EQ(sessions_->CountJoined("call:a").value(), 1u);

  ASSERT_TRUE(sessions_->UpsertParticipant(Participant(CallParticipantState::Left, 100)));
  EXPECT_EQ(sessions_->CountJoined("call:a").value(), 0u);
  ASSERT_TRUE(sessions_->UpsertParticipant(Participant(CallParticipantState::Joined, 300)));
  EXPECT_EQ(sessions_->FindParticipant("call:a", "account:bob").value()->joined_at, std::optional<int64_t>(300))
      << "coming back is a new stay (B102)";
  EXPECT_EQ(sessions_->ListParticipants("call:a").value().size(), 1u);
}

TEST_P(CallStoreContractTest, TheInvitesToAnswerAreThePendingOnesNewestFirst) {
  PendingCallInvite first;
  first.call_id = "call:1";
  first.invitee_identity = "account:me";
  first.inviter_identity = "account:bob";
  first.created_at = 10;
  PendingCallInvite second = first;
  second.call_id = "call:2";
  second.created_at = 20;
  ASSERT_TRUE(sessions_->UpsertPendingInvite(first));
  ASSERT_TRUE(sessions_->UpsertPendingInvite(second));
  auto pending = sessions_->ListPendingInvitesForInvitee("account:me").value();
  ASSERT_EQ(pending.size(), 2u);
  EXPECT_EQ(pending[0].call_id, "call:2");
  ASSERT_TRUE(sessions_->UpdateInviteStatus("call:2", "account:me", "declined"));
  EXPECT_EQ(sessions_->ListPendingInvitesForInvitee("account:me").value().size(), 1u);
  EXPECT_EQ(sessions_->LoadPendingInvite("call:2", "account:me").value()->status, "declined");
}

TEST_P(CallStoreContractTest, AnEpochKeyComesBackForItsCallAndEpochOnly) {
  auto key = keys_->GenerateEpochKey();
  ASSERT_TRUE(key);
  ASSERT_EQ(key->size(), 32u);
  auto id = keys_->PutEpochKey("call:a", 1, *key);
  ASSERT_TRUE(id) << id.error().message;
  EXPECT_EQ(id->rfind("mk:", 0), 0u);
  EXPECT_EQ(keys_->LoadEpochKey("call:a", 1).value(), std::optional<ByteVector>(*key));
  EXPECT_FALSE(keys_->LoadEpochKey("call:a", 2).value().has_value());
  EXPECT_FALSE(keys_->PutEpochKey("", 1, *key)) << "a call id is required";
  auto replacement = keys_->GenerateEpochKey().value();
  ASSERT_TRUE(keys_->PutEpochKey("call:a", 1, replacement));
  EXPECT_EQ(keys_->LoadEpochKey("call:a", 1).value(), std::optional<ByteVector>(replacement));
}

INSTANTIATE_TEST_SUITE_P(Impls, CallStoreContractTest, ::testing::Values(Impl::Sqlite, Impl::Memory),
                         [](const ::testing::TestParamInfo<Impl>& info) {
                           return info.param == Impl::Sqlite ? std::string("Sqlite") : std::string("Memory");
                         });

} // namespace
} // namespace pbr
