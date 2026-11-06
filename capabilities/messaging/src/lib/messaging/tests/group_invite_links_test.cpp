// G021 (o5): invite links — the link's text form, the secret's hash, the wire of a join by link, the answerer's
// checks (each refusal, the rate, the uses), the drafts of a link, its revoke and a joiner's add, and a link dying
// with its maker.
#include "lib/messaging/GroupInviteLink.h"
#include "lib/messaging/GroupLinkJoin.h"
#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/tests/GroupChangeTestKit.h"

#include "foundation/crypto/CryptoUtil.h"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using test::kGroupId;
using test::kNowMs;
using test::Named;
using test::Person;
using test::Row;
namespace gil = group_invite_link;
namespace glj = group_link_join;
namespace grc = group_change;
namespace grs = group_roster_sync;
using Refusal = GroupJoinRefusal;

constexpr char kMaker[] = "account:carol";

GroupJoinLink ALink() {
  return {.link_id = "00112233445566778899aabbccddeeff",
          .secret = ByteVector(16, 0x5a),
          .expires_at = kNowMs + gil::kDefaultLifetimeMs,
          .maker = kMaker};
}

// ---- The link's text form ----

TEST(GroupInviteLinkTest, ALinkSurvivesItsTextAndNamesNothingOfTheGroup) {
  const GroupJoinLink link = ALink();
  const std::string text = gil::Encode(link);
  EXPECT_EQ(text.rfind("pp://join/", 0), 0u);
  EXPECT_EQ(text.find(kGroupId), std::string::npos);
  auto back = gil::Find(text);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(back->link_id, link.link_id);
  EXPECT_EQ(back->secret, link.secret);
  EXPECT_EQ(back->expires_at, link.expires_at);
  EXPECT_EQ(back->maker, link.maker);

  auto pasted = gil::Find("Join us: " + text + " (it lasts a week)");
  ASSERT_TRUE(pasted) << "found inside pasted text";
  EXPECT_EQ(pasted->link_id, link.link_id);
}

TEST(GroupInviteLinkTest, AMintedLinkIsRandomAndItsIdIsHex) {
  const GroupJoinLink a = gil::Mint(kMaker, kNowMs + 1000);
  const GroupJoinLink b = gil::Mint(kMaker, kNowMs + 1000);
  EXPECT_EQ(a.link_id.size(), 32u);
  EXPECT_EQ(a.secret.size(), gil::kSecretBytes);
  EXPECT_NE(a.link_id, b.link_id);
  EXPECT_NE(a.secret, b.secret);
  EXPECT_TRUE(gil::Find(gil::Encode(a)));
}

TEST(GroupInviteLinkTest, AMalformedLinkIsRefused) {
  const auto bytes_of = [](const GroupJoinLink& link) {
    ByteVector b = HexToBytes(link.link_id).value();
    b.insert(b.end(), link.secret.begin(), link.secret.end());
    for (int shift = 56; shift >= 0; shift -= 8) {
      b.push_back(static_cast<uint8_t>(static_cast<uint64_t>(link.expires_at) >> shift));
    }
    return b;
  };
  const auto text_of = [](ByteVector b, const std::string& maker) {
    b.insert(b.end(), maker.begin(), maker.end());
    return std::string(kGroupJoinLinkPrefix) + Base64UrlEncode(b);
  };
  const GroupJoinLink link = ALink();
  EXPECT_FALSE(gil::Find("no link here"));
  EXPECT_FALSE(gil::Find("pp://join/"));
  EXPECT_FALSE(gil::Find("pp://join/***"));
  EXPECT_FALSE(gil::Find(text_of(ByteVector(20, 1), ""))) << "short";
  EXPECT_FALSE(gil::Find(text_of(bytes_of(link), ""))) << "no maker";
  EXPECT_FALSE(gil::Find(text_of(bytes_of(link), "relay:carol"))) << "a maker that is not an account";
  EXPECT_FALSE(gil::Find(text_of(bytes_of(link), "account:"))) << "an empty account";
  EXPECT_FALSE(gil::Find(text_of(bytes_of(link), "account:ca rol"))) << "a space in the account";
  EXPECT_FALSE(gil::Find(text_of(bytes_of(link), "account:" + std::string(200, 'c')))) << "overlong";
  GroupJoinLink no_expiry = link;
  no_expiry.expires_at = 0;
  EXPECT_FALSE(gil::Find(text_of(bytes_of(no_expiry), kMaker)));
  EXPECT_TRUE(gil::Find(text_of(bytes_of(link), kMaker)));
}

TEST(GroupInviteLinkTest, TheSecretsHashBindsTheLinkAndTheSecret) {
  const GroupJoinLink link = ALink();
  const std::string hash = gil::SecretHash(link.link_id, link.secret);
  EXPECT_EQ(hash.size(), 64u);
  EXPECT_EQ(hash, gil::SecretHash(link.link_id, link.secret));
  EXPECT_NE(hash, gil::SecretHash("ffeeddccbbaa99887766554433221100", link.secret));
  EXPECT_NE(hash, gil::SecretHash(link.link_id, ByteVector(16, 0x5b)));
  const GroupInviteLink in_roster{.link_id = link.link_id, .secret_hash = hash, .maker = kMaker};
  EXPECT_TRUE(gil::SecretMatches(in_roster, link.secret));
  EXPECT_FALSE(gil::SecretMatches(in_roster, ByteVector(16, 0)));
  EXPECT_FALSE(gil::SecretMatches(in_roster, {}));
}

// ---- The wire ----

template <typename T>
T RoundTrip(const GroupJoinMessage& message) {
  auto built = glj::Build(message);
  EXPECT_TRUE(built);
  EXPECT_TRUE(glj::IsJoinMessage(*built));
  auto parsed = glj::Parse(*built);
  EXPECT_TRUE(parsed && *parsed) << (parsed ? "not a join message" : parsed.error().message);
  EXPECT_TRUE(std::holds_alternative<T>(**parsed));
  return std::get<T>(**parsed);
}

TEST(GroupLinkJoinWireTest, EachMessageSurvivesTheLane) {
  const GroupJoinRequest request =
      RoundTrip<GroupJoinRequest>(GroupJoinRequest{.request_id = "r1", .link_id = "l1", .secret = ByteVector(16, 7)});
  EXPECT_EQ(request.link_id, "l1");
  EXPECT_EQ(request.secret, ByteVector(16, 7));

  const GroupJoinPreview preview = RoundTrip<GroupJoinPreview>(GroupJoinPreview{.request_id = "r1",
                                                                                .group_id = kGroupId,
                                                                                .owner_identity = "account:alice",
                                                                                .group_title = "Hikers",
                                                                                .member_count = 4,
                                                                                .device = "12D3device"});
  EXPECT_EQ(preview.group_title, "Hikers");
  EXPECT_EQ(preview.member_count, 4u);
  EXPECT_EQ(preview.owner_identity, "account:alice");
  EXPECT_EQ(preview.device, "12D3device");

  for (const Refusal reason : {Refusal::Expired, Refusal::Revoked, Refusal::UsedUp, Refusal::Removed,
                               Refusal::GroupFull, Refusal::Rate, Refusal::Unknown}) {
    EXPECT_EQ(RoundTrip<GroupJoinRefused>(GroupJoinRefused{.request_id = "r1", .reason = reason}).reason, reason);
  }
  EXPECT_EQ(glj::RefusalToString(Refusal::UsedUp), "used_up");
  EXPECT_EQ(glj::RefusalFromString("something new"), Refusal::Unknown);

  EXPECT_EQ(RoundTrip<GroupJoinConfirm>(GroupJoinConfirm{.request_id = "r1", .device = "12D3device"}).device,
            "12D3device");
}

TEST(GroupLinkJoinWireTest, ABrokenOrForeignMessageIsNotTaken) {
  ThreadMessage text;
  text.content_type = ChatContentType::Text;
  EXPECT_FALSE(glj::IsJoinMessage(text));

  ThreadMessage other;
  other.content_type = ChatContentType::System;
  other.payload_json = R"({"control_type":"card_ask","detail":"{}"})";
  EXPECT_FALSE(glj::IsJoinMessage(other));
  EXPECT_FALSE(glj::Parse(other).value().has_value());

  ThreadMessage short_secret;
  short_secret.content_type = ChatContentType::System;
  short_secret.payload_json =
      R"({"control_type":"group_join_request","detail":"{\"request_id\":\"r\",\"link_id\":\"l\",\"secret\":\"00\"}"})";
  EXPECT_TRUE(glj::IsJoinMessage(short_secret));
  EXPECT_FALSE(glj::Parse(short_secret)) << "a secret of the wrong size";

  ThreadMessage no_device;
  no_device.content_type = ChatContentType::System;
  no_device.payload_json = R"({"control_type":"group_join_confirm","detail":"{\"request_id\":\"r\"}"})";
  EXPECT_FALSE(glj::Parse(no_device)) << "a confirm names the device that previewed";
}

// ---- The answerer's checks ----

/** Alice owns, Bob is admin, Carol and Dave are members; Carol made link-1 (its secret 0x5a…). */
class GroupLinkJudgeTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (Person* p : {&alice, &bob, &carol, &dave, &erin, &frank}) {
      book.Hold(p->Card());
    }
  }

  GroupRosterStatement Statement(const GroupPreset preset = GroupPreset::Open,
                                 const std::function<void(GroupRosterStatement&)>& edit = {}) {
    GroupRosterStatement draft;
    draft.roster_epoch = 5;
    draft.members = {Row(alice.Id(), MemberRole::Owner), Row(bob.Id(), MemberRole::Admin), Row(carol.Id()),
                     Row(dave.Id())};
    draft.preset = preset;
    draft.title = "Hikers";
    if (edit) {
      edit(draft);
    }
    return test::SignedStatement(alice, draft);
  }

  GroupChange CarolsLink(const uint64_t max_uses = 0, const int64_t expires_at = kNowMs + 86'400'000) {
    GroupChange c = carol.Draft(GroupChangeKind::LinkAdd);
    c.link_id = "link-1";
    c.secret_hash = gil::SecretHash("link-1", secret);
    c.expires_at = expires_at;
    c.max_uses = max_uses;
    return carol.Signed(std::move(c));
  }

  /** Carol's device judging `joiner`'s request with `with_secret`, over `changes` on top of `statement`. */
  std::optional<Refusal> Judge(const GroupRosterStatement& statement, const std::vector<GroupChange>& changes,
                               const std::string& joiner, const ByteVector& with_secret,
                               const std::optional<int64_t> made_here = std::nullopt, const bool within_rate = true) {
    const GroupRosterView view = ComputeRoster(statement, changes, book.Lookup(), kNowMs);
    return glj::JudgeJoin({.view = &view,
                           .made_here_expires_at = made_here,
                           .link_id = "link-1",
                           .secret = with_secret,
                           .joiner = joiner,
                           .local_identity = carol.Id(),
                           .now_ms = kNowMs,
                           .within_rate = within_rate});
  }
  std::optional<Refusal> Judge(const std::vector<GroupChange>& changes, const std::string& joiner) {
    return Judge(Statement(), changes, joiner, secret);
  }

  Person& alice = Named("alice");
  Person& bob = Named("bob");
  Person& carol = Named("carol");
  Person& dave = Named("dave");
  Person& erin = Named("erin");
  Person& frank = Named("frank");
  test::CardBook book;
  const ByteVector secret = ByteVector(16, 0x5a);
};

TEST_F(GroupLinkJudgeTest, ALiveLinkWithItsSecretLetsAStrangerSeeThePreview) {
  EXPECT_EQ(Judge({CarolsLink()}, erin.Id()), std::nullopt);
  EXPECT_EQ(Judge({CarolsLink()}, dave.Id()), std::nullopt) << "someone already in gets the bundle again";
}

TEST_F(GroupLinkJudgeTest, AWrongSecretAndNoSuchLinkLookTheSame) {
  EXPECT_EQ(Judge(Statement(), {CarolsLink()}, erin.Id(), ByteVector(16, 0)), Refusal::Unknown);
  EXPECT_EQ(Judge({}, erin.Id()), Refusal::Unknown) << "the roster knows no such link";
  EXPECT_EQ(glj::JudgeJoin({.link_id = "link-1", .secret = secret, .joiner = erin.Id(), .now_ms = kNowMs}),
            Refusal::Unknown)
      << "no group here knows it";
}

TEST_F(GroupLinkJudgeTest, ARevokedLinkIsRefusedAsRevoked) {
  EXPECT_EQ(Judge({CarolsLink(), bob.LinkRevoke("link-1")}, erin.Id()), Refusal::Revoked);
  EXPECT_EQ(Judge({CarolsLink(), carol.LinkRevoke("link-1")}, erin.Id()), Refusal::Revoked);
  EXPECT_EQ(Judge({CarolsLink(), dave.LinkRevoke("link-1")}, erin.Id()), std::nullopt)
      << "a member may not revoke someone else's link";
}

TEST_F(GroupLinkJudgeTest, ALinkDiesWithItsMaker) {
  EXPECT_EQ(Judge({CarolsLink(), carol.Leave()}, erin.Id()), Refusal::Revoked) << "the maker left";
  EXPECT_EQ(Judge({CarolsLink(), bob.Remove(carol.Id())}, erin.Id()), Refusal::Revoked) << "the maker was removed";
}

TEST_F(GroupLinkJudgeTest, AnExpiredLinkIsRefusedAsExpired) {
  EXPECT_EQ(Judge({CarolsLink(0, kNowMs - 1)}, erin.Id()), Refusal::Expired);
}

TEST_F(GroupLinkJudgeTest, OurLinkTheRosterNoLongerNamesIsRevokedOrExpired) {
  // Its revoke was folded: the statement no longer lists it, but this device made it and remembers.
  EXPECT_EQ(Judge(Statement(), {}, erin.Id(), secret, kNowMs + 1000), Refusal::Revoked);
  EXPECT_EQ(Judge(Statement(), {}, erin.Id(), secret, kNowMs - 1), Refusal::Expired);
}

TEST_F(GroupLinkJudgeTest, ALinkIsUsedUpAtItsLimit) {
  const GroupChange link = CarolsLink(/*max_uses=*/2);
  const GroupChange erins = carol.AddByLink(erin.Id(), "link-1");
  EXPECT_EQ(Judge({link, erins}, frank.Id()), std::nullopt) << "one of two used";
  const GroupChange franks = carol.AddByLink(frank.Id(), "link-1");
  EXPECT_EQ(Judge({link, erins, franks}, Named("gina").Id()), Refusal::UsedUp);
}

TEST_F(GroupLinkJudgeTest, SomeoneRemovedIsRefusedAsRemoved) {
  const GroupRosterStatement s = Statement(GroupPreset::Open, [&](GroupRosterStatement& d) {
    d.removed = {{.identity = erin.Id(), .removal = "removal-1", .removed_by = bob.Id()}};
  });
  EXPECT_EQ(Judge(s, {CarolsLink()}, erin.Id(), secret), Refusal::Removed);
  EXPECT_EQ(Judge(s, {CarolsLink()}, frank.Id(), secret), std::nullopt);
}

TEST_F(GroupLinkJudgeTest, AFullGroupIsRefused) {
  const GroupRosterStatement s = Statement(GroupPreset::Open, [&](GroupRosterStatement& d) {
    for (size_t i = d.members.size(); i < kMaxGroupMembers; ++i) {
      d.members.push_back(Row("account:filler" + std::to_string(i)));
    }
  });
  EXPECT_EQ(Judge(s, {CarolsLink()}, erin.Id(), secret), Refusal::GroupFull);
  EXPECT_EQ(Judge(s, {CarolsLink()}, dave.Id(), secret), std::nullopt) << "someone in already takes no place";
}

TEST_F(GroupLinkJudgeTest, AManagedGroupsMemberCannotAnswerForALink) {
  // The preset went Managed: a member's link_add no longer counts, and the roster does not know the link.
  EXPECT_EQ(Judge(Statement(GroupPreset::Managed), {CarolsLink()}, erin.Id(), secret), Refusal::Unknown);
  EXPECT_EQ(Judge(Statement(GroupPreset::Managed), {CarolsLink()}, erin.Id(), secret, kNowMs + 1000), Refusal::Revoked)
      << "one this device made reads as revoked";
}

TEST_F(GroupLinkJudgeTest, ThePastTheRateRequestIsRefusedBeforeItsSecretIsLookedAt) {
  EXPECT_EQ(Judge(Statement(), {CarolsLink()}, erin.Id(), ByteVector(16, 0), std::nullopt, /*within_rate=*/false),
            Refusal::Rate);
}

TEST(GroupJoinLimiterTest, TwentyAnHourPerLink) {
  glj::JoinRequestLimiter limiter;
  for (size_t i = 0; i < glj::kRequestsPerHour; ++i) {
    EXPECT_TRUE(limiter.Allow("link-1", kNowMs + static_cast<int64_t>(i)));
  }
  EXPECT_FALSE(limiter.Allow("link-1", kNowMs + 100));
  EXPECT_TRUE(limiter.Allow("link-2", kNowMs + 100)) << "per link";
  EXPECT_FALSE(limiter.Allow("link-1", kNowMs + glj::kRateWindowMs - 1));
  EXPECT_TRUE(limiter.Allow("link-1", kNowMs + glj::kRateWindowMs)) << "the first one of the hour is out of the window";
}

// ---- The drafts, on a device's store ----

class GroupLinkDraftTest : public GroupLinkJudgeTest {
protected:
  void SetUp() override {
    GroupLinkJudgeTest::SetUp();
    store_ = std::make_unique<InMemoryDeliveryStore>();
    ASSERT_TRUE(store_->ListThreads());
    roster_ = std::make_unique<InMemoryGroupRosterStore>(*store_);
  }

  void TearDown() override {
    roster_.reset();
    store_.reset();
  }

  void Seed(const GroupRosterStatement& statement) {
    GroupMetadata metadata;
    metadata.group_id = kGroupId;
    metadata.owner_identity = alice.Id();
    metadata.roster_epoch = statement.roster_epoch;
    metadata.title = statement.title;
    metadata.policy.preset = statement.preset;
    ASSERT_TRUE(roster_->UpsertMetadata(metadata));
    ASSERT_TRUE(*grs::KeepStatement(*roster_, statement));
    ASSERT_TRUE(grs::ApplyView(*roster_, kGroupId, book.Lookup(), kNowMs));
  }

  void Hold(const GroupChange& change) {
    auto received = grs::ReceiveChange(*roster_, change, book.Lookup(), kNowMs);
    ASSERT_TRUE(received) << received.error().message;
    ASSERT_EQ(received->held, grs::ChangeHeld::Proved);
  }

  Roe<GroupChange> LinkBy(const Person& who, const std::string& link_id = "link-1") {
    auto draft = grs::DraftLinkAdd(*roster_, kGroupId, who.Id(), who.Device().peer_id, link_id,
                                   gil::SecretHash(link_id, secret), kNowMs + 86'400'000, 0, book.Lookup(), kNowMs);
    if (!draft) {
      return draft.error();
    }
    return who.Signed(std::move(*draft));
  }

  std::optional<GroupRosterView> View() { return grs::CurrentView(*roster_, kGroupId, book.Lookup(), kNowMs); }

  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<InMemoryGroupRosterStore> roster_;
};

TEST_F(GroupLinkDraftTest, WhoMakesALinkFollowsThePreset) {
  Seed(Statement());
  auto carols = LinkBy(carol);
  ASSERT_TRUE(carols) << carols.error().message;
  EXPECT_EQ(carols->kind, GroupChangeKind::LinkAdd);
  EXPECT_EQ(carols->secret_hash, gil::SecretHash("link-1", secret));
  Hold(*carols);
  ASSERT_NE(View()->LiveLink("link-1"), nullptr);
  EXPECT_FALSE(LinkBy(carol)) << "the id is taken";

  TearDown();
  SetUp();
  Seed(Statement(GroupPreset::Managed));
  auto refused = LinkBy(carol);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().message, kGroupLinkNotAllowedError);
  EXPECT_TRUE(LinkBy(bob)) << "an admin may in a Managed group";
}

TEST_F(GroupLinkDraftTest, ItsMakerAdminsAndTheOwnerRevokeAndNoOneElse) {
  Seed(Statement());
  Hold(CarolsLink());
  const auto revoke_by = [&](const Person& who) {
    return grs::DraftLinkRevoke(*roster_, kGroupId, who.Id(), who.Device().peer_id, "link-1", book.Lookup(), kNowMs);
  };
  EXPECT_TRUE(revoke_by(carol));
  EXPECT_TRUE(revoke_by(bob));
  EXPECT_TRUE(revoke_by(alice));
  auto daves = revoke_by(dave);
  ASSERT_FALSE(daves);
  EXPECT_EQ(daves.error().message, kGroupLinkRevokeNotAllowedError);

  Hold(carol.Signed(*revoke_by(carol)));
  auto again = revoke_by(bob);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().message, kGroupLinkGoneError);
}

TEST_F(GroupLinkDraftTest, AConfirmedJoinIsAnAddWithTheLinkThatTheLinkCounts) {
  Seed(Statement());
  Hold(CarolsLink(/*max_uses=*/1));
  auto draft = grs::DraftLinkJoin(*roster_, kGroupId, carol.Id(), carol.Device().peer_id, erin.Id(), "link-1",
                                  book.Lookup(), kNowMs);
  ASSERT_TRUE(draft) << draft.error().message;
  EXPECT_EQ(draft->kind, GroupChangeKind::Add);
  EXPECT_EQ(draft->link_id, "link-1");
  EXPECT_TRUE(draft->invite_nonce.empty());
  const GroupChange add = carol.Signed(std::move(*draft));
  Hold(add);
  const std::optional<GroupRosterView> view = View();
  ASSERT_TRUE(view);
  EXPECT_TRUE(view->Lists(erin.Id()));
  EXPECT_EQ(view->LiveLink("link-1")->uses, 1u);
  EXPECT_TRUE(roster_->IsMember(kGroupId, erin.Id()).value());
  EXPECT_EQ(glj::JudgeJoin({.view = &*view,
                            .link_id = "link-1",
                            .secret = secret,
                            .joiner = frank.Id(),
                            .local_identity = carol.Id(),
                            .now_ms = kNowMs}),
            Refusal::UsedUp)
      << "its one use is taken";
}

TEST_F(GroupLinkDraftTest, AJoinByLinkIsNeverAReadmit) {
  Seed(Statement(GroupPreset::Open, [&](GroupRosterStatement& d) {
    d.removed = {{.identity = erin.Id(), .removal = "removal-1", .removed_by = bob.Id()}};
  }));
  for (const Person* answerer : {&carol, &bob}) {
    auto draft = grs::DraftLinkJoin(*roster_, kGroupId, answerer->Id(), answerer->Device().peer_id, erin.Id(), "link-1",
                                    book.Lookup(), kNowMs);
    ASSERT_FALSE(draft) << answerer->Name();
    EXPECT_EQ(draft.error().message, kGroupInviteRemovedError);
  }
}

TEST_F(GroupLinkDraftTest, TheLinkGroupIsFoundByItsLinkLiveOrEnded) {
  Seed(Statement());
  EXPECT_FALSE(glj::FindLinkGroup(*roster_, "link-1", book.Lookup(), kNowMs));
  Hold(CarolsLink());
  auto found = glj::FindLinkGroup(*roster_, "link-1", book.Lookup(), kNowMs);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->group_id, kGroupId);
  Hold(carol.Leave());
  found = glj::FindLinkGroup(*roster_, "link-1", book.Lookup(), kNowMs);
  ASSERT_TRUE(found) << "ended: its maker left";
  ASSERT_NE(found->view.EndedLink("link-1"), nullptr);
  EXPECT_EQ(found->view.EndedLink("link-1")->end, GroupLinkEnd::MakerOut);
}

TEST(GroupJoinRecordTest, TheJoinersRecordIsAnAcceptedInviteFromTheAnswerer) {
  const PendingGroupInvite record =
      glj::JoinRecord("req-1", kGroupId, "Hikers", "account:alice", kMaker, "account:erin", kNowMs);
  EXPECT_EQ(record.invite_nonce, "req-1");
  EXPECT_EQ(record.inviter_identity, kMaker);
  EXPECT_EQ(record.invitee_identity, "account:erin");
  EXPECT_EQ(record.Owner(), "account:alice") << "the bundle's statement must verify for the preview's owner";
  EXPECT_EQ(record.status, InviteStatus::Accepted);
  EXPECT_FALSE(record.outbound);
  EXPECT_EQ(glj::JoinRecord("req-1", kGroupId, "Hikers", kMaker, kMaker, "account:erin", kNowMs).owner_identity, "")
      << "an owner's own link: the inviter is the owner";
}

} // namespace
} // namespace pbr
