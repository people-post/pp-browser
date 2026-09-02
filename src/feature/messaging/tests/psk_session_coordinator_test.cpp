#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/PskBundleCodec.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/SqliteThreadStore.h"
#include "feature/messaging/PskSessionCoordinator.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

TEST(PskSessionCoordinatorTest, GenerateImportVerifyAndRotate) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_psk_session_coordinator";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  SqlitePskSessionStore psk_store(store.ProfileDbPath(), "test");
  ASSERT_TRUE(static_cast<bool>(psk_store.SetDek(TestDek())));
  PskSessionCoordinator coordinator(store, psk_store);

  DirectChatTarget target;
  target.peer_identity_kind = "relay_user";
  target.peer_identity_value = "relay:bob456";
  target.channel = ThreadChannel::E2e;
  auto thread = store.FindOrCreateDirectThread(target, "contact-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(thread));

  auto generated = coordinator.EnsureGenerated(thread->id);
  ASSERT_TRUE(static_cast<bool>(generated));
  EXPECT_FALSE(generated->master_psk_b64.empty());

  auto status = coordinator.GetStatus(thread->id);
  ASSERT_TRUE(static_cast<bool>(status));
  EXPECT_TRUE(status->has_psk);
  EXPECT_FALSE(status->verified);

  const std::string peer_key_b64 = generated->master_psk_b64;
  auto imported = coordinator.ImportRawBase64(thread->id, peer_key_b64);
  ASSERT_TRUE(static_cast<bool>(imported));

  ASSERT_TRUE(static_cast<bool>(coordinator.MarkVerified(thread->id, 123)));
  status = coordinator.GetStatus(thread->id);
  ASSERT_TRUE(static_cast<bool>(status));
  EXPECT_TRUE(status->verified);

  auto bundle_json = coordinator.RotatePskAndExportBundle(thread->id, 456);
  ASSERT_TRUE(static_cast<bool>(bundle_json));
  auto parsed = PskBundleCodec::ParseBundleJson(*bundle_json);
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed->active_epoch, 2u);
  EXPECT_EQ(parsed->retired_epochs.size(), 1u);
  EXPECT_NE(parsed->master_psk_b64, peer_key_b64);
}

} // namespace
