#include "foundation/crypto/AttachmentContentCipher.h"
#include "domain/attachments/AttachmentContent.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatBlobResponder.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/ChatPayloadValidator.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/people/IdentityStore.h"
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

class ChatBlobResponderHarness {
public:
  explicit ChatBlobResponderHarness(const std::string& suffix)
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_chat_blob_" + suffix + "_" + util::GenerateUuid())),
        store(data_dir.string()), identity(data_dir.string(), "test"),
        requester((data_dir / "requester").string(), "requester"), keys_resolver(keys) {
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir / "requester");
    if (!identity.SetDek(TestDek()) || !store.SetDek(TestDek()) || !requester.SetDek(TestDek())) {
      throw std::runtime_error("dek setup failed");
    }
    if (!identity.LoadOrCreate() || !requester.LoadOrCreate()) {
      throw std::runtime_error("identity load failed");
    }
    requester_account = requester.Get()->account_id;
    requester_peer = *PeerRequestAuth::DevicePeerId(requester.Get()->public_key_b64);
    PeerSigningKeyRecord key;
    key.signing_public_key_b64 = requester.Get()->account_signing_public_key_b64;
    key.source = "test";
    keys.Put("account", requester_account, key);
    {
      LocalIdentity updated = *identity.Get();
      updated.relay_user_id = "relay:local";
      updated.registered = true;
      if (!identity.Update(updated)) {
        throw std::runtime_error("Failed to set test relay id");
      }
    }
    local_relay_id = identity.Get()->relay_user_id;
    profile_data_dir = data_dir.string();

    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = requester_account;
    target.channel = ThreadChannel::E2e;
    auto created = store.FindOrCreateDirectThread(target, "contact-peer", "Peer");
    if (!created) {
      throw std::runtime_error("thread create failed");
    }
    thread = *created;
  }

  ChatAttachmentFields MakeAttachmentFields() {
    auto hash = AttachmentContentHash(plain);
    if (!hash) {
      throw std::runtime_error("attachment hash failed");
    }
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/blobs/peer-only";
    fields.mime = "image/png";
    fields.filename = "photo.png";
    fields.byte_length = plain.size();
    fields.content_hash.assign(hash->begin(), hash->end());
    fields.content_key.assign(kSessionKeySize, 0x55);
    fields.blob_nonce.assign(kAeadNonceSize, 0x66);
    return fields;
  }

  void SeedAttachmentMessage(const ChatAttachmentFields& fields, const std::string& sender = kLocalSelfContactId) {
    auto encoded = ChatPayloadCodec::EncodeAttachment(fields, fields.filename);
    if (!encoded) {
      throw std::runtime_error("encode attachment failed");
    }
    auto message = ChatPayloadValidator::DecodeValidated(*encoded);
    if (!message) {
      throw std::runtime_error("validate attachment failed");
    }
    ThreadMessage row;
    row.id = "att-1";
    row.thread_id = thread.id;
    row.sender_contact_id = sender;
    row.text = message->text;
    row.payload_json = message->payload_json;
    row.content_type = message->content_type;
    row.timestamp = 1;
    row.delivery = MessageDelivery::Relayed;
    row.relay_visible = true;
    row.transport = MessageTransport::Local;
    if (!store.AppendMessage(row)) {
      throw std::runtime_error("append attachment failed");
    }
  }

  ChatBlobRequest MakeFetchRequest(const ChatAttachmentFields& fields) {
    ChatBlobRequest request;
    request.op = ChatBlobOp::Fetch;
    request.requester_identity_kind = "relay_user";
    request.requester_identity_value = "relay:peer";
    request.peer_identity_kind = "relay_user";
    request.peer_identity_value = local_relay_id;
    request.thread_id = thread.id;
    request.channel = ThreadChannel::E2e;
    request.content_hash_hex = AttachmentHashHex(fields.content_hash);
    return Signed(request);
  }

  ChatBlobRequest MakePushRequest(const ChatAttachmentFields& fields) {
    ChatBlobRequest request = MakeFetchRequest(fields);
    request.op = ChatBlobOp::Push;
    return Signed(request);
  }

  ChatBlobRequest Signed(ChatBlobRequest request) {
    if (!PeerRequestAuth::Sign(request, requester_account, requester_peer, util::NowUnixMs(),
                               [this](const std::vector<uint8_t>& bytes) { return requester.SignBytes(bytes); })) {
      throw std::runtime_error("sign request");
    }
    return request;
  }

  /** The request arrived on the requester's own link. */
  ChatBlobRequester OnItsLink() { return ChatBlobRequester{requester_peer, &keys_resolver, util::NowUnixMs()}; }

  std::filesystem::path data_dir;
  SqliteThreadStore store;
  IdentityStore identity;
  IdentityStore requester;
  PeerSigningKeyStore keys;
  PeerSigningKeyResolver keys_resolver;
  std::string requester_account;
  std::string requester_peer;
  Thread thread;
  std::string local_relay_id;
  std::string profile_data_dir;
  ByteVector plain{'h', 'e', 'l', 'l', 'o'};
};

} // namespace

TEST(ChatBlobResponderTest, RejectsNonParticipantRequester) {
  ChatBlobResponderHarness harness("reject");
  const auto fields = harness.MakeAttachmentFields();
  harness.SeedAttachmentMessage(fields);

  ChatBlobRequest request = harness.MakeFetchRequest(fields);
  auto deleted = harness.store.DeleteThread(harness.thread.id);  // the signer has no thread with us
  ASSERT_TRUE(deleted && *deleted);

  auto response = ChatBlobResponder::ServeFetch(harness.store, request, harness.local_relay_id,
                                                harness.profile_data_dir, ChatBlobContentSource{}, harness.OnItsLink());
  EXPECT_FALSE(static_cast<bool>(response));
}

TEST(ChatBlobResponderTest, ServeFetchReturnsCiphertextForCachedPlaintext) {
  ChatBlobResponderHarness harness("serve");
  const auto fields = harness.MakeAttachmentFields();
  harness.SeedAttachmentMessage(fields);
  const ByteVector dek = TestDek();
  ASSERT_TRUE(static_cast<bool>(
      SaveAttachmentPlaintext(harness.profile_data_dir, "profile-a", fields.content_hash, fields.mime, harness.plain, fields.filename, dek)));

  ChatBlobContentSource content;
  content.private_plaintext = [&](const std::vector<uint8_t>& content_hash) {
    return LoadAttachmentPlaintext(harness.profile_data_dir, "profile-a", content_hash, dek);
  };
  auto response = ChatBlobResponder::ServeFetch(harness.store, harness.MakeFetchRequest(fields),
                                                harness.local_relay_id, harness.profile_data_dir, content,
                                                harness.OnItsLink());
  ASSERT_TRUE(static_cast<bool>(response));

  const ByteVector cipher_bytes(response->begin(), response->end());
  auto decrypted =
      AttachmentContentCipher::Decrypt(fields.content_key, fields.blob_nonce, cipher_bytes, fields.content_hash);
  ASSERT_TRUE(static_cast<bool>(decrypted));
  EXPECT_EQ(*decrypted, harness.plain);
}

TEST(ChatBlobResponderTest, ServePushAcceptsHashBeforeAttachmentMessageArrives) {
  // Real wire order: the sender pushes the ciphertext first, then the attachment message
  // itself follows separately. ServePush must not require the hash to already be in history.
  ChatBlobResponderHarness harness("push-before-message");
  const auto fields = harness.MakeAttachmentFields();
  const ByteVector ciphertext{'c', 'i', 'p', 'h', 'e', 'r'};

  auto result = ChatBlobResponder::ServePush(harness.store, harness.MakePushRequest(fields), harness.local_relay_id,
                                             harness.profile_data_dir, ciphertext, harness.OnItsLink());
  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;

  auto loaded = LoadPendingAttachmentCiphertext(harness.profile_data_dir, harness.thread.id, fields.content_hash);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(*loaded, ciphertext);

  harness.SeedAttachmentMessage(fields);
}

TEST(ChatBlobResponderTest, ServePushRejectsPastPerThreadPendingCap) {
  ChatBlobResponderHarness harness("push-cap");
  const ByteVector ciphertext{'c', 'i', 'p', 'h', 'e', 'r'};

  Roe<void> last_result;
  for (int i = 0; i < 65; ++i) {
    ChatAttachmentFields fields = harness.MakeAttachmentFields();
    fields.content_hash.assign(kAttachmentContentHashSize, static_cast<uint8_t>(i));
    last_result = ChatBlobResponder::ServePush(harness.store, harness.MakePushRequest(fields),
                                               harness.local_relay_id, harness.profile_data_dir, ciphertext,
                                               harness.OnItsLink());
  }
  EXPECT_FALSE(static_cast<bool>(last_result));
}

// Fetch serves only what this device sent in the requester's thread: an attachment row the peer
// sent us (its key, our content's hash) never authorizes serving our content.
TEST(ChatBlobResponderTest, FetchServesOnlyAttachmentsWeSent) {
  ChatBlobResponderHarness harness("ours-only");
  const auto fields = harness.MakeAttachmentFields();
  harness.SeedAttachmentMessage(fields, harness.requester_account);  // a row the peer sent us
  const ByteVector dek = TestDek();
  ASSERT_TRUE(static_cast<bool>(SaveAttachmentPlaintext(harness.profile_data_dir, "profile-a", fields.content_hash,
                                                        fields.mime, harness.plain, fields.filename, dek)));
  ChatBlobContentSource content;
  content.private_plaintext = [&](const std::vector<uint8_t>& content_hash) {
    return LoadAttachmentPlaintext(harness.profile_data_dir, "profile-a", content_hash, dek);
  };
  auto response = ChatBlobResponder::ServeFetch(harness.store, harness.MakeFetchRequest(fields), harness.local_relay_id,
                                                harness.profile_data_dir, content, harness.OnItsLink());
  EXPECT_FALSE(static_cast<bool>(response));
}

// The requester is who signed the request, on the link it signed for.
TEST(ChatBlobResponderTest, FetchNeedsARequestSignedForItsLink) {
  ChatBlobResponderHarness harness("signed");
  const auto fields = harness.MakeAttachmentFields();
  harness.SeedAttachmentMessage(fields);
  const ChatBlobRequest request = harness.MakeFetchRequest(fields);

  ChatBlobRequester other_link = harness.OnItsLink();
  other_link.remote_peer_id = "QmSomeOtherPeer";
  EXPECT_FALSE(ChatBlobResponder::ServeFetch(harness.store, request, harness.local_relay_id, harness.profile_data_dir,
                                             ChatBlobContentSource{}, other_link));
  ChatBlobRequest unsigned_request = request;
  unsigned_request.signature.clear();
  EXPECT_FALSE(ChatBlobResponder::ServeFetch(harness.store, unsigned_request, harness.local_relay_id,
                                             harness.profile_data_dir, ChatBlobContentSource{}, harness.OnItsLink()));
}
