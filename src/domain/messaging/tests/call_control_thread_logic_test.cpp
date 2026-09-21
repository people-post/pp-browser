#include "domain/messaging/CallControlThreadLogic.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <unordered_set>

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
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_control_thread_" + util::GenerateUuid());
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

TEST_F(CallControlThreadLogicTest, PruneDeletesEmptyShadowTwin) {
  DirectChatTarget priv;
  priv.peer_identity_kind = "account";
  priv.peer_identity_value = "account:bob";
  priv.channel = ThreadChannel::E2e;
  auto private_dm = store_->FindOrCreateDirectThread(priv, "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(private_dm));

  DirectChatTarget pub_target;
  pub_target.peer_identity_kind = "account";
  pub_target.peer_identity_value = "account:bob";
  pub_target.channel = ThreadChannel::E2ePublic;
  auto pub = store_->FindOrCreateDirectThread(pub_target, "c-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(pub));

  auto pruned = PruneOrphanCallControlShadows(*store_);
  ASSERT_TRUE(static_cast<bool>(pruned));
  EXPECT_EQ(*pruned, 1u);
  EXPECT_FALSE(store_->GetThread(pub->id)->has_value());
  EXPECT_TRUE(store_->GetThread(private_dm->id)->has_value());
}

TEST_F(CallControlThreadLogicTest, PruneDeletesCallControlOnlyShadow) {
  DirectChatTarget priv;
  priv.peer_identity_kind = "account";
  priv.peer_identity_value = "account:carol";
  priv.channel = ThreadChannel::E2e;
  ASSERT_TRUE(static_cast<bool>(store_->FindOrCreateDirectThread(priv, "c-carol", "Carol")));

  DirectChatTarget pub_target;
  pub_target.peer_identity_kind = "account";
  pub_target.peer_identity_value = "account:carol";
  pub_target.channel = ThreadChannel::E2ePublic;
  auto pub = store_->FindOrCreateDirectThread(pub_target, "c-carol", "Carol");
  ASSERT_TRUE(static_cast<bool>(pub));

  auto invite =
      CallControlCodec::BuildSystemMessage(pub->id, CallControlType::CallInvite, "ring", "{}", "me");
  ASSERT_TRUE(static_cast<bool>(invite));
  ASSERT_TRUE(static_cast<bool>(store_->AppendMessage(*invite)));
  auto ended =
      CallControlCodec::BuildSystemMessage(pub->id, CallControlType::CallEnded, "ended", "{}", "me");
  ASSERT_TRUE(static_cast<bool>(ended));
  ASSERT_TRUE(static_cast<bool>(store_->AppendMessage(*ended)));

  auto pruned = PruneOrphanCallControlShadows(*store_);
  ASSERT_TRUE(static_cast<bool>(pruned));
  EXPECT_EQ(*pruned, 1u);
  EXPECT_FALSE(store_->GetThread(pub->id)->has_value());
}

TEST_F(CallControlThreadLogicTest, PruneKeepsShadowWithUserChat) {
  DirectChatTarget priv;
  priv.peer_identity_kind = "account";
  priv.peer_identity_value = "account:dave";
  priv.channel = ThreadChannel::E2e;
  ASSERT_TRUE(static_cast<bool>(store_->FindOrCreateDirectThread(priv, "c-dave", "Dave")));

  DirectChatTarget pub_target;
  pub_target.peer_identity_kind = "account";
  pub_target.peer_identity_value = "account:dave";
  pub_target.channel = ThreadChannel::E2ePublic;
  auto pub = store_->FindOrCreateDirectThread(pub_target, "c-dave", "Dave");
  ASSERT_TRUE(static_cast<bool>(pub));

  ThreadMessage chat;
  chat.id = util::GenerateUuid();
  chat.thread_id = pub->id;
  chat.sender_contact_id = "me";
  chat.content_type = ChatContentType::Text;
  chat.text = "hello on public twin";
  chat.timestamp = util::NowUnixMs();
  ASSERT_TRUE(static_cast<bool>(store_->AppendMessage(chat)));

  auto pruned = PruneOrphanCallControlShadows(*store_);
  ASSERT_TRUE(static_cast<bool>(pruned));
  EXPECT_EQ(*pruned, 0u);
  EXPECT_TRUE(store_->GetThread(pub->id)->has_value());
}

TEST_F(CallControlThreadLogicTest, PruneRespectsProtectIds) {
  DirectChatTarget priv;
  priv.peer_identity_kind = "account";
  priv.peer_identity_value = "account:eve";
  priv.channel = ThreadChannel::E2e;
  ASSERT_TRUE(static_cast<bool>(store_->FindOrCreateDirectThread(priv, "c-eve", "Eve")));

  DirectChatTarget pub_target;
  pub_target.peer_identity_kind = "account";
  pub_target.peer_identity_value = "account:eve";
  pub_target.channel = ThreadChannel::E2ePublic;
  auto pub = store_->FindOrCreateDirectThread(pub_target, "c-eve", "Eve");
  ASSERT_TRUE(static_cast<bool>(pub));

  auto pruned = PruneOrphanCallControlShadows(*store_, std::unordered_set<std::string>{pub->id});
  ASSERT_TRUE(static_cast<bool>(pruned));
  EXPECT_EQ(*pruned, 0u);
  EXPECT_TRUE(store_->GetThread(pub->id)->has_value());
}

TEST_F(CallControlThreadLogicTest, PruneSkipsPublicWithoutPrivateSibling) {
  DirectChatTarget pub_target;
  pub_target.peer_identity_kind = "account";
  pub_target.peer_identity_value = "account:solo";
  pub_target.channel = ThreadChannel::E2ePublic;
  auto pub = store_->FindOrCreateDirectThread(pub_target, "c-solo", "Solo");
  ASSERT_TRUE(static_cast<bool>(pub));

  auto pruned = PruneOrphanCallControlShadows(*store_);
  ASSERT_TRUE(static_cast<bool>(pruned));
  EXPECT_EQ(*pruned, 0u);
  EXPECT_TRUE(store_->GetThread(pub->id)->has_value());
}

} // namespace
