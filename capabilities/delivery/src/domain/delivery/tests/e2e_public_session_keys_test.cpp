#include "domain/delivery/E2ePublicSessionKeys.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"

#include <gtest/gtest.h>

#include <memory>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x5a + i);
  }
  return dek;
}

/** The peer's KEM key, counting how often it is asked for (a miss is a directory lookup in the app). */
class CountingKemResolver final : public IPeerKemKeyResolver {
public:
  Roe<PeerKemKeyRecord> Resolve(const std::string& /*kind*/, const std::string& /*value*/) override {
    ++asked;
    if (!available) {
      return Error("directory unreachable (test)");
    }
    PeerKemKeyRecord record;
    record.kem_public_key_b64 = Base64Encode(public_key);
    return record;
  }

  ByteVector public_key;
  bool available = true;
  int asked = 0;
};

class E2ePublicSessionKeysTest : public ::testing::Test {
protected:
  void SetUp() override {
    store_ = std::make_unique<InMemoryDeliveryStore>();
    psk_ = std::make_unique<InMemoryPeerSessions>(*store_);
    ASSERT_TRUE(static_cast<bool>(store_->SetDek(TestDek())));
    ASSERT_TRUE(static_cast<bool>(psk_->SetDek(TestDek())));
    auto bob = HybridKem::GenerateKeyPair();
    ASSERT_TRUE(bob);
    kem_.public_key = bob->public_key;
    keys_ = std::make_unique<E2ePublicSessionKeys>(*psk_, kem_);
  }

  void TearDown() override {
    keys_.reset();
    psk_.reset();
    store_.reset();
  }

  CountingKemResolver kem_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<InMemoryPeerSessions> psk_;
  std::unique_ptr<E2ePublicSessionKeys> keys_;
};

// An open session's key needs nothing from the directory: a call's media-key wrap (or a control send)
// on the owner thread never waits on a lookup for a key it does not use.
TEST_F(E2ePublicSessionKeysTest, AnOpenSessionIsUsedWithoutAskingForThePeersKemKey) {
  auto opened = keys_->Ensure("account:bob");
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(kem_.asked, 1) << "opening the session needs the peer's KEM key";

  kem_.available = false;
  auto again = keys_->Ensure("account:bob");
  ASSERT_TRUE(again) << again.error().message;
  EXPECT_EQ(kem_.asked, 1) << "the open session was used as it is";
  EXPECT_EQ(*again, *opened);
}

// A call's media-key wrap opens the account pair's session (M024 bridge); its opener needs no caller to carry it:
// every account copy the lane seals (Resolve) carries it until the peer confirms the session.
TEST_F(E2ePublicSessionKeysTest, ASessionTheWrapOpenedCarriesItsOpenerOnTheNextAccountCopies) {
  ASSERT_TRUE(keys_->Ensure("account:bob"));
  auto first = keys_->Resolve("account:bob");
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(first->key_init_b64.has_value()) << "the next account copy carries the opener";
  EXPECT_FALSE(first->key_init_b64->empty());
  auto second = keys_->Resolve("account:bob");
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_EQ(second->key_init_b64, first->key_init_b64) << "an unconfirmed opener keeps riding";
}

// Reading a call media key's old form (M024) uses the session held, never opens one: a session opened on the read
// path could not open what the peer wrapped, and would write a row and an opener for nothing.
TEST_F(E2ePublicSessionKeysTest, AHeldKeyIsReadWithoutOpeningASession) {
  EXPECT_FALSE(keys_->Held("account:bob")) << "none held";
  EXPECT_EQ(kem_.asked, 0) << "nothing opened";
  PeerSessionKey pair{
      .peer_identity_kind = "account", .peer_identity_value = "account:bob", .channel = CryptoChannel::E2ePublic};
  auto row = psk_->Load(pair);
  ASSERT_TRUE(row);
  EXPECT_FALSE(row->has_value()) << "no row written";

  auto opened = keys_->Ensure("account:bob");
  ASSERT_TRUE(opened) << opened.error().message;
  auto held = keys_->Held("account:bob");
  ASSERT_TRUE(held) << held.error().message;
  EXPECT_EQ(*held, *opened);
}

TEST_F(E2ePublicSessionKeysTest, OpeningASessionFailsWhenThePeersKemKeyCannotBeHad) {
  kem_.available = false;
  EXPECT_FALSE(keys_->Ensure("account:bob"));
  EXPECT_EQ(kem_.asked, 1);
}

} // namespace
} // namespace pbr
