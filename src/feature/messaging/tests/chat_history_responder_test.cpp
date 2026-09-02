#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "feature/messaging/ChatHistoryResponder.h"
#include "base/messaging/ChatHistoryStreamCodec.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/SqliteThreadStore.h"
#include "domain/people/Ed25519Signer.h"
#include "domain/people/IdentityStore.h"
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

class ResponderHarness {
public:
  explicit ResponderHarness(const std::string& suffix)
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_chat_history_" + suffix)),
        store(data_dir.string()), identity(data_dir.string(), "test"), psk_store(store.ProfileDbPath(), "test") {
    std::filesystem::remove_all(data_dir);
    if (!identity.SetDek(TestDek()) || !psk_store.SetDek(TestDek()) || !store.SetDek(TestDek())) {
      throw std::runtime_error("dek setup failed");
    }
    if (!identity.LoadOrCreate()) {
      throw std::runtime_error("identity load failed");
    }
    {
      LocalIdentity updated = *identity.Get();
      updated.relay_user_id = "relay:local";
      updated.registered = true;
      if (!identity.Update(updated)) {
        throw std::runtime_error("Failed to set test relay id");
      }
    }
    local_relay_id = identity.Get()->relay_user_id;

    DirectChatTarget target;
    target.peer_identity_kind = "relay_user";
    target.peer_identity_value = "relay:peer";
    target.channel = ThreadChannel::E2e;
    auto created = store.FindOrCreateDirectThread(target, "contact-peer", "Peer");
    if (!created) {
      throw std::runtime_error("thread create failed");
    }
    thread = *created;

    PskSessionRecord psk;
    psk.key = E2eRelayPayloadCodec::ChatTargetFromThread(thread);
    psk.session_epoch = 1;
    const auto master = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    if (!master) {
      throw std::runtime_error("psk bytes");
    }
    psk.master_psk_b64 = Base64Encode(*master);
    psk.psk_verified_at = 1;
    if (!psk_store.Save(psk)) {
      throw std::runtime_error("psk save");
    }
  }

  void SeedOutbound(uint64_t seq, const std::string& text) {
    ThreadMessage message;
    message.id = "out-" + std::to_string(seq);
    message.thread_id = thread.id;
    message.sender_contact_id = kLocalSelfContactId;
    message.text = text;
    message.timestamp = static_cast<int64_t>(seq);
    message.delivery = MessageDelivery::Relayed;
    message.relay_visible = true;
    message.transport = MessageTransport::Local;
    message.sender_seq = seq;
    message.session_epoch = 1;
    if (!store.AppendMessage(message)) {
      throw std::runtime_error("append failed");
    }
  }

  std::filesystem::path data_dir;
  SqliteThreadStore store;
  IdentityStore identity;
  SqlitePskSessionStore psk_store;
  Thread thread;
  std::string local_relay_id;
};

} // namespace

TEST(ChatHistoryResponderTest, ServesSignedOutboundRows) {
  ResponderHarness harness("serve");
  harness.SeedOutbound(1, "hello");
  harness.SeedOutbound(2, "world");

  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = "relay:peer";
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = harness.local_relay_id;
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.limit = 10;
  request.order = "asc";

  auto response =
      ChatHistoryResponder::Serve(harness.store, harness.identity, harness.psk_store, request, harness.local_relay_id);
  ASSERT_TRUE(static_cast<bool>(response));
  ASSERT_EQ(response->messages.size(), 2u);
  EXPECT_EQ(response->messages[0].sender_seq, 1u);
  EXPECT_EQ(response->messages[1].sender_seq, 2u);
  auto verified = EnvelopeSigner::Verify(response->messages[0], harness.identity.Get()->account_signing_public_key_b64);
  ASSERT_TRUE(static_cast<bool>(verified));
  EXPECT_TRUE(*verified);
}

TEST(ChatHistoryResponderTest, RejectsNonParticipantRequester) {
  ResponderHarness harness("reject");
  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = "relay:stranger";
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = harness.local_relay_id;
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;

  auto response =
      ChatHistoryResponder::Serve(harness.store, harness.identity, harness.psk_store, request, harness.local_relay_id);
  EXPECT_FALSE(static_cast<bool>(response));
}

TEST(ChatHistoryStreamCodecTest, RoundTripFrame) {
  const std::string json = R"({"session_epoch":1,"order":"asc"})";
  auto frame = ChatHistoryStreamCodec::EncodeFrame(json);
  ASSERT_TRUE(static_cast<bool>(frame));
  auto decoded = ChatHistoryStreamCodec::DecodeFrame(*frame);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(*decoded, json);
}
