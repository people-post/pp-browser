#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "base/messaging/E2eIngestClassifier.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "base/messaging/SqliteThreadStore.h"
#include "foundation/crypto/MlDsa.h"
#include "base/people/IdentityStore.h"
#include "base/messaging/GroupRosterStore.h"
#include "feature/messaging/RelayReceivePipeline.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

ByteVector TestMasterPsk() {
  const auto bytes = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  EXPECT_TRUE(bytes);
  return *bytes;
}

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

void InstallTestPsk(SqlitePskSessionStore& psk_store, const Thread& thread) {
  PskSessionRecord record;
  record.key = E2eRelayPayloadCodec::ChatTargetFromThread(thread);
  record.session_epoch = 1;
  record.master_psk_b64 = Base64Encode(TestMasterPsk());
  record.psk_verified_at = 1;
  ASSERT_TRUE(static_cast<bool>(psk_store.Save(record)));
}

TEST(E2eRelayCryptoTest, EncryptDecryptRoundTrip) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_e2e_relay_crypto";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  SqlitePskSessionStore psk_store(store.ProfileDbPath(), "test");
  ASSERT_TRUE(static_cast<bool>(store.SetDek(TestDek())));
  ASSERT_TRUE(static_cast<bool>(psk_store.SetDek(TestDek())));

  DirectChatTarget target;
  target.peer_identity_kind = "account";
  target.peer_identity_value = "account:bob";
  target.channel = ThreadChannel::E2e;
  auto thread = store.FindOrCreateDirectThread(target, "contact-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(thread));
  InstallTestPsk(psk_store, *thread);

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "660e8400-e29b-41d4-a716-446655440001";
  envelope.sender_contact_id = "account:alice";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.sender_seq = 42;
  envelope.session_epoch = 1;
  envelope.timestamp = 1719662400456;

  E2eEncryptParams params;
  params.text = "Hello";
  params.channel = CryptoChannel::E2e;
  params.peer_contact_id = "account:local";
  params.sender_contact_id = "account:alice";
  params.message_id = envelope.message_id;
  params.sender_seq = envelope.sender_seq;
  params.session_epoch = envelope.session_epoch;
  params.timestamp = envelope.timestamp;

  auto payload_b64 = E2eRelayPayloadCodec::EncryptText(params, TestMasterPsk());
  ASSERT_TRUE(static_cast<bool>(payload_b64));
  envelope.body.e2e.payload_b64 = *payload_b64;

  auto body_hash = EnvelopeSigner::BodyHash(envelope.body);
  ASSERT_TRUE(static_cast<bool>(body_hash));
  auto body_hash_again = EnvelopeSigner::BodyHash(envelope.body);
  ASSERT_TRUE(static_cast<bool>(body_hash_again));
  EXPECT_EQ(*body_hash, *body_hash_again);

  const ChatTargetKey target_key = E2eRelayPayloadCodec::ChatTargetFromThread(*thread);
  auto decrypted = E2eRelayPayloadCodec::DecryptEnvelope(envelope, "account:local", target_key, psk_store);
  ASSERT_TRUE(static_cast<bool>(decrypted));
  EXPECT_EQ(decrypted->text, "Hello");
}

TEST(E2eRelayCryptoTest, ReceivePipelineDecryptsEncryptedEnvelope) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_e2e_pipeline_crypto";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  SqlitePskSessionStore psk_store(store.ProfileDbPath(), "test");
  ASSERT_TRUE(static_cast<bool>(store.SetDek(TestDek())));
  ASSERT_TRUE(static_cast<bool>(psk_store.SetDek(TestDek())));
  IdentityStore identity(data_dir.string(), "test");
  ASSERT_TRUE(static_cast<bool>(identity.SetDek(TestDek())));
  ASSERT_TRUE(static_cast<bool>(identity.LoadOrCreate()));
  const std::string local_account_id = identity.Get()->account_id;
  PeerSigningKeyStore key_store;

  auto peer_keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(peer_keys));
  PeerSigningKeyRecord record;
  record.signing_public_key_b64 = Base64Encode(peer_keys->public_key);
  record.source = "test";
  key_store.Put("account", "account:peer", record);

  DirectChatTarget target;
  target.peer_identity_kind = "account";
  target.peer_identity_value = "account:peer";
  target.channel = ThreadChannel::E2e;
  auto thread = store.FindOrCreateDirectThread(target, "contact-peer", "Peer");
  ASSERT_TRUE(static_cast<bool>(thread));
  InstallTestPsk(psk_store, *thread);

  PeerSigningKeyResolver key_resolver(key_store);
  GroupRosterStore roster_store(store.ProfileDbPath());
  RelayReceivePipeline pipeline(store, key_resolver, psk_store, identity, roster_store);

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "peer-msg-1";
  envelope.sender_contact_id = "account:peer";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope.timestamp = 1;

  E2eEncryptParams params;
  params.text = "secret";
  params.channel = CryptoChannel::E2e;
  params.peer_contact_id = local_account_id;
  params.sender_contact_id = "account:peer";
  params.message_id = envelope.message_id;
  params.sender_seq = envelope.sender_seq;
  params.session_epoch = envelope.session_epoch;
  params.timestamp = envelope.timestamp;
  auto payload_b64 = E2eRelayPayloadCodec::EncryptText(params, TestMasterPsk());
  ASSERT_TRUE(static_cast<bool>(payload_b64));
  envelope.body.e2e.payload_b64 = *payload_b64;

  auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  ASSERT_TRUE(static_cast<bool>(sign_bytes));
  auto signature =
      MlDsa::Sign(peer_keys->secret_key, *sign_bytes);
  ASSERT_TRUE(static_cast<bool>(signature));
  envelope.signature = Base64Encode(*signature);

  const RelayReceiveOutcome outcome = pipeline.ProcessEnvelope(envelope, local_account_id);
  EXPECT_EQ(outcome.decision, IngestDecision::AcceptBootstrap);
  EXPECT_TRUE(outcome.persisted);
}

} // namespace
