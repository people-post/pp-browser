#include "foundation/crypto/ReplayWindow.h"
#include "domain/messaging/E2eIngestClassifier.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/messaging/SyncStateCodec.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {
using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}
} // namespace

TEST(V6PipelineTest, ClearHistorySetsHistoryFloorToLoadedMax) {
  using namespace pbr;

  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_v6_floor_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.SetDek(TestDek()));

  DirectChatTarget target;
  target.peer_identity_kind = "relay_user";
  target.peer_identity_value = "relay:dana";
  target.channel = ThreadChannel::E2e;

  auto thread = store.FindOrCreateDirectThread(target, "contact-dana", "Dana");
  ASSERT_TRUE(static_cast<bool>(thread));

  auto append_peer = [&](uint64_t seq, const std::string& text) {
    ThreadMessage message;
    message.id = "peer-" + std::to_string(seq);
    message.thread_id = thread->id;
    message.sender_contact_id = "contact-dana";
    message.text = text;
    message.timestamp = static_cast<int64_t>(seq);
    message.delivery = MessageDelivery::Relayed;
    message.relay_visible = true;
    message.transport = MessageTransport::Relay;
    message.sender_seq = seq;
    message.session_epoch = 1;
    return store.AppendMessage(message);
  };

  ASSERT_TRUE(static_cast<bool>(append_peer(1, "one")));
  ASSERT_TRUE(static_cast<bool>(append_peer(3, "three")));

  ASSERT_TRUE(static_cast<bool>(store.ClearMessages(thread->id, {})));

  auto state = store.GetPeerSyncState(thread->id, 1);
  ASSERT_TRUE(static_cast<bool>(state));
  EXPECT_EQ(state->history_floor_seq, 3u);
  EXPECT_EQ(state->contiguous_peer_seq, 0u);
  EXPECT_EQ(state->loaded_max_seq, 0u);
}

TEST(V6PipelineTest, BelowFloorInboundDiscardedByClassifier) {
  using namespace pbr;

  PeerSyncState state = DefaultPeerSyncState();
  state.history_floor_seq = 10;
  state.contiguous_peer_seq = 10;

  IngestClassifierInput input;
  input.sender_seq = 8;
  input.session_epoch = 1;
  input.message_id = "old-msg";
  input.chat_target_epoch = 1;
  input.sync_state = state;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::SilentDiscard);
}
