#include "domain/messaging/BroadcastJoinTicket.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

#include "common/PbrCompat.h"

namespace {

using namespace pbr;

ByteVector TestBytes(uint8_t seed, size_t size = 32) {
  ByteVector bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<uint8_t>(seed + i);
  }
  return bytes;
}


BroadcastJoinTicketDraft SampleDraft() {
  BroadcastJoinTicketDraft draft;
  draft.publisher_peer_id = "12D3KooWPublisher";
  draft.program_id = "show-1";
  draft.join_handle = "live:show-1";
  draft.viewer_peer_id = "12D3KooWViewer";
  draft.media_epoch = 1;
  draft.hop_peer_id = "12D3KooWHop";
  draft.expires_at_ms = 2'000'000'000'000;
  return draft;
}

} // namespace

TEST(BroadcastJoinTicketTest, MintVerifyKeyMaterialPath) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  const ByteVector media_key = TestBytes(0x40);

  auto ticket = MintBroadcastJoinTicket(SampleDraft(), media_key, keys->secret_key, nullptr);
  ASSERT_TRUE(ticket) << ticket.error().message;
  EXPECT_TRUE(ticket->wrapped_key_b64.empty());
  EXPECT_FALSE(ticket->key_material_b64.empty());
  EXPECT_FALSE(ticket->signature_b64.empty());
  EXPECT_EQ(ticket->hop_peer_id, "12D3KooWHop");

  auto extracted =
      ExtractBroadcastMediaKey(*ticket, keys->public_key, /*now_ms=*/1'900'000'000'000, "12D3KooWViewer");
  ASSERT_TRUE(extracted) << extracted.error().message;
  EXPECT_EQ(extracted->call_id, "live:show-1");
  EXPECT_EQ(extracted->media_epoch, 1u);
  EXPECT_EQ(extracted->key_bytes, media_key);
}

TEST(BroadcastJoinTicketTest, MintVerifyWrappedPath) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  const ByteVector media_key = TestBytes(0x50);
  const ByteVector session_key = TestBytes(0x10);

  auto ticket = MintBroadcastJoinTicket(SampleDraft(), media_key, keys->secret_key, &session_key);
  ASSERT_TRUE(ticket) << ticket.error().message;
  EXPECT_FALSE(ticket->wrapped_key_b64.empty());
  EXPECT_TRUE(ticket->key_material_b64.empty());

  auto extracted = ExtractBroadcastMediaKey(*ticket, keys->public_key, /*now_ms=*/1'900'000'000'000,
                                            "12D3KooWViewer", &session_key);
  ASSERT_TRUE(extracted) << extracted.error().message;
  EXPECT_EQ(extracted->key_bytes, media_key);
}

TEST(BroadcastJoinTicketTest, RejectsExpiredViewerMismatchAndBadSig) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto ticket = MintBroadcastJoinTicket(SampleDraft(), TestBytes(0x60), keys->secret_key, nullptr);
  ASSERT_TRUE(ticket);

  EXPECT_FALSE(VerifyBroadcastJoinTicket(*ticket, keys->public_key, /*now_ms=*/3'000'000'000'000,
                                         "12D3KooWViewer"));
  EXPECT_FALSE(VerifyBroadcastJoinTicket(*ticket, keys->public_key, /*now_ms=*/1'900'000'000'000,
                                         "12D3KooWOther"));

  auto other = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(other);
  EXPECT_FALSE(VerifyBroadcastJoinTicket(*ticket, other->public_key, /*now_ms=*/1'900'000'000'000,
                                         "12D3KooWViewer"));
}

TEST(BroadcastJoinTicketTest, JsonRoundTripPreservesSignature) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto ticket = MintBroadcastJoinTicket(SampleDraft(), TestBytes(0x70), keys->secret_key, nullptr);
  ASSERT_TRUE(ticket);

  auto json = EncodeBroadcastJoinTicketJson(*ticket);
  ASSERT_TRUE(json);
  auto decoded = DecodeBroadcastJoinTicketJson(*json);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->signature_b64, ticket->signature_b64);
  EXPECT_EQ(decoded->join_handle, ticket->join_handle);
  ASSERT_TRUE(VerifyBroadcastJoinTicket(*decoded, keys->public_key, 1'900'000'000'000, "12D3KooWViewer"));
}

