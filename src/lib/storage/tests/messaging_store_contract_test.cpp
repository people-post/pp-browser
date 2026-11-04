// Messaging's store ports (CR024): one contract, run against the SQLite stores and the in-memory ones the tests use,
// so a test passing in memory means what it would mean on the app's storage.
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/storage/SqliteGroupRosterStore.h"
#include "lib/storage/SqliteThreadLocalPrefsStore.h"
#include "lib/storage/SqliteThreadMentionsStore.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <map>

#include <filesystem>
#include <memory>

namespace pbr {
namespace {

/** One implementation of messaging's store ports, with whatever it keeps them in. */
class Stores {
public:
  virtual ~Stores() = default;
  virtual IGroupRosterStore& Rosters() = 0;
  virtual IThreadLocalPrefsStore& Prefs() = 0;
  virtual IThreadMentionsStore& Mentions() = 0;
};

class SqliteStores : public Stores {
public:
  SqliteStores() : dir_(std::filesystem::temp_directory_path() / ("pp_store_contract_" + util::GenerateUuid())) {
    std::filesystem::create_directories(dir_);
    const std::string db = (dir_ / "profile.db").string();
    rosters_ = std::make_unique<SqliteGroupRosterStore>(db);
    prefs_ = std::make_unique<SqliteThreadLocalPrefsStore>(db);
    mentions_ = std::make_unique<SqliteThreadMentionsStore>(db);
  }
  ~SqliteStores() override {
    // Close every handle before the directory goes (Windows: a file in use cannot be removed).
    rosters_.reset();
    prefs_.reset();
    mentions_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  IGroupRosterStore& Rosters() override { return *rosters_; }
  IThreadLocalPrefsStore& Prefs() override { return *prefs_; }
  IThreadMentionsStore& Mentions() override { return *mentions_; }

private:
  std::filesystem::path dir_;
  std::unique_ptr<SqliteGroupRosterStore> rosters_;
  std::unique_ptr<SqliteThreadLocalPrefsStore> prefs_;
  std::unique_ptr<SqliteThreadMentionsStore> mentions_;
};

class MemoryStores : public Stores {
public:
  IGroupRosterStore& Rosters() override { return rosters_; }
  IThreadLocalPrefsStore& Prefs() override { return prefs_; }
  IThreadMentionsStore& Mentions() override { return mentions_; }

private:
  InMemoryDeliveryStore threads_;
  InMemoryGroupRosterStore rosters_{threads_};
  InMemoryThreadLocalPrefsStore prefs_;
  InMemoryThreadMentionsStore mentions_;
};

enum class Impl { Sqlite, Memory };

class MessagingStoreContractTest : public ::testing::TestWithParam<Impl> {
protected:
  void SetUp() override {
    if (GetParam() == Impl::Sqlite) {
      stores_ = std::make_unique<SqliteStores>();
    } else {
      stores_ = std::make_unique<MemoryStores>();
    }
  }
  void TearDown() override { stores_.reset(); }

  IGroupRosterStore& rosters() { return stores_->Rosters(); }

  static PendingGroupInvite Invite(const std::string& nonce, const std::string& invitee, bool outbound,
                                   int64_t created_at, std::optional<int64_t> expires_at = std::nullopt) {
    PendingGroupInvite invite;
    invite.invite_nonce = nonce;
    invite.group_id = "group:g";
    invite.group_title = "G";
    invite.inviter_identity = outbound ? "account:me" : "account:carol";
    invite.invitee_identity = invitee;
    invite.created_at = created_at;
    invite.expires_at = expires_at;
    invite.outbound = outbound;
    return invite;
  }

  std::unique_ptr<Stores> stores_;
};

TEST_P(MessagingStoreContractTest, MetadataAndMembersRoundTripAndNotify) {
  std::vector<GroupRosterChange> changed;
  rosters().SetOnChanged([&](const GroupRosterChange& change) { changed.push_back(change); });
  GroupMetadata metadata;
  metadata.group_id = "group:g";
  metadata.owner_identity = "account:me";
  metadata.title = "Hike";
  metadata.roster_epoch = 3;
  ASSERT_TRUE(rosters().UpsertMetadata(metadata));
  GroupRosterMember bob{"account:bob", "contact:bob", MemberRole::Member, 10};
  ASSERT_TRUE(rosters().UpsertMember("group:g", bob));
  bob.joined_at = 20;
  ASSERT_TRUE(rosters().UpsertMember("group:g", bob));

  auto loaded = rosters().LoadMetadata("group:g");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->title, "Hike");
  EXPECT_EQ((*loaded)->roster_epoch, 3u);
  EXPECT_EQ(rosters().ListGroupIds().value(), std::vector<std::string>{"group:g"});
  auto members = rosters().ListMembers("group:g").value();
  ASSERT_EQ(members.size(), 1u) << "one row per member";
  EXPECT_EQ(members[0].joined_at, 20);
  EXPECT_TRUE(rosters().IsMember("group:g", "account:bob").value());
  ASSERT_TRUE(rosters().RemoveMember("group:g", "account:bob"));
  EXPECT_FALSE(rosters().IsMember("group:g", "account:bob").value());
  ASSERT_EQ(changed.size(), 4u) << "metadata, two member writes, a removal";
  EXPECT_EQ(changed[0].group_id, "group:g");
  EXPECT_TRUE(changed[1].member_gone.empty()) << "a write takes no one off";
  EXPECT_EQ(changed[3].member_gone, "account:bob") << "the removal names who is gone (a group call drops them)";
  ASSERT_TRUE(rosters().RemoveMember("group:g", "account:bob"));
  ASSERT_EQ(changed.size(), 5u);
  EXPECT_TRUE(changed[4].member_gone.empty()) << "nobody was left to take off";
}

TEST_P(MessagingStoreContractTest, AStoredInviteKeepsItsFirstVersion) {
  ASSERT_TRUE(rosters().UpsertPendingInvite(Invite("n1", "account:bob", true, 100)));
  PendingGroupInvite again = Invite("n1", "account:dave", true, 200);
  ASSERT_TRUE(rosters().UpsertPendingInvite(again));
  EXPECT_EQ(rosters().LoadPendingInvite("n1").value()->invitee_identity, "account:bob");
  ASSERT_TRUE(rosters().UpdateInviteStatus("n1", InviteStatus::Accepted));
  EXPECT_EQ(rosters().LoadPendingInvite("n1").value()->status, InviteStatus::Accepted);
  EXPECT_FALSE(rosters().LoadPendingInvite("unknown").value().has_value());
}

TEST_P(MessagingStoreContractTest, OutboundInvitesAreTheLatestLiveOnePerInvitee) {
  ASSERT_TRUE(rosters().UpsertPendingInvite(Invite("old", "account:bob", true, 100)));
  ASSERT_TRUE(rosters().UpsertPendingInvite(Invite("new", "account:bob", true, 200)));
  ASSERT_TRUE(rosters().UpsertPendingInvite(Invite("dave", "account:dave", true, 150, 1000)));
  ASSERT_TRUE(rosters().UpsertPendingInvite(Invite("in", "account:me", false, 300)));
  auto live = rosters().ListOutboundPendingInvites("group:g", 500).value();
  ASSERT_EQ(live.size(), 2u);
  EXPECT_EQ(live[0].invite_nonce, "new") << "newest first, one per invitee";
  EXPECT_EQ(live[1].invite_nonce, "dave");
  EXPECT_EQ(rosters().ListOutboundPendingInvites("group:g", 2000).value().size(), 1u) << "dave's expired by then";

  ASSERT_TRUE(rosters().ExpireOutboundInvitesTo("group:g", "account:bob"));
  EXPECT_EQ(rosters().LoadPendingInvite("new").value()->status, InviteStatus::Expired);
  EXPECT_EQ(rosters().CountPendingInvitesSince(0).value(), 1u) << "only the inbound one counts";
}

TEST_P(MessagingStoreContractTest, EndingAGroupIsOneStepAndOnlyOnce) {
  GroupMetadata metadata;
  metadata.group_id = "group:g";
  metadata.owner_identity = "account:me";
  metadata.roster_epoch = 5;
  ASSERT_TRUE(rosters().UpsertMetadata(metadata));
  ASSERT_TRUE(rosters().UpsertMember("group:g", {"account:bob", "contact:bob", MemberRole::Member, 1}));
  ASSERT_TRUE(rosters().UpsertPendingInvite(Invite("n1", "account:dave", true, 100)));
  ASSERT_TRUE(rosters().UpsertGroupTarget("group:g", "thread-g", 2, 7));

  GroupEnding ending;
  ending.owner_identity = "account:me";
  ending.roster_epoch = 9;
  ending.statement_json = "{}";
  ending.sends.push_back({"", "account:bob", "group_dissolved", "{}", "Bob"});
  std::vector<GroupRosterChange> changed;
  rosters().SetOnChanged([&](const GroupRosterChange& change) { changed.push_back(change); });
  auto ended = rosters().EndGroup("group:g", ending);
  ASSERT_TRUE(ended && ended->has_value());
  ASSERT_EQ(changed.size(), 1u);
  EXPECT_TRUE(changed[0].ended) << "a group call of it ends";
  ASSERT_EQ((*ended)->size(), 1u) << "the pending invite voided";
  EXPECT_TRUE(rosters().ListMembers("group:g").value().empty());
  EXPECT_FALSE(rosters().FindThreadIdForGroup("group:g").value().has_value()) << "no send target any more";
  auto after = rosters().LoadMetadata("group:g").value();
  EXPECT_EQ(after->sync_state, GroupSyncState::Dissolved);
  EXPECT_EQ(after->roster_epoch, 9u);
  auto sends = rosters().ListPendingSends().value();
  ASSERT_EQ(sends.size(), 1u);
  EXPECT_EQ(sends[0].group_id, "group:g");

  auto again = rosters().EndGroup("group:g", ending);
  ASSERT_TRUE(again);
  EXPECT_FALSE(again->has_value()) << "already dissolved";
  EXPECT_FALSE(rosters().EndGroup("group:unknown", ending).value().has_value());
  ASSERT_TRUE(rosters().DeletePendingSend(sends[0]));
  EXPECT_TRUE(rosters().ListPendingSends("group:g").value().empty());
}

TEST_P(MessagingStoreContractTest, AGroupNumbersOnItsTargetAndAMemberPerPairEpoch) {
  EXPECT_FALSE(rosters().AllocateGroupSenderSeq("group:g")) << "no target yet";
  EXPECT_FALSE(rosters().GetGroupSessionEpoch("group:g"));
  ASSERT_TRUE(rosters().UpsertGroupTarget("group:g", "thread-g", 1, 1));
  EXPECT_EQ(rosters().FindThreadIdForGroup("group:g").value(), std::optional<std::string>("thread-g"));
  EXPECT_EQ(rosters().AllocateGroupSenderSeq("group:g").value(), 1u);
  EXPECT_EQ(rosters().AllocateGroupSenderSeq("group:g").value(), 2u);
  ASSERT_TRUE(rosters().BumpGroupSessionEpoch("group:g", 2));
  EXPECT_EQ(rosters().GetGroupSessionEpoch("group:g").value(), 2u);
  EXPECT_EQ(rosters().AllocateGroupSenderSeq("group:g").value(), 1u) << "a new epoch numbers from 1";
  ASSERT_TRUE(rosters().ClearGroupTargetByThreadId("thread-g"));
  EXPECT_FALSE(rosters().FindThreadIdForGroup("group:g").value().has_value());

  EXPECT_EQ(rosters().AllocateMemberSeq("group:g", "account:bob", 1).value(), 1u);
  EXPECT_EQ(rosters().AllocateMemberSeq("group:g", "account:bob", 1).value(), 2u);
  EXPECT_EQ(rosters().AllocateMemberSeq("group:g", "account:bob", 1, "QmPhone").value(), 1u) << "a device pair";
  EXPECT_EQ(rosters().AllocateMemberSeq("group:g", "account:bob", 2).value(), 1u) << "a newer pair epoch";
  EXPECT_EQ(rosters().AllocateMemberSeq("group:g", "account:bob", 1).value(), 2u) << "an older one never repeats";
}

TEST_P(MessagingStoreContractTest, ThreadPrefsKeepOnlyWhatDiffersFromTheDefaults) {
  IThreadLocalPrefsStore& prefs = stores_->Prefs();
  EXPECT_EQ(prefs.Get("t1").value().pinned_at, 0) << "defaults for a thread with no row";
  ThreadLocalPrefs pinned;
  pinned.thread_id = "t1";
  pinned.pinned_at = 5;
  ASSERT_TRUE(prefs.Set(pinned));
  EXPECT_EQ(prefs.Get("t1").value().pinned_at, 5);
  EXPECT_EQ(prefs.List().value().size(), 1u);
  pinned.pinned_at = 0;
  ASSERT_TRUE(prefs.Set(pinned));
  EXPECT_TRUE(prefs.List().value().empty()) << "back at the defaults: no row";
}

TEST_P(MessagingStoreContractTest, MentionsAreOnePerMessageOldestFirst) {
  IThreadMentionsStore& mentions = stores_->Mentions();
  ASSERT_TRUE(mentions.Add("t1", "m2", 20));
  ASSERT_TRUE(mentions.Add("t1", "m1", 10));
  ASSERT_TRUE(mentions.Add("t1", "m1", 30));
  ASSERT_TRUE(mentions.Add("t2", "m3", 5));
  EXPECT_EQ(mentions.Oldest("t1").value(), std::optional<std::string>("m1"));
  EXPECT_EQ(mentions.Threads().value(), (std::set<std::string>{"t1", "t2"}));
  EXPECT_EQ(mentions.Counts().value(), (std::map<std::string, int>{{"t1", 2}, {"t2", 1}})) << "one per message";
  ASSERT_TRUE(mentions.DropMessage("t1", "m1"));
  EXPECT_EQ(mentions.Oldest("t1").value(), std::optional<std::string>("m2"));
  ASSERT_TRUE(mentions.DropThread("t1"));
  EXPECT_EQ(mentions.Threads().value(), std::set<std::string>{"t2"});
  EXPECT_EQ(mentions.Counts().value(), (std::map<std::string, int>{{"t2", 1}}));
  EXPECT_FALSE(mentions.Oldest("t1").value().has_value());
}

INSTANTIATE_TEST_SUITE_P(Impls, MessagingStoreContractTest, ::testing::Values(Impl::Sqlite, Impl::Memory),
                         [](const ::testing::TestParamInfo<Impl>& info) {
                           return info.param == Impl::Sqlite ? std::string("Sqlite") : std::string("Memory");
                         });

} // namespace
} // namespace pbr
