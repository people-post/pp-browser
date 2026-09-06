#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/AnnounceLiveJoin.h"
#include "domain/messaging/AnnounceLiveJoinHandoff.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/messaging/SqliteThreadStore.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <sodium.h>

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

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

std::filesystem::path MakeTempDir(const char* prefix) {
  EnsureSodiumInit();
  ByteVector rnd(8);
  randombytes_buf(rnd.data(), rnd.size());
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = std::filesystem::temp_directory_path() /
              (std::string(prefix) + "_" + std::to_string(stamp) + "_" + BytesToHex(rnd));
  std::filesystem::remove_all(path);
  return path;
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

TEST(BroadcastJoinTicketTest, ApplyPutsEpochKeyAndEnrichesHandoff) {
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  const ByteVector media_key = TestBytes(0x80);

  const auto data_dir = MakeTempDir("pp_bcast_ticket");
  auto store = std::make_unique<SqliteThreadStore>(data_dir.string());
  ASSERT_TRUE(store->ListThreads());
  auto media_keys = std::make_unique<CallMediaKeyStore>(store->ProfileDbPath());
  ASSERT_TRUE(media_keys->SetDek(TestDek()));

  auto ticket = MintBroadcastJoinTicket(SampleDraft(), media_key, keys->secret_key, nullptr);
  ASSERT_TRUE(ticket);
  auto applied = ApplyBroadcastJoinTicket(*media_keys, *ticket, keys->public_key, 1'900'000'000'000,
                                          "12D3KooWViewer");
  ASSERT_TRUE(applied) << applied.error().message;

  auto loaded = media_keys->LoadEpochKey(applied->call_id, applied->media_epoch);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ(**loaded, media_key);

  AnnounceLiveJoinPlan plan;
  plan.call_id = applied->call_id;
  plan.publisher_peer_id = ticket->publisher_peer_id;
  plan.program_id = ticket->program_id;
  plan.hop_peer_id = ticket->hop_peer_id;
  plan.media_epoch = applied->media_epoch;
  plan.media_key_id = applied->media_key_id;
  auto handoff =
      BuildAnnounceLiveJoinHandoff(plan, "account:viewer", "account:publisher", 1'900'000'000'000, true);
  ASSERT_TRUE(handoff) << handoff.error().message;
  EXPECT_EQ(handoff->session.media_epoch, 1u);
  EXPECT_EQ(handoff->session.media_key_id, applied->media_key_id);
  ASSERT_TRUE(handoff->session.sfu_hint.has_value());
  EXPECT_EQ(*handoff->session.sfu_hint, "12D3KooWHop");

  media_keys->ClearDek();
  media_keys.reset();
  store.reset();
  std::filesystem::remove_all(data_dir);
}
