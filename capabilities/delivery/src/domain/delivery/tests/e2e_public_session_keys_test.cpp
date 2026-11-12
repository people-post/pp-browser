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

// An open session needs nothing from the directory: a control send on the owner thread never waits on a lookup
// for a key it does not use.
TEST_F(E2ePublicSessionKeysTest, AnOpenSessionIsUsedWithoutAskingForThePeersKemKey) {
  auto opened = keys_->Resolve("account:bob");
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(kem_.asked, 1) << "opening the session needs the peer's KEM key";

  kem_.available = false;
  auto again = keys_->Resolve("account:bob");
  ASSERT_TRUE(again) << again.error().message;
  EXPECT_EQ(kem_.asked, 1) << "the open session was used as it is";
  EXPECT_EQ(again->master_psk, opened->master_psk);
}

// A session opened for an account copy carries its opener on every account copy until the peer confirms it.
TEST_F(E2ePublicSessionKeysTest, AnUnconfirmedSessionCarriesItsOpenerOnTheNextAccountCopies) {
  auto first = keys_->Resolve("account:bob");
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(first->key_init_b64.has_value()) << "the account copy carries the opener";
  EXPECT_FALSE(first->key_init_b64->empty());
  auto second = keys_->Resolve("account:bob");
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_EQ(second->key_init_b64, first->key_init_b64) << "an unconfirmed opener keeps riding";
}

TEST_F(E2ePublicSessionKeysTest, OpeningASessionFailsWhenThePeersKemKeyCannotBeHad) {
  kem_.available = false;
  EXPECT_FALSE(keys_->Resolve("account:bob"));
  EXPECT_EQ(kem_.asked, 1);
}

} // namespace
} // namespace pbr
