#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/messaging/ChatHistoryResponder.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "lib/messaging/EnvelopeSigner.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/messaging/tests/TestAccount.h"
#include "lib/messaging/SqlitePskSessionStore.h"
#include "lib/messaging/PeerRequestAuth.h"
#include "lib/messaging/PeerSigningKeyStore.h"
#include "common/Utilities.h"

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
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_chat_history_" + suffix + "_" + util::GenerateUuid())),
        store(data_dir.string()), local(test::TestAccount::Make()), psk_store(store.ProfileDbPath(), "test"),
        requester(test::TestAccount::Make()), keys_resolver(keys) {
    std::filesystem::remove_all(data_dir);
    if (!psk_store.SetDek(TestDek()) || !store.SetDek(TestDek())) {
      throw std::runtime_error("dek setup failed");
    }
    requester_account = requester.account_id;
    PeerSigningKeyRecord record;
    record.signing_public_key_b64 = requester.SigningPublicB64();
    record.source = "test";
    keys.Put("account", requester_account, record);
    local_relay_id = "relay:local";

    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = requester_account;
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

  /** A history request from the requester, signed as it would send it (PeerRequestAuth). */
  ChatHistoryRequest SignedRequest(int64_t now_ms = util::NowUnixMs()) {
    ChatHistoryRequest request;
    request.requester_identity_kind = "relay_user";
    request.requester_identity_value = "relay:peer";
    request.peer_identity_kind = "relay_user";
    request.peer_identity_value = local_relay_id;
    request.channel = ThreadChannel::E2e;
    request.session_epoch = 1;
    request.limit = 10;
    request.order = "asc";
    auto signed_ok = PeerRequestAuth::Sign(request, requester_account,
                                           *PeerRequestAuth::DevicePeerId(requester.DevicePublicB64()), now_ms,
                                           [this](const std::vector<uint8_t>& bytes) { return requester.Sign(bytes); });
    if (!signed_ok) {
      throw std::runtime_error("sign request");
    }
    return request;
  }

  Roe<ChatHistoryResponse> Serve(const ChatHistoryRequest& request, const std::string& remote_peer_id) {
    return ChatHistoryResponder::Serve(
        store, psk_store, request, local_relay_id,
        local.account_id, local.device.peer_id,
        [this](const std::vector<uint8_t>& bytes) { return local.device.Sign(bytes); }, remote_peer_id, keys_resolver,
        util::NowUnixMs());
  }

  std::filesystem::path data_dir;
  SqliteThreadStore store;
  test::TestAccount local;
  SqlitePskSessionStore psk_store;
  test::TestAccount requester;
  PeerSigningKeyStore keys;
  PeerSigningKeyResolver keys_resolver;
  std::string requester_account;
  Thread thread;
  std::string local_relay_id;
};

} // namespace

TEST(ChatHistoryResponderTest, ServesSignedOutboundRows) {
  ResponderHarness harness("serve");
  harness.SeedOutbound(1, "hello");
  harness.SeedOutbound(2, "world");

  const ChatHistoryRequest request = harness.SignedRequest();
  auto response = harness.Serve(request, request.requester_peer_id);
  ASSERT_TRUE(static_cast<bool>(response));
  ASSERT_EQ(response->messages.size(), 2u);
  EXPECT_EQ(response->messages[0].sender_seq, 1u);
  EXPECT_EQ(response->messages[1].sender_seq, 2u);
  auto verified = EnvelopeSigner::Verify(response->messages[0], Base64Encode(harness.local.device.signing_public));
  ASSERT_TRUE(static_cast<bool>(verified));
  EXPECT_TRUE(*verified);
}

// A message deleted on this device is never served again: its seq carries a retract aimed at itself, so the
// requester's gap closes; an annotation is served as an annotation (so a missed retract can be repaired).
TEST(ChatHistoryResponderTest, DeletedRowsServeAPlaceholderAndAnnotationsStayAnnotations) {
  ResponderHarness harness("deleted");
  harness.SeedOutbound(1, "hello");
  harness.SeedOutbound(2, "world");
  {
    ThreadMessage reaction;
    reaction.id = "out-3";
    reaction.thread_id = harness.thread.id;
    reaction.sender_contact_id = kLocalSelfContactId;
    reaction.content_type = ChatContentType::Annotation;
    reaction.target_message_id = "out-2";
    reaction.payload_json = R"({"annotation_type":"reaction","target_message_id":"out-2","value":"x"})";
    reaction.text = "x";
    reaction.timestamp = 3;
    reaction.delivery = MessageDelivery::Relayed;
    reaction.relay_visible = true;
    reaction.transport = MessageTransport::Local;
    reaction.sender_seq = 3;
    reaction.session_epoch = 1;
    ASSERT_TRUE(harness.store.AppendMessage(reaction)); // annotations are not re-sent as text
  }
  auto deleted = harness.store.DeleteMessageContent(harness.thread.id, "out-1");
  ASSERT_TRUE(deleted);
  ASSERT_TRUE(*deleted);

  const ChatHistoryRequest request = harness.SignedRequest();
  auto response = harness.Serve(request, request.requester_peer_id);
  ASSERT_TRUE(static_cast<bool>(response));
  ASSERT_EQ(response->messages.size(), 3u);
  EXPECT_EQ(response->messages[0].sender_seq, 1u); // the placeholder (content checked on the receive side)
  EXPECT_EQ(response->messages[0].message_id, "out-1");
  EXPECT_EQ(response->messages[1].sender_seq, 2u);
  EXPECT_EQ(response->messages[2].sender_seq, 3u);
}

TEST(ChatHistoryResponderTest, RejectsNonParticipantRequester) {
  ResponderHarness harness("reject");
  ChatHistoryRequest request = harness.SignedRequest();
  auto deleted = harness.store.DeleteThread(harness.thread.id);  // the signer has no thread with us
  ASSERT_TRUE(deleted && *deleted);
  auto response = harness.Serve(request, request.requester_peer_id);
  EXPECT_FALSE(static_cast<bool>(response));
}

// The requester is who signed, on the link it signed for, recently — never who the body names.
TEST(ChatHistoryResponderTest, ServesOnlyASignedRequestOnItsOwnLink) {
  ResponderHarness harness("auth");
  harness.SeedOutbound(1, "hello");
  const ChatHistoryRequest signed_request = harness.SignedRequest();
  ASSERT_TRUE(harness.Serve(signed_request, signed_request.requester_peer_id));

  EXPECT_FALSE(harness.Serve(signed_request, "QmSomeOtherPeer")) << "another link";

  ChatHistoryRequest unsigned_request = signed_request;
  unsigned_request.signature.clear();
  EXPECT_FALSE(harness.Serve(unsigned_request, signed_request.requester_peer_id)) << "unsigned";

  ChatHistoryRequest widened = signed_request;
  widened.limit = 500;
  EXPECT_FALSE(harness.Serve(widened, signed_request.requester_peer_id)) << "changed after signing";

  const ChatHistoryRequest old = harness.SignedRequest(util::NowUnixMs() - 2 * PeerRequestAuth::kMaxSkewMs);
  EXPECT_FALSE(harness.Serve(old, old.requester_peer_id)) << "stale";
}
