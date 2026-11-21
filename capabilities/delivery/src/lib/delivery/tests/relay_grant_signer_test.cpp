#include "lib/delivery/RelayGrantSigner.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/error/AppError.h"
#include "lib/people/HeldKeyForms.h"

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
  grant.signature = TypedSignatureB64FromSigner(*bob.Signer()(BuildRelayGrantSignBytes(grant))).value();
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
  for (const char* field :
       {"grant_version", "recipient_account_id", "sender_account_id", "issued_at", "expires_at", "signature"}) {
    EXPECT_TRUE(json.contains(field)) << field;
  }
  EXPECT_EQ(json.getIf<int64_t>("grant_version"), 2);
  EXPECT_FALSE(json.contains("ext_b64")) << "an empty area is left out";
  EXPECT_EQ(Base64Decode(json.getString("signature").value()).value().size(), 3310u) << "a typed signature";
  auto back = RelayGrantFromJson(json);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(*back, *grant);
  Object broken = json;
  broken.set("expires_at", kNow - 1);
  EXPECT_FALSE(RelayGrantFromJson(broken)) << "an end before its start";
}

// SIGNED_FORMATS § 2.4, RC020-ORDER: a grant this app cannot read is "update needed", never a forgery; an unsigned
// area is a forgery; a signed unknown entry is kept, a critical one is update needed once the signature verifies.
TEST_F(RelayGrantSignerTest, AGrantIsReceivedInTheReceiversOrder) {
  const RelayGrant grant = IssueRelayGrant(bob.id, alice.id, kNow, bob.Signer()).value();
  const Object json = RelayGrantToJson(grant);
  const auto receive = [&](const Object& j) {
    return ReceiveRelayGrant(j, bob.id, alice.id, bob.public_key_b64, kNow);
  };
  EXPECT_TRUE(receive(json));

  Object newer = json;
  newer.set("grant_version", int64_t{3});
  newer.erase("recipient_account_id");
  EXPECT_TRUE(AppError::IsNewerFormat(receive(newer).error())) << "read first: the rest is not";
  EXPECT_TRUE(AppError::IsNewerFormat(RelayGrantFromJson(newer).error()));
  Object older = json;
  older.erase("grant_version");
  EXPECT_FALSE(receive(older));
  EXPECT_FALSE(AppError::IsNewerFormat(receive(older).error())) << "an older app's is refused, not 'update'";

  Object unsigned_area = json;
  unsigned_area.set("ext_b64",
                    Base64Encode(pp::ExtArea::FromEntries({{.type = 0x8001, .value = {}}}).value().EncodeEntries()));
  ASSERT_FALSE(receive(unsigned_area));
  EXPECT_FALSE(AppError::IsNewerFormat(receive(unsigned_area).error())) << "an unsigned area raises no update";

  for (const uint16_t type : {uint16_t{0x0007}, uint16_t{0x8007}}) {
    RelayGrant extended = grant;
    extended.ext = pp::ExtArea::FromEntries({{.type = type, .value = {9}}}).value();
    extended.signature = TypedSignatureB64FromSigner(*bob.Signer()(BuildRelayGrantSignBytes(extended))).value();
    auto received = receive(RelayGrantToJson(extended));
    if (type & 0x8000) {
      EXPECT_TRUE(AppError::IsNewerFormat(received.error())) << "an unknown critical entry, signed";
    } else {
      ASSERT_TRUE(received) << received.error().message;
      EXPECT_EQ(received->ext, extended.ext) << "an unknown non-critical entry is kept";
      EXPECT_TRUE(CheckRelayGrant(*received, bob.id, alice.id, bob.public_key_b64, kNow)) << "and still verifies";
    }
  }
}

// The account locker's `grants` hold grant v2 as received (SIGNED_FORMATS § 2.6): the area and signature go round.
TEST_F(RelayGrantSignerTest, AGrantGoesThroughTheLockerAsItCame) {
  RelayGrant grant = IssueRelayGrant(bob.id, alice.id, kNow, bob.Signer()).value();
  grant.ext = pp::ExtArea::FromEntries({{.type = 0x0003, .value = {1, 2}}}).value();
  grant.signature = TypedSignatureB64FromSigner(*bob.Signer()(BuildRelayGrantSignBytes(grant))).value();
  const AccountStateGrant row = RelayGrantToLocker(grant);
  EXPECT_FALSE(row.ext_b64.empty());
  auto back = RelayGrantFromLocker(row);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(*back, grant);
  EXPECT_TRUE(CheckRelayGrant(*back, bob.id, alice.id, bob.public_key_b64, kNow));
}

} // namespace
} // namespace pbr
