#include "foundation/crypto/ConfigSecretsStore.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/ProfileSecretsEngine.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace pbr {
namespace {

ByteVector Dek(const uint8_t fill) {
  return ByteVector(kDataEncryptionKeySize, fill);
}

// PB-034: the codec round-trips, binds to the profile id and the DEK, and rejects other schemas.
TEST(ConfigSecretsStoreTest, SealAndOpenRoundTripBoundToDekAndProfile) {
  const ConfigSecrets secrets{.llm_api_key = "sk-typed"};
  auto blob = ConfigSecretsStore::Seal(Dek(1), "profile-a", secrets);
  ASSERT_TRUE(static_cast<bool>(blob)) << blob.error().message;
  const std::string raw(blob->begin(), blob->end());
  EXPECT_EQ(raw.find("sk-typed"), std::string::npos) << "the key must not be readable in the file";

  auto opened = ConfigSecretsStore::Open(Dek(1), "profile-a", *blob);
  ASSERT_TRUE(static_cast<bool>(opened)) << opened.error().message;
  EXPECT_EQ(*opened, secrets);

  EXPECT_FALSE(static_cast<bool>(ConfigSecretsStore::Open(Dek(2), "profile-a", *blob))) << "other DEK";
  EXPECT_FALSE(static_cast<bool>(ConfigSecretsStore::Open(Dek(1), "profile-b", *blob))) << "other profile";
}

class ConfigSecretsStoreFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_config_secrets_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    auto initialized = engine_.Initialize(dir_.string());
    ASSERT_TRUE(static_cast<bool>(initialized)) << initialized.error().message;
  }
  void TearDown() override {
    engine_.Shutdown();
    std::filesystem::remove_all(dir_);
  }
  std::filesystem::path dir_;
  ProfileSecretsEngine engine_;
};

// Locked: not ready, Load and Store refuse. Unlocked (DEK set): Store writes secrets.enc owner-only, Load
// reads it back, storing nothing removes the file, and the ready callback fires once the DEK arrives.
TEST_F(ConfigSecretsStoreFileTest, StoreLoadAndRemoveUnderTheDek) {
  ConfigSecretsStore store(engine_);
  int ready_calls = 0;
  store.SetOnReady([&] { ++ready_calls; });
  EXPECT_FALSE(store.Ready());
  EXPECT_FALSE(static_cast<bool>(store.Load()));
  EXPECT_FALSE(static_cast<bool>(store.Store(ConfigSecrets{.llm_api_key = "x"})));

  auto set = store.SetDek(Dek(7));
  ASSERT_TRUE(static_cast<bool>(set)) << set.error().message;
  EXPECT_EQ(ready_calls, 1);
  EXPECT_TRUE(store.Ready());

  auto empty = store.Load();
  ASSERT_TRUE(static_cast<bool>(empty)) << empty.error().message;
  EXPECT_TRUE(empty->Empty()) << "no file yet means no secrets, not an error";

  const ConfigSecrets secrets{.llm_api_key = "sk-typed"};
  auto stored = store.Store(secrets);
  ASSERT_TRUE(static_cast<bool>(stored)) << stored.error().message;
  const std::string path = ConfigSecretsStore::PathFor(dir_.string());
  ASSERT_TRUE(std::filesystem::exists(path));
#if !defined(_WIN32)
  EXPECT_EQ(std::filesystem::status(path).permissions(),
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
  auto loaded = store.Load();
  ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.error().message;
  EXPECT_EQ(*loaded, secrets);

  ASSERT_TRUE(static_cast<bool>(store.Store(ConfigSecrets{})));
  EXPECT_FALSE(std::filesystem::exists(path)) << "nothing to keep, no file";

  store.ClearDek();
  EXPECT_FALSE(store.Ready());
}

} // namespace
} // namespace pbr
