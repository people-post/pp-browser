#include "feature/messaging/CallMediaKeyStore.h"

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

} // namespace
