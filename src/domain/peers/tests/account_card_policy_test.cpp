#include "domain/peers/AccountCardPolicy.h"

#include "lib/messaging/PeerRequestAuth.h"
#include "lib/messaging/PeerSigningKeyStore.h"
#include "lib/messaging/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

AccountCard SignedCard(const test::TestAccount& account, std::vector<std::string> devices) {
  AccountCard card{.keys = account.Card(), .nickname = "me", .devices = std::move(devices), .issued_at = kNow};
  return SignAccountCard(card, [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
}

class AccountCardPolicyTest : public ::testing::Test {
protected:
  void SetUp() override {
    context_.own_account_id = me_.account_id;
    context_.own_card = [this]() -> Roe<AccountCard> { return SignedCard(me_, {"QmMe"}); };
    context_.own_keys = [this]() -> Roe<KeyCard> { return me_.Card(); };
    context_.authenticate = [this](const AccountCardRequest&, const std::string&) -> Roe<void> {
      return authenticated_ ? Roe<void>() : Roe<void>(Error("Request not authorized"));
    };
    context_.blocked = [this](const std::string& account) { return account == blocked_; };
    context_.audience_everyone = [this]() { return everyone_; };
  }

  AccountCardRequest AskFor(const std::string& account) {
    return AccountCardRequest{.account_id = account, .requester_account_id = asker_.account_id};
  }

  test::TestAccount me_ = test::TestAccount::Make();
  test::TestAccount asker_ = test::TestAccount::Make();
  CardServeContext context_;
  bool authenticated_ = true;
  bool everyone_ = false;
  std::string blocked_;
};

TEST_F(AccountCardPolicyTest, AnAuthenticatedAskerGetsTheCard) {
  const AccountCardReply reply = AnswerCardRequest(AskFor(me_.account_id), "QmAsker", context_);
  ASSERT_TRUE(reply.card.has_value());
  EXPECT_EQ(reply.card->keys.account_id, me_.account_id);
}

TEST_F(AccountCardPolicyTest, EveryoneElseGetsTheSameNotAvailable) {
  EXPECT_EQ(AnswerCardRequest(AskFor(asker_.account_id), "QmAsker", context_).error, "not_available")
      << "not this device's account";
  blocked_ = asker_.account_id;
  EXPECT_EQ(AnswerCardRequest(AskFor(me_.account_id), "QmAsker", context_).error, "not_available") << "blocked";
  blocked_.clear();
  authenticated_ = false;
  const AccountCardReply anonymous = AnswerCardRequest(AskFor(me_.account_id), "QmAsker", context_);
  EXPECT_FALSE(anonymous.card || anonymous.key_card);
  EXPECT_EQ(anonymous.error, "not_available");
}

TEST_F(AccountCardPolicyTest, WithAudienceEveryoneAnAnonymousAskerGetsTheKeysButNoDevices) {
  authenticated_ = false;
  everyone_ = true;
  const AccountCardReply reply = AnswerCardRequest(AskFor(me_.account_id), "QmAsker", context_);
  EXPECT_FALSE(reply.card.has_value());
  ASSERT_TRUE(reply.key_card.has_value());
  EXPECT_TRUE(SameKeys(*reply.key_card, me_.Card()));
}

TEST_F(AccountCardPolicyTest, AReplyCountsOnlyForTheAccountAskedAndADeviceItLists) {
  const AccountCardReply reply{.card = SignedCard(me_, {"QmMe"})};
  EXPECT_TRUE(AcceptCardReply(reply, me_.account_id, "QmMe", kNow));
  EXPECT_FALSE(AcceptCardReply(reply, asker_.account_id, "QmMe", kNow)) << "another account's card";
  EXPECT_FALSE(AcceptCardReply(reply, me_.account_id, "QmElse", kNow)) << "the answering device is not on it";
  AccountCardReply tampered = reply;
  tampered.card->devices.push_back("QmElse");
  EXPECT_FALSE(AcceptCardReply(tampered, me_.account_id, "QmElse", kNow));
  EXPECT_FALSE(AcceptCardReply(AccountCardReply{.error = "not_available"}, me_.account_id, "QmMe", kNow));
}

TEST(AccountCardRequestAuthTest, TheRequestersOwnKeyCardStandsInForALookup) {
  const test::TestAccount asker = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  AccountCardRequest request{.account_id = "account:someone", .requester_keys = asker.Card()};
  ASSERT_TRUE(PeerRequestAuth::Sign(request, asker.account_id, "QmAsker", kNow,
                                    [&](const std::vector<uint8_t>& bytes) { return asker.Sign(bytes); }));
  PeerSigningKeyStore none;
  PeerSigningKeyResolver no_keys{none};
  EXPECT_TRUE(PeerRequestAuth::Verify(request, "QmAsker", kNow, no_keys)) << "checked with the card it carries";
  EXPECT_FALSE(PeerRequestAuth::Verify(request, "QmOther", kNow, no_keys)) << "another link";
  AccountCardRequest swapped = request;
  swapped.requester_keys = mallory.Card();  // someone else's card: does not prove itself for the asker
  EXPECT_FALSE(PeerRequestAuth::Verify(swapped, "QmAsker", kNow, no_keys));
  AccountCardRequest retargeted = request;
  retargeted.account_id = "account:another";
  EXPECT_FALSE(PeerRequestAuth::Verify(retargeted, "QmAsker", kNow, no_keys)) << "the signature covers whose card";
}

} // namespace
} // namespace pbr
