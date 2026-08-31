#include "base/crypto/AttachmentContentCipher.h"
#include "base/crypto/AttachmentContentHash.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/messaging/AttachmentCache.h"
#include "base/messaging/ChatBlobResponder.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/people/IdentityStore.h"

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
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_chat_blob_" + suffix)),
        store(data_dir.string()), identity(data_dir.string(), "test") {
    std::filesystem::remove_all(data_dir);
    if (!identity.SetDek(TestDek()) || !store.SetDek(TestDek())) {
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
    profile_data_dir = data_dir.string();

    DirectChatTarget target;
    target.peer_identity_kind = "relay_user";
    target.peer_identity_value = "relay:peer";
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

  void SeedAttachmentMessage(const ChatAttachmentFields& fields) {
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
    row.sender_contact_id = kLocalSelfContactId;
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
    return request;
  }

  std::filesystem::path data_dir;
  SqliteThreadStore store;
  IdentityStore identity;
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
  request.requester_identity_value = "relay:stranger";

  auto response = ChatBlobResponder::ServeFetch(harness.store, request, harness.local_relay_id,
                                                harness.profile_data_dir);
  EXPECT_FALSE(static_cast<bool>(response));
}

TEST(ChatBlobResponderTest, ServeFetchReturnsCiphertextForCachedPlaintext) {
  ChatBlobResponderHarness harness("serve");
  const auto fields = harness.MakeAttachmentFields();
  harness.SeedAttachmentMessage(fields);
  ASSERT_TRUE(static_cast<bool>(
      SaveAttachmentPlaintext(harness.profile_data_dir, harness.thread.id, fields.content_hash, fields.mime,
                              harness.plain, fields.filename)));

  auto response = ChatBlobResponder::ServeFetch(harness.store, harness.MakeFetchRequest(fields),
                                                harness.local_relay_id, harness.profile_data_dir);
  ASSERT_TRUE(static_cast<bool>(response));

  const ByteVector cipher_bytes(response->begin(), response->end());
  auto decrypted =
      AttachmentContentCipher::Decrypt(fields.content_key, fields.blob_nonce, cipher_bytes, fields.content_hash);
  ASSERT_TRUE(static_cast<bool>(decrypted));
  EXPECT_EQ(*decrypted, harness.plain);
}
