#include "lib/messaging/GroupRosterStatement.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** An account that signs with its own ML-DSA key, as IdentityStore does. */
class TestSigner final : public IAccountSigningAccess {
public:
  TestSigner() {
    auto keys = MlDsa::GenerateKeyPair();
    EXPECT_TRUE(static_cast<bool>(keys));
    public_key_ = keys->public_key;
    secret_key_ = keys->secret_key;
    account_id_ = AccountIdFromMlDsaPublicKey(public_key_).value();
  }
  Roe<std::string> GetAccountId() const override { return account_id_; }
  Roe<std::string> SignAccountBytes(const AccountSignBytes& sign_bytes) const override {
    auto signature = MlDsa::Sign(secret_key_, sign_bytes);
    if (!signature) {
      return signature.error();
    }
    return Base64Encode(*signature);
  }
  std::string Key() const { return Base64Encode(public_key_); }
  const std::string& Id() const { return account_id_; }

private:
  ByteVector public_key_;
  ByteVector secret_key_;
  std::string account_id_;
};

std::vector<GroupRosterStatementMember> Roster(const std::string& owner, std::vector<std::string> members) {
  std::vector<GroupRosterStatementMember> out{{owner, MemberRole::Owner, 1000}};
  for (const std::string& m : members) {
    out.push_back({m, MemberRole::Member, 2000});
  }
  return out;
}

GroupRosterStatement SignedBy(TestSigner& owner, uint64_t epoch, std::vector<std::string> members,
                              std::vector<GroupOwnerTransfer> chain = {}) {
  return group_roster_statement::Sign(owner, owner.Key(), "group:g1", epoch, Roster(owner.Id(), std::move(members)),
                                      std::move(chain))
      .value();
}

TEST(GroupRosterStatementTest, OwnerSignedStatementVerifiesForAnyoneWhoKnowsTheOwnerOrNone) {
  TestSigner alice;
  const GroupRosterStatement s = SignedBy(alice, 3, {"account:bob", "account:carol"});
  EXPECT_TRUE(group_roster_statement::Verify(s, alice.Id()));
  EXPECT_TRUE(group_roster_statement::Verify(s, "")) << "a device that knows no owner yet";
  EXPECT_TRUE(s.Lists("account:bob"));
  EXPECT_FALSE(s.Lists("account:dave"));
}

TEST(GroupRosterStatementTest, ChangedMembersOrEpochNoLongerVerify) {
  TestSigner alice;
  GroupRosterStatement added = SignedBy(alice, 3, {"account:bob"});
  added.members.push_back({"account:mallory", MemberRole::Member, 3000});
  EXPECT_FALSE(group_roster_statement::Verify(added, alice.Id())) << "a member added by someone else";

  GroupRosterStatement bumped = SignedBy(alice, 3, {"account:bob"});
  bumped.roster_epoch = 99;
  EXPECT_FALSE(group_roster_statement::Verify(bumped, alice.Id())) << "an epoch raised by someone else";
}

TEST(GroupRosterStatementTest, KeyMustBeTheOwnersAccount) {
  TestSigner alice;
  TestSigner mallory;
  // Mallory signs a statement that names Alice as owner, with Mallory's key.
  GroupRosterStatement s = SignedBy(mallory, 3, {"account:bob"});
  s.owner_identity = alice.Id();
  s.members[0].identity = alice.Id();
  s.owner_signature_b64 = mallory.SignAccountBytes(group_roster_statement::SignBytes(s)).value();
  EXPECT_FALSE(group_roster_statement::Verify(s, alice.Id()));
}

TEST(GroupRosterStatementTest, ExactlyOneOwnerAndItIsTheSigner) {
  TestSigner alice;
  auto members = Roster(alice.Id(), {"account:bob"});
  members[1].role = MemberRole::Owner;
  auto two = group_roster_statement::Sign(alice, alice.Key(), "group:g1", 3, members, {});
  ASSERT_TRUE(two);
  EXPECT_FALSE(group_roster_statement::Verify(*two, alice.Id()));

  std::vector<GroupRosterStatementMember> not_listed{{"account:bob", MemberRole::Owner, 1}};
  auto absent = group_roster_statement::Sign(alice, alice.Key(), "group:g1", 3, not_listed, {});
  ASSERT_TRUE(absent);
  EXPECT_FALSE(group_roster_statement::Verify(*absent, alice.Id())) << "the signer is not the listed owner";
}

TEST(GroupRosterStatementTest, OwnerChainLetsAnEarlierOwnersViewTrustTheNewOwner) {
  TestSigner alice;
  TestSigner bob;
  auto handed = group_roster_statement::SignTransfer(alice, alice.Key(), "group:g1", bob.Id(), 5);
  ASSERT_TRUE(handed);
  const GroupRosterStatement s = SignedBy(bob, 6, {"account:carol"}, {*handed});
  EXPECT_TRUE(group_roster_statement::Verify(s, alice.Id())) << "restored from a backup that knew Alice";
  EXPECT_TRUE(group_roster_statement::Verify(s, bob.Id()));
  EXPECT_FALSE(group_roster_statement::Verify(s, "account:carol")) << "the chain does not start at Carol";

  // Without the chain, Bob's statement means nothing to a device that knows Alice.
  const GroupRosterStatement bare = SignedBy(bob, 6, {"account:carol"});
  EXPECT_FALSE(group_roster_statement::Verify(bare, alice.Id()));
}

TEST(GroupRosterStatementTest, BrokenOrForgedChainsFail) {
  TestSigner alice;
  TestSigner bob;
  TestSigner carol;
  // A transfer Alice never signed: Carol signs "Alice → Bob".
  GroupOwnerTransfer forged{alice.Id(), carol.Key(), bob.Id(), 5, ""};
  forged.signature_b64 = carol.SignAccountBytes(group_roster_statement::TransferSignBytes("group:g1", forged)).value();
  EXPECT_FALSE(group_roster_statement::Verify(SignedBy(bob, 6, {}, {forged}), alice.Id()));

  // A transfer for another group.
  auto other = group_roster_statement::SignTransfer(alice, alice.Key(), "group:other", bob.Id(), 5).value();
  EXPECT_FALSE(group_roster_statement::Verify(SignedBy(bob, 6, {}, {other}), alice.Id()));

  // A chain whose links do not meet (Alice → Carol, then Bob → ...).
  auto to_carol = group_roster_statement::SignTransfer(alice, alice.Key(), "group:g1", carol.Id(), 4).value();
  auto from_bob = group_roster_statement::SignTransfer(bob, bob.Key(), "group:g1", carol.Id(), 5).value();
  EXPECT_FALSE(group_roster_statement::Verify(SignedBy(carol, 6, {}, {to_carol, from_bob}), alice.Id()));

  // A transfer dated after the statement.
  auto late = group_roster_statement::SignTransfer(alice, alice.Key(), "group:g1", bob.Id(), 9).value();
  EXPECT_FALSE(group_roster_statement::Verify(SignedBy(bob, 6, {}, {late}), alice.Id()));
}

TEST(GroupRosterStatementTest, AFormerOwnerCannotOutbidItsSuccessor) {
  TestSigner alice;
  TestSigner bob;
  auto handed = group_roster_statement::SignTransfer(alice, alice.Key(), "group:g1", bob.Id(), 5).value();
  const GroupRosterStatement bobs = SignedBy(bob, 6, {"account:carol"}, {handed});
  // Alice, after handing the group on, signs a statement that would win on epoch alone.
  const GroupRosterStatement alices = SignedBy(alice, 50, {"account:mallory"});
  ASSERT_TRUE(group_roster_statement::Verify(alices, alice.Id())) << "valid on its own to a device that knows Alice";
  auto picked = group_roster_statement::Pick({alices, bobs});
  ASSERT_TRUE(picked);
  EXPECT_EQ(picked->owner_identity, bob.Id());
  EXPECT_EQ(picked->roster_epoch, 6u);

  // Otherwise the highest epoch wins.
  const GroupRosterStatement older = SignedBy(bob, 4, {});
  const GroupRosterStatement newer = SignedBy(bob, 7, {});
  EXPECT_EQ(group_roster_statement::Pick({older, newer})->roster_epoch, 7u);
  EXPECT_FALSE(group_roster_statement::Pick({}));
}

TEST(GroupRosterStatementTest, JsonRoundTripStillVerifies) {
  TestSigner alice;
  TestSigner bob;
  auto handed = group_roster_statement::SignTransfer(alice, alice.Key(), "group:g1", bob.Id(), 5).value();
  const GroupRosterStatement s = SignedBy(bob, 6, {"account:carol", "account:dave"}, {handed});
  auto parsed = ParseObject(DumpJson(group_roster_statement::ToJson(s)));
  ASSERT_TRUE(parsed);
  auto back = group_roster_statement::FromJson(*parsed);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_TRUE(group_roster_statement::Verify(*back, alice.Id()));
  EXPECT_EQ(back->members.size(), 3u);
  EXPECT_EQ(back->owner_chain.size(), 1u);
}

} // namespace
} // namespace pbr
