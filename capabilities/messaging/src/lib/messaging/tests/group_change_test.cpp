// A roster change (G018 item 2) and its signature (item 9): signed by the device, checked against the signer's
// card; the change id and the changes hash (item 5); the owner's statement v3 fields.
#include "lib/messaging/GroupChange.h"
#include "lib/messaging/GroupRosterStatement.h"
#include "lib/messaging/tests/GroupChangeTestKit.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <functional>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using test::kNowMs;
using test::Named;
using test::Person;

namespace grc = group_change;

/** One change of every kind, by Carol's device 1. */
std::vector<GroupChange> OneOfEach() {
  const Person& carol = Named("carol");
  return {carol.Add("account:erin"), carol.Leave({"a1"}, {"k1"}), carol.Remove("account:dave", {"a2"}, {}),
          carol.Rename("Hikers", 6), carol.LinkAdd("link-1"),     carol.LinkRevoke("link-1")};
}

TEST(GroupChangeTest, EveryKindVerifiesAgainstTheSignersCardFromItsDevice) {
  const Person& carol = Named("carol");
  for (const GroupChange& c : OneOfEach()) {
    EXPECT_TRUE(grc::Verify(c, carol.Card(), kNowMs)) << grc::KindToString(c.kind);
    EXPECT_EQ(grc::Id(c).size(), 64u) << "hex of BLAKE2b-256";
  }
}

TEST(GroupChangeTest, ChangingAnySignedFieldBreaksTheSignatureAndTheId) {
  const Person& carol = Named("carol");
  const std::vector<std::function<void(GroupChange&)>> tamper = {
      [](GroupChange& c) { c.group_id = "group:other"; },
      [](GroupChange& c) { c.base_epoch += 1; },
      [](GroupChange& c) { c.signer = Named("dave").Id(); },
      [](GroupChange& c) { c.signer_device = Named("carol").Device("2").peer_id; },
      [](GroupChange& c) { c.at += 1; },
      [](GroupChange& c) { c.member = "account:mallory"; },
      [](GroupChange& c) { c.invite_nonce = "invite-2"; },
      [](GroupChange& c) { c.readmits = "removal"; },
  };
  const GroupChange add = carol.Add("account:erin");
  for (size_t i = 0; i < tamper.size(); ++i) {
    GroupChange changed = add;
    tamper[i](changed);
    EXPECT_FALSE(grc::Verify(changed, carol.Card({"1", "2"}), kNowMs)) << "field " << i;
    EXPECT_NE(grc::Id(changed), grc::Id(add)) << "field " << i;
  }

  // The body of the other kinds.
  const std::vector<std::pair<GroupChange, std::function<void(GroupChange&)>>> bodies = {
      {carol.Leave({"a1"}, {"k1"}), [](GroupChange& c) { c.adds.push_back("a9"); }},
      {carol.Leave({"a1"}, {"k1"}), [](GroupChange& c) { c.keeps.clear(); }},
      {carol.Remove("account:dave", {"a2"}), [](GroupChange& c) { c.member = "account:erin"; }},
      {carol.Remove("account:dave", {"a2"}), [](GroupChange& c) { c.keeps = {"k2"}; }},
      {carol.Rename("Hikers", 6), [](GroupChange& c) { c.title = "Bikers"; }},
      {carol.Rename("Hikers", 6), [](GroupChange& c) { c.title_version = 7; }},
      {carol.LinkAdd("link-1"), [](GroupChange& c) { c.secret_hash = "other"; }},
      {carol.LinkAdd("link-1"), [](GroupChange& c) { c.expires_at += 1; }},
      {carol.LinkAdd("link-1"), [](GroupChange& c) { c.max_uses = 3; }},
      {carol.LinkRevoke("link-1"), [](GroupChange& c) { c.link_id = "link-2"; }},
      {carol.Add("account:erin"), [](GroupChange& c) { c.kind = GroupChangeKind::Remove; }},
  };
  for (const auto& [change, edit] : bodies) {
    GroupChange changed = change;
    edit(changed);
    EXPECT_FALSE(grc::Verify(changed, carol.Card(), kNowMs)) << grc::KindToString(change.kind);
  }

  GroupChange forged = add;
  forged.signature_b64 = carol.Add("account:mallory").signature_b64;
  EXPECT_FALSE(grc::Verify(forged, carol.Card(), kNowMs)) << "another change's signature";
}

TEST(GroupChangeTest, IdListsAreSignedSortedSoTheirOrderDoesNotMatter) {
  GroupChange a = Named("bob").Draft(GroupChangeKind::Remove);
  a.member = "account:carol";
  a.adds = {"b2", "a1"};
  GroupChange b = a;
  b.adds = {"a1", "b2"};
  EXPECT_EQ(grc::Id(a), grc::Id(b));
}

TEST(GroupChangeTest, TheDeviceMustBeOnTheSignersCard) {
  const Person& carol = Named("carol");
  const GroupChange by_second = carol.Add("account:erin", "invite-1", "", "2");
  EXPECT_TRUE(grc::Verify(by_second, carol.Card({"1", "2"}), kNowMs));
  EXPECT_FALSE(grc::Verify(by_second, carol.Card({"1"}), kNowMs)) << "the device is not on this card";

  // Another account's device signs in Carol's name.
  GroupChange posing = carol.Draft(GroupChangeKind::Add);
  posing.member = "account:erin";
  posing.invite_nonce = "invite-1";
  posing.signer_device = Named("mallory").Device().peer_id;
  const test::TestDevice& mallory_device = Named("mallory").Device();
  posing = grc::Sign(posing, [&](const ByteVector& bytes) { return mallory_device.Sign(bytes); }).value();
  EXPECT_FALSE(grc::Verify(posing, carol.Card(), kNowMs));
  EXPECT_FALSE(grc::Verify(posing, Named("mallory").Card(), kNowMs)) << "Mallory's card is not Carol's";
}

TEST(GroupChangeTest, TheCardMustProveItselfForTheSigner) {
  const Person& carol = Named("carol");
  const GroupChange add = carol.Add("account:erin");

  // A card that lists Carol's device but is Mallory's account.
  const AccountCard mallorys = Named("mallory").Card();
  AccountCard listing = mallorys;
  listing.devices = {carol.Device().Entry()};
  EXPECT_FALSE(grc::Verify(add, listing, kNowMs)) << "not the signer's card, and its signature no longer holds";

  // Carol's card with a device added after the account signed it.
  AccountCard padded = carol.Card({"2"});
  padded.devices.push_back(carol.Device("1").Entry());
  EXPECT_FALSE(grc::Verify(add, padded, kNowMs)) << "the account did not sign that device list";

  // A card claiming Carol's account id with Mallory's key.
  AccountCard claimed = mallorys;
  claimed.keys.account_id = carol.Id();
  claimed.devices = {carol.Device().Entry()};
  EXPECT_FALSE(grc::Verify(add, claimed, kNowMs)) << "the account id derives from the key";

  EXPECT_FALSE(grc::Verify(add, carol.Card({"1"}, kNowMs + 3'600'000), kNowMs)) << "a card from the future";
}

TEST(GroupChangeTest, JsonRoundTripKeepsTheIdAndSignatureAndCarriesTheCard) {
  const Person& carol = Named("carol");
  for (GroupChange c : OneOfEach()) {
    c.signer_card = carol.Card();
    auto parsed = ParseObject(DumpJson(grc::ToJson(c)));
    ASSERT_TRUE(parsed);
    auto back = grc::FromJson(*parsed);
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(grc::Id(*back), grc::Id(c)) << grc::KindToString(c.kind);
    ASSERT_TRUE(back->signer_card);
    EXPECT_EQ(*back->signer_card, *c.signer_card);
    EXPECT_TRUE(grc::Verify(*back, *back->signer_card, kNowMs)) << "the carried card proves the change";
  }
  GroupChange bare = carol.Add("account:erin");
  EXPECT_FALSE(grc::ToJson(bare).contains("signer_card"));
  EXPECT_EQ(grc::ToJson(bare).getString("change").value_or(""), "add");
}

TEST(GroupChangeTest, FromJsonRefusesWhatIsNotAChange) {
  Object json = grc::ToJson(Named("carol").Add("account:erin"));
  Object unknown = json;
  unknown.set("change", "promote");
  EXPECT_FALSE(grc::FromJson(unknown));
  Object unsigned_change = json;
  unsigned_change.erase("signature");
  EXPECT_FALSE(grc::FromJson(unsigned_change));
  Object no_body = json;
  no_body.erase("body");
  EXPECT_FALSE(grc::FromJson(no_body));
  Object foreign = json;
  Object body = *json.getObject("body");
  body.set("title", "Hikers");
  foreign.set("body", ObjectValue(body));
  EXPECT_FALSE(grc::FromJson(foreign)) << "an add carries no title";
}

TEST(GroupChangeTest, ShapeChecksEachKind) {
  const Person& carol = Named("carol");
  const auto shape = [](GroupChange c) { return static_cast<bool>(grc::CheckShape(c)); };
  GroupChange add = carol.Draft(GroupChangeKind::Add);
  add.member = "account:erin";
  EXPECT_FALSE(shape(add)) << "neither invite nor link";
  add.invite_nonce = "n";
  EXPECT_TRUE(shape(add));
  add.link_id = "l";
  EXPECT_FALSE(shape(add)) << "both invite and link";
  add.link_id.clear();
  add.member = carol.Id();
  EXPECT_FALSE(shape(add)) << "adds itself";

  GroupChange remove = carol.Draft(GroupChangeKind::Remove);
  remove.member = carol.Id();
  EXPECT_FALSE(shape(remove)) << "removes itself: that is a leave";
  remove.member = "account:dave";
  remove.keeps = {"k", "k"};
  EXPECT_FALSE(shape(remove)) << "a repeated id";
  remove.keeps = std::vector<std::string>(grc::kMaxNamedChanges + 1, "");
  for (size_t i = 0; i < remove.keeps.size(); ++i) {
    remove.keeps[i] = "k" + std::to_string(i);
  }
  EXPECT_FALSE(shape(remove)) << "too many";

  GroupChange rename = carol.Draft(GroupChangeKind::Rename);
  rename.title = "Hikers";
  EXPECT_FALSE(shape(rename)) << "no version";
  rename.title_version = 1;
  EXPECT_TRUE(shape(rename));
  rename.title = std::string(grc::kMaxTitleBytes + 1, 'x');
  EXPECT_FALSE(shape(rename));

  GroupChange link = carol.Draft(GroupChangeKind::LinkAdd);
  link.link_id = "l";
  link.secret_hash = "h";
  EXPECT_FALSE(shape(link)) << "no expiry";
  link.expires_at = kNowMs;
  EXPECT_TRUE(shape(link));
  link.member = "account:erin";
  EXPECT_FALSE(shape(link)) << "a field its kind does not carry";

  GroupChange leave = carol.Draft(GroupChangeKind::Leave);
  EXPECT_TRUE(shape(leave)) << "a leave naming nothing";
  GroupChange unsigned_by = leave;
  unsigned_by.signer_device.clear();
  EXPECT_FALSE(shape(unsigned_by));
  EXPECT_FALSE(grc::Sign(unsigned_by, [](const ByteVector&) -> Roe<std::string> { return std::string("sig"); }));
}

// Golden vectors: the encoding as DESIGN § Change writes it, computed outside this code.
TEST(GroupChangeTest, TheChangeIdIsTheHashOfTheDocumentedBytes) {
  GroupChange add;
  add.group_id = "group:g1";
  add.base_epoch = 7;
  add.signer = "account:alice";
  add.signer_device = "12D3KooWdevice";
  add.at = 1767225600000;
  add.kind = GroupChangeKind::Add;
  add.member = "account:carol";
  add.invite_nonce = "nonce-1";
  EXPECT_EQ(grc::Id(add), "7d564be96a385768db29f9ba24312ecc62e8bb23cbcb135ba72cec18e6bc56f3");

  GroupChange remove = add;
  remove.signer = "account:bob";
  remove.kind = GroupChangeKind::Remove;
  remove.invite_nonce.clear();
  remove.adds = {"b2", "a1"};
  remove.keeps = {"k1"};
  EXPECT_EQ(grc::Id(remove), "c55f2634da645bc202c4d71c99684dd47b49144362cd5c495863ec17f65f1884");
}

TEST(GroupChangeTest, TheChangesHashIsOfTheSortedDistinctIds) {
  EXPECT_EQ(grc::ChangesHash({"b", "a", "a"}), "24e403dfceeffb49896794cebe1bed2437670601112def81f8e6aae163bdfa35");
  EXPECT_EQ(grc::ChangesHash({"a", "b"}), grc::ChangesHash({"b", "a"}));
  EXPECT_EQ(grc::ChangesHash({}), "1b26f73c2f9a69c3f4ecc337bd8f2d4af85998c17937c95fe15f0a79b1e56585");
  EXPECT_NE(grc::ChangesHash({"a"}), grc::ChangesHash({"a", "b"}));
  EXPECT_NE(grc::ChangesHash({"ab"}), grc::ChangesHash({"a", "b"})) << "length-prefixed: ids cannot run together";
}

// ---- The owner's statement, v3 (G018 item 1) ----

GroupRosterStatement FullStatement(Person& alice) {
  GroupRosterStatement draft;
  draft.roster_epoch = 6;
  draft.members = {test::Row(alice.Id(), MemberRole::Owner), test::Row("account:bob", MemberRole::Admin),
                   test::Row("account:erin", MemberRole::Member, "account:bob")};
  draft.title = "Hikers";
  draft.title_version = 3;
  draft.removed = {{.identity = "account:mallory", .removal = "r1", .removed_by = "account:bob"}};
  draft.links = {
      {.link_id = "link-1", .secret_hash = "h", .maker = "account:bob", .expires_at = kNowMs, .max_uses = 5}};
  draft.folded = {"c2", "c1"};
  return test::SignedStatement(alice, draft);
}

TEST(GroupRosterStatementV3Test, TheOpenGroupFieldsAreWhatTheOwnerSigned) {
  Person& alice = Named("alice");
  const GroupRosterStatement s = FullStatement(alice);
  ASSERT_TRUE(group_roster_statement::Verify(s, alice.Id()));
  EXPECT_EQ(s.folded, (std::vector<std::string>{"c1", "c2"})) << "signed sorted";

  const std::vector<std::function<void(GroupRosterStatement&)>> tamper = {
      [](GroupRosterStatement& t) { t.members[2].added_by = "account:alice"; },
      [](GroupRosterStatement& t) { t.title = "Bikers"; },
      [](GroupRosterStatement& t) { t.title_version = 4; },
      [](GroupRosterStatement& t) { t.removed.clear(); },
      [](GroupRosterStatement& t) { t.removed[0].removal = "r2"; },
      [](GroupRosterStatement& t) { t.removed[0].removed_by = "account:alice"; },
      [](GroupRosterStatement& t) { t.links[0].expires_at += 1; },
      [](GroupRosterStatement& t) { t.links[0].maker = "account:erin"; },
      [](GroupRosterStatement& t) { t.links[0].max_uses = 0; },
      [](GroupRosterStatement& t) { t.links[0].secret_hash = "other"; },
      [](GroupRosterStatement& t) { t.folded.push_back("c3"); },
      [](GroupRosterStatement& t) { t.folded.pop_back(); },
  };
  for (size_t i = 0; i < tamper.size(); ++i) {
    GroupRosterStatement changed = s;
    tamper[i](changed);
    EXPECT_FALSE(group_roster_statement::Verify(changed, alice.Id())) << "field " << i;
  }
}

TEST(GroupRosterStatementV3Test, JsonRoundTripKeepsTheOpenGroupFields) {
  Person& alice = Named("alice");
  const GroupRosterStatement s = FullStatement(alice);
  auto parsed = ParseObject(DumpJson(group_roster_statement::ToJson(s)));
  ASSERT_TRUE(parsed);
  auto back = group_roster_statement::FromJson(*parsed);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_TRUE(group_roster_statement::Verify(*back, alice.Id()));
  EXPECT_EQ(back->title, "Hikers");
  EXPECT_EQ(back->title_version, 3u);
  ASSERT_EQ(back->removed.size(), 1u);
  EXPECT_EQ(back->removed[0].removal, "r1");
  ASSERT_EQ(back->links.size(), 1u);
  EXPECT_EQ(back->links[0].max_uses, 5u);
  EXPECT_EQ(back->folded.size(), 2u);
  const auto erin = std::find_if(back->members.begin(), back->members.end(),
                                 [](const GroupRosterStatementMember& m) { return m.identity == "account:erin"; });
  ASSERT_NE(erin, back->members.end());
  EXPECT_EQ(erin->added_by, "account:bob");
}

TEST(GroupRosterStatementV3Test, ListsMustBeConsistent) {
  Person& alice = Named("alice");
  GroupRosterStatement base = FullStatement(alice);
  const std::vector<std::function<void(GroupRosterStatement&)>> broken = {
      [](GroupRosterStatement& t) { t.removed.push_back({.identity = "account:bob"}); }, // removed and a member
      [](GroupRosterStatement& t) { t.removed.push_back(t.removed[0]); },
      [](GroupRosterStatement& t) { t.links.push_back(t.links[0]); },
      [](GroupRosterStatement& t) { t.folded.push_back("c1"); },
  };
  for (size_t i = 0; i < broken.size(); ++i) {
    GroupRosterStatement draft = base;
    broken[i](draft);
    const GroupRosterStatement signed_ = test::SignedStatement(alice, draft);
    EXPECT_FALSE(group_roster_statement::Verify(signed_, alice.Id())) << "case " << i << " signed but inconsistent";
  }
}

} // namespace
} // namespace pbr
