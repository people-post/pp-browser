#include "base/messaging/AtAiParser.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/PeopleDiscoveryBlocks.h"
#include "base/messaging/JsonThreadStore.h"
#include "base/people/Ed25519Signer.h"

#include <filesystem>
#include <gtest/gtest.h>

TEST(MessagingFoundationTest, CoreMessagingUtilitiesRoundTrip) {
  using namespace pbr;

  const AtAiParseResult parsed = ParseAtAiPrefix("  @ai summarize this thread  ");
  EXPECT_TRUE(parsed.is_ai_invoke);
  EXPECT_EQ(parsed.prompt, "summarize this thread");

  Thread thread;
  thread.id = "t1";
  thread.kind = ThreadKind::Direct;
  thread.title = "Alice";
  thread.participant_contact_ids = {"c1"};
  const nlohmann::json thread_json = ThreadToJson(thread);
  const Thread restored = ThreadFromJson(thread_json);
  EXPECT_EQ(restored.id, "t1");
  EXPECT_EQ(restored.kind, ThreadKind::Direct);

  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_messaging_test";
  std::filesystem::remove_all(data_dir);
  JsonThreadStore store(data_dir.string());
  (void)store.UpsertThread(thread);

  ThreadMessage message;
  message.id = "m1";
  message.thread_id = "t1";
  message.sender_contact_id = kLocalSelfContactId;
  message.text = "hello";
  (void)store.AppendMessage(message);
  EXPECT_TRUE(store.HasMessageId("m1"));

  RelayEnvelope envelope;
  envelope.thread_id = "t1";
  envelope.message_id = "m1";
  envelope.sender_relay_id = "relay:x";
  envelope.body.text = "hi";
  const RelayEnvelope roundtrip = RelayEnvelopeFromJson(RelayEnvelopeToJson(envelope));
  EXPECT_EQ(roundtrip.message_id, "m1");

  auto keys = Ed25519Signer::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  auto signature = Ed25519Signer::Sign("test payload", keys->private_key);
  ASSERT_TRUE(static_cast<bool>(signature));
  auto verified = Ed25519Signer::Verify("test payload", *signature, keys->public_key);
  ASSERT_TRUE(static_cast<bool>(verified));
  EXPECT_TRUE(*verified);

  DirectoryHit hit;
  hit.hit_id = "hit_alice";
  hit.display_name = "Alice Example";
  hit.nickname = "alice";
  hit.ids = {{ContactIdKind::RelayUser, "relay:alice123", true}};
  const std::string blocks = BuildPeopleDiscoveryBlocksJson({hit}, {});
  EXPECT_NE(blocks.find("long_list"), std::string::npos);
  EXPECT_NE(blocks.find("Alice Example"), std::string::npos);
  EXPECT_NE(blocks.find("start_conversation"), std::string::npos);
}
