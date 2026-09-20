#include "domain/messaging/CallControlThreadLogic.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "foundation/crypto/CryptoConstants.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xc2 + i);
  }
  return dek;
}

class CallControlThreadLogicTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / "pp_browser_call_control_thread_logic";
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(static_cast<bool>(store_->SetDek(TestDek())));
    ASSERT_TRUE(static_cast<bool>(store_->ListThreads()));
  }

  void TearDown() override {
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
};

TEST_F(CallControlThreadLogicTest, PrefersOriginWhenMatchingE2ePublic) {
  DirectChatTarget target;
  target.peer_identity_kind = "account";
  target.peer_identity_value = "account:bob";
  target.channel = ThreadChannel::E2ePublic;
  auto created = store_->FindOrCreateDirectThread(target, "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(created));

  auto resolved =
      ResolveOrCreateE2ePublicDirectThread(*store_, "account:bob", created->id, "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(resolved));
  EXPECT_EQ(*resolved, created->id);

  auto again = ResolveOrCreateE2ePublicDirectThread(*store_, "account:bob", std::nullopt, "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(again));
  EXPECT_EQ(*again, created->id);
}

TEST_F(CallControlThreadLogicTest, IgnoresPreferWhenWrongPeerOrChannel) {
  DirectChatTarget priv;
  priv.peer_identity_kind = "account";
  priv.peer_identity_value = "account:bob";
  priv.channel = ThreadChannel::E2e;
  auto private_dm = store_->FindOrCreateDirectThread(priv, "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(private_dm));

  auto resolved =
      ResolveOrCreateE2ePublicDirectThread(*store_, "account:bob", private_dm->id, "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(resolved));
  EXPECT_NE(*resolved, private_dm->id);

  auto pub = store_->GetThread(*resolved);
  ASSERT_TRUE(static_cast<bool>(pub) && pub->has_value());
  EXPECT_EQ((*pub)->channel, ThreadChannel::E2ePublic);
}

} // namespace
