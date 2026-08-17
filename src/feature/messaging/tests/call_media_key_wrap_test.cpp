#include "feature/messaging/CallMediaKeyStore.h"

#include "base/crypto/CryptoConstants.h"
#include "base/messaging/SqliteThreadStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

ByteVector TestBytes(uint8_t seed, size_t size = 32) {
  ByteVector bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<uint8_t>(seed + i);
  }
  return bytes;
}

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

TEST(CallMediaKeyStoreWrapTest, BuildWrapAadFormat) {
  EXPECT_EQ(CallMediaKeyStore::BuildWrapAad("call:abc", 3, "mk:xyz"), "call_media_key|call:abc|3|mk:xyz");
  EXPECT_EQ(CallMediaKeyStore::BuildWrapAad("call:abc", 1, "mk:xyz"), "call_media_key|call:abc|1|mk:xyz");
}

TEST(CallMediaKeyStoreWrapTest, WrapUnwrapRoundTrip) {
  const ByteVector session_key = TestBytes(0x10);
  const ByteVector key_bytes = TestBytes(0x40);
  const std::string call_id = "call:test-1";
  const uint32_t media_epoch = 1;
  const std::string media_key_id = "mk:test-1";

  auto wrapped = CallMediaKeyStore::WrapKeyB64(session_key, key_bytes, call_id, media_epoch, media_key_id);
  ASSERT_TRUE(wrapped);
  EXPECT_FALSE(wrapped->empty());

  auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(session_key, *wrapped, call_id, media_epoch, media_key_id);
  ASSERT_TRUE(unwrapped);
  EXPECT_EQ(*unwrapped, key_bytes);
}

TEST(CallMediaKeyStoreWrapTest, UnwrapFailsOnWrongSessionKey) {
  const ByteVector session_key = TestBytes(0x10);
  const ByteVector wrong_key = TestBytes(0x20);
  const ByteVector key_bytes = TestBytes(0x40);
  const std::string call_id = "call:test-2";
  const uint32_t media_epoch = 1;
  const std::string media_key_id = "mk:test-2";

  auto wrapped = CallMediaKeyStore::WrapKeyB64(session_key, key_bytes, call_id, media_epoch, media_key_id);
  ASSERT_TRUE(wrapped);

  auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(wrong_key, *wrapped, call_id, media_epoch, media_key_id);
  EXPECT_FALSE(unwrapped);
}

TEST(CallMediaKeyStoreWrapTest, UnwrapFailsOnMismatchedAad) {
  const ByteVector session_key = TestBytes(0x10);
  const ByteVector key_bytes = TestBytes(0x40);
  const std::string call_id = "call:test-3";
  const std::string media_key_id = "mk:test-3";

  auto wrapped = CallMediaKeyStore::WrapKeyB64(session_key, key_bytes, call_id, /*media_epoch=*/1, media_key_id);
  ASSERT_TRUE(wrapped);

  // Epoch mismatch changes the AAD, so decrypt must fail (authenticity, not just format).
  auto unwrapped = CallMediaKeyStore::UnwrapKeyB64(session_key, *wrapped, call_id, /*media_epoch=*/2, media_key_id);
  EXPECT_FALSE(unwrapped);
}

TEST(CallMediaKeyStoreWrapTest, WrapRejectsMissingArguments) {
  const ByteVector session_key = TestBytes(0x10);
  const ByteVector key_bytes = TestBytes(0x40);
  EXPECT_FALSE(CallMediaKeyStore::WrapKeyB64({}, key_bytes, "call:test-4", 1, "mk:test-4"));
  EXPECT_FALSE(CallMediaKeyStore::WrapKeyB64(session_key, {}, "call:test-4", 1, "mk:test-4"));
  EXPECT_FALSE(CallMediaKeyStore::WrapKeyB64(session_key, key_bytes, "", 1, "mk:test-4"));
  EXPECT_FALSE(CallMediaKeyStore::WrapKeyB64(session_key, key_bytes, "call:test-4", 1, ""));
}

/**
 * Product media-key defer path (B-CALL-DIRECT Tier B): answerer LoadEpochKey must see the
 * offerer's PutEpochKey after vault unlock — without this, CallLibp2pMediaBridge stays MediaPending.
 */
TEST(CallMediaKeyStoreEpochTest, PutLoadEpochKeyRoundTrip) {
  const auto data_dir =
      std::filesystem::temp_directory_path() / ("pp_call_media_key_" + util::GenerateUuid());
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore threads(data_dir.string());
  ASSERT_TRUE(threads.ListThreads());

  CallMediaKeyStore keys(threads.ProfileDbPath());
  ASSERT_TRUE(keys.SetDek(TestDek()));

  auto generated = keys.GenerateEpochKey();
  ASSERT_TRUE(generated);
  ASSERT_EQ(generated->size(), 32u);

  const std::string call_id = "call:epoch-defer";
  auto media_key_id = keys.PutEpochKey(call_id, /*media_epoch=*/1, *generated);
  ASSERT_TRUE(media_key_id) << media_key_id.error().message;
  EXPECT_FALSE(media_key_id->empty());

  auto loaded = keys.LoadEpochKey(call_id, 1);
  ASSERT_TRUE(loaded) << loaded.error().message;
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ(**loaded, *generated);

  auto missing = keys.LoadEpochKey(call_id, /*media_epoch=*/2);
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing->has_value());

  keys.ClearDek();
  std::filesystem::remove_all(data_dir);
}

} // namespace
