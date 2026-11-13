#include "lib/delivery/RelayGrantSigner.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Multi-relay MR005: a grant is the recipient's account signature over its sign bytes; the relay — and the sender
// that keeps it — takes it only for the pair it names, within 30 days, under the recipient's own key.

struct Account {
  MlDsaKeyPair keys;
  std::string id;
  std::string public_key_b64;

  static Account Make() {
    Account account;
    account.keys = *MlDsa::GenerateKeyPair();
    account.id = *AccountIdFromMlDsaPublicKey(account.keys.public_key);
    account.public_key_b64 = Base64Encode(account.keys.public_key);
    return account;
  }

  AccountBytesSigner Signer() const {
    return [this](const std::vector<uint8_t>& bytes) -> Roe<std::string> {
      auto signature = MlDsa::Sign(keys.secret_key, bytes);
      if (!signature) {
        return signature.error();
      }
      return Base64Encode(*signature);
    };
  }
};

constexpr int64_t kNow = 1761235200000;

class RelayGrantSignerTest : public ::testing::Test {
protected:
  Account bob = Account::Make();   // the recipient: it grants
  Account alice = Account::Make(); // the sender: it is granted
};

TEST_F(RelayGrantSignerTest, AnIssuedGrantChecksForItsPairUnderTheRecipientsKey) {
  auto grant = IssueRelayGrant(bob.id, alice.id, kNow, bob.Signer());
  ASSERT_TRUE(grant) << grant.error().message;
  EXPECT_EQ(grant->issued_at, kNow);
  EXPECT_EQ(grant->expires_at, kNow + kRelayGrantMaxLifetimeMs);
  EXPECT_TRUE(CheckRelayGrant(*grant, bob.id, alice.id, bob.public_key_b64, kNow + 1));
}

TEST_F(RelayGrantSignerTest, AGrantDoesNotHoldForAnotherPairOrKeyOrOnceExpired) {
  auto grant = IssueRelayGrant(bob.id, alice.id, kNow, bob.Signer());
  ASSERT_TRUE(grant);
  const Account carol = Account::Make();
  EXPECT_FALSE(CheckRelayGrant(*grant, bob.id, carol.id, bob.public_key_b64, kNow)) << "another sender";
  EXPECT_FALSE(CheckRelayGrant(*grant, carol.id, alice.id, carol.public_key_b64, kNow)) << "another recipient";
  EXPECT_FALSE(CheckRelayGrant(*grant, bob.id, alice.id, carol.public_key_b64, kNow)) << "a key not the recipient's";
  EXPECT_FALSE(CheckRelayGrant(*grant, bob.id, alice.id, bob.public_key_b64, grant->expires_at)) << "expired";

  RelayGrant tampered = *grant;
  tampered.expires_at += 1;
  EXPECT_FALSE(CheckRelayGrant(tampered, bob.id, alice.id, bob.public_key_b64, kNow)) << "the signature covers it";

  auto by_the_sender = IssueRelayGrant(bob.id, alice.id, kNow, alice.Signer());
  ASSERT_TRUE(by_the_sender);
  EXPECT_FALSE(CheckRelayGrant(*by_the_sender, bob.id, alice.id, bob.public_key_b64, kNow))
      << "only the recipient's own signature grants";
}

TEST_F(RelayGrantSignerTest, AGrantLongerThanThirtyDaysIsRefused) {
  RelayGrant grant{.recipient_account_id = bob.id,
                   .sender_account_id = alice.id,
                   .issued_at = kNow,
                   .expires_at = kNow + kRelayGrantMaxLifetimeMs + 1};
  grant.signature = *bob.Signer()(BuildRelayGrantSignBytes(grant));
  EXPECT_FALSE(CheckRelayGrant(grant, bob.id, alice.id, bob.public_key_b64, kNow));
}

TEST_F(RelayGrantSignerTest, NoGrantToOneself) {
  EXPECT_FALSE(IssueRelayGrant(bob.id, bob.id, kNow, bob.Signer())) << "a self-send needs none";
}

TEST_F(RelayGrantSignerTest, AHeldGrantGoesOnSendsUntilNearItsEndAndIsRenewedTenDaysBefore) {
  auto grant = IssueRelayGrant(bob.id, alice.id, kNow, bob.Signer());
  ASSERT_TRUE(grant);
  EXPECT_TRUE(RelayGrantUsable(*grant, kNow));
  EXPECT_FALSE(RelayGrantUsable(*grant, grant->expires_at - kRelayGrantSendMarginMs));
  EXPECT_FALSE(RelayGrantDueForRenewal(grant->expires_at, kNow + 19LL * 24 * 60 * 60 * 1000));
  EXPECT_TRUE(RelayGrantDueForRenewal(grant->expires_at, kNow + 20LL * 24 * 60 * 60 * 1000));
}

TEST_F(RelayGrantSignerTest, AGrantRoundTripsItsWireForm) {
  auto grant = IssueRelayGrant(bob.id, alice.id, kNow, bob.Signer());
  ASSERT_TRUE(grant);
  const Object json = RelayGrantToJson(*grant);
  for (const char* field : {"recipient_account_id", "sender_account_id", "issued_at", "expires_at", "signature"}) {
    EXPECT_TRUE(json.contains(field)) << field;
  }
  auto back = RelayGrantFromJson(json);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(*back, *grant);
  Object broken = json;
  broken.set("expires_at", kNow - 1);
  EXPECT_FALSE(RelayGrantFromJson(broken)) << "an end before its start";
}

} // namespace
} // namespace pbr
