#include "base/people/Ed25519Signer.h"
#include "base/people/IdentityStore.h"
#include "libp2p/integration/host/PeerIdUtil.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

using namespace pbr;

class IdentityStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_browser_identity_store_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(data_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(data_dir_); }

  std::filesystem::path data_dir_;
};

TEST_F(IdentityStoreTest, CreateHasPeerIdAndEmptyRelay) {
  IdentityStore store(data_dir_.string());
  auto identity = store.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(identity)) << identity.error().message;
  EXPECT_FALSE(identity->peer_id.empty());
  EXPECT_TRUE(identity->relay_user_id.empty());
  EXPECT_FALSE(identity->registered);

  auto public_key = Ed25519Signer::FromBase64(identity->public_key_b64);
  ASSERT_TRUE(static_cast<bool>(public_key));
  auto derived = PeerIdFromEd25519PublicKey(*public_key);
  ASSERT_TRUE(static_cast<bool>(derived));
  EXPECT_EQ(identity->peer_id, *derived);
}

TEST_F(IdentityStoreTest, DerivesSamePeerIdAcrossReloadWithoutPersisting) {
  std::string peer_id;
  {
    IdentityStore store(data_dir_.string());
    auto identity = store.LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(identity));
    peer_id = identity->peer_id;
  }
  {
    std::ifstream in(data_dir_ / "identity.json");
    ASSERT_TRUE(static_cast<bool>(in));
    const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
    ASSERT_FALSE(root.is_discarded());
    EXPECT_FALSE(root.contains("peer_id"));
  }
  IdentityStore reloaded(data_dir_.string());
  auto identity = reloaded.Get();
  ASSERT_TRUE(static_cast<bool>(identity));
  EXPECT_EQ(identity->peer_id, peer_id);
  EXPECT_TRUE(identity->relay_user_id.empty());
}

TEST_F(IdentityStoreTest, KeepsRegisteredRelayId) {
  auto keys = Ed25519Signer::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  const std::string public_key_b64 = Ed25519Signer::ToBase64(keys->public_key);
  const std::string private_key_b64 = Ed25519Signer::ToBase64(keys->private_key);

  std::filesystem::create_directories(data_dir_);
  const nlohmann::json root = {{"public_key_b64", public_key_b64},
                               {"encrypted_private_key_b64", private_key_b64},
                               {"nickname", "alice"},
                               {"relay_user_id", "relay:alice123"},
                               {"registered", true}};
  {
    std::ofstream out(data_dir_ / "identity.json");
    ASSERT_TRUE(static_cast<bool>(out));
    out << root.dump(2);
  }

  IdentityStore store(data_dir_.string());
  auto identity = store.LoadOrCreate();
  ASSERT_TRUE(static_cast<bool>(identity)) << identity.error().message;
  EXPECT_EQ(identity->relay_user_id, "relay:alice123");
  EXPECT_TRUE(identity->registered);
  EXPECT_FALSE(identity->peer_id.empty());
}

} // namespace
