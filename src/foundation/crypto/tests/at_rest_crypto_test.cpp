#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/EncryptedPayload.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/crypto/MessageCipher.h"
#include "foundation/crypto/PinKeyDeriver.h"
#include "foundation/error/AppError.h"

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

// The in-place paths (a large file is not copied three times) must read and write the same bytes as the
// MessageCipher + EncryptedPayload pair they replace.
TEST_F(AtRestCryptoTest, FileCipherSealsStraightIntoTheSameFormat) {
  ByteVector dek(kDataEncryptionKeySize, 0x21);
  const ByteVector plain(5000, 0x7a);
  const std::string aad = FileCipher::BuildAad("blob", "profile-a");
  auto sealed = FileCipher::Encrypt(dek, plain, aad);
  ASSERT_TRUE(sealed);
  EXPECT_EQ(sealed->size(), 1 + kAeadNonceSize + plain.size() + crypto_aead_xchacha20poly1305_ietf_abytes());
  EXPECT_EQ((*sealed)[0], kEncryptedPayloadVersion);
  // Decoded by the generic path and opened by MessageCipher: the layout is the documented one.
  auto decoded = EncryptedPayload::DecodeBlob(*sealed);
  ASSERT_TRUE(decoded);
  const ByteVector aad_bytes(aad.begin(), aad.end());
  auto opened = MessageCipher::Decrypt(dek, *decoded, aad_bytes);
  ASSERT_TRUE(opened);
  EXPECT_EQ(*opened, plain);
  // The consuming decrypt gives the same plaintext and leaves the wrong key / wrong AAD failing.
  ByteVector copy = *sealed;
  auto consumed = FileCipher::Decrypt(dek, std::move(copy), aad);
  ASSERT_TRUE(consumed);
  EXPECT_EQ(*consumed, plain);
  EXPECT_FALSE(FileCipher::Decrypt(dek, ByteVector(*sealed), FileCipher::BuildAad("blob", "profile-b")));
  EXPECT_FALSE(FileCipher::Decrypt(ByteVector(kDataEncryptionKeySize, 0x22), ByteVector(*sealed), aad));
  EXPECT_FALSE(FileCipher::Decrypt(dek, ByteVector{1, 2, 3}, aad));
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

TEST_F(AtRestCryptoTest, CreateWithDekRoundTrip) {
  const auto vault_path = (dir_ / "vault-link.bin").string();
  ByteVector dek(kDataEncryptionKeySize, 0x7e);
  DataKeyVault vault(vault_path, "profile-link");
  ASSERT_TRUE(vault.CreateWithDek("link-pin", dek));
  EXPECT_EQ(*vault.Dek(), dek);

  DataKeyVault reopened(vault_path, "profile-link");
  ASSERT_TRUE(reopened.Unlock("link-pin"));
  EXPECT_EQ(*reopened.Dek(), dek);
}

TEST_F(AtRestCryptoTest, ReplaceWithDekOverwritesExistingVault) {
  const auto vault_path = (dir_ / "vault-replace.bin").string();
  ByteVector local_dek(kDataEncryptionKeySize, 0x11);
  ByteVector shared_dek(kDataEncryptionKeySize, 0x22);
  DataKeyVault vault(vault_path, "profile-replace");
  ASSERT_TRUE(vault.CreateWithDek("device-pin", local_dek));
  ASSERT_TRUE(vault.ReplaceWithDek("device-pin", shared_dek));
  EXPECT_EQ(*vault.Dek(), shared_dek);

  DataKeyVault reopened(vault_path, "profile-replace");
  EXPECT_FALSE(static_cast<bool>(reopened.Unlock("wrong-pin")));
  ASSERT_TRUE(reopened.Unlock("device-pin"));
  EXPECT_EQ(*reopened.Dek(), shared_dek);
}

// A copy of the vault bytes opens only with the same PIN and
// profile id; a wrong PIN is a PIN error.
TEST_F(AtRestCryptoTest, UnwrapDekFromVaultBytesNeedsPinAndProfile) {
  const auto vault_path = (dir_ / "vault-copy.bin").string();
  ByteVector dek(kDataEncryptionKeySize, 0x5a);
  DataKeyVault vault(vault_path, "profile-copy");
  ASSERT_TRUE(vault.CreateWithDek("copy-pin", dek));
  auto bytes = vault.ReadVaultFile();
  ASSERT_TRUE(bytes) << bytes.error().message;

  auto ok = DataKeyVault::UnwrapDekFromVaultBytes(*bytes, "profile-copy", "copy-pin");
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(*ok, dek);
  auto wrong_pin = DataKeyVault::UnwrapDekFromVaultBytes(*bytes, "profile-copy", "other-pin");
  ASSERT_FALSE(static_cast<bool>(wrong_pin));
  EXPECT_EQ(AppError::CategoryOf(wrong_pin.error()), ErrorCategory::Pin);
  EXPECT_FALSE(static_cast<bool>(DataKeyVault::UnwrapDekFromVaultBytes(*bytes, "other-profile", "copy-pin")));
  EXPECT_FALSE(static_cast<bool>(DataKeyVault::UnwrapDekFromVaultBytes(ByteVector(8, 0), "profile-copy", "copy-pin")));
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
