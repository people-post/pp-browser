#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/DataKeyVault.h"
#include "base/crypto/FileCipher.h"
#include "base/crypto/PinKeyDeriver.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <sodium.h>

namespace pbr {
namespace {

class AtRestCryptoTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_at_rest_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(AtRestCryptoTest, VaultCreateUnlockWrongPin) {
  const auto vault_path = (dir_ / "vault.bin").string();
  DataKeyVault vault(vault_path, "profile-a");
  auto created = vault.Create("correct-pin-1234");
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  ASSERT_TRUE(vault.IsUnlocked());
  vault.Lock();
  ASSERT_FALSE(vault.IsUnlocked());

  DataKeyVault again(vault_path, "profile-a");
  auto bad = again.Unlock("wrong-pin");
  EXPECT_FALSE(static_cast<bool>(bad));
  auto ok = again.Unlock("correct-pin-1234");
  ASSERT_TRUE(static_cast<bool>(ok)) << ok.error().message;
  ASSERT_TRUE(again.IsUnlocked());
  EXPECT_EQ(again.Dek()->size(), kDataEncryptionKeySize);
}

TEST_F(AtRestCryptoTest, FileCipherRoundTripAndAadBind) {
  ByteVector dek(kDataEncryptionKeySize, 0x42);
  const ByteVector plain = HexToBytes("0102030405").value();
  const std::string aad = FileCipher::BuildAad("identity", "profile-a");
  auto cipher = FileCipher::Encrypt(dek, plain, aad);
  ASSERT_TRUE(cipher);
  auto round = FileCipher::Decrypt(dek, *cipher, aad);
  ASSERT_TRUE(round);
  EXPECT_EQ(*round, plain);

  const std::string wrong_aad = FileCipher::BuildAad("identity", "other-profile");
  auto fail = FileCipher::Decrypt(dek, *cipher, wrong_aad);
  EXPECT_FALSE(static_cast<bool>(fail));
}

TEST_F(AtRestCryptoTest, PinChangeKeepsDekUsable) {
  const auto vault_path = (dir_ / "vault.bin").string();
  DataKeyVault vault(vault_path, "profile-b");
  ASSERT_TRUE(vault.Create("old-pin"));
  const ByteVector dek_before = *vault.Dek();
  ASSERT_TRUE(vault.ChangePin("old-pin", "new-pin"));
  EXPECT_EQ(*vault.Dek(), dek_before);

  DataKeyVault reopened(vault_path, "profile-b");
  ASSERT_TRUE(reopened.Unlock("new-pin"));
  EXPECT_EQ(*reopened.Dek(), dek_before);
}

TEST(PinKeyDeriverVectors, DeterministicWithFixedSalt) {
  PinKdfParams params = PinKeyDeriver::DefaultParams();
  for (size_t i = 0; i < params.salt.size(); ++i) {
    params.salt[i] = static_cast<uint8_t>(i);
  }
  // Use minimal ops for test speed — still Argon2id.
  params.opslimit = crypto_pwhash_OPSLIMIT_MIN;
  params.memlimit = crypto_pwhash_MEMLIMIT_MIN;
  auto kek = PinKeyDeriver::DeriveKek("test-pin", params);
  ASSERT_TRUE(kek);
  EXPECT_EQ(kek->size(), kDataEncryptionKeySize);
  auto again = PinKeyDeriver::DeriveKek("test-pin", params);
  ASSERT_TRUE(again);
  EXPECT_EQ(*kek, *again);
}

} // namespace
} // namespace pbr
