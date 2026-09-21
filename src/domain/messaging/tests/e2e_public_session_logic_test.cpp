#include "domain/messaging/E2ePublicSessionLogic.h"

#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/HybridKem.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xb1 + i);
  }
  return dek;
}

class E2ePublicSessionLogicTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / "pp_browser_e2e_public_session_logic";
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    psk_ = std::make_unique<SqlitePskSessionStore>(store_->ProfileDbPath(), "test");
    ASSERT_TRUE(static_cast<bool>(store_->SetDek(TestDek())));
    ASSERT_TRUE(static_cast<bool>(psk_->SetDek(TestDek())));
    // Create profile.db (and chat_targets) before PSK OpenDb.
    ASSERT_TRUE(static_cast<bool>(store_->ListThreads()));
  }

  void TearDown() override {
    psk_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<SqlitePskSessionStore> psk_;
};

TEST_F(E2ePublicSessionLogicTest, CreatesOnceAndReusesWithoutNewKeyInit) {
  auto bob = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(bob));

  ChatTargetKey key;
  key.peer_identity_kind = "account";
  key.peer_identity_value = "account:bob";
  key.channel = CryptoChannel::E2ePublic;

  auto first = EnsureE2ePublicMasterPsk(*psk_, key, 1, bob->public_key);
  ASSERT_TRUE(static_cast<bool>(first)) << first.error().message;
  EXPECT_TRUE(first->created);
  ASSERT_TRUE(first->key_init_b64.has_value());
  EXPECT_FALSE(first->key_init_b64->empty());

  auto session = DeriveE2ePublicSessionKey(first->master_psk, 1);
  ASSERT_TRUE(static_cast<bool>(session));
  EXPECT_FALSE(session->empty());

  auto second = EnsureE2ePublicMasterPsk(*psk_, key, 1, bob->public_key);
  ASSERT_TRUE(static_cast<bool>(second));
  EXPECT_FALSE(second->created);
  EXPECT_FALSE(second->key_init_b64.has_value());
  EXPECT_EQ(second->master_psk, first->master_psk);

  auto opened = AutoKeyEstablishment::DeriveMasterPskFromKeyInit(bob->private_key, *first->key_init_b64);
  ASSERT_TRUE(static_cast<bool>(opened));
  EXPECT_EQ(*opened, first->master_psk);
}

} // namespace
