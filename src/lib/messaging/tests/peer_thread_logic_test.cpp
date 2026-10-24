#include "lib/messaging/PeerThreadLogic.h"

#include "lib/messaging/SqliteThreadStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "common/directory/DirectoryJson.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/MeshAlias.h"

namespace {

using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xc2 + i);
  }
  return dek;
}

class PeerThreadLogicTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_peer_thread_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(static_cast<bool>(store_->SetDek(TestDek())));
    ASSERT_TRUE(static_cast<bool>(store_->ListThreads()));
  }

  void TearDown() override {
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  Thread Dm(ThreadChannel channel) {
    DirectChatTarget target;
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = "account:bob";
    target.channel = channel;
    auto thread = store_->FindOrCreateDirectThread(target, "c-bob", "Bob");
    EXPECT_TRUE(static_cast<bool>(thread));
    return *thread;
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
};

TEST_F(PeerThreadLogicTest, PrefersThePrivateDm) {
  Dm(ThreadChannel::E2ePublic);
  const Thread priv = Dm(ThreadChannel::E2e);
  auto resolved = ResolveDirectThreadForPeer(*store_, "account:bob", "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(resolved));
  EXPECT_EQ(*resolved, priv.id);
}

TEST_F(PeerThreadLogicTest, ElseThePublicDm) {
  const Thread pub = Dm(ThreadChannel::E2ePublic);
  auto resolved = ResolveDirectThreadForPeer(*store_, "account:bob", "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(resolved));
  EXPECT_EQ(*resolved, pub.id);
}

TEST_F(PeerThreadLogicTest, MakesThePublicDmWhenThereIsNone) {
  auto resolved = ResolveDirectThreadForPeer(*store_, "account:carol", "", "");
  ASSERT_TRUE(static_cast<bool>(resolved));
  auto thread = store_->GetThread(*resolved);
  ASSERT_TRUE(thread && *thread);
  EXPECT_EQ((*thread)->channel, ThreadChannel::E2ePublic);
  EXPECT_EQ((*thread)->peer_identity_value, "account:carol");
  auto again = ResolveDirectThreadForPeer(*store_, "account:carol", "", "");
  ASSERT_TRUE(static_cast<bool>(again));
  EXPECT_EQ(*again, *resolved);
  EXPECT_FALSE(ResolveDirectThreadForPeer(*store_, "", "", ""));
}

} // namespace
